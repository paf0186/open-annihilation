// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

namespace oa::sim::ground_orders {

/// Selects compatible priority or the offline weighted priority experiment.
enum class ExperimentalPathfinder : uint8_t { compatible, weighted };

/// Minimum weighted priority in 16.16; larger compatible adaptive weights stay larger.
inline constexpr int32_t experimental_weighted_priority_floor = 7 << 14;

/// Sets process-wide path search priority before starting an offline match.
///
/// Weighted search uses g + max(compatible_weight, 1.75) * goal_cost to order
/// the heap. It retains movement and turn costs, goal acceptance, footprints
/// and search budgets. Routes and their publication ticks may change. Do not
/// change the mode while a search is active, or use the experiment with
/// compatible peers or recordings.
///
/// @param mode compatible or conservative weighted priority
void set_experimental_pathfinder(ExperimentalPathfinder mode) noexcept;

/// Returns the process-wide path search priority selection.
///
/// @return the mode selected before match startup
[[nodiscard]] ExperimentalPathfinder experimental_pathfinder() noexcept;

} // namespace oa::sim::ground_orders
