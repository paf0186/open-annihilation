// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The commander rule in the running match: the kills board, the deathmatch
// commander respawn at the end of the local defeat countdown, and the
// multiplayer game's victory test and abandoned-game countdown.
#include "combat_fixture.hpp"
#include "oa/sim/scenario/commander_rules.hpp"
#include "oa/base/text.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>

namespace {

using namespace combat_fixture;

constexpr uint8_t setup_role_host = 0x01;
constexpr uint32_t outcome_ticks = 200; // six 30-tick defeat checks

struct BoardNotices {
    int leads = 0;
    int16_t lead_score = -1;
    int flashes = 0;
};

void kill_board_ranking() {
    for (const bool deathmatch : {false, true}) {
        Fixture f;
        BoardNotices notices;
        f.match->kill_board.context = &notices;
        f.match->kill_board.took_lead = [](void* context, uint8_t player, int16_t score) {
            auto& n = *static_cast<BoardNotices*>(context);
            CHECK(player == 1);
            ++n.leads;
            n.lead_score = score;
        };
        f.match->kill_board.flash = [](void* context, uint8_t, uint8_t) {
            ++static_cast<BoardNotices*>(context)->flashes;
        };
        auto& game = f.match->state().game;
        game.session_rules =
            deathmatch ? static_cast<int32_t>(sim::scenario::CommanderRule::deathmatch) : 0;
        auto& winner = f.spawn(1, 160, 64);
        auto& first = f.spawn(0, 64, 64);
        CHECK(game.players[0].board_row == 0 && game.players[1].board_row == 1);
        kill(f, first, winner);
        CHECK(game.players[1].kills == 1);
        // Deathmatch ranks by commander kills, so an ordinary kill leaves the board alone.
        CHECK(game.players[1].board_row == (deathmatch ? 1 : 0));
        CHECK(notices.leads == (deathmatch ? 0 : 1) && notices.flashes == 0);

        // Side 0, every setup record's side here, names the type as its
        // commander.
        oa::base::text::copy_terminated(game.sides[0].commander, "testunit");
        auto& commander = f.spawn(0, 64, 96);
        game.graphics_flags = 0x80;
        kill(f, commander, winner);
        CHECK(game.players[1].commanders_killed == 1 && game.players[1].board_row == 0);
        CHECK(game.players[0].board_row == 1);
        CHECK(notices.leads == 1 && notices.lead_score == 1 && notices.flashes == 1);
    }
    std::cout << "kill board ranking passed\n";
}

// Setup records for both players, player 1 hosting with 1200 energy and 700
// metal, and side 0 naming the fixture's type as its commander.
void bind_setup(Fixture& f) {
    auto& world = f.match->state();
    for (uint32_t player = 0; player < 2; ++player) {
        world.game.players[player].info = oa_ref_from_index(player);
        world.player_info[player] = PlayerSetupInfo{};
    }
    world.player_info[1].role = setup_role_host;
    world.player_info[1].energy_hundreds = 12;
    world.player_info[1].metal_hundreds = 7;
    oa::base::text::copy_terminated(world.game.sides[0].commander, "testunit");
}

struct RespawnNotices {
    int sight = 0;
    int selections = 0;
};

// The local player's live unit other than `except`, or null.
const Unit* local_unit(Fixture& f, const sim::unit_spawn::Slot* except = nullptr) {
    auto& world = f.match->state();
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot) {
        const auto& unit = world.units[slot];
        if (unit.owner_index == 0 && (unit.flags & OA_UNIT_FLAG_LIVE) != 0 &&
            (except == nullptr || slot != except->unit_index))
            return &unit;
    }
    return nullptr;
}

