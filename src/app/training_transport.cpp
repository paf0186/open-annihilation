// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#include "training_transport.hpp"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#else
#include <unistd.h>
#endif
namespace oa::app {
namespace {
FILE* protocol_output{};
}

bool prepare_training_transport(int argc, char** argv) {
    bool wanted = false;
    for (int index = 1; index < argc; ++index)
        wanted = wanted || std::strcmp(argv[index], "--training-worker") == 0;
    if (!wanted)
        return false;
    std::fflush(stdout);
#ifdef _WIN32
    const auto duplicate_descriptor = [](DWORD stream, int mode) {
        const HANDLE source = GetStdHandle(stream);
        HANDLE copy = nullptr;
        if (source == nullptr || source == INVALID_HANDLE_VALUE ||
            !DuplicateHandle(
                GetCurrentProcess(),
                source,
                GetCurrentProcess(),
                &copy,
                0,
                FALSE,
                DUPLICATE_SAME_ACCESS
            ))
            throw std::runtime_error("training requires inherited standard streams");
        const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(copy), mode);
        if (descriptor < 0) {
            CloseHandle(copy);
            throw std::runtime_error("cannot open inherited training stream");
        }
        return descriptor;
    };
    const int saved = duplicate_descriptor(STD_OUTPUT_HANDLE, _O_WRONLY | _O_BINARY);
    const int diagnostics = duplicate_descriptor(STD_ERROR_HANDLE, _O_WRONLY | _O_BINARY);
    const int input = duplicate_descriptor(STD_INPUT_HANDLE, _O_RDONLY | _O_BINARY);
    if (std::freopen("NUL", "w", stdout) == nullptr ||
        std::freopen("NUL", "w", stderr) == nullptr || std::freopen("NUL", "r", stdin) == nullptr ||
        _dup2(diagnostics, _fileno(stdout)) != 0 || _dup2(diagnostics, _fileno(stderr)) != 0 ||
        _dup2(input, _fileno(stdin)) != 0)
        throw std::runtime_error("cannot attach training standard streams");
    _close(diagnostics);
    _close(input);
    SetStdHandle(STD_INPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdin))));
    SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))));
    SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr))));
    protocol_output = _fdopen(saved, "wb");
#else
    const int saved = dup(fileno(stdout));
    if (saved < 0 || dup2(fileno(stderr), fileno(stdout)) < 0)
        throw std::runtime_error("cannot reserve training output");
    protocol_output = fdopen(saved, "w");
#endif
    if (protocol_output == nullptr)
        throw std::runtime_error("cannot open training output");
    return true;
}

void write_training_response(std::string_view response) {
    if (protocol_output == nullptr ||
        std::fwrite(response.data(), 1, response.size(), protocol_output) != response.size() ||
        std::fputc('\n', protocol_output) == EOF || std::fflush(protocol_output) != 0)
        throw std::runtime_error("training output closed");
}
} // namespace oa::app
