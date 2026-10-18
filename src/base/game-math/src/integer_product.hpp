// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/base/game_math/extended.hpp"

namespace oa::base::game_math::detail {
/// Multiplies an Extended factor by an integer, rounding the product once.
///
/// The exact product is rounded to a 53-bit significand, then converted to
/// binary64. Subnormal results retain both rounding steps. A zero keeps the
/// product's sign, including when the integer coordinate is zero.
///
/// @param factor finite factor
/// @param coordinate integer coordinate
/// @return the same result as to_double(multiply(factor, to_extended(coordinate), bits_53))
[[nodiscard]] double integer_product(Extended factor, int32_t coordinate) noexcept;
} // namespace oa::base::game_math::detail
