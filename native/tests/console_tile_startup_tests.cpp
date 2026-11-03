// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/console_tile_startup.hpp"
#include "shelf/console_tile_remembered_startup.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
void check(bool condition, const char* reason) {
    if (!condition) {
        std::fprintf(stderr, "FAIL console tile startup: %s\n", reason);
        std::exit(1);
    }
}
const char* menu_title(std::size_t system) {
    const auto id = shelf::catalog[system].id;
    if (id == "ps3") return "PPSA99303";
    if (id == "ps2") return "PPSA99203";
    if (id == "ps1") return "PPSA99248";
    if (id == "switch") return "PPSA99008";
    if (id == "wiiu") return "PPSA99360";
    return shelf::catalog[system].suggested_core.empty() ? nullptr : "PPSA99169";
}
} // namespace

void test_console_tile_startup() {
    using namespace shelf;
    std::remove("console-tile-startup-absent.txt");
    char argument[console_tile_message_capacity + 1]{};
    static Library library;
    std::size_t emulators = 0;
    for (std::size_t system = 0; system < system_count; ++system) {
        for (auto destination : {ConsoleTileDestination::library, ConsoleTileDestination::home}) {
            const ConsoleTileIntent intent{system, destination, {}};
            check(encode_console_tile_arg(intent, argument, sizeof(argument)), "all console startup arguments encode");
            char* argv[] = {argument};
            ConsoleTileIntent selected;
            check(select_console_tile_intent(1, argv, "console-tile-startup-absent.txt", selected) ==
                      ConsoleTileInput::valid && console_tile_intents_equal(selected, intent),
                  "all console arguments select their destination from argv[0]");
            UiState state;
            state.page = Page::settings;
            state.tools = true;
            std::snprintf(state.query, sizeof(state.query), "unmatched search");
            state.settings.preferences.scope = Scope::available;
            state.settings.preferences.hidden[system] = true;
            if (destination == ConsoleTileDestination::library) {
                state.game_focus = 7;
                check(apply_console_tile_intent(state, selected) == ConsoleTileApply::library &&
                          state.page == Page::library && state.selected_system == system && state.game_focus == 0 &&
                          !state.tools && !state.query[0] && !state.launch_requested,
                      "library shortcut opens the requested console library without launching");
            } else {
                check(apply_console_tile_intent(state, selected) == ConsoleTileApply::home &&
                          state.page == Page::home && state.selected_system == system &&
                          state.tile_home_override == system && !state.tools && !state.query[0] &&
                          !state.launch_requested && state.settings.preferences.hidden[system],
                      "home shortcut selects its console without changing saved visibility");
                const auto shown = shown_systems(state);
                check(state.focus >= 0 && static_cast<std::size_t>(state.focus) < shown.count &&
                          shown.indices[static_cast<std::size_t>(state.focus)] == system,
                      "home shortcut focuses its console even when filters hide it");
            }
        }
        const char* title = menu_title(system);
        if (!title)
            continue;
        ++emulators;
        ConsoleTileIntent intent{system, ConsoleTileDestination::emulator, {}};
        std::snprintf(intent.emulator_title, sizeof(intent.emulator_title), "%s", title);
        check(encode_console_tile_arg(intent, argument, sizeof(argument)), "emulator argument encodes");
        char* argv[] = {argument};
        ConsoleTileIntent selected;
        check(select_console_tile_intent(1, argv, "console-tile-startup-absent.txt", selected) ==
                  ConsoleTileInput::valid && console_tile_intents_equal(selected, intent),
              "emulator argument selects its pinned title");
        library.route_count = 1;
        auto& route = library.routes[0];
        route = {};
        route.system = system;
        std::snprintf(route.title_id, sizeof(route.title_id), "%s", title);
        std::snprintf(route.folder, sizeof(route.folder), "/data/homebrew/%s", title);
        UiState state;
        state.library = &library;
        state.launch_connected = true;
        check(apply_console_tile_intent(state, selected) == ConsoleTileApply::emulator &&
                  state.selected_system == system && state.page == Page::details && state.detail_focus == 1 &&
                  state.launch_requested && state.pending_launch.mode == LaunchMode::emulator_menu &&
                  std::strcmp(state.pending_launch.title, title) == 0 && !state.pending_launch.rom[0] &&
                  !state.pending_launch.core[0],
              "emulator shortcut queues exactly its installed console menu");
        std::snprintf(state.pending_launch.id, sizeof(state.pending_launch.id), "%032x", 1);
        state.pending_launch.issued = 100;
        check(launch_request_valid(state.pending_launch, 100),
              "queued emulator menu is accepted by the existing launch handoff protocol");
    }
    check(emulators == system_count - 3, "all modeled emulator destinations were applied");

    const auto ps1 = find_system("ps1");
    ConsoleTileIntent ps1_emulator{ps1, ConsoleTileDestination::emulator, {}};
    std::snprintf(ps1_emulator.emulator_title, sizeof(ps1_emulator.emulator_title), "PPSA99248");
    library.route_count = 0;
    UiState missing;
    missing.library = &library;
    missing.launch_connected = true;
    check(apply_console_tile_intent(missing, ps1_emulator) == ConsoleTileApply::emulator_unavailable &&
              missing.page == Page::details && !missing.launch_requested &&
              std::strstr(missing.notice, "unavailable") != nullptr,
          "missing emulator route leaves the user in console details");
    library.route_count = 1;
    auto& route = library.routes[0];
    route = {};
    route.system = ps1;
    std::snprintf(route.title_id, sizeof(route.title_id), "PPSA97510");
    std::snprintf(route.folder, sizeof(route.folder), "/data/homebrew/PPSA97510");
    UiState changed;
    changed.library = &library;
    changed.launch_connected = true;
    check(apply_console_tile_intent(changed, ps1_emulator) == ConsoleTileApply::emulator_unavailable &&
              !changed.launch_requested,
          "a different installed emulator cannot replace the pinned menu title");
    std::snprintf(route.title_id, sizeof(route.title_id), "PPSA99248");
    std::snprintf(route.folder, sizeof(route.folder), "/tmp/PPSA99248");
    UiState wrong_folder;
    wrong_folder.library = &library;
    wrong_folder.launch_connected = true;
    check(apply_console_tile_intent(wrong_folder, ps1_emulator) == ConsoleTileApply::emulator_unavailable &&
              !wrong_folder.launch_requested,
          "an unrecognized route folder cannot launch the emulator");
    UiState no_service;
    no_service.library = &library;
    check(apply_console_tile_intent(no_service, ps1_emulator) == ConsoleTileApply::emulator_unavailable &&
              !no_service.launch_requested,
          "a disconnected launch service cannot start an emulator");

    static Library chooser_library;
    chooser_library.scan.complete = true;
    const auto ps2 = find_system("ps2");
    const ConsoleTileIntent ps2_source{ps2, ConsoleTileDestination::home, {}};
    UiState chooser;
    chooser.library = &chooser_library;
    chooser.launch_connected = true;
    chooser.settings.preferences.hidden[ps2] = true;
    std::snprintf(chooser.notice, sizeof(chooser.notice), "Earlier notice");
    check(begin_console_tile_route_choice(chooser, ps2_source) &&
              chooser.page == Page::shortcut_route && chooser.shortcut_route_system == ps2 &&
              chooser.shortcut_route_ready && !chooser.changed && !chooser.route_save_requested &&
              !chooser.tile_requested && !chooser.launch_requested && chooser.notice[0] == 0,
          "the existing PS2 Home source enters a three-choice screen without an old notice");
    input(chooser, Action::back);
    input(chooser, Action::settings);
    check(chooser.page == Page::shortcut_route && !chooser.changed && !chooser.launch_requested,
          "Back and Settings cannot bypass the launch choice into the fixed Home route");
    input(chooser, Action::confirm);
    check(chooser.page == Page::shortcut_route && chooser.selected_system == ps2 &&
              chooser.route_save_requested && chooser.route_save_busy &&
              chooser.route_save_apply_after &&
              chooser.pending_route_save.destination == ConsoleTileDestination::library &&
              !chooser.tile_requested && !chooser.launch_requested,
          "PS2 Library choice waits for a saved readback before opening");

    chooser = UiState{};
    chooser.library = &chooser_library;
    chooser.launch_connected = true;
    chooser.settings.preferences.hidden[ps2] = true;
    check(begin_console_tile_route_choice(chooser, ps2_source), "PS2 Home choice begins");
    input(chooser, Action::down);
    input(chooser, Action::down);
    input(chooser, Action::confirm);
    check(chooser.page == Page::shortcut_route && chooser.selected_system == ps2 &&
              chooser.settings.preferences.hidden[ps2] &&
              chooser.route_save_requested && chooser.route_save_apply_after &&
              chooser.pending_route_save.destination == ConsoleTileDestination::home &&
              !chooser.tile_requested && !chooser.launch_requested,
          "PS2 Home choice waits for durable save and preserves visibility");

    chooser = UiState{};
    chooser.library = &chooser_library;
    chooser.launch_connected = true;
    chooser_library.route_count = 0;
    check(begin_console_tile_route_choice(chooser, ps2_source), "PS2 emulator choice begins");
    input(chooser, Action::down);
    input(chooser, Action::confirm);
    check(chooser.page == Page::shortcut_route && !chooser.launch_requested &&
              std::strstr(chooser.notice, "No supported emulator") != nullptr,
          "missing emulator remains disabled on the route chooser");
    chooser_library.route_count = 1;
    auto& ps2_route = chooser_library.routes[0];
    ps2_route = {};
    ps2_route.system = ps2;
    std::snprintf(ps2_route.title_id, sizeof(ps2_route.title_id), "%s", "PPSA99203");
    std::snprintf(ps2_route.folder, sizeof(ps2_route.folder), "%s", "/data/homebrew/PPSA99203");
    UiState default_emulator_choice;
    default_emulator_choice.library = &chooser_library;
    default_emulator_choice.launch_connected = true;
    check(begin_console_tile_route_choice(default_emulator_choice, ps2_source) &&
              default_emulator_choice.shortcut_route_focus == 1,
          "an installed emulator is the safe default for an unresolved Home shortcut");
    input(chooser, Action::confirm);
    check(chooser.page == Page::shortcut_route && !chooser.launch_requested &&
              chooser.route_save_requested && chooser.route_save_apply_after &&
              chooser.pending_route_save.destination == ConsoleTileDestination::emulator &&
              std::strcmp(chooser.pending_route_save.emulator_title, "PPSA99203") == 0 &&
              !chooser.tile_requested,
          "installed PS2 emulator choice saves its pinned menu before launch");
    chooser_library.route_count = 0;
    chooser = UiState{};
    chooser.library = &chooser_library;
    chooser_library.scan.complete = false;
    std::snprintf(chooser.notice, sizeof(chooser.notice), "Earlier notice");
    check(begin_console_tile_route_choice(chooser, ps2_source) &&
              !chooser.shortcut_route_ready && chooser.shortcut_route_focus == 2 &&
              chooser.notice[0] == 0,
          "partial scan focuses Home without a duplicate global notice");
    ConsoleTileIntent incomplete_choice;
    check(!console_tile_route_choice_intent(chooser, 0, incomplete_choice) &&
              !console_tile_route_choice_intent(chooser, 1, incomplete_choice) &&
              console_tile_route_choice_intent(chooser, 2, incomplete_choice) &&
              incomplete_choice.system == ps2 &&
              incomplete_choice.destination == ConsoleTileDestination::home,
          "partial scan allows only the Home choice");
    input(chooser, Action::up);
    input(chooser, Action::confirm);
    check(chooser.page == Page::shortcut_route && !chooser.launch_requested &&
              std::strstr(chooser.notice, "Library scan incomplete") != nullptr,
          "partial scan keeps emulator disabled");
    input(chooser, Action::up);
    input(chooser, Action::confirm);
    check(chooser.page == Page::shortcut_route && !chooser.launch_requested &&
              !chooser.changed && !chooser.tile_requested,
          "partial scan keeps the Library choice disabled");
    input(chooser, Action::down);
    input(chooser, Action::down);
    input(chooser, Action::confirm);
    check(chooser.page == Page::shortcut_route && chooser.selected_system == ps2 &&
              chooser.route_save_requested && chooser.route_save_apply_after &&
              chooser.pending_route_save.destination == ConsoleTileDestination::home &&
              !chooser.launch_requested && !chooser.tile_requested,
          "partial scan can still save Home before opening with PS2 selected");
    UiState no_library_choice;
    check(begin_console_tile_route_choice(no_library_choice, ps2_source) &&
              no_library_choice.shortcut_route_focus == 2 &&
              console_tile_route_choice_intent(no_library_choice, 2, incomplete_choice),
          "missing library pointer still permits Home without a path or emulator guess");
    input(no_library_choice, Action::confirm);
    check(no_library_choice.page == Page::shortcut_route &&
              no_library_choice.route_save_requested &&
              no_library_choice.pending_route_save.destination == ConsoleTileDestination::home &&
              no_library_choice.selected_system == ps2,
          "Home choice can be saved without an accepted library pointer");
    chooser_library.scan.complete = true;
    for (std::size_t system = 0; system < system_count; ++system) {
        chooser = UiState{};
        chooser.library = &chooser_library;
        const ConsoleTileIntent source{system, ConsoleTileDestination::home, {}};
        check(begin_console_tile_route_choice(chooser, source) &&
                  chooser.shortcut_route_system == system,
              "every catalog console can present the same launch chooser");
        ConsoleTileIntent library_choice, home_choice;
        check(console_tile_route_choice_intent(chooser, 0, library_choice) &&
                  library_choice.system == system &&
                  library_choice.destination == ConsoleTileDestination::library,
              "all 52 consoles offer the isShelf Library destination");
        if (console_tile_isshelf_only(system))
            check(chooser.shortcut_route_focus == 0 &&
                      !console_tile_route_choice_intent(chooser, 1, home_choice) &&
                      !console_tile_route_choice_intent(chooser, 2, home_choice),
                  "PS5 and PS4 route choosers expose only isShelf Library");
        else
            check(console_tile_route_choice_intent(chooser, 2, home_choice) &&
                      home_choice.system == system &&
                      home_choice.destination == ConsoleTileDestination::home,
                  "non-native consoles retain their isShelf Home destination");
    }

    ConsoleTileIntent invalid;
    UiState untouched;
    untouched.page = Page::settings;
    check(apply_console_tile_intent(untouched, invalid) == ConsoleTileApply::invalid &&
              untouched.page == Page::settings && !untouched.launch_requested,
          "invalid shortcut data cannot change the UI");
    const char* marker_path = "console-tile-startup-marker-test.txt";
    std::remove(marker_path);
    const ConsoleTileIntent marker_intent{ps1, ConsoleTileDestination::home, {}};
    char marker[console_tile_message_capacity + 1]{};
    check(encode_console_tile_marker(marker_intent, marker, sizeof(marker)), "marker encodes");
    FILE* file = std::fopen(marker_path, "wb");
    check(file && std::fwrite(marker, 1, std::strlen(marker), file) == std::strlen(marker) &&
              std::fclose(file) == 0,
          "test marker writes completely");
    const ConsoleTileIntent conflicting{ps1, ConsoleTileDestination::library, {}};
    check(encode_console_tile_arg(conflicting, argument, sizeof(argument)), "conflicting argument encodes");
    char* argv[] = {argument};
    ConsoleTileIntent selected;
    check(select_console_tile_intent(1, argv, marker_path, selected) == ConsoleTileInput::invalid,
          "conflicting marker and forwarded argument stop before startup routing");
    check(std::remove(marker_path) == 0, "test marker is removed");
    static Library remembered_library;
    remembered_library.scan.complete = true;
    remembered_library.route_count = 0;
    Settings remembered_settings;
    auto& ps2_saved = remembered_settings.shortcut_routes[ps2];
    ps2_saved.input = tile_route::Input::valid;
    ps2_saved.preference = {ps2, ConsoleTileDestination::library, {}};
    char ps2_arg[] = "--isshelf-console-tile=ps2,home,-";
    char* ps2_argv[] = {ps2_arg};
    auto remembered = tile_route::resolve_remembered_launch(
        ConsoleTileInput::valid, false, ps2_source, 1, ps2_argv,
        remembered_settings, &remembered_library);
    check(remembered.state == tile_route::adoption::State::preferred &&
              remembered.intent.destination == ConsoleTileDestination::library,
          "an exact PS2 pilot source uses its saved Library choice");
    char ps2_fixed[] = "--isshelf-home-direct-v2=ps2";
    char* ps2_extra[] = {ps2_arg, ps2_fixed};
    remembered = tile_route::resolve_remembered_launch(
        ConsoleTileInput::valid, false, ps2_source, 2, ps2_extra,
        remembered_settings, &remembered_library);
    check(remembered.state == tile_route::adoption::State::invalid,
          "a pilot cannot smuggle a prepared fixed-Home token past source validation");
    remembered_library.scan.complete = false;
    remembered = tile_route::resolve_remembered_launch(
        ConsoleTileInput::valid, false, ps2_source, 1, ps2_argv,
        remembered_settings, &remembered_library);
    check(remembered.state == tile_route::adoption::State::preferred &&
              remembered.intent.destination == ConsoleTileDestination::library &&
              tile_route::can_select_surface_before_library(remembered),
          "a saved Library route selects its isShelf surface while the scan continues");
    ps2_saved.preference.destination = ConsoleTileDestination::home;
    remembered = tile_route::resolve_remembered_launch(
        ConsoleTileInput::valid, false, ps2_source, 1, ps2_argv,
        remembered_settings, &remembered_library);
    check(remembered.state == tile_route::adoption::State::preferred &&
              remembered.intent.destination == ConsoleTileDestination::home,
          "a saved Home route remains available during an incomplete scan");
    remembered_library.scan.complete = true;
    ps2_saved.preference.destination = ConsoleTileDestination::emulator;
    std::strcpy(ps2_saved.preference.emulator_title, "PPSA99203");
    remembered = tile_route::resolve_remembered_launch(
        ConsoleTileInput::valid, false, ps2_source, 1, ps2_argv,
        remembered_settings, &remembered_library);
    check(remembered.state == tile_route::adoption::State::stale_emulator,
          "an uninstalled saved emulator returns to the chooser");
    ps2_saved.input = tile_route::Input::invalid;
    remembered = tile_route::resolve_remembered_launch(
        ConsoleTileInput::valid, false, ps2_source, 1, ps2_argv,
        remembered_settings, &remembered_library);
    check(remembered.state == tile_route::adoption::State::choose_route,
          "a damaged saved route never silently chooses its original destination");
    const auto n64 = find_system("n64");
    const ConsoleTileIntent n64_source{n64, ConsoleTileDestination::library, {}};
    char n64_arg[] = "--isshelf-console-tile=n64,library,-";
    char* n64_argv[] = {n64_arg};
    auto& n64_saved = remembered_settings.shortcut_routes[n64];
    n64_saved.input = tile_route::Input::valid;
    n64_saved.preference = {n64, ConsoleTileDestination::home, {}};
    remembered = tile_route::resolve_remembered_launch(
        ConsoleTileInput::valid, false, n64_source, 1, n64_argv,
        remembered_settings, &remembered_library);
    check(remembered.state == tile_route::adoption::State::preferred &&
              remembered.intent.system == n64 &&
              remembered.intent.destination == ConsoleTileDestination::home,
          "the older N64 Library pilot can use a saved Home choice");
    const auto psp = find_system("psp");
    const ConsoleTileIntent psp_source{psp, ConsoleTileDestination::library, {}};
    char psp_arg[] = "--isshelf-console-tile=psp,library,-";
    char* psp_argv[] = {psp_arg};
    auto& psp_saved = remembered_settings.shortcut_routes[psp];
    psp_saved.input = tile_route::Input::valid;
    psp_saved.preference = {psp, ConsoleTileDestination::home, {}};
    remembered = tile_route::resolve_remembered_launch(
        ConsoleTileInput::valid, false, psp_source, 1, psp_argv,
        remembered_settings, &remembered_library);
    check(remembered.state == tile_route::adoption::State::preferred &&
              remembered.intent.destination == ConsoleTileDestination::home,
          "a prepared Library forwarder can use a later saved Home choice");
    psp_saved.input = tile_route::Input::absent;
    remembered = tile_route::resolve_remembered_launch(
        ConsoleTileInput::valid, false, psp_source, 1, psp_argv,
        remembered_settings, &remembered_library);
    check(remembered.state == tile_route::adoption::State::original &&
              remembered.intent.destination == ConsoleTileDestination::library,
          "a new prepared tile uses its creation-time Library route before any later edit");
    remembered_library.scan.complete = false;
    remembered = tile_route::resolve_remembered_launch(
        ConsoleTileInput::valid, false, psp_source, 1, psp_argv,
        remembered_settings, &remembered_library);
    check(remembered.state == tile_route::adoption::State::original &&
              remembered.intent.destination == ConsoleTileDestination::library &&
              tile_route::can_select_surface_before_library(remembered),
          "a creation-time Library route selects its isShelf surface while the scan continues");
    remembered_library.scan.complete = true;
    const ConsoleTileIntent psp_emulator{psp, ConsoleTileDestination::emulator,
                                          "PPSA99169"};
    char psp_emulator_arg[] = "--isshelf-console-tile=psp,emulator,PPSA99169";
    char* psp_emulator_argv[] = {psp_emulator_arg};
    remembered = tile_route::resolve_remembered_launch(
        ConsoleTileInput::valid, false, psp_emulator, 1,
        psp_emulator_argv, remembered_settings, &remembered_library);
    check(remembered.state == tile_route::adoption::State::original &&
              remembered.intent.destination == ConsoleTileDestination::library,
          "a legacy RetroArch creation route normalizes without requiring its generic menu");
    remembered_library.route_count = 1;
    auto& psp_menu = remembered_library.routes[0];
    psp_menu = {};
    psp_menu.system = psp;
    std::snprintf(psp_menu.title_id, sizeof(psp_menu.title_id), "PPSA99169");
    std::snprintf(psp_menu.folder, sizeof(psp_menu.folder),
                  "/data/homebrew/PPSA99169");
    remembered = tile_route::resolve_remembered_launch(
        ConsoleTileInput::valid, false, psp_emulator, 1,
        psp_emulator_argv, remembered_settings, &remembered_library);
    check(remembered.state == tile_route::adoption::State::original &&
              remembered.intent.destination == ConsoleTileDestination::library,
          "an installed legacy RetroArch route still opens the matching isShelf library");
    check(!tile_route::can_handoff_before_reveal(remembered),
          "a legacy RetroArch menu route cannot bypass isShelf's safe fallback");
    for (const auto destination : {ConsoleTileDestination::library,
                                   ConsoleTileDestination::home}) {
        tile_route::adoption::Decision screen;
        screen.state = tile_route::adoption::State::preferred;
        screen.intent = {psp, destination, {}};
        check(tile_route::can_select_surface_before_library(screen),
              "isShelf-rendered destinations can select their surface before the scan completes");
        check(!tile_route::can_handoff_before_reveal(screen),
              "destinations rendered by isShelf wait for the complete opening reveal");
    }
    tile_route::adoption::Decision chooser_decision;
    chooser_decision.state = tile_route::adoption::State::choose_route;
    chooser_decision.intent = psp_emulator;
    check(!tile_route::can_handoff_before_reveal(chooser_decision),
          "a route chooser never bypasses the complete opening reveal");
    check(!tile_route::can_select_surface_before_library(chooser_decision),
          "a route chooser waits for the completed library inventory");
    std::puts("PASS: every console shortcut startup destination, safe emulator fallback, and conflict rejection.");
}
