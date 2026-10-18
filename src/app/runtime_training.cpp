// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Synchronous local-player control of an ordinary headless skirmish.
#include "oa/app/runtime.hpp"
#include "training_transport.hpp"
#include "oa/formats/json.hpp"
#include "oa/sim/match_runtime/spawn_bridge.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace oa::app {
namespace {
namespace json = oa::formats::json;
constexpr std::size_t max_request_bytes = 1024 * 1024;
constexpr std::size_t max_commands = 256;
constexpr int64_t max_step_ticks = 3000;
constexpr int64_t max_request_id = 9007199254740991LL;
constexpr double fixed_scale = 65536.0;

/// Returns a required integer member or refuses the request.
/// @param value object
/// @param name member name
/// @return integer value
int64_t request_integer(const json::Json& value, std::string_view name) {
    const auto* member = value.find(name);
    const auto number = member == nullptr ? std::nullopt : member->integer();
    if (!number)
        throw std::runtime_error("invalid_request");
    return *number;
}

/// Returns a required string member or refuses the request.
/// @param value object
/// @param name member name
/// @return string value
std::string string(const json::Json& value, std::string_view name) {
    const auto* member = value.find(name);
    if (member == nullptr || member->string() == nullptr)
        throw std::runtime_error("invalid_request");
    return *member->string();
}

/// Refuses duplicate and unsupported members recursively before dispatch.
/// @param value object to inspect
/// @param allowed permitted names
void members(const json::Json& value, std::initializer_list<std::string_view> allowed) {
    if (value.type() != json::JsonType::object)
        throw std::runtime_error("invalid_request");
    const auto names = value.names();
    for (std::size_t index = 0; index < names.size(); ++index)
        if (std::find(allowed.begin(), allowed.end(), names[index]) == allowed.end() ||
            std::find(names.begin(), names.begin() + index, names[index]) != names.begin() + index)
            throw std::runtime_error("invalid_request");
}

/// Writes a finite game quantity as a JSON number.
/// @param writer response
/// @param value game quantity
void number(json::JsonWriter& writer, double value) {
    if (!std::isfinite(value))
        throw std::runtime_error("engine_fault");
    std::array<char, 64> text{};
    std::snprintf(text.data(), text.size(), "%.17g", value);
    writer.raw(text.data());
}

struct Handle {
    uint16_t slot{};
    uint32_t generation{};
    bool own{};
};

struct Command {
    uint16_t slot{};
    bool move{};
    bool queue{};
    sim::ground_orders::Point point{};
};

/// Returns the local outcome's protocol name.
/// @param outcome simulation outcome
/// @return protocol name
const char* outcome_name(sim::scenario::Outcome outcome) {
    switch (outcome) {
    case sim::scenario::Outcome::victory:
        return "victory";
    case sim::scenario::Outcome::defeat:
        return "defeat";
    case sim::scenario::Outcome::respawn:
        return "respawn";
    default:
        return "ongoing";
    }
}
} // namespace

