// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "console_tile_route_adoption.hpp"
#include "console_tile_job_routes.hpp"
#include "settings.hpp"
#include "library.hpp"

namespace shelf::tile_route {

// Validate the original shortcut argument and fixed-Home option before using
// a settings choice. The choice changes only the in-app destination; it is
// never evidence of a registered Home tile or authority to write one.
inline adoption::Decision resolve_remembered_launch(
    ConsoleTileInput source_input, bool marker_used,
    const ConsoleTileIntent& original, int argc, char* const argv[],
    const Settings& settings, const Library* library) {
    auto decision = adoption::require_launch_choice(source_input, marker_used,
                                                    original, argc, argv);
    if (decision.state == adoption::State::invalid ||
        original.system >= system_count || marker_used)
        return decision;
    const auto installed = [](void* context, std::size_t system, std::string_view title) {
        const auto* current = static_cast<const Library*>(context);
        return current && current->scan.complete &&
               tile_job::installed_emulator_route(current, system, title);
    };
    const auto& saved = settings.shortcut_routes[original.system];
    if (saved.input == Input::invalid) {
        decision.state = adoption::State::choose_route;
        decision.intent = {};
        return decision;
    }
    if (saved.input == Input::absent &&
        prepared::owned_prepared(original.system)) {
        // New managed sources already carry the owner's creation-time route.
        // The four older pilot forwarders still ask once when no choice exists.
        const auto initial = prepared::resolve(
            source_input, marker_used, original, argc, argv, Input::absent,
            Preference{}, installed, const_cast<Library*>(library));
        if (initial.state == prepared::State::original) {
            decision.state = adoption::State::original;
            decision.intent = initial.intent;
        } else if (initial.state == prepared::State::stale_emulator) {
            decision.state = adoption::State::stale_emulator;
            decision.intent = {};
        }
    } else if (saved.input == Input::valid) {
        decision = adoption::resolve(
            source_input, marker_used, original, argc, argv,
            saved.input, saved.preference, installed,
            const_cast<Library*>(library));
    }
    return decision;
}

// Library and Home destinations only select an isShelf surface. Their exact
// route has already been validated against the fixed launch argument and the
// saved preference, so the surface can be selected while the read-only
// library scan continues in the background. Emulator destinations still need
// the completed scan to prove the pinned emulator route before handoff.
inline bool can_select_surface_before_library(const adoption::Decision& decision) {
    return adoption::applicable(decision) &&
           (decision.intent.destination == ConsoleTileDestination::library ||
            decision.intent.destination == ConsoleTileDestination::home);
}

// An exact installed emulator destination can begin its cooperative handoff
// as soon as the background library scan has validated the route. Library,
// Home and chooser destinations never use this external-launch shortcut.
inline bool can_handoff_before_reveal(const adoption::Decision& decision) {
    return adoption::applicable(decision) &&
           decision.intent.destination == ConsoleTileDestination::emulator;
}

} // namespace shelf::tile_route
