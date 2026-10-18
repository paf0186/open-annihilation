// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

namespace oa::sim::ground_orders {

/// Selects the compatible search or a stronger heuristic priority.
enum class ExperimentalPathfinder : uint8_t { compatible, weighted };

/// Sets process-wide path search priority before starting an offline match.
///
/// Weighted search uses g + 24 * goal_cost to order the heap. Movement costs,
/// turn costs, goal acceptance, footprints and search budgets stay unchanged.
/// Routes and their publication ticks may change. Do not change the mode while
/// a search is active, or use weighted mode with compatible peers or recordings.
///
/// @param mode compatible priority or the experimental weighted priority
void set_experimental_pathfinder(ExperimentalPathfinder mode) noexcept;

/// Returns the process-wide path search priority selection.
///
/// @return the mode selected before match startup
[[nodiscard]] ExperimentalPathfinder experimental_pathfinder() noexcept;

} // namespace oa::sim::ground_orders
