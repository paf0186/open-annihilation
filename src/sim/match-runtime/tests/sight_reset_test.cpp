// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The match's sight grids: the rebuild (reset_sight_buffers) under each visibility
// rule, the fog and radar bits the viewpoint's sight stamps mark, and the
// scrollable map extents the terrain load stores.
#include "combat_fixture.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>

namespace {

using namespace combat_fixture;

// Game.visibility_flags bits 0 and 1: the mapping and line-of-sight rules.
constexpr uint8_t mapping_rule = 0x01;
constexpr uint8_t line_of_sight_rule = 0x02;

// The sight cell a unit's stamp centred on, which the fixture's one-cell
// mask covers.
std::size_t stamped_cell(const Fixture& f, const sim::unit_spawn::Slot& slot) {
    const auto x = static_cast<int16_t>(slot.record.sight_center_x);
    const auto z = static_cast<int16_t>(slot.record.sight_center_z);
    CHECK(x >= 0 && z >= 0 && x < f.match->sight().width && z < f.match->sight().height);
    return static_cast<std::size_t>(z) * static_cast<std::size_t>(f.match->sight().width) +
           static_cast<std::size_t>(x);
}

// The fog edge mask went stale and the mapped radar is due a rebuild.
bool viewpoint_marked(const Game& game) {
    return (game.visibility_flags & OA_VISIBILITY_FOG_MASK_CURRENT) == 0 &&
           (game.radar_blink_flags & OA_RADAR_MAPPED_DIRTY) != 0;
}

// The fog and radar passes caught up.
void settle(Game& game) {
    game.visibility_flags =
        static_cast<uint8_t>(game.visibility_flags | OA_VISIBILITY_FOG_MASK_CURRENT);
    game.radar_blink_flags = static_cast<uint16_t>(game.radar_blink_flags & ~OA_RADAR_MAPPED_DIRTY);
}

// The terrain load stores the map less 0x20 pixels across and 0x80 down.
void terrain_load_stores_scroll_extents() {
    Fixture f;
    const auto& game = f.match->state().game;
    CHECK(game.map_width_world == 256 && game.map_height_world == 256);
    CHECK(game.map_pixel_width == 256 - 0x20 && game.map_pixel_height == 256 - 0x80);
    std::cout << "terrain load stores scroll extents passed\n";
}

// Under both rules the mapped grid is wiped (memset 0x00), each active
// player's coverage is cleared, and every live unit stamps its sight again:
// its owner's bit in the mapped word and one count of coverage.
void rebuild_under_mapping_and_line_of_sight() {
    Fixture f;
    auto& game = f.match->state().game;
    game.visibility_flags = mapping_rule | line_of_sight_rule;
    const auto own = stamped_cell(f, f.spawn(0, 64, 64));
    const auto enemy = stamped_cell(f, f.spawn(1, 200, 200));
    CHECK(own != enemy);
    auto& grid = f.match->sight_mutable();
    std::fill(grid.player_bits.begin(), grid.player_bits.end(), uint16_t{0x0100});
    std::fill(grid.coverage.begin(), grid.coverage.end(), uint16_t{5});
    settle(game);
    f.match->reset_sight_buffers(true);
    for (std::size_t cell = 0; cell < grid.player_bits.size(); ++cell) {
        const uint16_t expected = cell == own ? 0x0001 : cell == enemy ? 0x0002 : 0x0000;
        CHECK(grid.player_bits[cell] == expected);
        CHECK(grid.coverage[cell] == (cell == own ? 1 : 0));
    }
    const auto enemy_coverage = f.match->player_coverage(1);
    CHECK(enemy_coverage[enemy] == 1 && enemy_coverage[own] == 0);
    CHECK(viewpoint_marked(game));
    std::cout << "rebuild under mapping and line of sight passed\n";
}

// Without a refill the mapped words stand and only gain the stamps.
void rebuild_keeps_mapped_grid_without_refill() {
    Fixture f;
    auto& game = f.match->state().game;
    game.visibility_flags = mapping_rule | line_of_sight_rule;
    const auto own = stamped_cell(f, f.spawn(0, 64, 64));
    CHECK(own != 0);
    auto& grid = f.match->sight_mutable();
    std::fill(grid.player_bits.begin(), grid.player_bits.end(), uint16_t{0x0100});
    f.match->reset_sight_buffers(false);
    CHECK(grid.player_bits[own] == 0x0101 && grid.player_bits[0] == 0x0100);
    CHECK(grid.coverage[own] == 1 && grid.coverage[0] == 0);
    std::cout << "rebuild keeps the mapped grid without refill passed\n";
}

// With neither rule every mapped word holds every player's bit (memset
// 0xff) and each active player sees every cell once; no unit stamps, as
// the creation stamp runs only under line of sight. A player not seated keeps
// its grid.
void rebuild_without_rules() {
    Fixture f;
    auto& game = f.match->state().game;
    game.visibility_flags = 0;
    (void)f.spawn(0, 64, 64);
    (void)f.spawn(1, 200, 200);
    f.match->reset_sight_buffers(true);
    const auto& grid = f.match->sight();
    CHECK(std::all_of(grid.player_bits.begin(), grid.player_bits.end(), [](uint16_t word) {
        return word == 0xffff;
    }));
    for (uint8_t player = 0; player < 2; ++player) {
        const auto coverage = f.match->player_coverage(player);
        CHECK(std::all_of(coverage.begin(), coverage.end(), [](uint16_t count) {
            return count == 1;
        }));
    }
    const auto unseated = f.match->player_coverage(5);
    CHECK(std::all_of(unseated.begin(), unseated.end(), [](uint16_t count) { return count == 0; }));
    // The mapped words answer the visibility test once line of sight is off
    // (the cell test falls back to the mapped-word test).
    CHECK(f.match->point_visible(1, {10u << 16, 0, 10u << 16}));
    std::cout << "rebuild without rules passed\n";
}

// A viewpoint unit's stamps mark the fog and radar bits; another player's
// stamps leave them.
void viewpoint_stamps_mark_fog_and_radar() {
    Fixture f;
    auto& game = f.match->state().game;
    game.visibility_flags = mapping_rule | line_of_sight_rule;
    settle(game);
    (void)f.spawn(1, 200, 200);
    CHECK(!viewpoint_marked(game));
    (void)f.spawn(0, 64, 64);
    CHECK(viewpoint_marked(game));
    std::cout << "viewpoint stamps mark fog and radar passed\n";
}

// The player loop moves every live unit's sight stamp each tick. Under the
// mapping rule alone creation stamps nothing (it needs line of sight), so a unit that stands still maps the cells it sees on the next
// tick; a player with no units maps nothing.
void player_loop_maps_around_standing_units() {
    Fixture f;
    auto& game = f.match->state().game;
    game.visibility_flags = mapping_rule;
    auto& slot = f.spawn(0, 64, 64);
    auto& other = f.spawn(0, 192, 192);
    auto& grid = f.match->sight_mutable();
    std::fill(grid.player_bits.begin(), grid.player_bits.end(), uint16_t{0});
    f.run(1);
    const auto own = stamped_cell(f, slot);
    const auto other_cell = stamped_cell(f, other);
    CHECK(own != other_cell);
    CHECK(grid.player_bits[own] == 0x0001);
    CHECK(grid.player_bits[other_cell] == 0x0001);
    CHECK(
        std::count(grid.player_bits.begin(), grid.player_bits.end(), uint16_t{0}) ==
        static_cast<std::ptrdiff_t>(grid.player_bits.size() - 2)
    );
    std::cout << "player loop maps around standing units passed\n";
}

} // namespace

int main() {
    try {
        terrain_load_stores_scroll_extents();
        rebuild_under_mapping_and_line_of_sight();
        rebuild_keeps_mapped_grid_without_refill();
        rebuild_without_rules();
        viewpoint_stamps_mark_fog_and_radar();
        player_loop_maps_around_standing_units();
    } catch (const std::exception& error) {
        std::cerr << "sight reset test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
