// SPDX-License-Identifier: GPL-3.0-only
// Compile the actual UI model with the same quarantine gate as SHELF_PS5:
// host library allocation remains linkable while the picker may save an
// untrusted UI route; no Add/Remove reaches a worker.
#include "shelf/ui.hpp"
#include "shelf/model.hpp"
#include "shelf/console_tile_manager.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
void check(bool okay, const char* message) {
    if (!okay) { std::fprintf(stderr, "FAIL PS5 shortcut quarantine: %s\n", message); std::exit(1); }
}
}

int main() {
    using namespace shelf;
    static_assert(!console_tile_writes_enabled, "PS5 shortcut writes must be explicitly qualified first");
    UiState state;
    for (std::size_t system = 0; system < system_count; ++system) {
        state.page = Page::console_tile;
        state.selected_system = system;
        // Route saving is independent of tile installation service state.
        for (bool connected : {false, true}) {
            state.tile_connected = connected;
            for (int destination = 0; destination < 3; ++destination) {
                state.tile_focus = destination;
                input(state, Action::confirm);
                if (console_tile_isshelf_only(system) && destination != 0) {
                    check(!state.route_save_requested && !state.tile_requested &&
                              !state.launch_requested && state.tile_focus == 0 &&
                              std::strstr(state.notice, "always open in isShelf") != nullptr,
                          "native PS5 and PS4 choices stay in isShelf");
                } else if (destination == 1) {
                    check(!state.route_save_requested && !state.tile_requested &&
                              !state.launch_requested &&
                              std::strstr(state.notice, "No supported emulator") != nullptr,
                          "unavailable emulator is not saved");
                } else {
                    check(!state.tile_requested && state.route_save_requested &&
                              state.route_save_busy && !state.route_save_apply_after &&
                              !state.launch_requested && state.page == Page::console_tile &&
                              state.route_save_system == system &&
                              state.route_save_status == RouteSaveStatus::pending &&
                              state.pending_route_save.destination ==
                                  (destination == 0 ? ConsoleTileDestination::library
                                                    : ConsoleTileDestination::home),
                          "all 52 Library and Home choices queue only settings saves");
                    state.route_save_requested = false;
                    state.route_save_busy = false;
                }
            }
        }
        state.tile_installed[system] = true;
        state.tile_removing[system] = true;
        state.tile_focus = 3;
        input(state, Action::down);
        check(state.tile_focus == (console_tile_isshelf_only(system) ? 0 : 2),
              "closed write gate exposes only valid destinations");
        input(state, Action::confirm);
        check(!state.tile_requested && state.route_save_requested &&
                  state.pending_route_save.destination ==
                      (console_tile_isshelf_only(system) ? ConsoleTileDestination::library
                                                        : ConsoleTileDestination::home),
              "installed or removing status still saves only an allowed isShelf choice");
        state.route_save_requested = false;
        state.route_save_busy = false;
        state.tile_removing[system] = false;
    }
    static Library ps1_library;
    state.selected_system = find_system("ps1");
    ps1_library.route_count = 1;
    ps1_library.routes[0].system = state.selected_system;
    std::snprintf(ps1_library.routes[0].id, sizeof(ps1_library.routes[0].id), "psswanstation");
    std::snprintf(ps1_library.routes[0].title_id, sizeof(ps1_library.routes[0].title_id), "PPSA99248");
    std::snprintf(ps1_library.routes[0].folder, sizeof(ps1_library.routes[0].folder),
                  "/data/homebrew/PPSA99248");
    state.library = &ps1_library;
    state.launch_connected = true;
    state.tile_focus = 1;
    input(state, Action::confirm);
    check(state.route_save_requested && !state.tile_requested && !state.launch_requested &&
              state.route_save_status == RouteSaveStatus::pending &&
              state.pending_route_save.destination == ConsoleTileDestination::emulator &&
              std::strcmp(state.pending_route_save.emulator_title, "PPSA99248") == 0,
          "installed emulator menu is pinned as a saved default without tile mutation");
    state.route_save_requested = false;
    state.route_save_busy = false;
    input(state, Action::back);
    check(state.page == Page::details && !state.route_save_requested,
          "leaving the informational card has no pending save");
    int api_calls = 0;
    ConsoleTileConfig config;
    config.post = [](void* context, const char*, const char*, char*, std::size_t) {
        ++*static_cast<int*>(context);
        return 200;
    };
    config.post_context = &api_calls;
    ConsoleTileManager manager(config);
    check(manager.availability().reason == ConsoleTileAvailabilityReason::hardware_qualification,
          "PS5 service remains unavailable during hardware qualification");
    const ConsoleTileIntent intent{state.selected_system, ConsoleTileDestination::library, {}};
    check(manager.upsert(intent).stage == ConsoleTileStage::unavailable &&
              manager.remove(state.selected_system).stage == ConsoleTileStage::unavailable &&
              manager.reconcile(state.selected_system).stage == ConsoleTileStage::unavailable &&
              manager.confirm_removed(state.selected_system).stage == ConsoleTileStage::unavailable &&
              api_calls == 0,
          "PS5 mutation entry points stop before filesystem or API activity");
    state.page = Page::home;
    input(state, triangle_action(state.page));
    check(state.page == Page::keyboard && state.keyboard_return == Page::home,
          "Home Triangle search survives the shortcut quarantine");
    std::puts("PASS PS5 shortcut UI quarantine");
}
