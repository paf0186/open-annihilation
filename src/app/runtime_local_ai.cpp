// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Unpaced, undrawn skirmishes with both computer players in one match.
#include "oa/app/runtime.hpp"
#include "oa/formats/json.hpp"
#include "oa/sim/ai.hpp"
#include "oa/sim/simulation_state.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace oa::app {
namespace {
namespace json = oa::formats::json;
using Clock = std::chrono::steady_clock;
constexpr uint8_t computer_count = 2;
constexpr uint16_t units_per_player = 250;
constexpr int32_t starting_resources = 1000;
constexpr uint32_t report_ticks = 1800;
constexpr double normal_ticks_per_second = 30.0;
constexpr std::string_view preferred_map = "Ashap Plateau";

/// Writes a finite quantity without dropping the game's stored precision.
/// @param writer benchmark record
/// @param value quantity to write
void number(json::JsonWriter& writer, double value) {
    if (!std::isfinite(value))
        throw std::runtime_error("local AI benchmark encountered a nonfinite quantity");
    std::array<char, 64> text{};
    std::snprintf(text.data(), text.size(), "%.17g", value);
    writer.raw(text.data());
}

/// Writes one complete benchmark record and makes it available to a reader.
/// @param writer completed JSON object
void report(const json::JsonWriter& writer) {
    std::printf("local-ai: %s\n", writer.text().c_str());
    std::fflush(stdout);
}

/// Appends both players' counters directly from the match's records.
/// @param writer benchmark record
/// @param world game and player records
/// @param controllers computer-player state
void player_counters(
    json::JsonWriter& writer, const oa::World& world, const sim::ai::ComputerPlayers& controllers
) {
    const auto integers = [&](std::string_view key, auto read) {
        writer.key(key);
        writer.begin_array();
        for (uint8_t player = 0; player < computer_count; ++player)
            writer.integer(read(world.game.players[player]));
        writer.end_array();
    };
    integers("units", [](const oa::Player& player) { return player.unit_count; });
    integers("kills", [](const oa::Player& player) { return player.kills; });
    integers("created", [](const oa::Player& player) { return player.units_created; });
    writer.key("participating");
    writer.begin_array();
    for (uint8_t player = 0; player < computer_count; ++player)
        writer.boolean(sim::simulation_state::player_participating(world.game.players[player]));
    writer.end_array();
    writer.key("controllers");
    writer.begin_array();
    for (uint8_t player = 0; player < computer_count; ++player)
        writer.boolean(controllers.players[player].present != 0);
    writer.end_array();
    const auto quantities = [&](std::string_view key, auto read) {
        writer.key(key);
        writer.begin_array();
        for (uint8_t player = 0; player < computer_count; ++player)
            number(writer, read(world.game.players[player]));
        writer.end_array();
    };
    quantities("metal", [](const oa::Player& player) { return player.metal_produced_total; });
    quantities("energy", [](const oa::Player& player) { return player.energy_produced_total; });
}

/// Appends elapsed ticks and their measured wall-clock rate.
/// @param writer benchmark record
/// @param ticks simulation ticks advanced
/// @param seconds measured wall-clock seconds
void timing(json::JsonWriter& writer, uint32_t ticks, double seconds) {
    writer.key("seconds");
    number(writer, seconds);
    const double rate = seconds > 0.0 ? static_cast<double>(ticks) / seconds : 0.0;
    writer.key("ticks_per_second");
    number(writer, rate);
    writer.key("normal_speed");
    number(writer, rate / normal_ticks_per_second);
}

/// Returns the local player's ordinary outcome name.
/// @param outcome match outcome
/// @return JSON outcome name
const char* outcome_name(sim::scenario::Outcome outcome) {
    switch (outcome) {
    case sim::scenario::Outcome::victory:
        return "victory";
    case sim::scenario::Outcome::defeat:
        return "defeat";
    case sim::scenario::Outcome::respawn:
        return "respawn";
    case sim::scenario::Outcome::watch:
        return "watch";
    default:
        return "ongoing";
    }
}
} // namespace

