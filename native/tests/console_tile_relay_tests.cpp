// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/console_tile_relay.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
void check_relay(bool value, const char* reason) {
    if (!value) {
        std::fprintf(stderr, "FAIL console tile relay: %s\n", reason);
        std::exit(1);
    }
}

const char* preferred_menu_title(std::size_t system) {
    const auto id = shelf::catalog[system].id;
    if (id == "ps3") return "PPSA99303";
    if (id == "ps2") return "PPSA99203";
    if (id == "ps1") return "PPSA99248";
    if (id == "switch") return "PPSA99008";
    if (id == "wiiu") return "PPSA99360";
    return shelf::catalog[system].suggested_core.empty() ? nullptr : "PPSA99169";
}

struct FakeRelay {
    enum class State { origin, idle, other, unknown };
    State states[128]{};
    unsigned count = 0, reads = 0, waits = 0, releases = 0, launches = 0;
    unsigned waited_ms = 0;
    unsigned release_after = 0;
    int release_result = 1;
    int launch_result = 1;
    const char* launched_route = nullptr;

    FakeRelay() {
        for (auto& state : states) state = State::idle;
    }
    static int current(void* raw, char (&title)[10]) {
        auto& fake = *static_cast<FakeRelay*>(raw);
        const auto state = fake.states[fake.reads < fake.count ? fake.reads :
                                       (fake.count ? fake.count - 1 : 0)];
        ++fake.reads;
        if (state == State::unknown) return -1;
        if (state == State::idle) return 0;
        std::strcpy(title, state == State::origin ? "PPSA99745" : "CUSA00001");
        return 1;
    }
    static void wait(void* raw, unsigned milliseconds) {
        auto& fake = *static_cast<FakeRelay*>(raw);
        ++fake.waits;
        fake.waited_ms += milliseconds;
    }
    static int released(void* raw, const char* origin) {
        auto& fake = *static_cast<FakeRelay*>(raw);
        if (!origin || std::strcmp(origin, "PPSA99745") != 0)
            return -1;
        const auto probe = fake.releases++;
        if (fake.release_result < 0) return -1;
        return probe >= fake.release_after ? fake.release_result : 0;
    }
    static int launch(void* raw, const char* route) {
        auto& fake = *static_cast<FakeRelay*>(raw);
        ++fake.launches;
        fake.launched_route = route;
        return fake.launch_result;
    }
    shelf::ConsoleTileRelayOps ops() {
        return {this, current, wait, released, launch};
    }
};
} // namespace