// Deathmatch: once the local player has lost every unit and its defeat
// countdown runs out, a commander of its side stands inside the map border
// on a clear site, the player holds the host's start storage, the host's
// start resources are credited through the unit, and no defeat is recorded.
void deathmatch_respawns_the_commander() {
    Fixture f({.defeat_allowed = true});
    auto& world = f.match->state();
    world.game.session_rules = static_cast<int32_t>(sim::scenario::CommanderRule::deathmatch);
    bind_setup(f);
    RespawnNotices notices;
    f.match->respawn.context = &notices;
    f.match->respawn.sight_rebuilt = [](void* context) {
        ++static_cast<RespawnNotices*>(context)->sight;
    };
    f.match->respawn.select_commander = [](void* context) {
        auto& n = *static_cast<RespawnNotices*>(context);
        CHECK(n.sight == 1);
        ++n.selections;
    };
    auto& enemy = f.spawn(1, 200, 200);
    auto& own = f.spawn(0, 64, 64);
    f.run(1);
    kill(f, own, enemy);
    CHECK(local_unit(f) == nullptr);

    const Unit* commander = nullptr;
    for (uint32_t tick = 0; tick < outcome_ticks && commander == nullptr; ++tick) {
        f.run(1);
        commander = local_unit(f);
    }
    CHECK(commander != nullptr);
    CHECK(commander->def == oa_ref_from_index(1) && commander->health == 1000);
    const auto x = commander->position.x >> 16;
    const auto z = commander->position.z >> 16;
    CHECK(x >= 25 && x < 231 && z >= 25 && z < 231);
    CHECK(commander->cell_x != enemy.record.cell_x || commander->cell_z != enemy.record.cell_z);
    const auto& player = world.game.players[0];
    CHECK((player.resource_flags & 1) != 0);
    CHECK(player.shared_energy_storage == 1200.0F && player.shared_metal_storage == 700.0F);
    // The economy tick of the respawn tick collects the credited resources.
    CHECK(player.energy_produced == 1200.0F && player.metal_produced == 700.0F);
    CHECK(notices.sight == 1 && notices.selections == 1);
    CHECK(
        f.match->outcome() == sim::scenario::Outcome::ongoing && f.match->outcome_state().flags == 0
    );
    f.run(outcome_ticks);
    CHECK(f.match->outcome() == sim::scenario::Outcome::ongoing);
    std::cout << "deathmatch respawns the commander passed\n";
}

// A skirmish seats no host: the host lookup returns 10 and the respawn reads
// the eleventh player record, whose setup block starts zeroed with the game
// state. The player keeps the start storage floor (200), nothing is
// credited, and the sight grids are rebuilt with the mapped words refilled,
// wiping terrain the player had mapped under the
// mapping rule.
void skirmish_respawn_reads_the_eleventh_record() {
    Fixture f({.defeat_allowed = true});
    auto& world = f.match->state();
    world.game.session_rules = static_cast<int32_t>(sim::scenario::CommanderRule::deathmatch);
    world.game.visibility_flags = static_cast<uint8_t>(
        sim::visibility_state::terrain_mapping | sim::visibility_state::update_sight_grid
    );
    world.player_info[0].side = 0;
    oa::base::text::copy_terminated(world.game.sides[0].commander, "testunit");
    CHECK(sim::scenario::host_player_index(world) == OA_PLAYER_COUNT);
    CHECK(world.game.no_player.index == OA_PLAYER_COUNT);
    CHECK(world_player_info(&world, &world.game.no_player) == &world.player_info[OA_PLAYER_COUNT]);
    auto& enemy = f.spawn(1, 200, 200);
    auto& own = f.spawn(0, 64, 64);
    f.run(1);
    kill(f, own, enemy);
    auto& remembered = f.match->sight_mutable().player_bits[0];
    remembered = 0x0001;
    const Unit* commander = nullptr;
    for (uint32_t tick = 0; tick < outcome_ticks && commander == nullptr; ++tick) {
        f.run(1);
        commander = local_unit(f);
    }
    CHECK(commander != nullptr);
    const auto& player = world.game.players[0];
    CHECK((player.resource_flags & 1) != 0);
    CHECK(player.shared_energy_storage == 200.0F && player.shared_metal_storage == 200.0F);
    CHECK(player.energy_produced == 0.0F && player.metal_produced == 0.0F);
    const auto& sight = f.match->sight();
    const auto commander_cell = static_cast<int16_t>(commander->sight_center_z) * sight.width +
                                static_cast<int16_t>(commander->sight_center_x);
    CHECK(remembered == (commander_cell == 0 ? 0x0001 : 0));
    CHECK((world.game.radar_blink_flags & OA_RADAR_MAPPED_DIRTY) != 0);
    CHECK(f.match->outcome() == sim::scenario::Outcome::ongoing);
    std::cout << "skirmish respawn reads the eleventh record passed\n";
}