int Runtime::run_local_ai_benchmark(std::size_t ticks) {
    try {
        exercise_click(menu::resource_name(menu::Button::single_player));
        exercise_click(entry::resource_name(entry::Button::skirmish));
        state_.player_count = computer_count;
        preferences_.difficulty = OA_DIFFICULTY_HARD;
        preferences_.skirmish_difficulty = OA_DIFFICULTY_HARD;
        preferences_.skirmish_location = 1;
        preferences_.skirmish.commander_death = 0;
        preferences_.skirmish.mapping = 1;
        preferences_.skirmish.line_of_sight = 1;
        preferences_.skirmish.los_type = 1;
        skirmish_settings_.slots.fill({});
        skirmish_settings_.slot_count = computer_count;
        for (uint8_t player = 0; player < computer_count; ++player)
            skirmish_settings_.slots[player] = {
                entry::controller::computer,
                player,
                entry::unassigned_alliance,
                starting_resources,
                starting_resources,
                player
            };
        const auto requested_map =
            options_.local_ai_map.empty() ? std::string(preferred_map) : options_.local_ai_map;
        if (select_map(requested_map) == 0) {
            if (!options_.local_ai_map.empty() || select_map(first_map_name_) == 0)
                throw std::runtime_error("local AI benchmark map cannot be selected");
        }
        if (map_player_capacity() < computer_count)
            throw std::runtime_error("local AI benchmark map needs two start positions");
        skirmish_settings_.map_name = selected_map_name_runtime_;
        // The interactive Start button requires a human. The same bootstrap
        // seats both computer roles before their profiles and commanders start.
        bootstrap_match({.units_per_player = units_per_player, .seat_roster = true});
        if (!match_ || !selected_tnt_ || altitude_sight_blocked_ || match_tick_blocked_)
            throw std::runtime_error("local AI benchmark did not prepare a runnable match");
        // The automated match has no human selection, HUD or presented frames.
        screen_ = Screen::match;
        const auto& world = match_->state();
        const auto* controllers = sim::ai::match_computer_players(*match_);
        if (controllers == nullptr || world.game.session_rules != 0 ||
            world.game.players[0].alliance[1] != 0 || world.game.players[1].alliance[0] != 0)
            throw std::runtime_error("local AI benchmark needs opposing annihilation players");
        for (uint8_t player = 0; player < computer_count; ++player)
            if (world.game.players[player].status != OA_PLAYER_STATUS_COMPUTER ||
                world.game.players[player].unit_count == 0)
                throw std::runtime_error("local AI benchmark needs both computer commanders");
        const uint32_t start_tick = world.game.tick;
        json::JsonWriter config;
        config.begin_object();
        config.key("type");
        config.string("config");
        config.key("map");
        config.string(selected_map_name_runtime_);
        config.key("math_mode");
        config.string(options_.experimental_math ? "hardware" : "compatible");
        config.key("pathfinder");
        config.string(options_.experimental_pathfinder);
        config.key("seed");
        config.integer(options_.seed.value_or(kFixedRandomSeed));
        config.key("requested_ticks");
        config.integer(static_cast<int64_t>(ticks));
        config.key("start_tick");
        config.integer(start_tick);
        config.key("players");
        config.raw("[0,1]");
        config.key("sides");
        config.raw("[\"Arm\",\"Core\"]");
        config.key("difficulty");
        config.string("hard");
        config.key("unit_limit");
        config.integer(units_per_player);
        config.key("unit_slots");
        config.integer(world.unit_slot_count);
        config.key("session_rules");
        config.integer(world.game.session_rules);
        config.key("visibility_flags");
        config.integer(world.game.visibility_flags);
        config.key("normal_ticks_per_second");
        number(config, normal_ticks_per_second);
        config.end_object();
        report(config);

        const auto started = Clock::now();
        auto window_started = started;
        uint32_t window_start_tick = start_tick;
        uint32_t peak_units = world.game.players[0].unit_count + world.game.players[1].unit_count;
        bool natural_end = false;
        for (std::size_t tick = 1; tick <= ticks; ++tick) {
            if (match_->outcome() != sim::scenario::Outcome::ongoing || match_finished_ ||
                match_tick_blocked_)
                throw std::runtime_error("local AI benchmark tried to advance a finished match");
            const auto previous_tick = world.game.tick;
            step_match_simulation();
            if (world.game.tick != previous_tick + 1 ||
                match_->simulation().tick != match_timing_.tick)
                throw std::runtime_error("local AI benchmark simulation did not advance a tick");
            for (uint8_t player = 0; player < computer_count; ++player) {
                const auto& record = world.game.players[player];
                if (record.unit_count != 0 &&
                    (record.status != OA_PLAYER_STATUS_COMPUTER ||
                     !sim::simulation_state::player_participating(record) ||
                     controllers->players[player].present == 0))
                    throw std::runtime_error("local AI benchmark lost an active controller");
            }
            peak_units = std::max(
                peak_units,
                static_cast<uint32_t>(
                    world.game.players[0].unit_count + world.game.players[1].unit_count
                )
            );
            natural_end =
                world.game.players[0].unit_count == 0 || world.game.players[1].unit_count == 0;
            if (match_->outcome() != sim::scenario::Outcome::ongoing && !natural_end)
                throw std::runtime_error("local AI benchmark ended with both armies present");
            if (tick % report_ticks == 0 || tick == ticks || natural_end) {
                const auto now = Clock::now();
                json::JsonWriter window;
                window.begin_object();
                window.key("type");
                window.string("window");
                window.key("tick");
                window.integer(world.game.tick);
                window.key("advanced_ticks");
                window.integer(world.game.tick - start_tick);
                window.key("window_ticks");
                window.integer(world.game.tick - window_start_tick);
                timing(
                    window,
                    world.game.tick - window_start_tick,
                    std::chrono::duration<double>(now - window_started).count()
                );
                player_counters(window, world, *controllers);
                window.end_object();
                report(window);
                window_started = now;
                window_start_tick = world.game.tick;
            }
            if (natural_end)
                break;
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
        const uint32_t advanced = world.game.tick - start_tick;
        if (advanced == 0 || (advanced != ticks && !natural_end))
            throw std::runtime_error("local AI benchmark stopped without clean progress");
        std::array<char, 17> digest{};
        std::snprintf(
            digest.data(),
            digest.size(),
            "%016llx",
            static_cast<unsigned long long>(frame_run_digest())
        );
        json::JsonWriter summary;
        summary.begin_object();
        summary.key("type");
        summary.string("summary");
        summary.key("requested_ticks");
        summary.integer(static_cast<int64_t>(ticks));
        summary.key("start_tick");
        summary.integer(start_tick);
        summary.key("tick");
        summary.integer(world.game.tick);
        summary.key("advanced_ticks");
        summary.integer(advanced);
        timing(summary, advanced, seconds);
        player_counters(summary, world, *controllers);
        summary.key("peak_units");
        summary.integer(peak_units);
        summary.key("natural_end");
        summary.boolean(natural_end);
        summary.key("outcome");
        summary.string(outcome_name(match_->outcome()));
        summary.key("digest");
        summary.string(digest.data());
        summary.end_object();
        report(summary);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "local AI benchmark failed: %s\n", error.what());
        return 1;
    }
}
} // namespace oa::app
