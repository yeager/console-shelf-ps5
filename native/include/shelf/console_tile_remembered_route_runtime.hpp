// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "console_tile_manager.hpp"
#include "console_tile_job_routes.hpp"
#include "settings.hpp"
#include <cstring>

namespace shelf::tile_route {

// The settings file records a UI destination only. Its contents are never
// authority to create a tile or to launch an arbitrary title. The installed
// emulator is checked again both here and when a shortcut next opens.
inline bool remember_shortcut_route(const char* path, const Settings& current,
                                    const ConsoleTileIntent& intent,
                                    const Library* library, Settings& readback) {
    if (!path || !tile_job::intent_valid(intent, tile_job::installed_emulator_route, library))
        return false;
    Preference preference;
    preference.system = intent.system;
    preference.destination = intent.destination;
    std::memcpy(preference.emulator_title, intent.emulator_title,
                sizeof(preference.emulator_title));
    if (!preference_valid(preference))
        return false;

    Settings next = current;
    auto& slot = next.shortcut_routes[intent.system];
    slot.input = Input::valid;
    slot.preference = preference;
    if (!save_settings(path, next))
        return false;
    Settings reopened;
    if (load_settings(path, reopened) != SettingsLoad::loaded)
        return false;
    const auto& observed = reopened.shortcut_routes[intent.system];
    if (observed.input != Input::valid ||
        observed.preference.system != preference.system ||
        observed.preference.destination != preference.destination ||
        std::strcmp(observed.preference.emulator_title,
                    preference.emulator_title) != 0)
        return false;
    readback = reopened;
    return true;
}

// Persist a verified batch in one atomic settings replacement. This avoids a
// separate write for every console while retaining the same reopen/readback
// requirement used by the single-shortcut path.
inline bool remember_shortcut_routes(
    const char* path, const Settings& current,
    const std::array<ConsoleTileIntent, system_count>& intents,
    const std::array<bool, system_count>& selected,
    const Library* library, Settings& readback) {
    if (!path || !library)
        return false;
    Settings next = current;
    std::size_t count = 0;
    for (std::size_t system = 0; system < system_count; ++system) {
        if (!selected[system])
            continue;
        const auto& intent = intents[system];
        if (intent.system != system ||
            !tile_job::intent_valid(intent, tile_job::installed_emulator_route,
                                    library))
            return false;
        Preference preference;
        preference.system = system;
        preference.destination = intent.destination;
        std::memcpy(preference.emulator_title, intent.emulator_title,
                    sizeof(preference.emulator_title));
        if (!preference_valid(preference))
            return false;
        auto& slot = next.shortcut_routes[system];
        slot.input = Input::valid;
        slot.preference = preference;
        ++count;
    }
    if (!count || !save_settings(path, next))
        return false;
    Settings reopened;
    if (load_settings(path, reopened) != SettingsLoad::loaded)
        return false;
    for (std::size_t system = 0; system < system_count; ++system) {
        if (!selected[system])
            continue;
        const auto& observed = reopened.shortcut_routes[system];
        const auto& expected = next.shortcut_routes[system];
        if (observed.input != Input::valid ||
            observed.preference.system != expected.preference.system ||
            observed.preference.destination != expected.preference.destination ||
            std::strcmp(observed.preference.emulator_title,
                        expected.preference.emulator_title) != 0)
            return false;
    }
    readback = reopened;
    return true;
}

enum class UpsertRouteSync { not_applicable, saved, failed };

// A write-enabled tile update changes forwarder.json. Keep the separately
// remembered launch preference in the same state only after the manager has
// read back ownership and the exact requested intent. Otherwise an older
// preference can silently override the route the owner just selected.
inline UpsertRouteSync remember_upsert_route(const char* path,
                                             const Settings& current,
                                             const ConsoleTileIntent& requested,
                                             const ConsoleTileOutcome& outcome,
                                             const Library* library,
                                             Settings& readback) {
    const bool source_committed = outcome.owned &&
        console_tile_intents_equal(outcome.intent, requested) &&
        (outcome.stage == ConsoleTileStage::registered ||
         outcome.stage == ConsoleTileStage::pending ||
         outcome.stage == ConsoleTileStage::deferred ||
         outcome.stage == ConsoleTileStage::unavailable);
    if (!source_committed)
        return UpsertRouteSync::not_applicable;
    return remember_shortcut_route(path, current, requested, library, readback)
        ? UpsertRouteSync::saved : UpsertRouteSync::failed;
}

} // namespace shelf::tile_route
