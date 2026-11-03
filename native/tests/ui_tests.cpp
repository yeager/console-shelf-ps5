// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/carousel.hpp"
#include "shelf/ui.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
namespace {
void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL UI: %s\n", message);
        std::exit(1);
    }
}
} // namespace
void test_ui() {
    using namespace shelf;
    UiState state;
    input(state, Action::right);
    check(state.selected_system == find_system("ps4"), "D-pad moves through console identities");
    check(triangle_action(Page::home) == Action::search &&
              triangle_action(Page::keyboard) == Action::favorite &&
              triangle_action(Page::library) == Action::favorite,
          "Triangle searches from Home and keeps keyboard clear and game favourites elsewhere");
    UiState triangle_home;
    input(triangle_home, triangle_action(triangle_home.page));
    check(triangle_home.page == Page::keyboard && triangle_home.keyboard_return == Page::home &&
              !triangle_home.keyboard_for_consoles && !triangle_home.keyboard_for_tv,
          "Home Triangle opens the main search keyboard immediately");
    input(triangle_home, Action::back);
    check(triangle_home.page == Page::home, "closing the search keyboard returns to Home");
    UiState spatial;
    for (int i = 0; i < 4; ++i)
        input(spatial, Action::right);
    check(spatial.focus == 3, "grid right stops at its row edge instead of jumping down a row");
    input(spatial, Action::up);
    check(spatial.tools && spatial.tool == 1, "up from the last column reaches the layout action");
    input(spatial, Action::confirm);
    check(spatial.page == Page::home && spatial.settings.preferences.layout == Layout::swipe,
          "layout button switches view directly without opening settings");
    input(spatial, Action::up);
    check(spatial.tools && spatial.tool == 2, "Up reaches the settings button above the toolbar");
    input(spatial, Action::right);
    check(spatial.tool == 2, "horizontal input does not move vertically from the settings row");
    input(spatial, Action::down);
    check(spatial.tools && spatial.tool == 1, "Down from settings returns to the layout button below it");
    input(spatial, Action::left);
    check(spatial.tool == 0, "search and layout remain neighbours on the same row");
    input(spatial, Action::up);
    input(spatial, Action::confirm);
    check(spatial.page == Page::settings, "settings is opened through its spatial Up route");
    input(state, Action::confirm);
    check(state.page == Page::details, "Cross opens a console");
    UiState detail_nav;
    detail_nav.page = Page::details;
    detail_nav.detail_focus = 1;
    input(detail_nav, Action::down);
    check(detail_nav.detail_focus == 3,
          "Down from Open emulator library reaches Home Screen shortcut management below it");
    input(detail_nav, Action::up);
    check(detail_nav.detail_focus == 1, "Up from the wide shortcut action returns to the left launch action");
    input(detail_nav, Action::right);
    check(detail_nav.detail_focus == 2,
          "Right from Open emulator library reaches Show game library on the same row");
    input(detail_nav, Action::down);
    check(detail_nav.detail_focus == 3,
          "Down from Show game library also reaches the shortcut action below it");
    input(detail_nav, Action::up);
    input(detail_nav, Action::right);
    input(detail_nav, Action::left);
    check(detail_nav.detail_focus == 1, "Left returns to Open emulator library on the same row");
    input(detail_nav, Action::up);
    check(detail_nav.detail_focus == 0, "Up from the launch row reaches the preferred emulator row");
    input(detail_nav, Action::down);
    check(detail_nav.detail_focus == 1, "Down from the preferred emulator row reaches the left launch action");
    input(state, Action::confirm);
    check(std::strstr(state.notice, "No compatible emulator") != nullptr, "absent route is explained");
    input(state, Action::right);
    input(state, Action::confirm);
    check(state.page == Page::library, "library is a separate action");
    input(state, Action::back);
    input(state, Action::back);
    check(state.page == Page::home, "Circle returns through the page stack");
    UiState tile;
    tile.page = Page::details;
    tile.detail_focus = 3;
    tile.selected_system = find_system("ps1");
    tile.tile_connected = true;
    input(tile, Action::confirm);
    check(tile.page == Page::console_tile, "console details open the home-screen destination picker");
    input(tile, Action::confirm);
    check(tile.tile_requested && tile.pending_tile.action == ConsoleTileAction::upsert &&
              tile.pending_tile.intent.system == find_system("ps1") &&
              tile.pending_tile.intent.destination == ConsoleTileDestination::library,
          "first shortcut destination opens the selected console library");
    tile.tile_requested = false;
    tile.tile_focus = 2;
    input(tile, Action::confirm);
    check(tile.tile_requested && tile.pending_tile.intent.destination == ConsoleTileDestination::home,
          "third shortcut destination opens isShelf with the console selected");
    tile.tile_requested = false;
    tile.tile_focus = 1;
    input(tile, Action::confirm);
    check(!tile.tile_requested && std::strstr(tile.notice, "No supported emulator") != nullptr,
          "emulator shortcut refuses a missing route");
    UiState native_tile;
    native_tile.page = Page::details;
    native_tile.detail_focus = 3;
    native_tile.selected_system = find_system("ps5");
    native_tile.tile_connected = true;
    input(native_tile, Action::confirm);
    check(native_tile.page == Page::console_tile && native_tile.tile_focus == 0,
          "the PS5 shortcut picker starts on its only isShelf destination");
    native_tile.tile_focus = 1;
    input(native_tile, Action::confirm);
    check(!native_tile.tile_requested && native_tile.tile_focus == 0 &&
              std::strstr(native_tile.notice, "always open in isShelf") != nullptr,
          "PS5 cannot be assigned an emulator destination");
    native_tile.tile_focus = 2;
    input(native_tile, Action::confirm);
    check(!native_tile.tile_requested && native_tile.tile_focus == 0,
          "PS5 cannot expose a second shortcut destination");
    input(native_tile, Action::confirm);
    check(native_tile.tile_requested &&
              native_tile.pending_tile.intent.destination == ConsoleTileDestination::library,
          "PS5 shortcut creation routes only into isShelf");
    native_tile = {};
    native_tile.page = Page::details;
    native_tile.detail_focus = 3;
    native_tile.selected_system = find_system("ps4");
    native_tile.tile_connected = true;
    input(native_tile, Action::confirm);
    input(native_tile, Action::down);
    check(native_tile.tile_focus == 3,
          "PS4 navigation skips hidden emulator and alternate destination rows");
    static Library tile_library;
    tile_library.route_count = 1;
    tile_library.routes[0].system = find_system("ps1");
    tile_library.scan.complete = true;
    std::snprintf(tile_library.routes[0].id, sizeof(tile_library.routes[0].id), "psswanstation");
    std::snprintf(tile_library.routes[0].title_id, sizeof(tile_library.routes[0].title_id), "PPSA99248");
    std::snprintf(tile_library.routes[0].folder, sizeof(tile_library.routes[0].folder), "/data/homebrew/PPSA99248");
    tile.library = &tile_library;
    tile.launch_connected = true;
    UiState default_emulator;
    default_emulator.page = Page::details;
    default_emulator.detail_focus = 3;
    default_emulator.selected_system = find_system("ps1");
    default_emulator.library = &tile_library;
    default_emulator.launch_connected = true;
    input(default_emulator, Action::confirm);
    check(default_emulator.page == Page::console_tile &&
              default_emulator.tile_focus == 1,
          "a new shortcut picker defaults to the verified installed emulator menu");
    input(tile, Action::confirm);
    check(tile.tile_requested && tile.pending_tile.intent.destination == ConsoleTileDestination::emulator &&
              std::strcmp(tile.pending_tile.intent.emulator_title, "PPSA99248") == 0,
          "emulator shortcut pins its verified installed menu route");
    tile.tile_requested = false;
    tile.tile_installed[tile.selected_system] = true;
    tile.tile_focus = 3;
    input(tile, Action::confirm);
    check(tile.tile_requested && tile.pending_tile.action == ConsoleTileAction::remove,
          "installed console shortcut can be removed");
    tile.tile_requested = false;
    tile.tile_focus = 4;
    input(tile, Action::confirm);
    check(tile.tile_requested &&
              tile.pending_tile.action == ConsoleTileAction::upsert_emulators,
          "installed shortcut page exposes one explicit add-or-repair emulator action");
    tile.tile_requested = false;
    tile.tile_focus = 5;
    input(tile, Action::confirm);
    check(tile.tile_requested &&
              tile.pending_tile.action == ConsoleTileAction::refresh_installed,
          "installed shortcut page exposes one explicit update-all action");
    tile.tile_requested = false;
    tile.tile_focus = 6;
    input(tile, Action::confirm);
    check(tile.tile_requested &&
              tile.pending_tile.action == ConsoleTileAction::remove_installed,
          "installed shortcut page exposes one explicit remove-all action");
    tile.tile_requested = false;
    tile.tile_focus = 5;
    input(tile, Action::confirm);
    check(tile.tile_requested &&
              tile.pending_tile.action == ConsoleTileAction::refresh_installed,
          "remove-all does not replace the separate update-all action");
    tile.tile_requested = false;
    tile.tile_emulator_batch_close_ready = true;
    tile.tile_connected = false;
    input(tile, Action::confirm);
    check(tile.exit_requested &&
              std::strstr(tile.notice, "ShadowMount") != nullptr,
          "prepared emulator batch closes cooperatively even if the shortcut service disconnects");
    tile.exit_requested = false;
    tile.tile_emulator_batch_close_ready = false;
    tile.tile_connected = true;
    tile.tile_busy = true;
    tile.tile_batch_busy = true;
    input(tile, Action::confirm);
    check(!tile.tile_requested && std::strstr(tile.notice, "Updating all") != nullptr,
          "update-all cannot be started twice while its worker is running");
    tile.tile_busy = false;
    tile.tile_batch_busy = false;
    tile.tile_connected = false;
    tile.tile_focus = 0;
    input(tile, Action::confirm);
    check(!tile.tile_requested && std::strstr(tile.notice, "unavailable") != nullptr,
          "picker does not claim a shortcut was installed without an installer");
    tile.tile_connected = true;
    tile.tile_removing[tile.selected_system] = true;
    tile.tile_focus = 3;
    input(tile, Action::down);
    check(tile.tile_focus == 2,
          "pending removal exposes all recovery actions beside the owner's finish action");
    input(tile, Action::confirm);
    check(tile.tile_requested &&
              tile.pending_tile.action == ConsoleTileAction::upsert_emulators,
          "repair-all remains reachable when this console has a confirmed pending removal");
    tile.tile_requested = false;
    tile.tile_focus = 1;
    input(tile, Action::confirm);
    check(tile.tile_requested &&
              tile.pending_tile.action == ConsoleTileAction::remove_installed,
          "pending removal can rebuild one batch from every durable recovery copy");
    tile.tile_requested = false;
    tile.tile_focus = 0;
    input(tile, Action::confirm);
    check(tile.tile_requested && tile.pending_tile.action == ConsoleTileAction::confirm_removed &&
              tile.pending_tile.intent.system == tile.selected_system,
          "owner can request confirmation only for the selected pending shortcut");
    tile.tile_requested = false;
    tile.tile_busy = true;
    input(tile, Action::confirm);
    check(!tile.tile_requested && std::strstr(tile.notice, "registration") != nullptr,
          "pending removal reports its system-registration check while the worker is busy");
    tile.tile_busy = false;
    tile.tile_connected = false;
    input(tile, Action::confirm);
    check(!tile.tile_requested && std::strstr(tile.notice, "unavailable") != nullptr,
          "pending removal cannot be confirmed without the shortcut service");
    tile.tile_connected = true;
    tile.tile_removal_handoff[tile.selected_system] = true;
    check(queue_console_tile_removal(tile, "PPSA99704"),
          "pending removal test can queue its exact owned title");
    input(tile, Action::confirm);
    check(tile.launch_requested && !tile.tile_requested &&
              tile.pending_launch.mode == LaunchMode::tile_remove &&
              std::strstr(tile.notice, "ShadowMount") != nullptr,
          "queued removal closes once through the explicit ShadowMount handoff action");
    tile.launch_requested = false;
    tile.tile_removal_handoff[tile.selected_system] = false;
    tile.tile_removal_count = 0;
    tile.tile_removal_titles[0] = 0;
    tile.tile_removing[tile.selected_system] = false;
    tile.tile_removal_batch_ready = true;
    check(queue_console_tile_removal(tile, "PPSA99700"),
          "bulk removal test can retain its bounded title list");
    input(tile, Action::confirm);
    check(tile.launch_requested && !tile.tile_requested &&
              tile.pending_launch.mode == LaunchMode::tile_remove,
          "remove-all exposes its close-and-apply action even from a console with no tile");
    tile.launch_requested = false;
    tile.tile_removal_batch_ready = false;
    tile.tile_removal_count = 0;
    tile.tile_removal_titles[0] = 0;
    tile.tile_installed.fill(true);
    tile.tile_removing.fill(true);
    tile.tile_removal_handoff.fill(true);
    tile.tile_removal_batch_ready = true;
    tile.tile_residual_sweep = true;
    tile.tile_removal_count = 2;
    std::snprintf(tile.tile_removal_titles,
                  sizeof(tile.tile_removal_titles),
                  "PPSA99700,PPSA99701");
    tile.tile_focus = 2;
    clear_console_tile_removal_state(tile);
    check(std::none_of(tile.tile_installed.begin(), tile.tile_installed.end(),
                       [](bool value) { return value; }) &&
              std::none_of(tile.tile_removing.begin(), tile.tile_removing.end(),
                           [](bool value) { return value; }) &&
              std::none_of(tile.tile_removal_handoff.begin(),
                           tile.tile_removal_handoff.end(),
                           [](bool value) { return value; }) &&
              !tile.tile_removal_batch_ready && !tile.tile_residual_sweep &&
              tile.tile_removal_count == 0 && !tile.tile_removal_titles[0] &&
              tile.tile_focus == 0,
          "completed remove-all clears stale installed and recovery UI state");
    check(system_count == 52, "the shortcut picker covers the pinned 52-console catalog");
    tile.tile_installed.fill(false);
    for (std::size_t system = 0; system < system_count; ++system) {
        tile.page = Page::details;
        tile.detail_focus = 3;
        tile.selected_system = system;
        input(tile, Action::confirm);
        check(tile.page == Page::console_tile && tile.selected_system == system,
              "every catalog console has a shortcut destination picker");
        const std::array choices = {
            std::pair<int, ConsoleTileDestination>{0, ConsoleTileDestination::library},
            std::pair<int, ConsoleTileDestination>{2, ConsoleTileDestination::home}};
        const std::size_t choice_count = console_tile_isshelf_only(system) ? 1 : choices.size();
        for (std::size_t choice_index = 0; choice_index < choice_count; ++choice_index) {
            const auto choice = choices[choice_index];
            tile.tile_focus = choice.first;
            tile.tile_requested = false;
            input(tile, Action::confirm);
            check(tile.tile_requested && tile.pending_tile.action == ConsoleTileAction::upsert &&
                      tile.pending_tile.intent.system == system &&
                      tile.pending_tile.intent.destination == choice.second &&
                      !tile.pending_tile.intent.emulator_title[0],
                  "every console admits its visible isShelf shortcut choices");
        }
        tile.tile_focus = 3;
        tile.tile_requested = false;
        input(tile, Action::confirm);
        check(tile.tile_requested &&
                  tile.pending_tile.action == ConsoleTileAction::upsert_emulators,
              "every console page can add or repair all verified emulator tiles");
        tile.tile_requested = false;
        tile.tile_focus = 4;
        input(tile, Action::confirm);
        check(tile.tile_requested &&
                  tile.pending_tile.action == ConsoleTileAction::refresh_installed,
              "every console page can safely update only already-installed shortcuts");
        tile.tile_requested = false;
        tile.tile_focus = 5;
        input(tile, Action::confirm);
        check(tile.tile_requested &&
                  tile.pending_tile.action == ConsoleTileAction::remove_installed,
              "every console page exposes the global remove-all action");
        tile.tile_installed[system] = true;
        tile.tile_focus = 3;
        tile.tile_requested = false;
        input(tile, Action::confirm);
        check(tile.tile_requested && tile.pending_tile.action == ConsoleTileAction::remove &&
                  tile.pending_tile.intent.system == system,
              "every installed console shortcut exposes its in-app removal action");
        tile.tile_requested = false;
        tile.tile_removing[system] = true;
        tile.tile_focus = 3;
        input(tile, Action::down);
        check(tile.tile_focus == 2,
              "every pending console removal offers both global recovery actions");
        input(tile, Action::confirm);
        check(tile.tile_requested &&
                  tile.pending_tile.action == ConsoleTileAction::upsert_emulators,
              "every pending console can recover all verified emulator tiles");
        tile.tile_requested = false;
        tile.tile_focus = 1;
        input(tile, Action::confirm);
        check(tile.tile_requested &&
                  tile.pending_tile.action == ConsoleTileAction::remove_installed,
              "every pending console can finish all durable queued removals in one batch");
        tile.tile_requested = false;
        tile.tile_focus = 0;
        input(tile, Action::confirm);
        check(tile.tile_requested &&
                  tile.pending_tile.action == ConsoleTileAction::confirm_removed &&
                  tile.pending_tile.intent.system == system,
              "every console shortcut can finish removal in-app");
        tile.tile_requested = false;
        tile.tile_removing[system] = false;
        tile.tile_installed[system] = false;
    }
    static Library available_route;
    tile.library = &available_route;
    tile.launch_connected = true;
    std::size_t emulator_cases = 0;
    std::size_t retroarch_library_cases = 0;
    for (std::size_t system = 0; system < system_count; ++system) {
        const auto id = catalog[system].id;
        const char* title = nullptr;
        if (id == "ps3") title = "PPSA99303";
        else if (id == "ps2") title = "PPSA99203";
        else if (id == "ps1") title = "PPSA99248";
        else if (id == "switch") title = "PPSA99008";
        else if (id == "wiiu") title = "PPSA99360";
        else if (!catalog[system].suggested_core.empty()) title = "PPSA99169";
        available_route.route_count = title ? 1 : 0;
        if (title) {
            auto& route = available_route.routes[0];
            route = {};
            route.system = system;
            std::snprintf(route.id, sizeof(route.id), "available-route");
            std::snprintf(route.title_id, sizeof(route.title_id), "%s", title);
            std::snprintf(route.folder, sizeof(route.folder), "/data/homebrew/%s", title);
        }
        tile.page = Page::console_tile;
        tile.selected_system = system;
        tile.tile_focus = 1;
        tile.tile_requested = false;
        input(tile, Action::confirm);
        if (title && std::strcmp(title, console_tile_shared_retroarch_title) != 0) {
            ++emulator_cases;
            check(tile.tile_requested && tile.pending_tile.action == ConsoleTileAction::upsert &&
                      tile.pending_tile.intent.system == system &&
                      tile.pending_tile.intent.destination == ConsoleTileDestination::emulator &&
                      std::strcmp(tile.pending_tile.intent.emulator_title, title) == 0 &&
                      console_tile_creation_intent_valid(tile.pending_tile.intent),
                  "each standalone emulator console pins an available, system-selecting menu route");
        } else if (title) {
            ++retroarch_library_cases;
            check(!tile.tile_requested &&
                      std::strstr(tile.notice, "RetroArch needs a game") != nullptr,
                  "a shared RetroArch console directs the user to its isShelf game library");
        } else if (console_tile_isshelf_only(system))
            check(!tile.tile_requested && tile.tile_focus == 0 &&
                      std::strstr(tile.notice, "always open in isShelf") != nullptr,
                  "native PS5 and PS4 cannot expose an emulator shortcut");
        else
            check(!tile.tile_requested && std::strstr(tile.notice, "No supported emulator") != nullptr,
                  "another console without a supported emulator route cannot create an emulator shortcut");
    }
    check(emulator_cases == 5 && retroarch_library_cases == 44,
          "five standalone emulators launch directly and 44 shared RetroArch systems use isShelf libraries");
    UiState shortcut_home;
    shortcut_home.settings.preferences.hidden[find_system("ps1")] = true;
    shortcut_home.tile_home_override = find_system("ps1");
    const auto shortcut_visible = shown_systems(shortcut_home);
    check(shortcut_visible.count && shortcut_visible.indices[0] == find_system("ps1") &&
              shortcut_home.settings.preferences.hidden[find_system("ps1")],
          "home shortcut shows its console for this session without changing saved visibility");
    input(state, Action::settings);
    input(state, Action::confirm);
    check(state.settings.preferences.layout == Layout::swipe && state.changed,
          "native layout control changes saved preferences");
    input(state, Action::next_tab);
    input(state, Action::next_tab);
    check(state.tab == SettingsTab::consoles, "shoulders change settings tabs");
    input(state, Action::down);
    input(state, Action::confirm);
    check(state.page == Page::keyboard, "controller search opens on-screen keyboard");
    text_input(state, "Nintendo");
    input(state, Action::settings);
    check(state.page == Page::settings, "Options completes search");
    check(console_picker(state).count == 17, "picker contains Nintendo header and sixteen consoles");
    input(state, Action::down);
    input(state, Action::down);
    input(state, Action::confirm);
    check(state.settings.preferences.hidden[find_system("n64")] &&
              !state.settings.preferences.hidden[find_system("ps2")],
          "Hide results only changes matched consoles");
    input(state, Action::up);
    input(state, Action::confirm);
    check(!state.settings.preferences.hidden[find_system("n64")], "Show results restores matched consoles");
    input(state, Action::back);
    input(state, Action::search);
    text_input(state, "PLAYSTATION");
    input(state, Action::back);
    check(shown_systems(state).count == 6, "main search is independent of console-picker search");
    input(state, Action::settings);
    input(state, Action::back);
    check(state.page == Page::home, "settings returns to the prior screen");
    input(state, Action::back);
    check(state.page == Page::home && state.close_hint_requested && !state.exit_requested,
          "a home Circle tap requests a hint and keeps browsing");
    input(state, Action::confirm);
    check(!state.exit_requested && !state.close_hint_requested,
          "Cross after the hint remains Select rather than confirming an exit");
    UiState detail_state;
    input(detail_state, Action::right);
    input(detail_state, Action::confirm);
    input(detail_state, Action::settings);
    input(detail_state, Action::confirm);
    input(detail_state, Action::back);
    check(detail_state.page == Page::details && detail_state.selected_system == find_system("ps4"),
          "global settings retain the console being viewed");
    UiState carousel;
    carousel.settings.preferences.layout = Layout::swipe;
    input(carousel, Action::left);
    check(carousel.focus == static_cast<int>(system_count) - 1, "Swipe loops left from the first console");
    input(carousel, Action::right);
    check(carousel.focus == 0, "Swipe loops right from the last console");
    input(carousel, Action::up);
    check(carousel.tools, "up from any Swipe position reaches the toolbar");
    UiState display;
    input(display, Action::layout);
    input(display, Action::layout);
    check(display.settings.preferences.layout == Layout::shelf, "Square includes the new Shelf layout");
    input(display, Action::down);
    check(display.focus == 4, "Shelf Down moves to the same column on the next tier");
    input(display, Action::up);
    check(display.focus == 0, "Shelf Up returns to the previous tier");
    input(display, Action::left);
    check(display.focus == 0, "Shelf does not wrap left onto another tier");
    input(display, Action::next_tab);
    check(display.focus == 8, "only a shoulder page action selects the next Shelf page");
    input(display, Action::down); input(display, Action::down);
    check(display.focus == 12 && !display.tools, "Down stops on the bottom tier of the current page");
    input(display, Action::up); input(display, Action::up);
    check(display.focus == 8 && display.tools, "Up from a top-tier console reaches the toolbar on page two");
    input(display, Action::down);
    check(!display.tools && display.focus == 8, "Down from the toolbar returns to the same page");
    Preferences noise;
    Availability missing;
    noise.tv_noise = TvNoise::emulator;
    check(tv_noise_for(noise, missing), "missing emulator can show TV static");
    missing.built_in = true;
    check(!tv_noise_for(noise, missing), "built-in PS5 systems are not mistaken for missing emulators");
    noise.tv_noise = TvNoise::games;
    check(tv_noise_for(noise, missing), "missing games can independently show static");
    noise.tv_mode = TvMode::off;
    check(!tv_noise_for(noise, missing), "screen off overrides the optional static policy");
    input(display, Action::layout);
    check(display.settings.preferences.layout == Layout::grid, "the three-layout cycle returns to Grid");
    input(display, Action::settings);
    input(display, Action::left);
    check(display.settings.preferences.layout == Layout::shelf, "settings can cycle layouts in reverse");
    input(display, Action::down);
    input(display, Action::down);
    input(display, Action::down);
    input(display, Action::confirm);
    check(!display.settings.preferences.monochrome_missing && display.settings_focus == 3,
          "the fourth Appearance setting allows full-colour missing emulator icons");
    const auto center = orbit_pose(0, 1760), near = orbit_pose(1, 1760), far = orbit_pose(2, 1760);
    check(center.scale > near.scale && near.scale > far.scale && center.z > near.z && near.z > far.z,
          "carousel selection dominates a progressively receding collection");
    check(center.x == 0 && near.x > 0 && orbit_pose(-1, 1760).x == -near.x,
          "carousel has a centred and symmetrical arc");
    OrbitMotion motion;
    motion.select(static_cast<double>(system_count) - 1, system_count, 0, false);
    motion.update(100);
    check(motion.active && motion.position < 0 && motion.position > -1, "wrap animates by one slot");
    const double interrupted = motion.position;
    motion.select(1, system_count, 100, false);
    check(motion.position == interrupted, "reversing input does not jump the current frame");
    motion.update(359);
    check(motion.active, "short carousel motion still presents an intermediate frame");
    motion.update(360);
    check(!motion.active && std::abs(motion.position - 1) < .0001, "animation settles at its most recent target");
    motion.select(0, system_count, 600, true);
    check(!motion.active && std::abs(motion.position) < .0001, "reduced motion settles immediately");
    UiState paged;
    paged.settings.preferences.layout = Layout::shelf;
    paged.focus = 3;
    input(paged, Action::next_tab);
    check(paged.focus == 11, "R1 advances a Shelf page while retaining the column and row");
    input(paged, Action::previous_tab);
    check(paged.focus == 3, "L1 returns to the same position on the previous page");
    for (int i = 0; i < 20; ++i) input(paged, Action::next_tab);
    check(paged.focus == 51, "repeated shoulder presses stop safely at the final partial page");
    input(paged, Action::previous_tab);
    check(paged.focus == 43, "paging back from a partial page retains a valid slot");
    paged.settings.preferences.scope = Scope::favorites;
    paged.settings.preferences.favorite[find_system("ps2")] = true;
    check(shown_systems(paged).count == 1, "favourites-only view is a filter, not just a sort");
    paged = {};
    paged.settings.preferences.layout = Layout::shelf;
    paged.settings.preferences.shelf_single = true;
    input(paged, Action::consoles);
    paged.settings_focus = 2;
    input(paged, Action::confirm);
    check(std::count(paged.settings.preferences.shelf_selected.begin(), paged.settings.preferences.shelf_selected.end(), true) == 8,
          "Fill spaces never adds a ninth console");
    check(shown_systems(paged).count == 8, "a curated Shelf shows only its selected consoles");
    paged.settings.preferences.layout = Layout::grid;
    check(shown_systems(paged).count == system_count, "curated selection does not hide the full Grid catalogue");
    std::puts("PASS: native controller navigation, settings tabs, independent searches, group visibility, honest "
              "unavailable routes and page return.");
}
