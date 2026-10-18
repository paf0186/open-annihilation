// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string_view>

namespace oa::app {
/// Reserves the original stdout for training before startup diagnostics and disables launch input.
/// @param argc argument count
/// @param argv arguments
/// @return whether training was requested
bool prepare_training_transport(int argc, char** argv);
/// Writes one response to the reserved training stream.
/// @param response complete JSON object
void write_training_response(std::string_view response);
} // namespace oa::app