int Runtime::run_training_worker() {
    try {
        exercise_click(menu::resource_name(menu::Button::single_player));
        exercise_click(entry::resource_name(entry::Button::skirmish));
        state_.player_count = 2;
        if (!options_.training_map.empty()) {
            if (select_map(options_.training_map) == 0)
                throw std::runtime_error("requested training map cannot be selected");
            first_map_name_ = options_.training_map;
        }
        if (map_player_capacity() < 2)
            throw std::runtime_error("training map needs two start positions");
        exercise_click(skirmish::resource_name(skirmish::Button::start));
        if (screen_ != Screen::match || !match_ || !selected_tnt_)
            throw std::runtime_error("training setup did not enter a match");
        if (match_->state().game.viewpoint_player != match_local_player_)
            throw std::runtime_error("training viewpoint differs from local player");
    } catch (const std::exception& error) {
        json::JsonWriter response;
        response.begin_object();
        response.key("id");
        response.null();
        response.key("ok");
        response.boolean(false);
        response.key("error");
        response.begin_object();
        response.key("code");
        response.string("setup_failed");
        response.key("message");
        response.string(error.what());
        response.end_object();
        response.end_object();
        write_training_response(response.text());
        return 1;
    }
    const auto width = static_cast<int64_t>(selected_tnt_->tile_width) * 32;
    const auto height = static_cast<int64_t>(selected_tnt_->tile_height) * 32;
    std::map<std::string, Handle> handles;
    std::vector<std::string> slot_handles(match_->world().slots.size());
    uint64_t next_handle = 0;
    bool faulted = false;
    bool greeted = false;
    const auto terminated = [&] { return match_->outcome() != sim::scenario::Outcome::ongoing; };
    const auto truncated = [&] {
        return match_timing_.tick >= options_.training_limit && !terminated();
    };
    const auto alive = [&](const Handle& handle) {
        const auto& slots = match_->world().slots;
        return handle.slot < slots.size() && slots[handle.slot].unit != nullptr &&
               slots[handle.slot].record.type_index != 0 &&
               match_->runtime_state(handle.slot).instance_generation == handle.generation;
    };
    const auto visible = [&](uint16_t slot) {
        return match_->unit_shown(match_local_player_, slot);
    };
    const auto sync_handles = [&] {
        std::erase_if(handles, [&](const auto& entry) {
            const auto& handle = entry.second;
            const bool retire =
                !alive(handle) ||
                handle.own != (match_->world().slots[handle.slot].record.owner_index ==
                               match_local_player_) ||
                (!handle.own && !visible(handle.slot));
            if (retire)
                slot_handles[handle.slot].clear();
            return retire;
        });
        const auto& slots = match_->world().slots;
        for (std::size_t index = 1; index < slots.size(); ++index) {
            const auto& slot = slots[index];
            if (slot.unit == nullptr || slot.record.type_index == 0)
                continue;
            const bool own = slot.record.owner_index == match_local_player_;
            if (!own && !visible(static_cast<uint16_t>(index)))
                continue;
            if (slot_handles[index].empty()) {
                const auto name = "u" + std::to_string(++next_handle);
                handles.emplace(
                    name,
                    Handle{
                        static_cast<uint16_t>(index),
                        match_->runtime_state(static_cast<uint16_t>(index)).instance_generation,
                        own
                    }
                );
                slot_handles[index] = name;
            }
        }
    };
    const auto observation = [&](json::JsonWriter& response) {
        response.begin_object();
        response.key("schema");
        response.string("local-visible-v1");
        response.key("player");
        response.integer(match_local_player_);
        response.key("tick");
        response.integer(match_timing_.tick);
        response.key("radar_supported");
        response.boolean(false);
        response.key("economy");
        response.begin_object();
        const auto& player = match_->state().game.players[match_local_player_];
        response.key("metal");
        number(response, player.metal);
        response.key("energy");
        number(response, player.energy);
        response.key("metal_storage");
        number(response, player.metal_storage);
        response.key("energy_storage");
        number(response, player.energy_storage);
        response.end_object();
        for (const bool own : {true, false}) {
            response.key(own ? "own_units" : "visible_units");
            response.begin_array();
            for (const auto& [name, handle] : handles) {
                if (handle.own != own)
                    continue;
                const auto& unit = match_->world().slots[handle.slot].record;
                response.begin_object();
                response.key("handle");
                response.string(name);
                response.key("type");
                response.string(spawn_type_names_.at(unit.type_index));
                response.key("position");
                response.begin_object();
                response.key("x_world");
                number(response, unit.position.x / fixed_scale);
                response.key("y_world");
                number(response, unit.position.y / fixed_scale);
                response.key("z_world");
                number(response, unit.position.z / fixed_scale);
                response.end_object();
                response.end_object();
            }
            response.end_array();
        }
        response.end_object();
    };
    sync_handles();
    for (;;) {
        std::string line;
        int byte = 0;
        while ((byte = std::cin.get()) != EOF && byte != '\n') {
            if (line.size() >= max_request_bytes)
                return 2;
            line.push_back(static_cast<char>(byte));
        }
        if (byte == EOF && line.empty())
            return faulted ? 1 : 0;
        json::JsonWriter response;
        response.begin_object();
        response.key("id");
        std::optional<int64_t> request_id;
        json::JsonError parse_error;
        const auto request = json::parse_json(line, parse_error);
        if (request && request->find("id"))
            request_id = request->find("id")->integer();
        if (request_id && *request_id >= 0 && *request_id <= max_request_id)
            response.integer(*request_id);
        else {
            request_id.reset();
            response.null();
        }
        bool close = false;
        try {
            if (!request || !request_id)
                throw std::runtime_error("invalid_request");
            const auto op = string(*request, "op");
            if (op == "hello") {
                members(*request, {"id", "op", "version"});
                if (request_integer(*request, "version") != 1)
                    throw std::runtime_error("unsupported_version");
                greeted = true;
                response.key("ok");
                response.boolean(true);
                response.key("version");
                response.integer(1);
                response.key("player");
                response.integer(match_local_player_);
                response.key("tick");
                response.integer(match_timing_.tick);
                response.key("seed");
                response.integer(options_.seed.value_or(1));
                response.key("ticks_per_simulated_second");
                response.integer(30);
                response.key("episode_limit");
                response.integer(options_.training_limit);
                response.key("map");
                response.begin_object();
                response.key("name");
                response.string(first_map_name_);
                response.key("width_world");
                response.integer(width);
                response.key("height_world");
                response.integer(height);
                response.end_object();
                response.key("observation_schema");
                response.string("local-visible-v1");
                response.key("commands");
                response.begin_array();
                response.string("move");
                response.string("stop");
                response.end_array();
            } else if (op == "close") {
                members(*request, {"id", "op"});
                response.key("ok");
                response.boolean(true);
                close = true;
            } else if (op == "observe" || op == "step") {
                if (!greeted)
                    throw std::runtime_error("hello_required");
                members(
                    *request,
                    op == "step" ? std::initializer_list<
                                       std::string_view>{"id", "op", "player", "ticks", "commands"}
                                 : std::initializer_list<std::string_view>{"id", "op", "player"}
                );
                if (request_integer(*request, "player") != match_local_player_)
                    throw std::runtime_error("unsupported_player");
                if (faulted)
                    throw std::runtime_error("engine_fault");
                const auto before = match_timing_.tick;
                std::vector<Command> commands;
                int64_t ticks = 0;
                if (op == "step") {
                    if (terminated() || truncated())
                        throw std::runtime_error("episode_ended");
                    ticks = request_integer(*request, "ticks");
                    if (ticks < 1 || ticks > max_step_ticks)
                        throw std::runtime_error("invalid_request");
                    const auto* batch = request->find("commands");
                    if (batch == nullptr || batch->type() != json::JsonType::array ||
                        batch->elements().size() > max_commands)
                        throw std::runtime_error("invalid_request");
                    for (const auto& command : batch->elements()) {
                        const auto kind = string(command, "kind");
                        if (kind != "move" && kind != "stop")
                            throw std::runtime_error("invalid_command");
                        members(
                            command,
                            kind == "move" ? std::initializer_list<
                                                 std::string_view>{"kind", "unit", "point", "queue"}
                                           : std::initializer_list<std::string_view>{"kind", "unit"}
                        );
                        const auto found = handles.find(string(command, "unit"));
                        if (found == handles.end() || !found->second.own || !alive(found->second) ||
                            match_->world().slots[found->second.slot].record.owner_index !=
                                match_local_player_)
                            throw std::runtime_error("invalid_unit");
                        Command order;
                        order.slot = found->second.slot;
                        order.move = kind == "move";
                        if (order.move) {
                            if (!match_->takes_move_order(order.slot))
                                throw std::runtime_error("invalid_command");
                            const auto* point = command.find("point");
                            if (point == nullptr)
                                throw std::runtime_error("invalid_request");
                            members(*point, {"x_world", "z_world"});
                            const auto x = request_integer(*point, "x_world"),
                                       z = request_integer(*point, "z_world");
                            constexpr int64_t fixed_limit =
                                std::numeric_limits<int32_t>::max() / 65536;
                            if (x < 0 || z < 0 || x >= width || z >= height || x > fixed_limit ||
                                z > fixed_limit)
                                throw std::runtime_error("out_of_bounds");
                            const auto* queue = command.find("queue");
                            if (queue == nullptr || !queue->boolean())
                                throw std::runtime_error("invalid_request");
                            order.queue = *queue->boolean();
                            order.point[0] = static_cast<int32_t>(x * 65536);
                            order.point[2] = static_cast<int32_t>(z * 65536);
                            order.point[1] = static_cast<int32_t>(
                                static_cast<uint32_t>(match_->map_height(
                                    static_cast<uint32_t>(order.point[0]),
                                    static_cast<uint32_t>(order.point[2])
                                ))
                                << 16
                            );
                        }
                        commands.push_back(order);
                    }
                    try {
                        for (const auto& command : commands) {
                            const auto& issued =
                                command.move ? match_->issue_ground_move(
                                                   command.slot, command.point, command.queue
                                               )
                                             : match_->issue_stop(command.slot);
                            if (match_->order_refused(issued))
                                throw std::runtime_error("preflighted training order refused");
                            if (match_->fault())
                                throw std::runtime_error(match_->fault());
                        }
                        for (int64_t tick = 0; tick < ticks && !terminated() && !truncated();
                             ++tick) {
                            step_match_simulation();
                            sync_handles();
                            if (match_->fault())
                                throw std::runtime_error(match_->fault());
                        }
                    } catch (const std::exception& error) {
                        faulted = true;
                        std::fprintf(stderr, "training simulation fault: %s\n", error.what());
                        throw std::runtime_error("engine_fault");
                    }
                }
                response.key("ok");
                response.boolean(true);
                if (op == "step") {
                    response.key("tick_before");
                    response.integer(before);
                    response.key("ticks_advanced");
                    response.integer(match_timing_.tick - before);
                    response.key("commands");
                    response.begin_array();
                    for (std::size_t index = 0; index < commands.size(); ++index) {
                        response.begin_object();
                        response.key("index");
                        response.integer(index);
                        response.key("status");
                        response.string("issued");
                        response.end_object();
                    }
                    response.end_array();
                }
                response.key("tick");
                response.integer(match_timing_.tick);
                response.key("terminated");
                response.boolean(terminated());
                response.key("truncated");
                response.boolean(truncated());
                response.key("outcome");
                response.string(outcome_name(match_->outcome()));
                response.key("observation");
                observation(response);
            } else
                throw std::runtime_error("unsupported_operation");
        } catch (const std::exception& error) {
            response = {};
            response.begin_object();
            response.key("id");
            if (request_id)
                response.integer(*request_id);
            else
                response.null();
            response.key("ok");
            response.boolean(false);
            response.key("error");
            response.begin_object();
            response.key("code");
            const std::string_view code(error.what());
            constexpr std::array expected_errors{
                "invalid_request",
                "unsupported_version",
                "hello_required",
                "unsupported_player",
                "engine_fault",
                "episode_ended",
                "invalid_command",
                "invalid_unit",
                "out_of_bounds",
                "unsupported_operation"
            };
            const bool expected = std::find(expected_errors.begin(), expected_errors.end(), code) !=
                                  expected_errors.end();
            if (!expected || code == "engine_fault") {
                faulted = true;
                std::fprintf(stderr, "training response fault: %s\n", error.what());
            }
            response.string(expected ? code : std::string_view("engine_fault"));
            response.end_object();
            response.key("tick");
            response.integer(match_timing_.tick);
        }
        response.end_object();
        write_training_response(response.text());
        if (close)
            return faulted ? 1 : 0;
    }
}
} // namespace oa::app
