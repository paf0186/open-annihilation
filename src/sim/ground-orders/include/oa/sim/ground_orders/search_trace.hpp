// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace oa::sim::ground_orders {
struct SearchMapCell;

inline constexpr std::size_t reconstructed_search_path_capacity = 64;

/// Holds the last 64 turns of a reconstructed route, start first.
struct ReconstructedSearchPath {
    std::array<std::array<int16_t, 2>, reconstructed_search_path_capacity> points{};
    std::size_t count{};

    /// Returns the first route point.
    /// @return iterator to the first route point
    auto begin() const noexcept { return points.begin(); }

    /// Returns the end of the retained route points.
    /// @return iterator past the last route point
    auto end() const noexcept { return points.begin() + static_cast<std::ptrdiff_t>(count); }

    /// Returns the number of retained route points.
    /// @return route point count, at most 64
    std::size_t size() const noexcept { return count; }

    /// Tells whether the route has no points.
    /// @return true for an empty route
    bool empty() const noexcept { return count == 0; }
};

/// Rebuilds a route from the finish cell back to the start through the predecessor directions.
///
/// Only cells where the direction changes are kept, in a ring of the last 64,
/// and the route is returned start first as footprint centres. A truncated map,
/// a direction past 7, a path leaving the map or a cycle gives no route.
///
/// @param width search map width in cells
/// @param height search map height in cells
/// @param start start cell
/// @param finish finish cell
/// @param footprint_x unit footprint width in cells
/// @param footprint_z unit footprint depth in cells
/// @param predecessor_direction SearchMapCell::predecessor of each cell, 0..7
/// @return route points in integer world X/Z, at most 64; none for a broken map
std::vector<std::array<int16_t, 2>> reconstruct_search_path(
    uint32_t width,
    uint32_t height,
    std::array<int16_t, 2> start,
    std::array<int16_t, 2> finish,
    int16_t footprint_x,
    int16_t footprint_z,
    std::span<const uint8_t> predecessor_direction
);
/// Rebuilds a route directly from search cells, reading their predecessor fields.
///
/// Retains the byte-map overload's validation and last-64-turn route.
///
/// @param width search map width in cells
/// @param height search map height in cells
/// @param start start cell
/// @param finish finish cell
/// @param footprint_x unit footprint width in cells
/// @param footprint_z unit footprint depth in cells
/// @param cells search cells containing predecessor directions, 0..7
/// @return route points in integer world X/Z, at most 64; none for a broken map
ReconstructedSearchPath reconstruct_search_path_from_cells(
    uint32_t width,
    uint32_t height,
    std::array<int16_t, 2> start,
    std::array<int16_t, 2> finish,
    int16_t footprint_x,
    int16_t footprint_z,
    std::span<const SearchMapCell> cells
);
} // namespace oa::sim::ground_orders
