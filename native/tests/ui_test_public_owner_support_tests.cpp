// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/ui_test_bridge.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL public owner support: %s\n", message);
        std::exit(1);
    }
}

void check_rejected_unchanged(shelf::UiState& state, shelf::Action action,
                              const char* message) {
    std::array<unsigned char, sizeof(state)> before{};
    std::memcpy(before.data(), &state, sizeof(state));
    check(!shelf::apply_ui_test_action(state, action), message);
    check(std::memcmp(before.data(), &state, sizeof(state)) == 0,
          "rejected public action leaves the complete live state unchanged");
}

shelf::UiState tile_fixture(shelf::ConsoleTileAction action, shelf::Library& library) {
    using namespace shelf;
    UiState state;
    state.page = Page::console_tile;
    state.selected_system = find_system("ps1");
    state.tile_connected = true;
    state.launch_connected = true;
    state.library = &library;
    switch (action) {
    case ConsoleTileAction::upsert:
        state.tile_focus = 0;
        break;
    case ConsoleTileAction::upsert_emulators:
        state.tile_focus = 3;
        break;
    case ConsoleTileAction::refresh_installed:
        state.tile_focus = 4;
        break;
    case ConsoleTileAction::remove_installed:
        state.tile_focus = 5;
        break;
    case ConsoleTileAction::remove:
        state.tile_installed[state.selected_system] = true;
        state.tile_focus = 3;
        break;
    case ConsoleTileAction::confirm_removed:
        state.tile_removing[state.selected_system] = true;
        state.tile_focus = 0;
        break;
    }
    return state;
}

void check_tile_action(shelf::ConsoleTileAction action, shelf::Library& library,
                       const char* message) {
    auto remote = tile_fixture(action, library);
    auto physical = remote;
    shelf::input(physical, shelf::Action::confirm);
    check(physical.tile_requested && physical.pending_tile.action == action,
          "physical Confirm still queues the exact tile action");
    check_rejected_unchanged(remote, shelf::Action::confirm, message);
}
} // namespace

int main() {
    using namespace shelf;

    UiState home;
    check(apply_ui_test_action(home, Action::right) && home.focus == 1,
          "public support retains ordinary navigation");
    check(apply_ui_test_action(home, Action::back) && !home.exit_requested &&
              home.close_hint_requested,
          "public support retains an ordinary Back hint without simulating close");

    static Library library;
    library.scan.complete = true;
    check_tile_action(ConsoleTileAction::upsert, library,
                      "public Confirm cannot create or update one tile");
    check_tile_action(ConsoleTileAction::upsert_emulators, library,
                      "public Confirm cannot add or repair all emulator tiles");
    check_tile_action(ConsoleTileAction::refresh_installed, library,
                      "public Confirm cannot refresh installed tiles");
    check_tile_action(ConsoleTileAction::remove_installed, library,
                      "public Confirm cannot remove all installed tiles");
    check_tile_action(ConsoleTileAction::remove, library,
                      "public Confirm cannot remove one installed tile");
    check_tile_action(ConsoleTileAction::confirm_removed, library,
                      "public Confirm cannot finish a deferred tile removal");

    for (const auto prompt : {UpdatePrompt::available, UpdatePrompt::recovery}) {
        UiState remote;
        remote.update_prompt = prompt;
        auto physical = remote;
        input(physical, Action::confirm);
        check(physical.update_accept_requested,
              "physical Confirm still approves the selected update flow");
        check_rejected_unchanged(remote, Action::confirm,
                                 "public Confirm cannot approve update or recovery");
    }

    UiState later;
    later.update_prompt = UpdatePrompt::available;
    check(apply_ui_test_action(later, Action::right) && later.update_focus == 1,
          "public support can move to the Later choice");
    check(apply_ui_test_action(later, Action::confirm) &&
              later.update_prompt == UpdatePrompt::none &&
              !later.update_accept_requested,
          "public support can dismiss an update without approving it");

    UiState preparing;
    preparing.update_prompt = UpdatePrompt::preparing;
    check(apply_ui_test_action(preparing, Action::back) &&
              preparing.update_cancel_requested &&
              !preparing.update_accept_requested,
          "public Back can still pause a preparing update safely");

    std::puts("PASS: public owner support keeps tile and update approval physical while preserving navigation.");
}
