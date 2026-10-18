// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Search effort, route safety and costs with compatible and stronger priorities.
#include "oa/sim/ground_orders/experimental.hpp"
#include "oa/sim/ground_orders/search_worker.hpp"
#include "oa/test/check.hpp"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
using namespace oa::sim::ground_orders;
constexpr uint32_t map_size = 64;

struct Terrain : MovementMapSampler {
    std::vector<uint8_t> cells = std::vector<uint8_t>(map_size * map_size, 3);

    /// Samples a cell from the fixed test terrain.
    uint8_t classify_cell(int32_t x, int32_t z) override {
        if (x < 0 || z < 0 || x >= int32_t(map_size) || z >= int32_t(map_size))
            return 0;
        return cells[static_cast<std::size_t>(z) * map_size + static_cast<uint32_t>(x)];
    }
};

struct Result {
    SearchAdvance status{};
    uint32_t expansions{};
    int32_t terminal_cost{};
    uint32_t travel_cost{};
    bool reached_goal{};
    std::vector<RoutePoint> route;
    std::vector<uint8_t> initial_flags;
    int32_t threshold{};
};

/// Runs a route through complete slices and checks every traversed cell.
///
/// @param terrain passable, blocked and difficult cells
/// @param mode heap priority to select before the search
/// @param target requested destination in cells
/// @return effort, costs, route and unchanged goal acceptance inputs
Result search(Terrain& terrain, ExperimentalPathfinder mode, std::array<int16_t, 2> target) {
    set_experimental_pathfinder(mode);
    MovementMap map(map_size, map_size, 1, 1, terrain);
    map.refresh({0, (map_size - 1) | ((map_size - 1) << 16)});
    oa::sim::unit_spawn::LegacyWorld world(2);
    auto& unit = world.unit(1);
    unit.record.cell_x = 2;
    unit.record.cell_z = 2;
    Navigation navigation;
    Goal goal;
    goal.cell = target;
    SearchWorker worker;
    SearchBegin begin;
    const std::vector<uint16_t> sight(map_size * map_size / 4, 0xffff);
    Result result;
    result.status = worker.begin({&unit, &navigation, &goal, &map}, begin, sight);
    result.threshold = worker.threshold();
    for (uint32_t z = 0; z < map_size; ++z)
        for (uint32_t x = 0; x < map_size; ++x)
            result.initial_flags.push_back(worker.cell(x, z).flags);
    for (uint32_t slice = 0; slice < 100 && result.status == SearchAdvance::in_progress; ++slice) {
        result.status = worker.advance_slice();
        result.expansions += static_cast<uint32_t>(worker.budget());
    }
    OA_CHECK(result.status != SearchAdvance::in_progress);
    result.route = worker.route();
    if (result.status != SearchAdvance::succeeded) {
        OA_CHECK(result.route.empty());
        return result;
    }
    OA_CHECK(result.route.size() >= 2 && result.route.size() < 64);
    const auto finish = worker.finish_cell();
    result.reached_goal = goal_contains(goal, finish[0], finish[1]);
    OA_CHECK((worker.cell(finish[0], finish[1]).flags & search_goal_bit) != 0);
    OA_CHECK(worker.heuristic(finish[0], finish[1]) <= worker.threshold());
    const auto handle = worker.cell(finish[0], finish[1]).handle;
    result.terminal_cost = std::bit_cast<int32_t>(worker.heap().payload(handle)[1]);
    for (std::size_t segment = 1; segment < result.route.size(); ++segment) {
        const auto& previous = result.route[segment - 1];
        const auto& next = result.route[segment];
        int32_t x = (previous[0] - 8) / 16;
        int32_t z = (previous[1] - 8) / 16;
        const int32_t end_x = (next[0] - 8) / 16;
        const int32_t end_z = (next[1] - 8) / 16;
        const auto dx = end_x - x;
        const auto dz = end_z - z;
        const bool aligned = dx == 0 || dz == 0 || std::abs(dx) == std::abs(dz);
        OA_CHECK(aligned);
        if (!aligned)
            continue;
        const auto step_x = (dx > 0) - (dx < 0);
        const auto step_z = (dz > 0) - (dz < 0);
        while (x != end_x || z != end_z) {
            x += step_x;
            z += step_z;
            OA_CHECK(terrain.classify_cell(x, z) != 0);
            result.travel_cost += step_x != 0 && step_z != 0 ? 22u : 16u;
        }
    }
    return result;
}

