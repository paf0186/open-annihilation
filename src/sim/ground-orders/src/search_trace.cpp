// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/ground_orders/search_trace.hpp"
#include "oa/sim/ground_orders/search_worker.hpp"
#include <algorithm>

namespace oa::sim::ground_orders {
namespace {
constexpr std::size_t retained_turns = reconstructed_search_path_capacity;
constexpr std::array<int8_t, 8> direction_x{0, -1, -1, -1, 0, 1, 1, 1};
constexpr std::array<int8_t, 8> direction_z{-1, -1, 0, 1, 1, 1, 0, -1};

/// Rebuilds a route by reading predecessor directions from each visited cell.
///
/// @param width search map width in cells
/// @param height search map height in cells
/// @param start start cell
/// @param finish finish cell
/// @param footprint_x unit footprint width in cells
/// @param footprint_z unit footprint depth in cells
/// @param cells map entries, one per cell
/// @param predecessor direction accessor for a map entry
/// @return route points in integer world X/Z, at most 64; none for a broken map
template <class Cell, class Predecessor>
ReconstructedSearchPath reconstruct_path(
    uint32_t width,
    uint32_t height,
    std::array<int16_t, 2> start,
    std::array<int16_t, 2> finish,
    int16_t footprint_x,
    int16_t footprint_z,
    std::span<const Cell> cells,
    Predecessor predecessor
) {
    if (cells.size() < std::size_t(width) * height)
        return {};
    std::array<std::array<int16_t, 2>, retained_turns> ring{};
    ring[0] = finish;
    auto current = finish;
    uint32_t written = 1;
    auto inside = [&](const std::array<int16_t, 2>& p) {
        return p[0] >= 0 && p[1] >= 0 && uint32_t(p[0]) < width && uint32_t(p[1]) < height;
    };
    auto index = [&](const std::array<int16_t, 2>& p) {
        return std::size_t(uint32_t(p[1])) * width + uint32_t(p[0]);
    };
    if (!inside(current))
        return {};
    auto previous = predecessor(cells[index(current)]);
    if (previous >= direction_x.size())
        return {};
    std::size_t guard = 0;
    while (current != start) {
        if (!inside(current))
            return {};
        const auto direction = predecessor(cells[index(current)]);
        if (direction >= direction_x.size())
            return {};
        if (direction != previous) {
            ring[written & 63u] = current;
            ++written;
            previous = direction;
        }
        current[0] = static_cast<int16_t>(int32_t(current[0]) - direction_x[direction]);
        current[1] = static_cast<int16_t>(int32_t(current[1]) - direction_z[direction]);
        if (++guard > cells.size())
            return {};
    }
    ring[written & 63u] = start;
    const auto count = std::min<uint32_t>(written + 1, retained_turns);
    ReconstructedSearchPath result;
    for (uint32_t i = 0; i < count; ++i) {
        const auto cell = ring[written & 63u];
        --written;
        result.points[result.count++] = {
            static_cast<int16_t>((int32_t(cell[0]) * 2 + footprint_x) * 8),
            static_cast<int16_t>((int32_t(cell[1]) * 2 + footprint_z) * 8)
        };
    }
    return result;
}
} // namespace

std::vector<std::array<int16_t, 2>> reconstruct_search_path(
    uint32_t width,
    uint32_t height,
    std::array<int16_t, 2> start,
    std::array<int16_t, 2> finish,
    int16_t footprint_x,
    int16_t footprint_z,
    std::span<const uint8_t> predecessor
) {
    const auto result = reconstruct_path(
        width, height, start, finish, footprint_x, footprint_z, predecessor, [](uint8_t direction) {
            return direction;
        }
    );
    return std::vector<std::array<int16_t, 2>>(result.begin(), result.end());
}

ReconstructedSearchPath reconstruct_search_path_from_cells(
    uint32_t width,
    uint32_t height,
    std::array<int16_t, 2> start,
    std::array<int16_t, 2> finish,
    int16_t footprint_x,
    int16_t footprint_z,
    std::span<const SearchMapCell> cells
) {
    return reconstruct_path(
        width,
        height,
        start,
        finish,
        footprint_x,
        footprint_z,
        cells,
        [](const SearchMapCell& cell) { return cell.predecessor; }
    );
}
} // namespace oa::sim::ground_orders
