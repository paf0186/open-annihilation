#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Synchronous JSON-lines client for the native training worker (stdlib only).

Coordinates use the engine's world axes: X/Z horizontal, Y elevation. A move
point such as (800, 640) means x_world=800, z_world=640, not screen pixels.
Unit handles are opaque and must come from the current episode's observation.
The client is single-threaded; only its bounded stdout reader runs concurrently.
"""
import json
import math
import os
from pathlib import Path
import queue
import subprocess
import tempfile
import threading

MAX_LINE_BYTES = 1024 * 1024
MAX_REQUEST_ID = (1 << 53) - 1


class WorkerError(RuntimeError):
    """A valid native error response; validation errors leave the worker usable."""

    def __init__(self, response):
        self.response = response
        super().__init__(str(response.get("error", response)))


class ProtocolError(RuntimeError):
    """The worker timed out, exited or violated its transport contract."""


def _json_float(text):
    value = float(text)
    if not math.isfinite(value):
        raise ValueError("nonfinite JSON number")
    return value


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate response key: {key}")
        result[key] = value
    return result


def _read_responses(stream, mailbox):
    try:
        while True:
            line = stream.readline(MAX_LINE_BYTES + 1)
            if not line:
                message = ProtocolError("worker stdout closed")
            elif len(line) > MAX_LINE_BYTES or not line.endswith(b"\n"):
                message = ProtocolError("worker response exceeded limit or was not newline terminated")
            else:
                try:
                    message = json.loads(line.decode("utf-8"), object_pairs_hook=_unique_object, parse_float=_json_float,
                                         parse_constant=lambda value: (_ for _ in ()).throw(
                                             ValueError(f"nonfinite JSON value: {value}")))
                except (ValueError, UnicodeError) as error:
                    message = ProtocolError(f"invalid worker JSON: {error}")
            try:
                mailbox.put_nowait(message)
            except queue.Full:
                return
            if isinstance(message, Exception):
                return
    except (OSError, ValueError) as error:
        try:
            mailbox.put_nowait(ProtocolError(str(error)))
        except queue.Full:
            pass


class TrainingClient:
    """Own one worker process; reset destroys it and creates a fresh episode.

    Diagnostics go directly to a persistent file, so stderr cannot block on an
    unread pipe. Inspect stderr_path on startup or transport failure. A native
    request error raises WorkerError; a transport error kills and reaps the
    process. Command-line setup arguments are forwarded without a shell.
    """

    def __init__(self, native, game_dir, *, seed=123, timeout=30, scratch_root=None,
                 setup_args=()):
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("timeout must be finite and positive")
        self.native = str(Path(native).resolve())
        self.game_dir = str(Path(game_dir).resolve())
        self.seed = seed
        self.timeout = timeout
        self.setup_args = list(setup_args)
        self.scratch_root = scratch_root
        self.process = None
        self.stderr_path = None
        self.hello_response = None
        self._reader = None
        self._next_id = 1

    def reset(self, *, seed=None):
        """Create a fresh process and return its readiness handshake."""
        self.close()
        if seed is not None:
            self.seed = seed
        if self.scratch_root is not None:
            Path(self.scratch_root).mkdir(parents=True, exist_ok=True)
        folder = Path(tempfile.mkdtemp(prefix="oa-training-", dir=self.scratch_root)).resolve()
        preferences = folder / "preferences.conf"
        preferences.write_text("open-annihilation-preferences 1\n", encoding="utf-8")
        self.stderr_path = folder / "worker.stderr.log"
        command = [self.native, "--game-dir", self.game_dir, "--training-worker",
                   "--headless-check", "--skip-intro", "--mute", "--seed", str(self.seed),
                   "--preferences-file", str(preferences), *self.setup_args]
        environment = os.environ.copy()
        environment.update(SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                           SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
        self._mailbox = queue.Queue(maxsize=2)
        self._next_id = 1
        try:
            with self.stderr_path.open("wb") as diagnostics:
                self.process = subprocess.Popen(command, cwd=folder, env=environment,
                                                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                                stderr=diagnostics, bufsize=0)
            self._reader = threading.Thread(target=_read_responses,
                                            args=(self.process.stdout, self._mailbox), daemon=True)
            self._reader.start()
            self.hello_response = self.hello()
            return self.hello_response
        except BaseException:
            self._dispose()
            raise

    def request(self, op, **fields):
        """Send one bounded request; verify the response ID and success envelope."""
        if self.process is None:
            raise ProtocolError("worker is not running; call reset()")
        if self._next_id > MAX_REQUEST_ID:
            self._dispose()
            raise ProtocolError("request IDs exhausted; reset the worker")
        if "id" in fields or "op" in fields:
            raise ValueError("request envelope fields cannot be overridden")
        request_id = self._next_id
        self._next_id += 1
        request = {"id": request_id, "op": op, **fields}
        data = (json.dumps(request, separators=(",", ":"), allow_nan=False) + "\n").encode("utf-8")
        if len(data) > MAX_LINE_BYTES:
            raise ValueError("request exceeds line limit")
        # A full pipe could otherwise block before the response deadline starts.
        # Requests are bounded and the worker reads a full request before replying;
        # write in a helper so a wedged worker is still terminated on deadline.
        process, mailbox = self.process, self._mailbox

        def write_request():
            try:
                remaining = memoryview(data)
                while remaining:
                    written = process.stdin.write(remaining)
                    if not written:
                        raise OSError("worker stdin closed during write")
                    remaining = remaining[written:]
                process.stdin.flush()
            except (OSError, ValueError) as error:
                try:
                    mailbox.put_nowait(ProtocolError(f"worker stdin failed: {error}"))
                except queue.Full:
                    pass
        writer = threading.Thread(target=write_request, daemon=True)
        writer.start()
        try:
            response = self._mailbox.get(timeout=self.timeout)
            if isinstance(response, Exception):
                raise response
            if (op == "hello" and request_id == 1 and isinstance(response, dict) and
                    response.get("id") is None and response.get("ok") is False and
                    isinstance(response.get("error"), dict) and
                    response["error"].get("code") == "setup_failed"):
                raise WorkerError(response)
            if (not isinstance(response, dict) or type(response.get("id")) is not int or
                    response["id"] != request_id or type(response.get("ok")) is not bool):
                raise ProtocolError("worker response has an invalid envelope or mismatched ID")
            if not response["ok"]:
                raise WorkerError(response)
            return response
        except queue.Empty:
            self._dispose()
            raise ProtocolError(f"worker response timed out after {self.timeout} seconds") from None
        except ProtocolError:
            self._dispose()
            raise
        finally:
            writer.join(timeout=0.1)

    def hello(self):
        response = self.request("hello", version=1)
        if response.get("version") != 1:
            self._dispose()
            raise ProtocolError("worker did not acknowledge protocol version 1")
        return response

    def observe(self):
        return self.request("observe", player=self.hello_response["player"])

    def step(self, commands=(), *, ticks=15):
        return self.request("step", player=self.hello_response["player"], ticks=ticks, commands=list(commands))

    @staticmethod
    def move(unit, x_world, z_world, *, queue=False):
        return {"kind": "move", "unit": unit,
                "point": {"x_world": x_world, "z_world": z_world}, "queue": queue}

    @staticmethod
    def stop(unit):
        return {"kind": "stop", "unit": unit}

    def _dispose(self):
        process, self.process = self.process, None
        if process is None:
            return
        if process.poll() is None:
            process.kill()
        process.wait()
        for stream in (process.stdin, process.stdout):
            if stream is not None:
                stream.close()
        if self._reader is not None:
            self._reader.join(timeout=1)
            self._reader = None

    def close(self):
        if self.process is not None:
            try:
                self.request("close")
                self.process.wait(timeout=min(self.timeout, 2))
            except (WorkerError, ProtocolError, OSError, subprocess.TimeoutExpired):
                pass
            finally:
                self._dispose()

    def __enter__(self):
        self.reset()
        return self

    def __exit__(self, *_):
        self.close()
