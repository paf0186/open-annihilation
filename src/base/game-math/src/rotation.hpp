// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/base/game_math.hpp"

namespace oa::base::game_math::detail {
/// Rotates coordinates using the integer product path on every processor.
///
/// @param first first coordinate
/// @param second second coordinate
/// @param angle angle word, 65536 per turn
/// @return the pair with four 53-bit products and two binary64 sums
[[nodiscard]] RotatedPair
integer_rotate_pair(int32_t first, int32_t second, int16_t angle) noexcept;

/// Reports whether exact native rotations support this processor and table.
///
/// @return whether native products can keep the integer path's results and flags
[[nodiscard]] bool native_rotation_available() noexcept;
} // namespace oa::base::game_math::detail