void test_console_tile_relay() {
    using namespace shelf;
    constexpr char route[] = "--isshelf-console-tile=threedo,library,-";
    constexpr char emulator_route[] =
        "--isshelf-console-tile=threedo,emulator,PPSA99169";
    constexpr char standalone_route[] =
        "--isshelf-console-tile=ps2,emulator,PPSA99203";
    check_relay(console_tile_relay_title_valid("PPSA99700") &&
                    console_tile_relay_title_valid("PPSA99899") &&
                    !console_tile_relay_title_valid("PPSA99764") &&
                    !console_tile_relay_title_valid("PPSA99699") &&
                    !console_tile_relay_title_valid("CUSA99745"),
                "only isShelf's bounded non-reserved title range is accepted");
    check_relay(console_tile_relay_route_valid(route) &&
                    !console_tile_relay_route_valid("--isshelf-console-tile=unknown,library,-") &&
                    !console_tile_relay_route_valid("--isshelf-console-tile=threedo,emulator,PPSA00000"),
                "the existing console intent policy validates the one route");
    ConsoleTileRelayLaunchPlan launch_plan;
    check_relay(console_tile_relay_launch_plan(route, launch_plan) &&
                    std::strcmp(launch_plan.title, "PPSA99541") == 0 &&
                    launch_plan.forward_route &&
                    std::strcmp(launch_plan.argument, route) == 0,
                "library and Home destinations retain the isShelf dispatcher");
    check_relay(console_tile_relay_launch_plan(emulator_route, launch_plan) &&
                    std::strcmp(launch_plan.title, "PPSA99541") == 0 &&
                    launch_plan.forward_route &&
                    std::strcmp(launch_plan.argument,
                                "--isshelf-console-tile=threedo,library,-") == 0,
                "a legacy RetroArch console route opens its selected isShelf library");
    check_relay(console_tile_relay_launch_plan(standalone_route, launch_plan) &&
                    std::strcmp(launch_plan.title, "PPSA99203") == 0 &&
                    !launch_plan.forward_route && !launch_plan.argument[0],
                "a standalone emulator destination launches its exact pinned menu directly");
    check_relay(!console_tile_relay_launch_plan(
                    "--isshelf-console-tile=ps5,emulator,PPSA99169",
                    launch_plan),
                "native PS5 cannot acquire a direct emulator relay target");

    std::size_t direct_emulators = 0, normalized_retroarch = 0;
    for (std::size_t system = 0; system < system_count; ++system) {
        for (const auto destination : {ConsoleTileDestination::library,
                                       ConsoleTileDestination::home}) {
            ConsoleTileIntent intent{system, destination, {}};
            char encoded[console_tile_message_capacity + 1]{};
            check_relay(encode_console_tile_arg(intent, encoded, sizeof(encoded)) &&
                            console_tile_relay_launch_plan(encoded, launch_plan) &&
                            std::strcmp(launch_plan.title, console_tile_relay_target) == 0 &&
                            launch_plan.forward_route,
                        "every isShelf-rendered route stays on the isShelf dispatcher");
        }
        const char* title = preferred_menu_title(system);
        if (!title) continue;
        ConsoleTileIntent intent{system, ConsoleTileDestination::emulator, {}};
        std::snprintf(intent.emulator_title, sizeof(intent.emulator_title),
                      "%s", title);
        char encoded[console_tile_message_capacity + 1]{};
        check_relay(encode_console_tile_arg(intent, encoded, sizeof(encoded)) &&
                        console_tile_relay_launch_plan(encoded, launch_plan),
                    "every historical emulator route has a safe relay plan");
        if (std::strcmp(title, console_tile_shared_retroarch_title) == 0) {
            ConsoleTileIntent normalized;
            check_relay(normalize_console_tile_intent(intent, normalized) &&
                            std::strcmp(launch_plan.title, console_tile_relay_target) == 0 &&
                            launch_plan.forward_route &&
                            normalized.destination == ConsoleTileDestination::library,
                        "shared RetroArch systems normalize to their isShelf library");
            ++normalized_retroarch;
        } else {
            check_relay(std::strcmp(launch_plan.title, title) == 0 &&
                            !launch_plan.forward_route,
                        "standalone emulator routes launch their pinned menu directly");
            ++direct_emulators;
        }
    }
    check_relay(direct_emulators == 5 && normalized_retroarch == 44 &&
                    direct_emulators + normalized_retroarch == system_count - 3,
                "all 49 emulator-capable consoles use a truthful supported destination");

    unsigned char wire[console_tile_relay_request_size];
    char decoded_origin[10], decoded_route[console_tile_message_capacity + 1];
    check_relay(encode_console_tile_relay_request("PPSA99745", route, wire, sizeof(wire)) &&
                    decode_console_tile_relay_request(wire, sizeof(wire), decoded_origin, decoded_route) &&
                    std::strcmp(decoded_origin, "PPSA99745") == 0 &&
                    std::strcmp(decoded_route, route) == 0,
                "the fixed request wire format round-trips exactly");
    wire[console_tile_relay_request_size - 1] = 1;
    check_relay(!decode_console_tile_relay_request(wire, sizeof(wire), decoded_origin, decoded_route),
                "nonzero trailing request bytes are rejected");
    unsigned char reply[console_tile_relay_reply_size];
    ConsoleTileRelayReply reply_status = ConsoleTileRelayReply::invalid;
    std::int32_t reply_code = 0;
    encode_console_tile_relay_reply(ConsoleTileRelayReply::foreground_mismatch, -17, reply);
    check_relay(decode_console_tile_relay_reply(reply, sizeof(reply), reply_status, reply_code) &&
                    reply_status == ConsoleTileRelayReply::foreground_mismatch && reply_code == -17,
                "the fixed reply wire format preserves status and signed code");
    constexpr std::string_view released_body =
        R"({"status":0,"title_id":"PPSA99745","mounted":false})";
    constexpr std::string_view mounted_body =
        R"({"status":0,"title_id":"PPSA99745","mounted":true})";
    check_relay(parse_console_tile_release_probe(200, released_body, "PPSA99745") ==
                        ConsoleTileReleaseProbe::ready &&
                    parse_console_tile_release_probe(200, mounted_body, "PPSA99745") ==
                        ConsoleTileReleaseProbe::pending &&
                    parse_console_tile_release_probe(409, R"({"status":16})", "PPSA99745") ==
                        ConsoleTileReleaseProbe::pending,
                "ShadowMount mounted and busy replies keep the outgoing tile gated");
    check_relay(parse_console_tile_release_probe(
                    200, R"({"status":0,"title_id":"PPSA99744","mounted":false})",
                    "PPSA99745") == ConsoleTileReleaseProbe::invalid &&
                    parse_console_tile_release_probe(
                        200,
                        R"({"status":0,"status":0,"title_id":"PPSA99745","mounted":false})",
                        "PPSA99745") == ConsoleTileReleaseProbe::invalid,
                "a wrong title or duplicate release field cannot admit a launch");
    check_relay(parse_console_tile_unmount_probe(
                    200,
                    R"({"status":0,"title_id":"PPSA99745","mounted":false})",
                    "PPSA99745") == ConsoleTileUnmountProbe::ready &&
                    parse_console_tile_unmount_probe(
                        409, R"({"status":16,"error_reason":"game_active"})",
                        "PPSA99745") == ConsoleTileUnmountProbe::pending,
                "an exact unmount success releases immediately while busy remains pending");
    check_relay(parse_console_tile_unmount_probe(
                    200,
                    R"({"status":0,"title_id":"PPSA99744","mounted":false})",
                    "PPSA99745") == ConsoleTileUnmountProbe::invalid &&
                    parse_console_tile_unmount_probe(
                        200,
                        R"({"status":0,"status":0,"title_id":"PPSA99745","mounted":false})",
                        "PPSA99745") == ConsoleTileUnmountProbe::invalid,
                "a mismatched or duplicate unmount receipt never authorizes launch");
    check_relay(
        !console_tile_relay_should_retry_unmount(20, 1, true) &&
            console_tile_relay_should_retry_unmount(21, 1, true) &&
            !console_tile_relay_should_retry_unmount(21, 2, true) &&
            !console_tile_relay_should_retry_unmount(21, 1, false),
        "a busy unmount receives one calm retry only after five idle seconds");
    check_relay(console_tile_relay_should_poll_info(1, 0) &&
                    !console_tile_relay_should_poll_info(4, 1) &&
                    console_tile_relay_should_poll_info(5, 1),
                "fallback game-info reads are paced to one request per second");

    FakeRelay success;
    success.count = 7;
    success.states[0] = success.states[1] = FakeRelay::State::origin;
    check_relay(cooperative_console_tile_relay("PPSA99745", route, success.ops()) ==
                        ConsoleTileRelayResult::launch_requested &&
                    success.launches == 1 && success.reads == 8 &&
                    success.releases == 1 && success.waits == 6 &&
                    success.waited_ms == 1500 && success.launched_route == route,
                "the tile closes and its runtime releases before exactly one launch");

    FakeRelay gated;
    gated.release_after = 4;
    check_relay(cooperative_console_tile_relay("PPSA99745", route, gated.ops()) ==
                        ConsoleTileRelayResult::launch_requested &&
                    gated.releases == 5 && gated.launches == 1 &&
                    gated.waited_ms == 2000,
                "a busy outgoing runtime is polled at a calm bound before launch");

    FakeRelay release_changed;
    release_changed.count = 7;
    release_changed.states[6] = FakeRelay::State::other;
    release_changed.release_after = 2;
    check_relay(cooperative_console_tile_relay(
                    "PPSA99745", route, release_changed.ops()) ==
                        ConsoleTileRelayResult::foreground_changed &&
                    release_changed.releases == 1 && release_changed.launches == 0,
                "a different foreground app cancels during the release gate");

    for (auto state : {FakeRelay::State::other, FakeRelay::State::unknown}) {
        FakeRelay changed;
        changed.count = 2;
        changed.states[0] = FakeRelay::State::origin;
        changed.states[1] = state;
        check_relay(cooperative_console_tile_relay("PPSA99745", route, changed.ops()) ==
                            ConsoleTileRelayResult::foreground_changed &&
                        changed.launches == 0,
                    "a different or unknown foreground app cancels without launch");
    }

    FakeRelay timeout;
    timeout.count = 1;
    timeout.states[0] = FakeRelay::State::origin;
    check_relay(cooperative_console_tile_relay("PPSA99745", route, timeout.ops()) ==
                        ConsoleTileRelayResult::close_timeout &&
                    timeout.launches == 0 &&
                    timeout.reads == console_tile_relay_max_observations,
                "a tile that does not close times out without launch");

    FakeRelay refused;
    refused.launch_result = -1;
    check_relay(cooperative_console_tile_relay("PPSA99745", route, refused.ops()) ==
                        ConsoleTileRelayResult::launch_refused &&
                    refused.launches == 1,
                "a refused launch is never retried");

    FakeRelay release_invalid;
    release_invalid.release_result = -1;
    check_relay(cooperative_console_tile_relay(
                    "PPSA99745", route, release_invalid.ops()) ==
                        ConsoleTileRelayResult::release_unavailable &&
                    release_invalid.launches == 0,
                "an invalid ShadowMount release reply fails closed");

    FakeRelay release_timeout;
    release_timeout.release_after = console_tile_relay_release_max_observations + 1;
    check_relay(cooperative_console_tile_relay(
                    "PPSA99745", route, release_timeout.ops()) ==
                        ConsoleTileRelayResult::release_timeout &&
                    release_timeout.releases == console_tile_relay_release_max_observations &&
                    release_timeout.launches == 0,
                "a runtime that remains busy times out without a destination launch");

    FakeRelay invalid;
    check_relay(cooperative_console_tile_relay("PPSA99764", route, invalid.ops()) ==
                        ConsoleTileRelayResult::invalid &&
                    invalid.reads == 0 && invalid.launches == 0,
                "invalid origin fails before observing or launching");
    std::puts("PASS: console tile relay validates owned routes, normalizes shared RetroArch systems, waits for stable Home and ShadowMount release, then launches once.");
}
