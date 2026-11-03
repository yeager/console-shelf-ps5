// SPDX-License-Identifier: GPL-3.0-only
// Offline routing prototype for the four already installed shortcut pilots.
// It changes only the destination applied by isShelf, never their source bytes.
#pragma once
#include "console_tile_route_preference.hpp"
#include <array>
#include <cstring>
#include <string_view>

namespace shelf::tile_route::pilot {

struct FixedSource {
    std::string_view system;
    const char* title;
    std::string_view argument;
    ConsoleTileDestination destination;
};

inline constexpr std::array<FixedSource, 4> fixed_sources{{
    {"ps1", "PPSA99704", "--isshelf-console-tile=ps1,home,-", ConsoleTileDestination::home},
    {"ps2", "PPSA99703", "--isshelf-console-tile=ps2,home,-", ConsoleTileDestination::home},
    {"ps3", "PPSA99702", "--isshelf-console-tile=ps3,home,-", ConsoleTileDestination::home},
    {"n64", "PPSA99711", "--isshelf-console-tile=n64,library,-", ConsoleTileDestination::library},
}};

enum class State { not_pilot, original, chosen, choose_route, stale_emulator, invalid_source };
struct Decision {
    State state = State::not_pilot;
    ConsoleTileIntent intent{};
};

// The main title has no marker when these forwarders launch it. The exact
// argv token is a UX routing hint, not proof of which PS5 Home tile was used.
// The existing selector must have accepted the input before this resolver is
// called; malformed or duplicate route tokens still fail closed here.
inline Decision resolve(ConsoleTileInput source_input, bool marker_used,
                        const ConsoleTileIntent& original, int argc, char* const argv[],
                        Input preference_input, const Preference& preference,
                        InstalledMenu installed, void* context) {
    Decision result;
    if (source_input != ConsoleTileInput::valid || argc < 0 || argc > 64 ||
        (argc > 0 && !argv)) {
        result.state = State::invalid_source;
        return result;
    }
    if (marker_used) return result;
    std::string_view argument;
    bool found = false;
    constexpr std::string_view option = "--isshelf-console-tile";
    for (int i = 0; i < argc; ++i) {
        if (!argv[i]) {
            result.state = State::invalid_source;
            return result;
        }
        if (std::strncmp(argv[i], option.data(), option.size()) != 0) continue;
        if (found) {
            result.state = State::invalid_source;
            return result;
        }
        const void* end = std::memchr(argv[i], 0, console_tile_message_capacity + 1);
        if (!end) {
            result.state = State::invalid_source;
            return result;
        }
        argument = std::string_view(argv[i], static_cast<const char*>(end) - argv[i]);
        found = true;
    }
    if (!found) return result;
    const FixedSource* fixed = nullptr;
    for (const auto& candidate : fixed_sources)
        if (argument == candidate.argument) {
            fixed = &candidate;
            break;
        }
    if (!fixed) return result;
    const auto system = system_for(fixed->system);
    const char* owned_title = system < system_count ? tile_job::owned_title_for(system) : nullptr;
    if (!owned_title || std::strcmp(owned_title, fixed->title) != 0 ||
        !console_tile_intent_valid(original) || original.system != system ||
        original.destination != fixed->destination || original.emulator_title[0]) {
        result.state = State::invalid_source;
        return result;
    }
    if (preference_input == Input::absent) {
        result.state = State::original;
        result.intent = original;
        return result;
    }
    const auto chosen = tile_route::resolve(Source{system}, preference_input, preference,
                                            installed, context);
    switch (chosen.resolution) {
    case Resolution::ready:
        result.state = State::chosen;
        result.intent = chosen.intent;
        break;
    case Resolution::choose_route:
        result.state = State::choose_route;
        break;
    case Resolution::stale_emulator:
        result.state = State::stale_emulator;
        break;
    case Resolution::invalid_source:
        result.state = State::invalid_source;
        break;
    }
    return result;
}

} // namespace shelf::tile_route::pilot
