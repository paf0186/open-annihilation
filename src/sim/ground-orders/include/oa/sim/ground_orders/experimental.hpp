// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

namespace oa::sim::ground_orders {

/// Selects compatible priority or one of the offline priority experiments.
enum class ExperimentalPathfinder : uint8_t { compatible, weighted, greedy };

/// Minimum weighted priority in 16.16; larger compatible adaptive weights stay larger.
inline constexpr int32_t experimental_weighted_priority_floor = 7 << 14;
/// Greedy priority in 16.16, trading movement cost for fewer expansions.
inline constexpr int32_t experimental_greedy_priority_scale = 24 << 16;

/// Sets process-wide path search priority before starting an offline match.
///
/// Weighted search uses g + max(compatible_weight, 1.75) * goal_cost to order
/// the heap. Greedy search uses g + 24 * goal_cost and can choose substantially
/// more expensive routes. Both retain movement and turn costs, goal acceptance,
/// footprints and search budgets. Routes and their publication ticks may change.
/// Do not change the mode while a search is active, or use either experiment
/// with compatible peers or recordings.
///
/// @param mode compatible, conservative weighted, or greedy priority
void set_experimental_pathfinder(ExperimentalPathfinder mode) noexcept;

/// Returns the process-wide path search priority selection.
///
/// @return the mode selected before match startup
[[nodiscard]] ExperimentalPathfinder experimental_pathfinder() noexcept;

} // namespace oa::sim::ground_orders
