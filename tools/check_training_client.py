#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Test client deadlines, cleanup and framing using a subprocess fixture.

The fixture tests transport failure paths only; native game checks live in
check_native_training_worker.py.
"""
from pathlib import Path
import sys
import tempfile
import unittest
from training_client import ProtocolError, TrainingClient, WorkerError

FIXTURE = r'''
import json, sys, time
mode = sys.argv[sys.argv.index('--fixture-mode') + 1]
if mode == 'setup-failed':
    print(json.dumps({'id': None, 'ok': False, 'error': {
        'code': 'setup_failed', 'message': 'map could not be loaded'}}), flush=True)
    raise SystemExit(1)
if mode == 'exit':
    raise SystemExit(2)
if mode == 'stderr':
    sys.stderr.write('diagnostic' * 100000)
    sys.stderr.flush()
for line in sys.stdin:
    request = json.loads(line)
    op = request['op']
    response = {'id': request['id'], 'ok': True, 'version': 1, 'tick': 0, 'player': 0}
    if mode == 'malformed':
        print('not json', flush=True)
        continue
    if mode == 'duplicate-key':
        print('{"id":1,"id":1,"ok":true}', flush=True)
        continue
    if mode == 'oversize':
        print('x' * (1024 * 1024 + 1), flush=True)
        continue
    if mode == 'wrong-id':
        response['id'] += 1
    if mode == 'timeout':
        time.sleep(60)
    if mode == 'blocked-write' and op == 'hello':
        print(json.dumps(response), flush=True)
        time.sleep(60)
    if mode == 'reject' and op == 'step':
        response = {'id': request['id'], 'ok': False,
                    'error': {'code': 'invalid_unit', 'message': 'not controllable'}}
    print(json.dumps(response), flush=True)
    if op == 'close':
        break
'''


class ClientChecks(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="training-client-check-")
        self.addCleanup(self.scratch.cleanup)
        self.folder = Path(self.scratch.name)
        self.fixture = self.folder / "worker"
        self.fixture.write_text(f"#!{sys.executable}\n" + FIXTURE, encoding="utf-8")
        self.fixture.chmod(0o700)

    def worker(self, mode):
        worker = TrainingClient(self.fixture, self.folder, timeout=0.5,
                                scratch_root=self.folder,
                                setup_args=["--fixture-mode", mode])
        self.addCleanup(worker.close)
        return worker

    def test_reset_context_and_stderr_drain(self):
        worker = self.worker("stderr")
        with worker:
            old = worker.process
            self.assertEqual(worker.observe()["tick"], 0)
            worker.reset()
            self.assertIsNot(worker.process, old)
            self.assertIsNotNone(old.poll())
            self.assertGreater(worker.stderr_path.stat().st_size, 65536)
        self.assertIsNone(worker.process)

    def test_startup_error_preserves_diagnostics_and_reaps(self):
        worker = self.worker("setup-failed")
        with self.assertRaises(WorkerError) as caught:
            worker.reset()
        self.assertEqual(caught.exception.response["error"]["message"],
                         "map could not be loaded")
        self.assertIsNone(worker.process)

    def test_native_error_preserves_process(self):
        worker = self.worker("reject")
        worker.reset()
        with self.assertRaises(WorkerError):
            worker.step([worker.stop("missing")])
        self.assertEqual(worker.observe()["tick"], 0)

    def test_startup_failures_reap(self):
        for mode in ("exit", "malformed", "duplicate-key", "oversize", "wrong-id", "timeout"):
            with self.subTest(mode=mode):
                worker = self.worker(mode)
                with self.assertRaises(ProtocolError):
                    worker.reset()
                self.assertIsNone(worker.process)

    def test_blocked_write_deadline(self):
        worker = self.worker("blocked-write")
        worker.reset()
        with self.assertRaises(ProtocolError):
            worker.request("step", padding="x" * 900000)
        self.assertIsNone(worker.process)

    def test_local_validation(self):
        worker = self.worker("valid")
        worker.reset()
        with self.assertRaises(ValueError):
            worker.request("step", padding="x" * (1024 * 1024))
        with self.assertRaises(ValueError):
            worker.request("step", id=2)
        self.assertEqual(worker.observe()["tick"], 0)


if __name__ == "__main__":
    unittest.main()
