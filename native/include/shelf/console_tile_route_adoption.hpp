// SPDX-License-Identifier: GPL-3.0-only
// Side-effect-free startup routing for the four fixed pilots and 48 prepared
// console shortcuts. The forwarder argument is a UI hint, not proof of title
// provenance or authority to create, register, or edit a PS5 Home tile.
#pragma once

#include "console_tile_pilot_route_adoption.hpp"
#include "console_tile_prepared_route_adoption.hpp"

namespace shelf::tile_route::adoption {

enum class State { absent, original, preferred, choose_route, stale_emulator, invalid };
struct Decision {
    State state = State::absent;
    ConsoleTileIntent intent{};
};

// Call after select_console_tile_intent. A successful result can be passed to
// apply_console_tile_intent after the library scan. The preference is only a
// route choice; source and registration checks remain separate.
inline Decision resolve(ConsoleTileInput source_input, bool marker_used,
                        const ConsoleTileIntent& original, int argc,
                        char* const argv[], Input preference_input,
                        const Preference& preference, InstalledMenu installed,
                        void* context) {
    Decision result;
    if (source_input == ConsoleTileInput::absent) return result;
    if (source_input != ConsoleTileInput::valid || !console_tile_intent_valid(original)) {
        result.state = State::invalid;
        return result;
    }
    if (marker_used) {
        result.state = State::original;
        result.intent = original;
        return result;
    }
    const auto pilot_result = pilot::resolve(source_input, marker_used, original,
                                             argc, argv, preference_input,
                                             preference, installed, context);
    if (pilot_result.state != pilot::State::not_pilot) {
        switch (pilot_result.state) {
        case pilot::State::original: result.state = State::original; break;
        case pilot::State::chosen: result.state = State::preferred; break;
        case pilot::State::choose_route: result.state = State::choose_route; break;
        case pilot::State::stale_emulator: result.state = State::stale_emulator; break;
        case pilot::State::invalid_source: result.state = State::invalid; break;
        case pilot::State::not_pilot: break;
        }
        result.intent = pilot_result.intent;
        return result;
    }
    const auto prepared_result = prepared::resolve(source_input, marker_used,
                                                   original, argc, argv,
                                                   preference_input, preference,
                                                   installed, context);
    if (prepared_result.state != prepared::State::not_prepared) {
        switch (prepared_result.state) {
        case prepared::State::original: result.state = State::original; break;
        case prepared::State::chosen: result.state = State::preferred; break;
        case prepared::State::choose_route: result.state = State::choose_route; break;
        case prepared::State::stale_emulator: result.state = State::stale_emulator; break;
        case prepared::State::invalid_source: result.state = State::invalid; break;
        case prepared::State::not_prepared: break;
        }
        result.intent = prepared_result.intent;
        return result;
    }
    // Existing marker/argument shortcuts outside the adopted set keep their
    // already validated destination. This preserves compatibility without
    // silently applying an unrelated preference.
    result.state = State::original;
    result.intent = original;
    return result;
}

inline bool applicable(const Decision& decision) {
    return decision.state == State::original || decision.state == State::preferred;
}

// Until durable preference writes are qualified, an owned v1 shortcut asks
// on each launch. Input::invalid is deliberate here: it makes the existing
// exact pilot/prepared source checks return choose_route without treating a
// static Home or Library argument as the user's selected destination.
// Other valid shortcut arguments keep their existing behavior.
inline Decision require_launch_choice(ConsoleTileInput source_input, bool marker_used,
                                      const ConsoleTileIntent& original, int argc,
                                      char* const argv[]) {
    std::size_t fixed_home_system = system_count;
    const auto fixed_home = select_console_tile_fixed_home_arg(argc, argv,
                                                               fixed_home_system);
    if (fixed_home == FixedHomeInput::invalid) {
        Decision invalid;
        invalid.state = State::invalid;
        return invalid;
    }
    Decision decision = resolve(source_input, marker_used, original, argc, argv,
                                Input::invalid, Preference{}, nullptr, nullptr);
    if (fixed_home == FixedHomeInput::absent) return decision;
    // Only the new-title prepared Home source can bypass the legacy chooser.
    // Exact argv and title/system checks remain in prepared::resolve above.
    if (source_input != ConsoleTileInput::valid || marker_used ||
        !console_tile_intent_valid(original) ||
        original.destination != ConsoleTileDestination::home ||
        original.system != fixed_home_system ||
        !prepared::owned_prepared(original.system) ||
        decision.state != State::choose_route) {
        decision.state = State::invalid;
        decision.intent = {};
        return decision;
    }
    decision.state = State::original;
    decision.intent = original;
    return decision;
}

} // namespace shelf::tile_route::adoption
