// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "console_tile_intent.hpp"
#include "console_tile_write_gate.hpp"
#include "generic_tile_create_state.hpp"
#include "launch_plan.hpp"
#include "library.hpp"
#include "presentation.hpp"
#include "settings.hpp"

namespace shelf {
// Keep existing numeric page values stable for the private UI test bridge.
enum class Page { home, details, library, settings, keyboard, console_tile, shortcut_route, pairing_code };
enum class SettingsTab { appearance, browse, consoles, room };
enum class ConsoleTileAction {
    upsert,
    refresh_installed,
    remove,
    confirm_removed,
    remove_installed,
    upsert_emulators
};
// Route preferences are separate from installing or removing a PS5 Home tile.
// The host sets saved only after a durable readback of the chosen route.
enum class RouteSaveStatus { none, pending, saved, unavailable, invalid };
struct ConsoleTileRequest {
    ConsoleTileIntent intent;
    ConsoleTileAction action = ConsoleTileAction::upsert;
};
enum class Action {
    up,
    down,
    left,
    right,
    confirm,
    back,
    settings,
    filters,
    consoles,
    favorite,
    previous_tab,
    next_tab,
    search,
    erase,
    layout
};
inline constexpr Action triangle_action(Page page) {
    return page == Page::home ? Action::search : Action::favorite;
}
inline constexpr std::string_view keyboard_keys = "QWERTYUIOPASDFGHJKL<ZXCVBNM0123456789_!^";
struct PickerEntry {
    bool group;
    std::size_t index;
};
struct PickerList {
    std::array<PickerEntry, system_count + brands.size()> entries{};
    std::size_t count = 0;
};
struct UiState {
    Presentation presentation;
    Settings settings;
    Inventory inventory{};
    const Library* library = nullptr;
    Page page = Page::home, return_page = Page::home, keyboard_return = Page::home;
    SettingsTab tab = SettingsTab::appearance;
    std::size_t selected_system = 0;
    // A home-screen shortcut may show its console for this app session even
    // when saved filters or a curated shelf would otherwise hide it.
    std::size_t tile_home_override = system_count;
    int focus = 0, tool = 0, detail_focus = 1, tile_focus = 0, settings_focus = 0, keyboard_focus = 0;
    int shortcut_route_focus = 0;
    std::size_t shortcut_route_system = system_count;
    bool shortcut_route_ready = false;
    int game_focus = 0;
    bool tools = false, keyboard_for_consoles = false, changed = false, exit_requested = false;
    // True only while the bounded background inventory scan is still running.
    // If its opening grace expires, Home renders an honest discovery state
    // instead of looking like the owner's consoles disappeared.
    bool library_discovering = false;
    bool keyboard_for_tv = false;
    bool launch_connected = false, launch_requested = false, launch_busy = false;
    bool tile_connected = false, tile_requested = false, tile_busy = false, tile_batch_busy = false;
    bool tile_emulator_batch_close_ready = false;
    std::size_t tile_emulator_batch_created = 0;
    std::size_t tile_emulator_batch_updated = 0;
    bool route_save_requested = false, route_save_busy = false;
    bool route_save_apply_after = false;
    std::size_t route_save_system = system_count;
    RouteSaveStatus route_save_status = RouteSaveStatus::none;
    ConsoleTileIntent pending_route_save;
    std::array<bool, system_count> tile_installed{};
    std::array<bool, system_count> tile_removing{};
    std::array<bool, system_count> tile_removal_handoff{};
    bool tile_removal_batch_ready = false;
    bool tile_residual_sweep = false;
    std::size_t tile_removal_count = 0;
    char tile_removal_titles[521]{};
    // One read-only ShadowMount observation for the currently inspected
    // catalog title. This does not establish source ownership or Home presence.
    std::size_t tile_observation_system = system_count;
    char tile_observation[192]{};
    // Offline CREATE model; no PS5 worker is wired to this state.
    generic_tile_create::CatalogState tile_create;
    ConsoleTileRequest pending_tile;
    bool ui_test_active = false;
    bool close_hint_requested = false;
    LaunchRequest pending_launch;
    char query[128]{}, console_query[128]{}, notice[192]{};
};
BrowseResult shown_systems(const UiState& state);
PickerList console_picker(const UiState& state);
bool begin_console_tile_uninstall_handoff(UiState& state, const char* title_id);
bool queue_console_tile_removal(UiState& state, const char* title_id);
bool queue_console_tile_removals(UiState& state, const char* title_ids,
                                 std::size_t capacity);
// Applies the authoritative result of a completed remove-all pass when no
// post-close handoff remains. This also clears cached recovery presentation.
void clear_console_tile_removal_state(UiState& state);
bool begin_console_tile_removal_handoff(UiState& state);
bool begin_console_tile_refresh_handoff(UiState& state, const char* title_ids,
                                        std::size_t capacity);
void input(UiState& state, Action action);
void text_input(UiState& state, const char* utf8);
} // namespace shelf
