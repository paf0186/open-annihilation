// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

namespace oa::base::game_math {
/// Selects binary64 coordinate rotations for an experimental offline run.
///
/// The default is disabled. Set this before starting a match or any worker
/// threads and keep it unchanged throughout the run. Enabled rotations use
/// the same angle table, with coefficients and products rounded to binary64;
/// their results may differ from normal simulation arithmetic. This setting
/// is not saved and must not be used with saved, networked or recorded games.
///
/// @param enabled whether experimental rotations are enabled
void set_experimental_math(bool enabled) noexcept;

/// Reports whether binary64 coordinate rotations are selected.
///
/// @return whether experimental rotations are enabled
[[nodiscard]] bool experimental_math_enabled() noexcept;
} // namespace oa::base::game_math