// On a lava world only a site whose ground is above the sea is accepted:
// here only the eastern half of the map.
void lava_world_respawns_on_high_ground() {
    Fixture f({.defeat_allowed = true, .lava_world = 1, .high_ground_from = 8});
    f.match->state().game.session_rules =
        static_cast<int32_t>(sim::scenario::CommanderRule::deathmatch);
    bind_setup(f);
    auto& enemy = f.spawn(1, 200, 200);
    auto& own = f.spawn(0, 200, 64);
    f.run(1);
    kill(f, own, enemy);
    const Unit* commander = nullptr;
    for (uint32_t tick = 0; tick < outcome_ticks && commander == nullptr; ++tick) {
        f.run(1);
        commander = local_unit(f);
    }
    CHECK(commander != nullptr && (commander->position.x >> 20) >= 8);
    std::cout << "lava world respawns on high ground passed\n";
}

// Without the side table (and setup records) bound the match cannot place a
// commander, and a deathmatch defeat ends the game as under the other rules.
void unbound_deathmatch_defeat_stands() {
    Fixture f({.defeat_allowed = true});
    f.match->state().game.session_rules =
        static_cast<int32_t>(sim::scenario::CommanderRule::deathmatch);
    auto& enemy = f.spawn(1, 200, 200);
    auto& own = f.spawn(0, 64, 64);
    f.run(1);
    kill(f, own, enemy);
    f.run(outcome_ticks);
    CHECK(local_unit(f) == nullptr);
    CHECK(f.match->outcome() == sim::scenario::Outcome::defeat);
    CHECK((f.match->outcome_state().flags & sim::scenario::outcome_flag::finished) != 0);
    std::cout << "unbound deathmatch defeat stands passed\n";
}

// A multiplayer game's victory: the only other player lost every unit
// it built. Deathmatch has no victory.
void multiplayer_victory() {
    for (const bool deathmatch : {false, true}) {
        Fixture f({.multiplayer = true});
        f.match->state().game.session_rules =
            deathmatch ? static_cast<int32_t>(sim::scenario::CommanderRule::deathmatch) : 0;
        auto& own = f.spawn(0, 64, 64);
        auto& enemy = f.spawn(1, 200, 200);
        f.run(1);
        kill(f, enemy, own);
        f.run(outcome_ticks);
        CHECK(
            f.match->outcome() ==
            (deathmatch ? sim::scenario::Outcome::ongoing : sim::scenario::Outcome::victory)
        );
    }
    std::cout << "multiplayer victory passed\n";
}

struct WatchNotices {
    int announcements = 0;
    int notices = 0;
    sim::match_runtime::Match::WatchNotice last{};
};

// Seats player 2 as a computer player of the local machine, with its own
// setup block when `with_setup` is set, and makes the host (player 1) a mirrored
// human, so a human is still in the game.
void seat_hosted_computer(Fixture& f, bool with_setup) {
    auto& world = f.match->state();
    auto& computer = world.game.players[2];
    computer.in_use = 1;
    computer.status = OA_PLAYER_STATUS_COMPUTER;
    computer.index = 2;
    if (with_setup) {
        computer.info = oa_ref_from_index(2);
        world.player_info[2] = PlayerSetupInfo{};
        world.player_info[2].state = OA_PLAYER_STATUS_COMPUTER;
    }
    std::array<uint8_t, 10> own{};
    own[2] = 1;
    f.match->configure_player_alliances(2, own);
    world.game.players[1].status = OA_PLAYER_STATUS_MIRRORED;
    world.player_info[1].state = OA_PLAYER_STATUS_LOCAL;
}

void observe_watching(Fixture& f, WatchNotices& notices) {
    f.match->multiplayer.context = &notices;
    f.match->multiplayer.player_status_changed = [](void* context) {
        ++static_cast<WatchNotices*>(context)->announcements;
    };
    f.match->watch.context = &notices;
    f.match->watch.became_watcher = [](void* context,
                                       sim::match_runtime::Match::WatchNotice notice) {
        auto& n = *static_cast<WatchNotices*>(context);
        ++n.notices;
        n.last = notice;
    };
}

