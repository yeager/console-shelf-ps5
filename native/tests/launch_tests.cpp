// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/launch_plan.hpp"
#include "shelf/launch_protocol.hpp"
#include "shelf/ui.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
namespace {
void check(bool value, const char* why) {
    if (!value) {
        std::fprintf(stderr, "FAIL launch protocol: %s\n", why);
        std::exit(1);
    }
}
shelf::LaunchRequest request() {
    shelf::LaunchRequest result;
    std::strcpy(result.id, "0123456789abcdef0123456789abcdef");
    std::strcpy(result.title, "PPSA99203");
    result.issued = 1000;
    return result;
}
struct FakeHandoff {
    unsigned checks = 0, launches = 0, waits = 0, ready_calls = 0, idle_checks = 0;
    unsigned focus_reads = 0, target_checks = 0, origin_close_calls = 0;
    int scenario = 0, launch_result = 1, focus_result = 1;
    std::uint64_t clock = 1000;
    static int current(void* raw, char (&title)[10]) {
        auto& f = *static_cast<FakeHandoff*>(raw);
        ++f.checks;
        if (f.scenario == 1 || (f.scenario == 2 && f.checks > 1) || (f.scenario == 11 && f.checks >= 5)) {
            std::strcpy(title, "CUSA00001");
            return 1;
        }
        if (f.scenario == 3) {
            std::strcpy(title, shelf::shelf_title_id);
            return 1;
        }
        if (f.scenario == 20 && !f.origin_close_calls) {
            std::strcpy(title, shelf::shelf_title_id);
            return 1;
        }
        if (f.scenario == 21 && f.checks >= 3 && f.checks <= 4)
            return shelf::handoff_origin_releasing;
        if (f.scenario == 4 && f.checks > 1)
            return -1;
        if (f.checks <= 2) {
            std::strcpy(title, shelf::shelf_title_id);
            return 1;
        }
        if (f.launches && f.scenario != 5) {
            ++f.target_checks;
            if (f.target_checks >= 3 && f.scenario == 12) {
                std::strcpy(title, "CUSA00001");
                return 1;
            }
            if (f.target_checks >= 3 && f.scenario == 13)
                return -1;
            if (f.target_checks >= 3 && f.scenario == 14)
                return 0;
            std::strcpy(title, "PPSA99203");
            return 1;
        }
        ++f.idle_checks;
        return 0;
    }
    static bool ready(void* raw, const char* id) {
        auto& f = *static_cast<FakeHandoff*>(raw);
        ++f.ready_calls;
        check(std::strlen(id) == 32, "ready response is bound to its request ID");
        return f.scenario != 6;
    }
    static void wait(void* raw, unsigned milliseconds) {
        auto& f = *static_cast<FakeHandoff*>(raw);
        check(milliseconds == 250, "handoff polling stays bounded");
        ++f.waits;
        if (f.scenario == 7 || (f.scenario == 16 && f.launches))
            f.clock += 31;
    }
    static int launch(void* raw, const shelf::LaunchRequest& r) {
        auto& f = *static_cast<FakeHandoff*>(raw);
        ++f.launches;
        check(f.idle_checks >= 5, "the shell settles for a full monitored second before launching");
        check(r.mode == shelf::LaunchMode::tile_uninstall
                  ? std::strcmp(r.title, "PPSA99745") == 0
                  : r.mode == shelf::LaunchMode::tile_remove
                  ? std::strcmp(r.title, "PPSA99704") == 0 &&
                        std::strcmp(r.rom, "PPSA99745") == 0
                  : r.mode == shelf::LaunchMode::tile_refresh
                  ? std::strcmp(r.title, "PPSA99700") == 0 &&
                        std::strcmp(r.rom, "PPSA99704,PPSA99745") == 0
                  : r.mode == shelf::LaunchMode::tile_residual
                  ? std::strcmp(r.title, "PPSA99700") == 0 &&
                        std::strstr(r.rom, "PPSA99701,PPSA99702") == r.rom &&
                        std::strstr(r.rom, "PPSA99750,PPSA99751") != nullptr
                  : std::strcmp(r.title, "PPSA99203") == 0,
              "only the requested target is handed off");
        return f.launch_result;
    }
    static int close_origin(void* raw) {
        auto& f = *static_cast<FakeHandoff*>(raw);
        ++f.origin_close_calls;
        check(f.launches == 0 && f.origin_close_calls == 1,
              "the exact-origin close is issued once and only before launch");
        return f.scenario == 3 ? -1 : 0;
    }
    static std::uint64_t now(void* raw) {
        return static_cast<FakeHandoff*>(raw)->clock;
    }
    static shelf::HandoffConsent consent(void* raw) {
        auto& f = *static_cast<FakeHandoff*>(raw);
        if (f.scenario == 8)
            return shelf::HandoffConsent::pending;
        if (f.scenario == 9 || (f.scenario == 10 && f.waits >= 2) ||
            (f.scenario == 15 && f.launches && f.target_checks >= 3))
            return shelf::HandoffConsent::cancelled;
        return shelf::HandoffConsent::committed;
    }
    static int focused(void* raw, const shelf::LaunchRequest& r) {
        auto& f = *static_cast<FakeHandoff*>(raw);
        ++f.focus_reads;
        check(f.launches == 1 && f.focus_reads <= 32, "focus observation is bounded after exactly one launch");
        check(std::strcmp(r.title, "PPSA99203") == 0, "focus observation stays bound to the selected title");
        if (f.scenario >= 12 && f.scenario <= 16)
            return 0;
        if (f.scenario == 19 && f.focus_reads < 3)
            return 0;
        return f.focus_result;
    }
    shelf::HandoffOps ops() {
        return {this, current, ready, wait, close_origin, launch, now, consent, focused};
    }
};
struct FakeMaintenanceEndpoint {
    unsigned probes = 0, checks = 0, waits = 0, unavailable = 0;
    int current_result = 0;
    bool hard_failure = false;
    static shelf::MaintenanceEndpointProbe probe(void* raw) {
        auto& f = *static_cast<FakeMaintenanceEndpoint*>(raw);
        ++f.probes;
        if (f.hard_failure)
            return shelf::MaintenanceEndpointProbe::failed;
        return f.probes <= f.unavailable
            ? shelf::MaintenanceEndpointProbe::unavailable
            : shelf::MaintenanceEndpointProbe::ready;
    }
    static int current(void* raw, char (&)[10]) {
        auto& f = *static_cast<FakeMaintenanceEndpoint*>(raw);
        ++f.checks;
        return f.current_result;
    }
    static void wait(void* raw, unsigned milliseconds) {
        auto& f = *static_cast<FakeMaintenanceEndpoint*>(raw);
        check(milliseconds == 250,
              "maintenance readiness uses bounded 250 ms waits");
        ++f.waits;
    }
    shelf::MaintenanceEndpointOps ops() {
        return {this, probe, current, wait};
    }
};
} // namespace
void test_launch() {
    using namespace shelf;
    FakeMaintenanceEndpoint delayed_endpoint;
    delayed_endpoint.unavailable = 4;
    check(wait_for_maintenance_endpoint(delayed_endpoint.ops()) ==
              MaintenanceEndpointResult::ready &&
          delayed_endpoint.probes == 5 && delayed_endpoint.checks == 5 &&
          delayed_endpoint.waits == 4,
          "read-only endpoint readiness may settle before one mutation");
    FakeMaintenanceEndpoint missing_endpoint;
    missing_endpoint.unavailable = 40;
    check(wait_for_maintenance_endpoint(missing_endpoint.ops()) ==
              MaintenanceEndpointResult::timeout &&
          missing_endpoint.probes == 33 && missing_endpoint.checks == 33 &&
          missing_endpoint.waits == 32,
          "endpoint readiness stops after the bounded eight-second window");
    FakeMaintenanceEndpoint failed_endpoint;
    failed_endpoint.hard_failure = true;
    check(wait_for_maintenance_endpoint(failed_endpoint.ops()) ==
              MaintenanceEndpointResult::failed &&
          failed_endpoint.probes == 1 && failed_endpoint.waits == 0,
          "a hard readiness failure is never retried");
    FakeMaintenanceEndpoint foreground_endpoint;
    foreground_endpoint.current_result = 1;
    check(wait_for_maintenance_endpoint(foreground_endpoint.ops()) ==
              MaintenanceEndpointResult::foreground_changed &&
          foreground_endpoint.probes == 0 && foreground_endpoint.waits == 0,
          "a foreground app stops readiness polling before any endpoint probe");
    check(wait_for_maintenance_endpoint({}) ==
              MaintenanceEndpointResult::invalid,
          "missing readiness hooks are rejected");
    auto r = request();
    auto library = std::make_unique<Library>();
    const auto n64 = find_system("n64"), ps2 = find_system("ps2"), ps5 = find_system("ps5"), ps1 = find_system("ps1");
    library->route_count = 4;
    auto& ra = library->routes[0];
    ra.system = n64;
    std::strcpy(ra.id, "retroarch:mupen64plus_next");
    std::strcpy(ra.title_id, "PPSA99169");
    std::strcpy(ra.folder, "/data/homebrew/PPSA99169");
    std::strcpy(ra.core, "/data/homebrew/PPSA99169/cores/mupen64plus_next_libretro.so");
    auto& standalone = library->routes[1];
    standalone.system = n64;
    std::strcpy(standalone.id, "PPSA99064");
    std::strcpy(standalone.title_id, "PPSA99064");
    std::strcpy(standalone.folder, "/data/homebrew/PPSA99064");
    auto& ps2route = library->routes[2];
    ps2route.system = ps2;
    std::strcpy(ps2route.id, "PPSA99203");
    std::strcpy(ps2route.title_id, "PPSA99203");
    std::strcpy(ps2route.folder, "/data/homebrew/PPSA99203");
    auto& psswan = library->routes[3];
    psswan.system = ps1;
    std::strcpy(psswan.id, "PPSA99248");
    std::strcpy(psswan.title_id, "PPSA99248");
    std::strcpy(psswan.folder, "/data/homebrew/PPSA99248");
    library->game_count = 4;
    library->games[3].system = ps1;
    library->games[3].kind = MediaKind::cue;
    std::strcpy(library->games[3].path, "/data/PSXS5/games/Own game/Own game.cue");
    library->games[0].system = n64;
    std::strcpy(library->games[0].path, "/data/homebrew/PPSA99169/content/n64/Own game.z64");
    library->games[1].system = ps2;
    library->games[1].kind = MediaKind::disc_image;
    std::strcpy(library->games[1].path, "/data/PCSX2/games/Own game.iso");
    library->games[2].system = ps5;
    library->games[2].kind = MediaKind::native_title;
    std::strcpy(library->games[2].title_id, "PPSA00001");
    std::strcpy(library->games[2].path, "/user/app/PPSA00001");
    LaunchRequest planned;
    check(plan_game(*library, 0, ra.id, planned) == LaunchPlan::direct_game &&
              std::strcmp(planned.core, "mupen64plus_next") == 0 && std::strstr(planned.rom, "Own game.z64"),
          "the selected installed RetroArch core produces the external direct-game plan");
    check(plan_game(*library, 0, standalone.id, planned) == LaunchPlan::menu && !planned.rom[0],
          "the chosen standalone emulator remains a menu route");
    check(std::strcmp(launch_caption(plan_game(*library, 0, standalone.id, planned)).action, "Open emulator library") == 0 &&
              std::strstr(launch_caption(LaunchPlan::menu).expectation, "Choose the game there"),
          "the pre-launch menu caption states the real action before the user selects a game");
    check(std::strcmp(launch_caption(plan_game(*library, 0, ra.id, planned)).action, "Play game") == 0,
          "a direct RetroArch route has a different pre-launch action");
    check(plan_game(*library, 1, {}, planned) == LaunchPlan::menu && std::strcmp(planned.title, "PPSA99203") == 0,
          "a PS2 game honestly opens the emulator's own shelf");
    check(plan_game(*library, 3, "PPSA99248", planned) == LaunchPlan::direct_game &&
              planned.mode == LaunchMode::psswan_game && !planned.core[0] &&
              std::strcmp(planned.rom, "ftp://127.0.0.1:2121/data/PSXS5/games/Own game/Own game.cue") == 0,
          "PSSwanStation gets the exact selected PS1 game through the existing console-local FTP server");
    check(plan_game(*library, 2, {}, planned) == LaunchPlan::native_game && !planned.rom[0],
          "registered native games use a title launch instead of a ROM argument");
    std::strcpy(library->games[2].title_id, shelf_title_id);
    check(plan_game(*library, 2, {}, planned) == LaunchPlan::unavailable, "Shelf cannot launch itself as a game");
    std::strcpy(ra.core, "/data/other/core_libretro.so");
    check(plan_game(*library, 0, ra.id, planned) == LaunchPlan::unavailable,
          "a core outside the detected emulator is refused");
    std::strcpy(ra.core, "/data/homebrew/PPSA99169/cores/mupen64plus_next_libretro.so");
    UiState connected;
    connected.library = library.get();
    connected.selected_system = n64;
    connected.launch_connected = true;
    connected.page = Page::library;
    std::snprintf(connected.settings.preferred_emulator[n64].data(), 48, "%s", ra.id);
    input(connected, Action::confirm);
    check(connected.launch_requested && connected.pending_launch.mode == LaunchMode::retroarch_game,
          "the game action queues the selected route without executing it in the UI model");
    input(connected, Action::up);
    check(connected.tools && connected.tool == 2, "Up from the first game reaches the settings gear");
    input(connected, Action::down);
    check(!connected.tools && connected.game_focus == 0, "Down returns from the gear to the game list");
    check(launch_request_valid(r, 1000), "known emulator menu is accepted");
    char message[launch_message_capacity];
    check(encode_launch_request(r, 1000, message, sizeof(message)), "menu request is encoded");
    const std::string original(message);
    LaunchStatus response;
    std::strcpy(response.id, r.id);
    response.issued = r.issued;
    response.stage = LaunchStage::ready;
    response.code = 0x80940033;
    char encoded_status[256];
    LaunchStatus decoded_status;
    check(encode_launch_status(response, encoded_status, sizeof(encoded_status)) &&
              parse_launch_status(encoded_status, 1000, decoded_status) && decoded_status.stage == LaunchStage::ready &&
              decoded_status.code == response.code && std::strcmp(decoded_status.id, r.id) == 0,
          "reply roundtrip binds its ID, timestamp, state and numeric code");
    check(!parse_launch_status(std::string(encoded_status) + "state=committed\n", 1000, decoded_status) &&
              !parse_launch_status(encoded_status, 1031, decoded_status),
          "duplicate and stale replies are rejected");
    LaunchRequest wrong_owner = r;
    wrong_owner.id[0] = wrong_owner.id[0] == '0' ? '1' : '0';
    check(launch_status_owned_by_request(encoded_status, r, decoded_status) &&
              !launch_status_owned_by_request(encoded_status, wrong_owner, decoded_status),
          "a long helper may update only the exact status record owned by its consumed request");
    response.stage = LaunchStage::foreground_requested;
    response.code = 0x8094000c;
    check(encode_launch_status(response, encoded_status, sizeof(encoded_status)) &&
              parse_launch_status(encoded_status, 1000, decoded_status) &&
              decoded_status.stage == LaunchStage::foreground_requested && decoded_status.code == 0x8094000c,
          "an accepted foreground request is distinct from confirmed on-screen visibility");
    response.stage = LaunchStage::foreground_observed;
    check(encode_launch_status(response, encoded_status, sizeof(encoded_status)) &&
              parse_launch_status(encoded_status, 1000, decoded_status) &&
              decoded_status.stage == LaunchStage::foreground_observed,
          "observed shell focus has a distinct status without claiming a TV display test");
    LaunchRequest parsed;
    check(parse_launch_request(original, 1000, parsed) && std::strcmp(parsed.id, r.id) == 0 &&
              std::strcmp(parsed.title, r.title) == 0 && parsed.mode == r.mode,
          "request roundtrip keeps its identity and route");
    check(!parse_launch_request(original + "title=PPSA99169\n", 1000, parsed), "duplicate/extra fields are rejected");
    check(!parse_launch_request(original.substr(0, original.size() - 1), 1000, parsed), "partial records are rejected");
    check(!parse_launch_request(original, 1031, parsed) && !parse_launch_request(original, 997, parsed),
          "expired and future requests cannot be replayed");
    check(!parse_launch_request(std::string("\0", 1) + original, 1000, parsed), "embedded NUL is rejected");
    auto malformed = original;
    malformed.replace(malformed.find("1000"), 4, "18446744073709551616");
    check(!parse_launch_request(malformed, 1000, parsed), "timestamp overflow is rejected");
    std::strcpy(r.title, "NPXS40087");
    check(!launch_request_valid(r, 1000), "system utilities are not launch targets");
    std::strcpy(r.title, shelf_title_id);
    check(!launch_request_valid(r, 1000), "a request cannot relaunch Shelf itself");
    std::strcpy(r.title, "PPSA00001");
    check(!launch_request_valid(r, 1000), "an unknown emulator does not gain a menu contract");
    r.mode = LaunchMode::native_title;
    check(launch_request_valid(r, 1000), "a native game has an explicit separate mode");
    r.mode = LaunchMode::retroarch_game;
    std::strcpy(r.title, "PPSA99169");
    std::strcpy(r.rom, "/data/homebrew/PPSA99169/content/n64/Mario Party 3.z64");
    std::strcpy(r.core, "mupen64plus_next");
    check(launch_request_valid(r, 1000) && encode_launch_request(r, 1000, message, sizeof(message)) &&
              parse_launch_request(message, 1000, parsed),
          "an external RetroArch game request is accepted");
    const auto argv = launch_arguments(r);
    check(std::strcmp(argv[0], "--rom") == 0 && argv[1] == r.rom && std::strcmp(argv[2], "--core") == 0 &&
              argv[3] == r.core && std::strcmp(argv[4], "--exit-after-game") == 0 && !argv[5],
          "external argv preserves spaces and does not use the internal frontend mode");
    for (const char* path :
         {"/data/homebrew/PPSA99169/content/../settings.ini", "/data/homebrew/PPSA99169/content//game.z64",
          "/user/settings/game.z64", "/data/homebrew/PPSA99169/content/a\nwrite=/data/other", "/data/PSXS5/games/"}) {
        std::strcpy(r.rom, path);
        check(!launch_request_valid(r, 1000), "out-of-root, traversal, empty segments and line injection are rejected");
    }
    std::strcpy(r.rom, "/data/PSXS5/games/Own game.cue");
    std::strcpy(r.core, "../other");
    check(!launch_request_valid(r, 1000), "a core ID cannot select an arbitrary file");
    r = request();
    r.mode = LaunchMode::psswan_game;
    std::strcpy(r.title, "PPSA99248");
    std::strcpy(r.rom, "ftp://127.0.0.1:2121/data/PSXS5/games/Own game.cue");
    check(launch_request_valid(r, 1000) && encode_launch_request(r, 1000, message, sizeof(message)) &&
              parse_launch_request(message, 1000, parsed) && parsed.mode == LaunchMode::psswan_game,
          "a PSSwanStation direct-game request survives the protocol roundtrip");
    const auto psswan_argv = launch_arguments(r);
    check(std::strcmp(psswan_argv[0], "--rom") == 0 && psswan_argv[1] == r.rom &&
              std::strcmp(psswan_argv[2], "--exit-after-game") == 0 && !psswan_argv[3],
          "PSSwanStation receives its own documented arguments without a RetroArch core argument");
    for (const char* path : {"ftp://192.168.1.2:2121/data/PSXS5/games/Own game.cue",
                             "ftp://127.0.0.1:21/data/PSXS5/games/Own game.cue",
                             "ftp://127.0.0.1:2121/data/PSXS5/games/../settings.cfg",
                             "ftp://127.0.0.1:2121/data/PCSX2/games/Own game.iso"}) {
        std::strcpy(r.rom, path);
        check(!launch_request_valid(r, 1000), "PSSwanStation cannot use another server, port, traversal or PS2 library");
    }
    r = request();
    r.mode = LaunchMode::tile_uninstall;
    std::strcpy(r.title, "PPSA99745");
    check(launch_request_valid(r, 1000) &&
              encode_launch_request(r, 1000, message, sizeof(message)) &&
              parse_launch_request(message, 1000, parsed) &&
              parsed.mode == LaunchMode::tile_uninstall,
          "an isShelf tile uninstall survives the nonce-bound handoff protocol");
    char http_request[512];
    check(encode_tile_uninstall_http_request(r, http_request, sizeof(http_request)) &&
              std::strstr(http_request, "POST /api/v1/games/uninstall HTTP/1.1\r\n") &&
              std::strstr(http_request, "{\"title_id\":\"PPSA99745\"}"),
          "tile removal produces one fixed loopback uninstall request");
    const std::string accepted_http =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
        "{\"status\":0,\"title_id\":\"PPSA99745\",\"uninstall_requested\":true}";
    const std::string game_active_http =
        "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\n\r\n"
        "{\"status\":16,\"error\":\"a game is active or awaiting exit; close the game and try again\","
        "\"error_reason\":\"game_active\"}";
    const std::string scanner_busy_http =
        "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\n\r\n"
        "{\"status\":16,\"error\":\"scanner or another storage operation is busy; try again when it finishes\","
        "\"error_reason\":\"scanner_busy\"}";
    check(tile_uninstall_http_response_valid(accepted_http, r.title) &&
              tile_uninstall_http_response(accepted_http, r.title) ==
                  TileUninstallHttpResult::accepted &&
              tile_uninstall_http_response(game_active_http, r.title) ==
                  TileUninstallHttpResult::game_active &&
              tile_uninstall_http_response(scanner_busy_http, r.title) ==
                  TileUninstallHttpResult::scanner_busy &&
              !tile_uninstall_http_response_valid(accepted_http, "PPSA99744") &&
              !tile_uninstall_http_response_valid(
                  "HTTP/1.1 409 Conflict\r\n\r\n{\"status\":16}", r.title) &&
              tile_uninstall_http_response(
                  "HTTP/1.1 409 Conflict\r\n\r\n{\"status\":16,\"error_reason\":\"runtime_busy\"}",
                  r.title) == TileUninstallHttpResult::invalid,
          "only the exact accepted or reviewed transient uninstall receipts are classified");
    std::strcpy(r.title, "PPSA99764");
    check(!launch_request_valid(r, 1000), "the reserved forwarder title cannot be uninstalled");
    std::strcpy(r.title, "PPSA99804");
    check(!launch_request_valid(r, 1000), "a title outside both isShelf allocation ranges is rejected");
    UiState removal_ui;
    check(begin_console_tile_uninstall_handoff(removal_ui, "PPSA99745") &&
              removal_ui.launch_requested &&
              removal_ui.pending_launch.mode == LaunchMode::tile_uninstall &&
              std::strcmp(removal_ui.pending_launch.title, "PPSA99745") == 0 &&
              std::strstr(removal_ui.notice, "Closing isShelf"),
          "a completed source isolation queues the cooperative uninstall handoff");
    check(!begin_console_tile_uninstall_handoff(removal_ui, "PPSA99764"),
          "the UI cannot hand off an unowned reserved title");
    std::strcpy(r.id, "0123456789abcdef0123456789abcdef");
    std::strcpy(r.title, "PPSA99745");
    FakeHandoff removal;
    removal.launch_result = 0;
    check(cooperative_handoff(r, removal.ops()) == HandoffResult::started &&
              removal.launches == 1 && removal.focus_reads == 0,
          "tile uninstall waits for cooperative closure, executes once and never polls app focus");
    UiState queued_removals;
    check(queue_console_tile_removal(queued_removals, "PPSA99704") &&
              queue_console_tile_removal(queued_removals, "PPSA99704") &&
              queue_console_tile_removals(
                  queued_removals, "PPSA99745", sizeof("PPSA99745")) &&
              queued_removals.tile_removal_count == 2 &&
              std::strcmp(queued_removals.tile_removal_titles,
                          "PPSA99704,PPSA99745") == 0 &&
              !queue_console_tile_removal(queued_removals, "PPSA99764"),
          "the UI deduplicates only bounded isShelf-owned titles in its removal queue");
    check(begin_console_tile_removal_handoff(queued_removals) &&
              queued_removals.launch_requested &&
              queued_removals.pending_launch.mode == LaunchMode::tile_remove &&
              std::strcmp(queued_removals.pending_launch.title, "PPSA99704") == 0 &&
              std::strcmp(queued_removals.pending_launch.rom, "PPSA99745") == 0 &&
              std::strstr(queued_removals.notice, "ShadowMount"),
          "one explicit finish action converts every queued removal into one post-close handoff");
    r = request();
    r.mode = LaunchMode::tile_remove;
    std::strcpy(r.title, "PPSA99704");
    std::strcpy(r.rom, "PPSA99745");
    check(launch_request_valid(r, 1000) &&
              encode_launch_request(r, 1000, message, sizeof(message)) &&
              parse_launch_request(message, 1000, parsed) &&
              parsed.mode == LaunchMode::tile_remove &&
              std::strcmp(parsed.rom, r.rom) == 0,
          "a bounded multi-tile removal survives the nonce-bound handoff protocol");
    FakeHandoff remove_many;
    remove_many.launch_result = 0;
    check(cooperative_handoff(r, remove_many.ops()) == HandoffResult::started &&
              remove_many.launches == 1 && remove_many.focus_reads == 0,
          "all queued removals wait for one cooperative close and execute without focus polling");
    std::string residual_titles;
    for (unsigned number = 99700; number < 99752; ++number) {
        char title[10];
        std::snprintf(title, sizeof(title), "PPSA%05u", number);
        if (!residual_titles.empty()) residual_titles.push_back(',');
        residual_titles += title;
    }
    const auto residual_comma = residual_titles.find(',');
    const std::string residual_first = residual_titles.substr(0, residual_comma);
    const std::string residual_remaining = residual_titles.substr(residual_comma + 1);
    std::size_t residual_count = 0;
    check(residual_titles.size() == 519 &&
              launch_tile_residual_titles_valid(residual_first,
                                                 residual_remaining,
                                                 &residual_count) &&
              residual_count == 52 &&
              !launch_tile_residual_titles_valid(
                  "PPSA99701", residual_remaining) &&
              !launch_tile_residual_titles_valid(
                  residual_first, residual_remaining.substr(0,
                      residual_remaining.rfind(','))),
          "residual cleanup accepts only the complete canonical preferred-title sequence");
    UiState residual_ui;
    residual_ui.tile_residual_sweep = true;
    check(queue_console_tile_removals(residual_ui, residual_titles.c_str(),
                                       residual_titles.size() + 1) &&
              begin_console_tile_removal_handoff(residual_ui) &&
              residual_ui.pending_launch.mode == LaunchMode::tile_residual &&
              residual_ui.tile_removal_count == 52 &&
              std::strcmp(residual_ui.pending_launch.title, "PPSA99700") == 0 &&
              std::strcmp(residual_ui.pending_launch.rom,
                          residual_remaining.c_str()) == 0 &&
              std::strstr(residual_ui.notice, "residual") != nullptr,
          "the UI marks the full preferred list as a separately guarded post-close residual sweep");
    r = request();
    r.mode = LaunchMode::tile_residual;
    std::strcpy(r.title, residual_first.c_str());
    std::strcpy(r.rom, residual_remaining.c_str());
    check(launch_request_valid(r, 1000) &&
              encode_launch_request(r, 1000, message, sizeof(message)) &&
              parse_launch_request(message, 1000, parsed) &&
              parsed.mode == LaunchMode::tile_residual &&
              std::strcmp(parsed.rom, residual_remaining.c_str()) == 0,
          "the exact 52-title residual sweep survives the nonce-bound handoff protocol");
    FakeHandoff residual_handoff;
    residual_handoff.launch_result = 0;
    check(cooperative_handoff(r, residual_handoff.ops()) == HandoffResult::started &&
              residual_handoff.launches == 1 && residual_handoff.focus_reads == 0,
          "residual cleanup waits for one cooperative close and never polls app focus");
    const std::string residual_param =
        "{\"applicationDrmType\":\"free\",\"conceptId\":\"99700\","
        "\"contentBadgeType\":1,\"contentId\":\"UP9000-PPSA99700_00-PS5FORWARDER0000\","
        "\"titleId\":\"PPSA99700\"}\n";
    std::size_t residual_system = 99;
    check(tile_residual_title_index("PPSA99700", residual_system) &&
              residual_system == 0 &&
              tile_residual_param_identity(residual_param, "PPSA99700") &&
              !tile_residual_param_identity(residual_param, "PPSA99701") &&
              !tile_residual_title_index("PPSA99752", residual_system) &&
              !tile_residual_param_identity(residual_param +
                  "{\"titleId\":\"PPSA99700\"}\n", "PPSA99700"),
          "residual registration metadata binds one preferred title/content/concept identity and rejects alternates or duplicates");
    char manual_request[256];
    const std::string manual_clear =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
        "{\"status\":0,\"count\":2,\"paths\":[\"/data/homebrew/PPSA99170\","
        "\"/data/homebrew/PPSA99171\"]}";
    check(encode_tile_manual_list_http_request(manual_request,
                                                sizeof(manual_request)) &&
              std::strstr(manual_request,
                          "POST /api/v1/manual/list HTTP/1.1\r\n") &&
              tile_residual_manual_list_valid(manual_clear) &&
              !tile_residual_manual_list_valid(
                  "HTTP/1.1 200 OK\r\n\r\n"
                  "{\"status\":0,\"count\":1,\"paths\":[\"/data/homebrew/PPSA99700\"]}") &&
              !tile_residual_manual_list_valid(
                  "HTTP/1.1 500 Internal Server Error\r\n\r\n"
                  "{\"status\":0,\"count\":0,\"paths\":[]}"),
          "the residual sweep proceeds only after the manual source list excludes every protected slot");
    r = request();
    r.mode = LaunchMode::tile_refresh;
    std::strcpy(r.title, "PPSA99700");
    std::strcpy(r.rom, "PPSA99704,PPSA99745");
    check(launch_request_valid(r, 1000) &&
              encode_launch_request(r, 1000, message, sizeof(message)) &&
              parse_launch_request(message, 1000, parsed) &&
              parsed.mode == LaunchMode::tile_refresh &&
              std::strcmp(parsed.rom, r.rom) == 0,
          "a bounded owned-title artwork refresh survives the handoff protocol");
    char scan_request[256];
    check(encode_tile_scan_http_request(scan_request, sizeof(scan_request)) &&
              std::strstr(scan_request, "POST /api/v1/scan HTTP/1.1\r\n") &&
              tile_scan_http_response_valid(
                  "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
                  "{\"status\":0,\"scan_queued\":true,\"scan_deferred\":true}"),
          "bulk artwork refresh ends with one exact accepted scan request");
    UiState refresh_ui;
    check(begin_console_tile_refresh_handoff(
              refresh_ui, "PPSA99700,PPSA99704,PPSA99745",
              sizeof("PPSA99700,PPSA99704,PPSA99745")) &&
              refresh_ui.launch_requested &&
              refresh_ui.pending_launch.mode == LaunchMode::tile_refresh &&
              std::strcmp(refresh_ui.pending_launch.title, "PPSA99700") == 0 &&
              std::strcmp(refresh_ui.pending_launch.rom,
                          "PPSA99704,PPSA99745") == 0,
          "the UI converts only a validated owned-title list into the post-close refresh");
    check(!begin_console_tile_refresh_handoff(
              refresh_ui, "PPSA99700,PPSA99700",
              sizeof("PPSA99700,PPSA99700")) &&
              !begin_console_tile_refresh_handoff(
                  refresh_ui, "PPSA99700,PPSA99764",
                  sizeof("PPSA99700,PPSA99764")),
          "duplicate and reserved titles cannot enter the bulk refresh handoff");
    FakeHandoff refresh;
    refresh.launch_result = 0;
    check(cooperative_handoff(r, refresh.ops()) == HandoffResult::started &&
              refresh.launches == 1 && refresh.focus_reads == 0,
          "bulk artwork refresh waits for cooperative closure and executes once without focus polling");
    FakeHandoff refresh_release_gap;
    refresh_release_gap.scenario = 21;
    refresh_release_gap.launch_result = 0;
    check(cooperative_handoff(r, refresh_release_gap.ops()) == HandoffResult::started &&
              refresh_release_gap.launches == 1 && refresh_release_gap.idle_checks >= 5,
          "bulk refresh waits through the exact-origin title release gap, then requires stable Home");
    r = request();
    std::memset(r.title, 'A', sizeof(r.title));
    check(!launch_request_valid(r, 1000), "nonterminated fields are bounded before string operations");
    r = request();
    FakeHandoff f;
    check(cooperative_handoff(r, f.ops()) == HandoffResult::foreground_observed && f.launches == 1 &&
              f.focus_reads == 1 && f.ready_calls == 1 && f.origin_close_calls == 0,
          "cooperative closure permits one launch followed by a read-only focus check");
    for (int scenario : {1, 2, 3, 4, 6, 8, 9, 10, 11}) {
        FakeHandoff refused;
        refused.scenario = scenario;
        const auto result = cooperative_handoff(r, refused.ops());
        check(result != HandoffResult::foreground_observed && result != HandoffResult::unconfirmed && refused.launches == 0 &&
                  refused.focus_reads == 0 && refused.origin_close_calls <= 1,
              "busy/unknown foreground, failed acknowledgement and closure timeout never launch");
    }
    FakeHandoff stuck_then_closed;
    stuck_then_closed.scenario = 20;
    check(cooperative_handoff(r, stuck_then_closed.ops()) == HandoffResult::foreground_observed &&
              stuck_then_closed.origin_close_calls == 1 && stuck_then_closed.launches == 1,
          "a committed exact Shelf origin receives one bounded close fallback before launch");
    FakeHandoff accepted;
    accepted.scenario = 5;
    check(cooperative_handoff(r, accepted.ops()) == HandoffResult::unconfirmed && accepted.launches == 1 &&
              accepted.waits < 100 && accepted.focus_reads == 0,
          "an accepted but unobserved launch is not called successful or retried");
    FakeHandoff denied;
    denied.scenario = 5;
    denied.launch_result = -1;
    check(cooperative_handoff(r, denied.ops()) == HandoffResult::refused && denied.launches == 1 && denied.focus_reads == 0,
          "a refused launch has no alternate call or retry");
    FakeHandoff appearing;
    appearing.launch_result = -1;
    check(cooperative_handoff(r, appearing.ops()) == HandoffResult::foreground_observed && appearing.launches == 1 &&
              appearing.focus_reads == 1,
          "an observed focused target wins over a negative return without a second launch");
    FakeHandoff expired;
    expired.scenario = 7;
    check(cooperative_handoff(r, expired.ops()) == HandoffResult::invalid && expired.launches == 0,
          "a request expiring during closure cannot launch later");
    for (int scenario : {12, 13, 14, 15, 16}) {
        FakeHandoff changed;
        changed.scenario = scenario;
        const auto result = cooperative_handoff(r, changed.ops());
        check(result != HandoffResult::foreground_observed && changed.launches == 1 && changed.focus_reads <= 2,
              "a changed, unknown, closed, cancelled or expired target stops focus observation without another launch");
    }
    FakeHandoff focus_unknown;
    focus_unknown.focus_result = -1;
    check(cooperative_handoff(r, focus_unknown.ops()) == HandoffResult::unconfirmed && focus_unknown.launches == 1 &&
              focus_unknown.focus_reads == 1,
          "unavailable focus information stays unconfirmed and causes no alternate launch");
    FakeHandoff background;
    background.focus_result = 0;
    check(cooperative_handoff(r, background.ops()) == HandoffResult::unconfirmed && background.launches == 1 &&
              background.focus_reads == 32 && background.waits < 100,
          "a running background app is never called foreground or relaunched");
    FakeHandoff delayed;
    delayed.scenario = 19;
    check(cooperative_handoff(r, delayed.ops()) == HandoffResult::foreground_observed && delayed.launches == 1 &&
              delayed.focus_reads == 3,
          "the asynchronous shell flow can finish without an extra launch call");
    std::puts("PASS: bounded launch protocol, stale/injected request refusal, exact RetroArch argv and cooperative "
              "single-launch and read-only focus mocks. No console launch performed.");
}
