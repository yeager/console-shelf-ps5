// SPDX-License-Identifier: GPL-3.0-only
// Offline route adoption for the prepared, non-reserved v1 Home sources.
// The forwarder launches the main app with argv only; it cannot pass its own
// /app0 marker. This is a UX preference, never proof of tile provenance or
// authority to write, mount, or register a Home tile.
#pragma once

#include "console_tile_route_preference.hpp"
#include "generic_tile_create_state.hpp"
#include <cstring>
#include <string_view>

namespace shelf::tile_route::prepared {

enum class State { not_prepared, original, chosen, choose_route, stale_emulator, invalid_source };
struct Decision {
    State state = State::not_prepared;
    ConsoleTileIntent intent{};
};

inline bool owned_prepared(std::size_t system) {
    return source_valid({system}) && !generic_tile_create::reserved_existing_title(system) &&
           !isshelf_badge::generic_tile::tiles[system].adopted &&
           std::strcmp(tile_job::owned_title_for(system),
                       isshelf_badge::generic_tile::tiles[system].title_id) == 0;
}

// Call only after select_console_tile_intent. The main title has no marker
// when the stock forwarder launches it. Require the exact original Library,
// Home, or pinned Emulator argument before applying an untrusted preference;
// the four existing pilot titles are excluded.
inline Decision resolve(ConsoleTileInput source_input, bool marker_used,
                        const ConsoleTileIntent& original, int argc,
                        char* const argv[], Input preference_input,
                        const Preference& preference, InstalledMenu installed,
                        void* context) {
    Decision result;
    if (source_input != ConsoleTileInput::valid || argc < 0 || argc > 64 ||
        (argc > 0 && !argv)) {
        result.state = State::invalid_source;
        return result;
    }
    if (marker_used || !owned_prepared(original.system)) return result;
    if (!console_tile_intent_valid(original)) {
        result.state = State::invalid_source;
        return result;
    }

    std::size_t fixed_home_system = system_count;
    const auto fixed_home = select_console_tile_fixed_home_arg(argc, argv,
                                                               fixed_home_system);
    if (fixed_home == FixedHomeInput::invalid ||
        (fixed_home == FixedHomeInput::valid &&
         (original.destination != ConsoleTileDestination::home ||
          fixed_home_system != original.system))) {
        result.state = State::invalid_source;
        return result;
    }

    char expected[console_tile_message_capacity + 1]{};
    if (!encode_console_tile_arg(original, expected, sizeof(expected))) {
        result.state = State::invalid_source;
        return result;
    }
    bool found = false;
    std::string_view argument;
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
    if (!found || argument != expected) {
        result.state = State::invalid_source;
        return result;
    }
    if (preference_input == Input::absent) {
        ConsoleTileIntent normalized;
        if (!normalize_console_tile_intent(original, normalized)) {
            result.state = State::invalid_source;
            return result;
        }
        if (normalized.destination == ConsoleTileDestination::emulator &&
            (!installed || !installed(context, normalized.system,
                                       normalized.emulator_title))) {
            result.state = State::stale_emulator;
            return result;
        }
        result.state = State::original;
        result.intent = normalized;
        return result;
    }
    const auto chosen = tile_route::resolve(Source{original.system},
                                            preference_input, preference,
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

} // namespace shelf::tile_route::prepared
