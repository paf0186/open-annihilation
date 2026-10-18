// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Network play's long options through oa-game's command line: the loopback,
// host-not-found, demo and network game options, with the options the engine keeps, none
// of them without the extension, and "-y", which network play takes and which
// only skips the intro; the names the frontend's entry takes from a launch;
// and the multiplayer frontend states' answers about joining and leaving a
// session.
#include "oa/app/app.hpp"
#include "net_options.hpp"
#include "oa/netgame/dplay.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++failures;
    }
}

struct Parsed {
    oa::app::NetOptions net;
    oa::app::Options app;
};

// Each parse starts from a fresh network play context, as a start does.
Parsed parse(const std::vector<const char*>& arguments, bool extended = true) {
    std::vector<std::string> storage{"open-annihilation"};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& argument : storage)
        argv.push_back(argument.data());
    auto& context = oa::app::netgame_context();
    context = {};
    oa::app::Extension table{};
    if (extended)
        oa::app::fill_option_hooks(table, context);
    auto app = oa::app::parse_options(static_cast<int>(argv.size()), argv.data(), table);
    return {context.options, std::move(app)};
}

// The message parse_options rejects the arguments with, or "" when it takes them.
std::string rejection(const std::vector<const char*>& arguments, bool extended = true) {
    try {
        (void)parse(arguments, extended);
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    return {};
}

bool g_launch_active = false;

/// Answers NetgameContext::launch_active from the test's flag.
///
/// @param context unused
/// @return the flag
bool answer_launch_active(void*) {
    return g_launch_active;
}

/// Checks the frontend's entry: the "-h" game name always, and while a
/// launch is active the launch's user name as the nickname.
void check_frontend_entry() {
    oa::app::NetgameContext context{};
    oa::app::Extension table{};
    oa::app::fill_option_hooks(table, context);
    auto& block = context.launch.block;
    std::snprintf(block.session_name, sizeof block.session_name, "%s", "Room");
    std::snprintf(block.user_name, sizeof block.user_name, "%s", "Launcher");
    oa::app::FrontendEntry entry{};
    table.frontend_entry(table.context, entry);
    expect(
        entry.game_name != nullptr && std::strcmp(entry.game_name, "Room") == 0 &&
            entry.nickname == nullptr,
        "without a launch only the game name"
    );
    context.launch_active = answer_launch_active;
    g_launch_active = true;
    entry = {};
    table.frontend_entry(table.context, entry);
    expect(
        entry.nickname != nullptr && std::strcmp(entry.nickname, "Launcher") == 0 &&
            std::strcmp(entry.game_name, "Room") == 0,
        "an active launch names the nickname and the game"
    );
    g_launch_active = false;
}

/// Checks the answers the multiplayer frontend states get about joining and leaving a session.
///
/// Joining never waits for a match report to start, every game plays over TCP/IP,
/// so leaving the battle room returns to the game list, and opening a
/// provider's session is left to the dispatcher's 0.
void check_frontend_state_queries() {
    namespace query = oa::netgame::frontend::query;
    oa::app::ScreenRegistry registry{};
    oa::app::register_frontend_state_queries(&registry);
    expect(registry.rejected == nullptr && registry.query_count == 2, "two queries are answered");
    const auto answer = [&](oa::ui::frontend_state::Query asked, uint32_t* out) {
        const oa::app::QueryDesc* desc = oa::app::query_find(&registry, asked);
        if (desc == nullptr)
            return false;
        *out = desc->run(nullptr, desc->state);
        return true;
    };
    uint32_t value = 7;
    expect(
        answer(query::init_score_reporting, &value) && value == 0,
        "a join never waits for the match report"
    );
    expect(
        answer(query::transport_kind, &value) &&
            value == oa::netgame::frontend::transport_kind::tcpip,
        "every game plays over TCP/IP"
    );
    expect(
        !answer(query::open_service_session, &value), "opening a provider's session keeps the 0"
    );
}

} // namespace

