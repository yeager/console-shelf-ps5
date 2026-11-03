// SPDX-License-Identifier: GPL-3.0-only
// UI-facing, side-effect-free state for the app-owned CREATE path.
// This object is session memory, not the durable history or live title probe.
#pragma once

#include "console_tile_job.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace shelf::generic_tile_create {

static_assert(system_count == isshelf_badge::generic_tile::tile_count);
static_assert(std::string_view(isshelf_badge::generic_tile::tiles[2].title_id) == "PPSA99702");
static_assert(std::string_view(isshelf_badge::generic_tile::tiles[3].title_id) == "PPSA99703");
static_assert(std::string_view(isshelf_badge::generic_tile::tiles[4].title_id) == "PPSA99704");
static_assert(std::string_view(isshelf_badge::generic_tile::tiles[11].title_id) == "PPSA99711");

enum class Phase {
    existing_title,
    inspect_required,
    preparing_source,
    source_ready,
    source_incomplete,
    registration_unknown,
    managed_without_manual,
    registered_home_unverified,
    home_launch_confirmed,
    conflict,
};

enum class Presence { unknown, absent, present };

// The caller must get these from a fresh, complete read-only inventory. This
// model does not perform a PS5 read or grant an exclusive-writer capability.
struct Admission {
    Presence hidden_source = Presence::unknown;
    Presence homebrew_source = Presence::unknown;
    Presence user_app = Presence::unknown;
    Presence user_appmeta = Presence::unknown;
    Presence game_info = Presence::unknown;
    Presence manual_list = Presence::unknown;
    bool durable_history_clear = false;
    bool exclusive_writer = false;
};

inline bool all_absent(const Admission& evidence) {
    return evidence.hidden_source == Presence::absent &&
           evidence.homebrew_source == Presence::absent &&
           evidence.user_app == Presence::absent &&
           evidence.user_appmeta == Presence::absent &&
           evidence.game_info == Presence::absent &&
           evidence.manual_list == Presence::absent;
}

inline bool any_present(const Admission& evidence) {
    return evidence.hidden_source == Presence::present ||
           evidence.homebrew_source == Presence::present ||
           evidence.user_app == Presence::present ||
           evidence.user_appmeta == Presence::present ||
           evidence.game_info == Presence::present ||
           evidence.manual_list == Presence::present;
}

enum class Begin { started, no_route, existing_title, duplicate_or_unresolved, occupied,
                   incomplete_inventory, history_or_writer_unproved, invalid_nonce };

enum class ManualEntry { unknown, absent, exact, conflict };

struct RegistrationObservation {
    bool complete = false;
    bool managed = false;
    bool exact_source = false;
    bool exact_mount = false;
    bool exact_appmeta = false;
    bool exact_db_rows = false;
    ManualEntry manual = ManualEntry::unknown;
};

struct TitleState {
    Phase phase = Phase::inspect_required;
    ConsoleTileIntent route;
    bool route_selected = false;
    char nonce[33]{};
    std::size_t source_receipt_bytes = 0;
    std::uint64_t source_receipt_fnv = 0;
    bool home_seen = false;
    bool play_route_seen = false;
};

inline bool reserved_existing_title(std::size_t system) {
    return system == 2 || system == 3 || system == 4 || system == 11;
}

inline const char* phase_text(Phase phase) {
    switch (phase) {
    case Phase::existing_title: return "Existing shortcut: inspect separately";
    case Phase::inspect_required: return "Shortcut not checked";
    case Phase::preparing_source: return "Preparing shortcut source";
    case Phase::source_ready: return "Source ready; Home tile not registered";
    case Phase::source_incomplete: return "Source incomplete; review required";
    case Phase::registration_unknown: return "Registration result unknown; check before retry";
    case Phase::managed_without_manual: return "Installed; ShadowMount tracking needs review";
    case Phase::registered_home_unverified: return "Registered; PS5 Home launch unverified";
    case Phase::home_launch_confirmed: return "PS5 Home launch confirmed";
    case Phase::conflict: return "Shortcut state conflicts; review required";
    }
    return "Shortcut state unknown";
}

class CatalogState {
    std::array<TitleState, system_count> titles_{};

  public:
    CatalogState() {
        for (std::size_t system = 0; system < system_count; ++system)
            if (reserved_existing_title(system)) titles_[system].phase = Phase::existing_title;
    }