// A defeated multiplayer player whom the host lets watch, or whose machine
// still runs a computer player, goes on as a watcher when its countdown runs
// out: its setup block is marked watching and announced again,
// mapping and line of sight go off, the view asks to go on watching (or, with
// a computer player still playing, says why it must watch), and no defeat is
// recorded then or later. Without either it is defeated, as is a player
// whose setup block is not bound.
void defeated_player_watches() {
    enum class Case { allowed, computer_hosted, refused, unbound };
    for (const auto which : {Case::allowed, Case::computer_hosted, Case::refused, Case::unbound}) {
        Fixture f({.defeat_allowed = true, .multiplayer = true});
        auto& world = f.match->state();
        if (which != Case::unbound)
            bind_setup(f);
        if (which == Case::unbound)
            world.game.players[0].info = 0;
        if (which == Case::allowed)
            world.player_info[1].options = OA_SETUP_OPTION_WATCHING_ALLOWED;
        if (which == Case::computer_hosted || which == Case::unbound)
            seat_hosted_computer(f, which == Case::computer_hosted);
        world.game.visibility_flags = 0x07;
        WatchNotices notices;
        observe_watching(f, notices);
        auto& enemy = f.spawn(1, 200, 200);
        auto& own = f.spawn(0, 64, 64);
        f.run(1);
        kill(f, own, enemy);
        f.run(outcome_ticks);
        const bool watches = which == Case::allowed || which == Case::computer_hosted;
        CHECK(((world.player_info[0].options & OA_SETUP_OPTION_WATCHER) != 0) == watches);
        CHECK(
            f.match->outcome() ==
            (watches ? sim::scenario::Outcome::ongoing : sim::scenario::Outcome::defeat)
        );
        if (!watches) {
            CHECK(notices.announcements == 0 && notices.notices == 0);
            continue;
        }
        CHECK(
            (world.game.visibility_flags & (OA_VISIBILITY_MAPPING | OA_VISIBILITY_LINE_OF_SIGHT |
                                            OA_VISIBILITY_ALTITUDE_SIGHT)) ==
            OA_VISIBILITY_ALTITUDE_SIGHT
        );
        CHECK(notices.announcements == 1 && notices.notices == 1);
        CHECK(
            notices.last == (which == Case::allowed
                                 ? sim::match_runtime::Match::WatchNotice::continue_prompt
                                 : sim::match_runtime::Match::WatchNotice::hosting_computers)
        );
        CHECK(f.match->outcome_state().flags == 0);
        f.run(outcome_ticks);
        CHECK(f.match->outcome() == sim::scenario::Outcome::ongoing && notices.notices == 1);
    }
    std::cout << "defeated player watches passed\n";
}

// Under network.host-stays-as-watcher a defeated host watches on whatever
// the watching option, so the game goes on for the others; a defeated
// player that does not host loses as before, and so does the host without
// the rule.
void defeated_host_stays_watching() {
    enum class Case { host_kept, host_without_rule, guest_with_rule };
    for (const auto which : {Case::host_kept, Case::host_without_rule, Case::guest_with_rule}) {
        Fixture f({.defeat_allowed = true, .multiplayer = true});
        auto& world = f.match->state();
        bind_setup(f);
        if (which != Case::guest_with_rule) {
            world.player_info[1].role = 0;
            world.player_info[0].role = setup_role_host;
        }
        f.match->set_host_stays_watching(which != Case::host_without_rule);
        WatchNotices notices;
        observe_watching(f, notices);
        auto& enemy = f.spawn(1, 200, 200);
        auto& own = f.spawn(0, 64, 64);
        f.run(1);
        kill(f, own, enemy);
        f.run(outcome_ticks);
        const bool watches = which == Case::host_kept;
        CHECK(((world.player_info[0].options & OA_SETUP_OPTION_WATCHER) != 0) == watches);
        CHECK(
            f.match->outcome() ==
            (watches ? sim::scenario::Outcome::ongoing : sim::scenario::Outcome::defeat)
        );
        if (watches)
            CHECK(
                notices.notices == 1 &&
                notices.last == sim::match_runtime::Match::WatchNotice::host_watching
            );
    }
    std::cout << "defeated host stays watching passed\n";
}