/// Checks repeated weighted routes and reports their effort and quality tradeoff.
///
/// @param name terrain case label
/// @param terrain passable, blocked and difficult cells
/// @param target requested destination in cells
/// @param reachable whether the requested destination is reachable
void compare(const char* name, Terrain& terrain, std::array<int16_t, 2> target, bool reachable) {
    const auto compatible = search(terrain, ExperimentalPathfinder::compatible, target);
    const auto weighted = search(terrain, ExperimentalPathfinder::weighted, target);
    const auto repeated = search(terrain, ExperimentalPathfinder::weighted, target);
    const auto restored = search(terrain, ExperimentalPathfinder::compatible, target);
    OA_CHECK(compatible.threshold == weighted.threshold);
    OA_CHECK(compatible.initial_flags == weighted.initial_flags);
    OA_CHECK(weighted.status == repeated.status);
    OA_CHECK(weighted.expansions == repeated.expansions);
    OA_CHECK(weighted.terminal_cost == repeated.terminal_cost);
    OA_CHECK(weighted.travel_cost == repeated.travel_cost);
    OA_CHECK(weighted.reached_goal == repeated.reached_goal);
    OA_CHECK(weighted.route == repeated.route);
    OA_CHECK(weighted.terminal_cost == compatible.terminal_cost);
    OA_CHECK(weighted.travel_cost == compatible.travel_cost);
    OA_CHECK(compatible.status == restored.status);
    OA_CHECK(compatible.expansions == restored.expansions);
    OA_CHECK(compatible.terminal_cost == restored.terminal_cost);
    OA_CHECK(compatible.route == restored.route);
    // A positive seed threshold can publish a partial route to an unreachable
    // goal. Route publication alone does not establish that the goal was reached.
    OA_CHECK(compatible.reached_goal == reachable);
    OA_CHECK(weighted.reached_goal == reachable);
    const std::array<const Result*, 2> results{&compatible, &weighted};
    constexpr std::array<const char*, 2> names{"compatible", "weighted"};
    for (std::size_t mode = 0; mode < results.size(); ++mode) {
        const auto& result = *results[mode];
        std::printf(
            "%s %s status=%u reached-goal=%u expansions=%u terminal-cost=%d travel-cost=%u "
            "points=%zu\n",
            name,
            names[mode],
            static_cast<unsigned>(result.status),
            static_cast<unsigned>(result.reached_goal),
            result.expansions,
            result.terminal_cost,
            result.travel_cost,
            result.route.size()
        );
    }
}
} // namespace

int main() {
    Terrain open;
    compare("open", open, {61, 61}, true);
    Terrain wall;
    for (uint32_t z = 0; z < 60; ++z)
        wall.cells[z * map_size + 32] = 0;
    compare("wall-detour", wall, {61, 2}, true);
    Terrain maze;
    for (uint32_t x = 10; x < 60; x += 10) {
        for (uint32_t z = 0; z < map_size; ++z) {
            if (((x / 10) & 1u) != 0 ? z < 59 : z > 4)
                maze.cells[z * map_size + x] = 0;
        }
    }
    compare("alternating-walls", maze, {61, 61}, true);
    Terrain difficult;
    for (uint32_t z = 4; z < 60; ++z)
        for (uint32_t x = 4; x < 60; ++x)
            if (std::abs(int32_t(x) - int32_t(z)) < 5)
                difficult.cells[z * map_size + x] = 1;
    compare("difficult-diagonal", difficult, {61, 61}, true);
    Terrain enclosed;
    for (uint32_t coordinate = 40; coordinate <= 60; ++coordinate) {
        enclosed.cells[40 * map_size + coordinate] = 0;
        enclosed.cells[60 * map_size + coordinate] = 0;
        enclosed.cells[coordinate * map_size + 40] = 0;
        enclosed.cells[coordinate * map_size + 60] = 0;
    }
    compare("enclosed-goal", enclosed, {50, 50}, false);
    Terrain divided;
    for (uint32_t z = 0; z < map_size; ++z)
        divided.cells[z * map_size + 32] = 0;
    compare("divided-map", divided, {61, 2}, false);
    set_experimental_pathfinder(ExperimentalPathfinder::compatible);
    return oa::test::check_exit_status();
}
