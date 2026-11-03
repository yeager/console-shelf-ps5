// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "ui.hpp"
#include "console_tile_job_routes.hpp"
#include <cstdio>
#include <cstring>

namespace shelf {
enum class ConsoleTileApply {
    invalid,
    library,
    home,
    home_unavailable,
    emulator,
    emulator_unavailable
};

// The temporary launch chooser makes no durable preference or Home-tile
// change. A missing library scan disables Library and emulator; Home remains
// a valid console selection because it needs no game or emulator inventory.
inline bool begin_console_tile_route_choice(UiState& state, const ConsoleTileIntent& source) {
    if (!console_tile_intent_valid(source)) return false;
    state.selected_system = source.system;
    state.shortcut_route_system = source.system;
    state.shortcut_route_ready = state.library && state.library->scan.complete;
    LaunchRequest emulator;
    const bool emulator_ready = state.shortcut_route_ready && state.launch_connected &&
        plan_emulator(*state.library, source.system,
                      state.settings.preferred_emulator[source.system].data(), emulator) !=
            LaunchPlan::unavailable &&
        console_tile_emulator_menu_selects_system(emulator.title);
    state.shortcut_route_focus = console_tile_isshelf_only(source.system)
        ? 0
        : emulator_ready ? 1 : state.shortcut_route_ready ? 0 : 2;
    state.page = Page::shortcut_route;
    state.tools = false;
    state.pending_launch = {};
    state.launch_requested = false;
    // The chooser renders scan status inside its bounded left panel. Clear a
    // stale global toast so that message does not duplicate across the TV.
    state.notice[0] = 0;
    return true;
}

// Revalidate the installed emulator at the moment the user confirms. A
// selected row never supplies an arbitrary title, path, or file body.
inline bool console_tile_route_choice_intent(const UiState& state, int focus, ConsoleTileIntent& intent) {
    if (state.page != Page::shortcut_route ||
        state.shortcut_route_system >= system_count || focus < 0 || focus > 2) return false;
    if (console_tile_isshelf_only(state.shortcut_route_system) && focus != 0)
        return false;
    intent = {};
    intent.system = state.shortcut_route_system;
    if (focus == 2) {
        intent.destination = ConsoleTileDestination::home;
        return tile_job::intent_valid(intent);
    }
    if (!state.shortcut_route_ready || !state.library ||
        !state.library->scan.complete) return false;
    if (focus == 0)
        intent.destination = ConsoleTileDestination::library;
    else {
        intent.destination = ConsoleTileDestination::emulator;
        LaunchRequest launch;
        if (!state.launch_connected ||
            plan_emulator(*state.library, intent.system,
                          state.settings.preferred_emulator[intent.system].data(), launch) ==
                LaunchPlan::unavailable) return false;
        std::snprintf(intent.emulator_title, sizeof(intent.emulator_title), "%s", launch.title);
    }
    return tile_job::intent_valid(intent, tile_job::installed_emulator_route, state.library);
}

// Apply an already validated shortcut. Library and Home surfaces do not need
// to wait for the background library scan; emulator handoffs are validated
// separately and still require its completed route inventory. Keep this
// independent of the PS5 event loop so every destination can be host tested.
inline ConsoleTileApply apply_console_tile_intent(UiState& state, const ConsoleTileIntent& intent) {
    if (!console_tile_intent_valid(intent))
        return ConsoleTileApply::invalid;
    state.selected_system = intent.system;
    state.tools = false;
    state.query[0] = 0;
    const auto system_name = catalog[intent.system].name;
    if (intent.destination == ConsoleTileDestination::library) {
        state.page = Page::library;
        state.game_focus = 0;
        return ConsoleTileApply::library;
    }
    if (intent.destination == ConsoleTileDestination::home) {
        state.tile_home_override = intent.system;
        const auto shown = shown_systems(state);
        for (std::size_t i = 0; i < shown.count; ++i)
            if (shown.indices[i] == intent.system) {
                state.page = Page::home;
                state.focus = static_cast<int>(i);
                return ConsoleTileApply::home;
            }
        // A valid catalog ID always fits in shown_systems' one-slot override.
        return ConsoleTileApply::home_unavailable;
    }
    state.page = Page::details;
    state.detail_focus = 1;
    if (state.library && state.launch_connected)
        for (std::size_t i = 0; i < state.library->route_count; ++i) {
            const auto& route = state.library->routes[i];
            if (route.system == intent.system && std::strcmp(route.title_id, intent.emulator_title) == 0 &&
                launch_route_folder(route)) {
                state.pending_launch = {};
                std::snprintf(state.pending_launch.title, sizeof(state.pending_launch.title), "%s", route.title_id);
                state.launch_requested = true;
                std::snprintf(state.notice, sizeof(state.notice), "Opening %.*s emulator...",
                              static_cast<int>(system_name.size()), system_name.data());
                return ConsoleTileApply::emulator;
            }
        }
    std::snprintf(state.notice, sizeof(state.notice), "That %.*s emulator is unavailable. Opened console details.",
                  static_cast<int>(system_name.size()), system_name.data());
    return ConsoleTileApply::emulator_unavailable;
}
} // namespace shelf