// An allied victory is counted (finished, won and the victory transition),
// then the winner loses every unit. Its machine still hosts a computer
// player and a human is left, so it watches and the win is dropped: only
// bit 0x10 is cleared.
void defeated_winner_drops_the_win() {
    Fixture f({.defeat_allowed = true, .multiplayer = true});
    auto& world = f.match->state();
    bind_setup(f);
    seat_hosted_computer(f, true);
    std::array<uint8_t, 10> everyone{};
    for (uint8_t player = 0; player < 3; ++player)
        everyone[player] = 1;
    for (uint8_t player = 0; player < 3; ++player) {
        f.match->configure_player_alliances(player, everyone);
        world.player_info[player].status = OA_SETUP_STATUS_ALLIED_VICTORY;
        for (uint8_t other = 0; other < 3; ++other)
            world.game.players[player].allied_by[other] = 1;
    }
    WatchNotices notices;
    observe_watching(f, notices);
    auto& host = f.spawn(1, 200, 200);
    f.spawn(2, 200, 64);
    auto& own = f.spawn(0, 64, 64);
    f.run(outcome_ticks);
    CHECK(f.match->outcome() == sim::scenario::Outcome::victory);
    constexpr auto counted = static_cast<uint16_t>(
        sim::scenario::outcome_flag::finished | sim::scenario::outcome_flag::won |
        sim::scenario::outcome_flag::victory_transition
    );
    CHECK(f.match->outcome_state().flags == counted);
    kill(f, own, host);
    f.run(outcome_ticks);
    CHECK((world.player_info[0].options & OA_SETUP_OPTION_WATCHER) != 0);
    CHECK(notices.notices == 1);
    CHECK(notices.last == sim::match_runtime::Match::WatchNotice::hosting_computers);
    CHECK(
        f.match->outcome_state().flags ==
        static_cast<uint16_t>(counted & ~sim::scenario::outcome_flag::won)
    );
    f.match->choose_continue_watching(true);
    CHECK(notices.announcements == 2);
    CHECK(
        f.match->outcome_state().flags ==
        static_cast<uint16_t>(counted & ~sim::scenario::outcome_flag::won)
    );
    f.match->choose_continue_watching(false);
    CHECK(notices.announcements == 2);
    CHECK((f.match->outcome_state().flags & sim::scenario::outcome_flag::finished) != 0);
    CHECK((f.match->outcome_state().flags & sim::scenario::outcome_flag::won) == 0);
    CHECK(f.match->outcome() == sim::scenario::Outcome::defeat);
    std::cout << "defeated winner drops the win passed\n";
}

// Once no human participant is left (both players watch here) the defeat
// countdown runs every tick on the local branch's state, except in
// deathmatch.
void abandoned_game_ends() {
    for (const bool deathmatch : {false, true}) {
        Fixture f({.multiplayer = true});
        auto& world = f.match->state();
        world.game.session_rules =
            deathmatch ? static_cast<int32_t>(sim::scenario::CommanderRule::deathmatch) : 0;
        bind_setup(f);
        world.player_info[0].options = OA_SETUP_OPTION_WATCHER;
        world.player_info[1].options = OA_SETUP_OPTION_WATCHER;
        (void)f.spawn(0, 64, 64);
        (void)f.spawn(1, 200, 200);
        CHECK(sim::scenario::connected_participants(world) == 0);
        f.run(10);
        if (deathmatch) {
            CHECK(f.match->outcome() == sim::scenario::Outcome::ongoing);
        } else {
            CHECK(f.match->outcome() == sim::scenario::Outcome::defeat);
            CHECK(
                (f.match->outcome_state().flags & sim::scenario::outcome_flag::defeat_transition) !=
                0
            );
        }
    }
    std::cout << "abandoned game ends passed\n";
}

// A session with no human participants can keep its outcome open, then
// return to the usual abandoned-game rule.
void watchers_can_keep_match_running() {
    Fixture f({.multiplayer = true});
    auto& world = f.match->state();
    bind_setup(f);
    world.player_info[0].options = OA_SETUP_OPTION_WATCHER;
    world.game.players[1].status = OA_PLAYER_STATUS_COMPUTER;
    world.unit_defs[1].energy_make = 30.0F;
    (void)f.spawn(0, 64, 64);
    (void)f.spawn(1, 200, 200);
    CHECK(sim::scenario::connected_participants(world) == 0);
    f.match->set_human_participant_required(false);
    f.run(30);
    CHECK(f.match->outcome() == sim::scenario::Outcome::ongoing);
    CHECK(f.match->outcome_state().countdown < 0);
    CHECK((f.match->outcome_state().flags & sim::scenario::outcome_flag::finished) == 0);
    CHECK(world.game.players[1].energy_produced_total > 0.0F);
    f.match->set_human_participant_required(true);
    f.run(10);
    CHECK(f.match->outcome() == sim::scenario::Outcome::defeat);
    std::cout << "watchers can keep match running passed\n";
}

} // namespace

int main() {
    try {
        kill_board_ranking();
        deathmatch_respawns_the_commander();
        skirmish_respawn_reads_the_eleventh_record();
        lava_world_respawns_on_high_ground();
        unbound_deathmatch_defeat_stands();
        multiplayer_victory();
        abandoned_game_ends();
        watchers_can_keep_match_running();
        defeated_player_watches();
        defeated_host_stays_watching();
        defeated_winner_drops_the_win();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