int main() {
    check_frontend_entry();
    check_frontend_state_queries();
    const auto plain = parse({"--headless-check"});

    for (const auto& run : std::vector<std::vector<const char*>>{
             {"--play-demo", "game.tad"},
             {"--net-loopback-check", "60"},
             {"--net-ai-benchmark", "60"},
             {"--check-recording-hook"},
             {"--net-record", "game.tad"},
             {"--list-content"},
             {"--launch", "-"},
             {"-c", "-"}
         }) {
        std::vector<const char*> arguments{"--training-worker"};
        arguments.insert(arguments.end(), run.begin(), run.end());
        expect(
            rejection(arguments) == "--training-worker cannot be combined with network run modes",
            "training refuses network run modes"
        );
    }

    for (const auto& run : std::vector<std::vector<const char*>>{
             {"--play-demo", "game.tad"},
             {"--net-ai-benchmark", "60"},
             {"--net-loopback-check", "60"},
             {"--net-record", "game.tad"},
             {"--launch", "-"},
         }) {
        std::vector<const char*> arguments{"--local-ai-benchmark", "60"};
        arguments.insert(arguments.end(), run.begin(), run.end());
        expect(
            rejection(arguments) ==
                "--local-ai-benchmark cannot be combined with network run modes",
            "local AI refuses network run modes"
        );
    }

    expect(
        rejection(
            {"--headless-check",
             "--simulation-only",
             "--match-ticks",
             "60",
             "--experimental-math",
             "hardware",
             "--net-record",
             "game.tad"}
        ) == "experimental simulation modes cannot use network run modes",
        "experimental simulation refuses a network recording"
    );
    const auto demo = parse({"--play-demo", "game.tad", "--trace-digest", "demo.trace"});
    expect(
        demo.net.play_demo == "game.tad" && demo.app.trace_digest == "demo.trace",
        "demo runs take a trace"
    );

    for (const auto& run : std::vector<std::vector<const char*>>{
             {"--play-demo", "game.tad"}, {"--net-loopback-check", "60"}, {"--check-recording-hook"}
         }) {
        std::vector<const char*> arguments{
            "--simulation-only", "--headless-check", "--match-ticks", "60"
        };
        arguments.insert(arguments.end(), run.begin(), run.end());
        expect(
            rejection(arguments) == "--simulation-only cannot be used with --net-loopback-check, "
                                    "--check-recording-hook or --play-demo",
            "--simulation-only refuses network play's headless runs"
        );
    }

    // A recording named in Chinese (U+5F55 U+50CF) is read by its UTF-8 name.
    const auto named = parse({"--play-demo", "\xe5\xbd\x95\xe5\x83\x8f.tad"});
    expect(named.net.play_demo.u8string() == u8"\u5f55\u50cf.tad", "--play-demo reads UTF-8");
    const auto recorded = parse({"--net-record", "\xe5\xbd\x95\xe5\x83\x8f.tad"});
    expect(recorded.net.net_record.u8string() == u8"\u5f55\u50cf.tad", "--net-record reads UTF-8");

    // The 3.1c network switches reach network play's handler.
    const auto hosted = parse({"-nTCPIP", "-hHost", "-t60", "-e5"});
    expect(hosted.app.launch.unavailable_switch == 0, "the network switches are taken");

    // A network loopback run is unattended, so it opens no folder dialog.
    expect(parse({"--net-loopback-check", "300"}).app.unattended, "--net-loopback-check");
    expect(
        rejection({"--net-loopback-check", "300", "--choose-game-dir"})
                .find("--choose-game-dir opens a dialog") == 0,
        "--net-loopback-check with --choose-game-dir"
    );
    const auto ai = parse({"--net-ai-benchmark", "36000"});
    expect(
        ai.net.net_ai_benchmark && ai.net.net_loopback_each_ai &&
            ai.net.net_loopback_ticks == 36000 && ai.app.headless_check && ai.app.unattended,
        "AI benchmark runs opposing computer players unattended and headless"
    );
    expect(
        rejection(
            {"--simulation-only",
             "--headless-check",
             "--match-ticks",
             "60",
             "--net-ai-benchmark",
             "60"}
        ) == "--simulation-only cannot be used with --net-ai-benchmark",
        "AI benchmark refuses the separate simulation-only run mode"
    );
    // The host-not-found check plays through the main loop, unattended and
    // without the intro.
    const auto host_not_found = parse({"--check-host-not-found"});
    expect(
        host_not_found.net.check_host_not_found && host_not_found.app.unattended &&
            host_not_found.app.skip_intro && !host_not_found.app.headless_check,
        "--check-host-not-found runs unattended through the main loop, without the intro"
    );
    expect(!plain.net.check_host_not_found, "no host-not-found check without the flag");
    expect(!plain.net.dplay_port, "the standard DirectPlay port unless --dplay-port gives one");
    expect(
        !plain.net.dplay_guid, "Total Annihilation's application GUID unless --dplay-guid names one"
    );
    expect(
        parse({"--dplay-port", "51234"}).net.dplay_port == uint16_t{51234},
        "--dplay-port takes a port"
    );
    const auto guid_bytes = [](const char* spelling) {
        const auto taken = parse({"--dplay-guid", spelling}).net.dplay_guid;
        return taken ? *taken : std::array<uint8_t, 16>{};
    };
    const auto guid_named = [](const char* spelling) {
        return parse({"--dplay-guid", spelling}).net.dplay_guid.has_value();
    };
    std::array<uint8_t, 16> ta_guid{};
    std::memcpy(ta_guid.data(), oa::netgame::application_guid, ta_guid.size());
    const char* braced = "{99797420-F5F5-11CF-9827-00A0241496C8}";
    const char* bare = "99797420-F5F5-11CF-9827-00A0241496C8";
    const char* mixed = "{99797420-f5f5-11cf-9827-00a0241496c8}";
    expect(
        guid_named(braced) && guid_named(bare) && guid_named(mixed) &&
            guid_bytes(braced) == ta_guid && guid_bytes(bare) == ta_guid &&
            guid_bytes(mixed) == ta_guid,
        "--dplay-guid takes a GUID with or without braces, in any case"
    );
    const std::array<uint8_t, 16> custom_guid = {
        0xdd,
        0xcc,
        0xbb,
        0xaa,
        0xff,
        0xee,
        0x22,
        0x11,
        0x33,
        0x44,
        0x55,
        0x66,
        0x77,
        0x88,
        0x99,
        0xaa,
    };
    expect(
        guid_named("{AABBCCDD-EEFF-1122-3344-5566778899AA}") &&
            guid_bytes("{AABBCCDD-EEFF-1122-3344-5566778899AA}") == custom_guid,
        "--dplay-guid stores the first three groups little-endian"
    );
    expect(
        rejection({"--dplay-guid", "99797420-F5F5-11CF-9827-00A0241496C8}"}) ==
                "--dplay-guid expects a GUID, with or without braces" &&
            rejection({"--dplay-guid", "{99797420-F5F5-11CF-9827-00A0241496C8"}) ==
                "--dplay-guid expects a GUID, with or without braces" &&
            rejection({"--dplay-guid", "99797420F5F511CF982700A0241496C8"}) ==
                "--dplay-guid expects a GUID, with or without braces" &&
            rejection({"--dplay-guid", "nope"}) ==
                "--dplay-guid expects a GUID, with or without braces" &&
            rejection({"--dplay-guid"}) == "--dplay-guid requires a value",
        "--dplay-guid rejects anything that is not a GUID"
    );
    {
        oa::app::NetgameContext context{};
        oa::app::Extension table{};
        oa::app::fill_option_hooks(table, context);
        const char* runs = table.text(table.context, oa::app::ExtensionText::usage_runs);
        expect(
            runs != nullptr && std::strstr(runs, "[--dplay-guid GUID] ") != nullptr,
            "--help lists --dplay-guid"
        );
    }
    expect(
        rejection({"--dplay-port", "0"}) == "--dplay-port expects a port from 1 through 65535" &&
            rejection({"--dplay-port", "65536"}) ==
                "--dplay-port expects a port from 1 through 65535" &&
            rejection({"--dplay-port", "x"}) == "--dplay-port expects a port from 1 through 65535",
        "--dplay-port rejects anything but 1 through 65535"
    );

    // --host and --join name the game the multiplayer screens host or join
    // at once, without the intro; the names and the password go with them,
    // and the frontend opens multiplayer over TCP/IP.
    using Kind = oa::ui::frontend_multiplayer::DirectGame::Kind;
    expect(plain.net.direct_game.kind == Kind::none, "no network game without --host or --join");
    const auto host = parse(
        {"--host", "--player-name", "EngineHost", "--game-name", "XPlay", "--game-password", "pw"}
    );
    expect(
        host.net.direct_game.kind == Kind::host &&
            host.net.direct_game.player_name == "EngineHost" &&
            host.net.direct_game.game_name == "XPlay" && host.net.direct_game.password == "pw" &&
            host.net.direct_game.address.empty() && host.app.skip_intro && !host.app.unattended,
        "--host takes the names and the password, and skips the intro"
    );
    expect(
        host.app.launch.unavailable_switch == 0 && parse({"--host"}).app.skip_intro, "--host alone"
    );
    const auto join = parse({"--join", "192.0.2.10", "--player-name", "EngineJoin"});
    expect(
        join.net.direct_game.kind == Kind::join && join.net.direct_game.address == "192.0.2.10" &&
            join.net.direct_game.player_name == "EngineJoin" &&
            join.net.direct_game.game_name.empty() && join.app.skip_intro,
        "--join takes the address and the name, and skips the intro"
    );
    {
        oa::app::NetgameContext context{};
        oa::app::Extension table{};
        oa::app::fill_option_hooks(table, context);
        std::vector<std::string> storage{"open-annihilation", "--join", "host.example"};
        std::vector<char*> argv;
        for (auto& argument : storage)
            argv.push_back(argument.data());
        (void)oa::app::parse_options(static_cast<int>(argv.size()), argv.data(), table);
        expect(
            context.launch.block.connection_type ==
                oa::ui::frontend_multiplayer::launch::connection_type::tcpip,
            "a game to join opens multiplayer over TCP/IP"
        );
    }
    expect(
        rejection({"--host", "--join", "192.0.2.10"}) ==
                "--host and --join cannot be used together" &&
            rejection({"--join", "192.0.2.10", "--host"}) ==
                "--host and --join cannot be used together",
        "--host with --join"
    );
    expect(
        rejection({"--game-password", "pw"}) == "--game-password needs --host or --join" &&
            rejection({"--game-name", "XPlay"}) == "--game-name needs --host or --join" &&
            rejection({"--player-name", "EngineHost"}) == "--player-name needs --host or --join",
        "the names and the password need --host or --join"
    );
    expect(rejection({"--join"}) == "--join requires a value", "--join needs an address");

    // Without the extension the demo and loopback flags do not exist.
    for (const char* flag :
         {"--play-demo",
          "--demo-unit-table",
          "--net-loopback-check",
          "--net-ai-benchmark",
          "--net-loopback-watcher",
          "--check-host-not-found",
          "--dplay-guid",
          "--host",
          "--join"})
        expect(rejection({flag}, false) == std::string("unknown option: ") + flag, flag);
    // "-y" opens 3.1c's built-in internet lobby, which this game does not
    // have: network play takes it, and it only skips the intro.
    const auto lobby = parse({"-y"});
    expect(
        lobby.app.launch.skip_intro == 1 && !lobby.net.launch_planned &&
            !oa::app::start_notes().empty() &&
            oa::app::start_notes().front().starts_with("-y opens"),
        "-y skips the intro and is noted"
    );
    expect(rejection({"-y"}, false) == "-y is not handled by this build", "-y is network play's");
    return failures == 0 ? 0 : 1;
}