    const TitleState* get(std::size_t system) const {
        return system < system_count ? &titles_[system] : nullptr;
    }

    bool select_route(const ConsoleTileIntent& route,
                      tile_job::EmulatorRouteCheck installed_menu = nullptr,
                      const void* library = nullptr) {
        if (!tile_job::intent_valid(route, installed_menu, library) ||
            !tile_job::catalog_pin_matches(route.system) ||
            reserved_existing_title(route.system)) return false;
        auto& state = titles_[route.system];
        if (state.phase != Phase::inspect_required) return false;
        state.route = route;
        state.route_selected = true;
        return true;
    }

    bool cancel_selection(std::size_t system) {
        if (system >= system_count) return false;
        auto& state = titles_[system];
        if (state.phase != Phase::inspect_required || !state.route_selected) return false;
        state.route = {};
        state.route_selected = false;
        return true;
    }

    Begin begin_source(std::size_t system, const char (&nonce)[33],
                       const Admission& evidence,
                       tile_job::EmulatorRouteCheck installed_menu = nullptr,
                       const void* library = nullptr) {
        if (system >= system_count || !tile_job::catalog_pin_matches(system))
            return Begin::no_route;
        auto& state = titles_[system];
        if (state.phase == Phase::existing_title) return Begin::existing_title;
        if (state.phase != Phase::inspect_required) return Begin::duplicate_or_unresolved;
        if (!state.route_selected || state.route.system != system ||
            !tile_job::intent_valid(state.route, installed_menu, library)) return Begin::no_route;
        if (!tile_job::nonce_valid(nonce)) return Begin::invalid_nonce;
        if (any_present(evidence)) return Begin::occupied;
        if (!all_absent(evidence)) return Begin::incomplete_inventory;
        if (!evidence.durable_history_clear || !evidence.exclusive_writer)
            return Begin::history_or_writer_unproved;
        std::memcpy(state.nonce, nonce, sizeof(state.nonce));
        state.phase = Phase::preparing_source;
        return Begin::started;
    }

    bool finish_source(std::size_t system, bool complete, std::size_t receipt_bytes,
                       std::uint64_t receipt_fnv) {
        if (system >= system_count || titles_[system].phase != Phase::preparing_source)
            return false;
        auto& state = titles_[system];
        if (complete && receipt_bytes && receipt_fnv) {
            state.phase = Phase::source_ready;
            state.source_receipt_bytes = receipt_bytes;
            state.source_receipt_fnv = receipt_fnv;
        } else {
            // Even a pre-write refusal needs fresh inspection before a new
            // nonce. The caller may have lost its view of a partial source.
            state.phase = Phase::source_incomplete;
        }
        return true;
    }

    // Only after a separate durable attempt record is synced and exactly one
    // manual/add is attempted. This function makes no API call.
    bool note_registration_attempt(std::size_t system, bool durable_attempt_recorded) {
        if (system >= system_count || !durable_attempt_recorded ||
            titles_[system].phase != Phase::source_ready) return false;
        titles_[system].phase = Phase::registration_unknown;
        return true;
    }

    bool observe_registration(std::size_t system, const RegistrationObservation& seen) {
        if (system >= system_count || !seen.complete) return false;
        auto& state = titles_[system];
        if (state.phase != Phase::registration_unknown &&
            state.phase != Phase::managed_without_manual &&
            state.phase != Phase::registered_home_unverified) return false;
        if (seen.manual == ManualEntry::conflict) {
            state.phase = Phase::conflict;
            return true;
        }
        if (!seen.managed) return false;
        if (!seen.exact_source || !seen.exact_mount || !seen.exact_appmeta ||
            !seen.exact_db_rows) {
            state.phase = Phase::conflict;
            return true;
        }
        if (seen.manual == ManualEntry::unknown) return false;
        state.phase = seen.manual == ManualEntry::exact
                          ? Phase::registered_home_unverified
                          : Phase::managed_without_manual;
        return true;
    }

    bool note_home_observation(std::size_t system, bool icon_seen, bool play_route_seen) {
        if (system >= system_count ||
            titles_[system].phase != Phase::registered_home_unverified) return false;
        auto& state = titles_[system];
        state.home_seen = icon_seen;
        state.play_route_seen = play_route_seen;
        if (icon_seen && play_route_seen) state.phase = Phase::home_launch_confirmed;
        return true;
    }
};

} // namespace shelf::generic_tile_create
