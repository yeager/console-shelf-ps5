// SPDX-License-Identifier: GPL-3.0-only
// Offline protocol prototype. No installed tile or PS5 writer uses this yet.
#pragma once
#include "console_tile_intent.hpp"
#include <cstdio>
#include <cstring>
#include <string_view>

namespace shelf::tile_route {
inline constexpr std::string_view source_header = "ISSHELF-CONSOLE-TILE-2\n";
inline constexpr std::string_view route_header = "ISSHELF-CONSOLE-ROUTE-1\n";
inline constexpr std::string_view dynamic_arg_prefix = "--isshelf-console-tile=";
inline constexpr std::size_t maximum_record = 160;

// One immutable source per catalog console. Its title is always the pinned
// catalog title, never a user-supplied filename or arbitrary app ID.
struct Source {
    std::size_t system = system_count;
};
struct Preference {
    std::size_t system = system_count;
    ConsoleTileDestination destination = ConsoleTileDestination::library;
    char emulator_title[10]{};
};
enum class Input { absent, valid, invalid };
enum class Resolution { ready, choose_route, stale_emulator, invalid_source };
struct Decision {
    Resolution resolution = Resolution::invalid_source;
    ConsoleTileIntent intent{};
};
using InstalledMenu = bool (*)(void*, std::size_t, std::string_view);

inline bool source_valid(Source source) {
    return source.system < system_count && tile_job::owned_title_for(source.system) != nullptr;
}
inline bool preference_valid(const Preference& preference) {
    if (!source_valid({preference.system})) return false;
    const ConsoleTileIntent intent{preference.system, preference.destination, {}};
    if (preference.destination == ConsoleTileDestination::emulator) {
        ConsoleTileIntent menu = intent;
        if (!launch_bounded(preference.emulator_title)) return false;
        std::memcpy(menu.emulator_title, preference.emulator_title, sizeof(menu.emulator_title));
        return console_tile_creation_intent_valid(menu);
    }
    return (preference.destination == ConsoleTileDestination::library ||
            preference.destination == ConsoleTileDestination::home) &&
           !preference.emulator_title[0] && launch_bounded(preference.emulator_title);
}
inline bool take_line(std::string_view& input, std::string_view key, std::string_view& value) {
    if (input.size() < key.size() || input.substr(0, key.size()) != key) return false;
    input.remove_prefix(key.size());
    const auto end = input.find('\n');
    if (end == input.npos) return false;
    value = input.substr(0, end);
    input.remove_prefix(end + 1);
    return true;
}
inline std::size_t system_for(std::string_view id) {
    for (std::size_t i = 0; i < system_count; ++i)
        if (catalog[i].id == id) return i;
    return system_count;
}
inline bool parse_source(std::string_view body, Source& output) {
    if (body.size() > maximum_record || body.substr(0, source_header.size()) != source_header)
        return false;
    body.remove_prefix(source_header.size());
    std::string_view system, title, destination;
    if (!take_line(body, "system=", system) || !take_line(body, "title=", title) ||
        !take_line(body, "destination=", destination) || !body.empty() ||
        destination != "preference") return false;
    const Source parsed{system_for(system)};
    if (!source_valid(parsed) || title != tile_job::owned_title_for(parsed.system)) return false;
    output = parsed;
    return true;
}
inline bool encode_source(Source source, char* output, std::size_t capacity) {
    if (!source_valid(source) || !output || !capacity) return false;
    const auto id = catalog[source.system].id;
    const int used = std::snprintf(output, capacity,
        "ISSHELF-CONSOLE-TILE-2\nsystem=%.*s\ntitle=%s\ndestination=preference\n",
        static_cast<int>(id.size()), id.data(), tile_job::owned_title_for(source.system));
    return used > 0 && static_cast<std::size_t>(used) < capacity &&
           static_cast<std::size_t>(used) <= maximum_record;
}
inline bool parse_argument(std::string_view argument, Source& output) {
    if (argument.size() > console_tile_message_capacity ||
        argument.substr(0, dynamic_arg_prefix.size()) != dynamic_arg_prefix) return false;
    argument.remove_prefix(dynamic_arg_prefix.size());
    const auto comma = argument.find(',');
    if (comma == argument.npos || argument.substr(comma) != ",preference,-") return false;
    const Source parsed{system_for(argument.substr(0, comma))};
    if (!source_valid(parsed)) return false;
    output = parsed;
    return true;
}
inline bool encode_argument(Source source, char* output, std::size_t capacity) {
    if (!source_valid(source) || !output || !capacity) return false;
    const auto id = catalog[source.system].id;
    const int used = std::snprintf(output, capacity, "--isshelf-console-tile=%.*s,preference,-",
                                   static_cast<int>(id.size()), id.data());
    return used > 0 && static_cast<std::size_t>(used) < capacity &&
           static_cast<std::size_t>(used) <= console_tile_message_capacity;
}
// Both marker and argument are required. The current live selector rejects
// this new token, so no source can silently turn into a Home route before an
// explicitly reviewed startup integration.
inline Input select_source(std::string_view marker, int argc, char* const argv[], Source& output) {
    if (marker.empty() && argc == 0) return Input::absent;
    if (argc < 0 || argc > 64 || !argv) return Input::invalid;
    Source parsed_marker;
    if (!parse_source(marker, parsed_marker)) return Input::invalid;
    bool found = false;
    Source parsed_arg;
    for (int i = 0; i < argc; ++i) {
        if (!argv[i]) return Input::invalid;
        constexpr std::string_view option = "--isshelf-console-tile";
        if (std::strncmp(argv[i], option.data(), option.size()) != 0) continue;
        if (found) return Input::invalid;
        std::size_t length = 0;
        while (length <= console_tile_message_capacity && argv[i][length]) ++length;
        if (length > console_tile_message_capacity ||
            !parse_argument(std::string_view(argv[i], length), parsed_arg)) return Input::invalid;
        found = true;
    }
    if (!found || parsed_arg.system != parsed_marker.system) return Input::invalid;
    output = parsed_marker;
    return Input::valid;
}
inline bool parse_preference(std::string_view body, Preference& output) {
    if (body.size() > maximum_record || body.substr(0, route_header.size()) != route_header)
        return false;
    body.remove_prefix(route_header.size());
    std::string_view system, title, destination, emulator;
    if (!take_line(body, "system=", system) || !take_line(body, "title=", title) ||
        !take_line(body, "destination=", destination) ||
        !take_line(body, "emulator=", emulator) || !body.empty()) return false;
    Preference parsed;
    parsed.system = system_for(system);
    if (!source_valid({parsed.system}) || title != tile_job::owned_title_for(parsed.system)) return false;
    if (destination == "library") parsed.destination = ConsoleTileDestination::library;
    else if (destination == "home") parsed.destination = ConsoleTileDestination::home;
    else if (destination == "emulator") parsed.destination = ConsoleTileDestination::emulator;
    else return false;
    if (parsed.destination == ConsoleTileDestination::emulator) {
        if (emulator.size() >= sizeof(parsed.emulator_title)) return false;
        std::memcpy(parsed.emulator_title, emulator.data(), emulator.size());
    } else if (emulator != "-") return false;
    if (!preference_valid(parsed)) return false;
    output = parsed;
    return true;
}
inline bool encode_preference(const Preference& preference, char* output, std::size_t capacity) {
    if (!preference_valid(preference) || !output || !capacity) return false;
    const auto id = catalog[preference.system].id;
    const auto destination = console_tile_destination_name(preference.destination);
    const char* emulator = preference.destination == ConsoleTileDestination::emulator
                               ? preference.emulator_title : "-";
    const int used = std::snprintf(output, capacity,
        "ISSHELF-CONSOLE-ROUTE-1\nsystem=%.*s\ntitle=%s\ndestination=%.*s\nemulator=%s\n",
        static_cast<int>(id.size()), id.data(), tile_job::owned_title_for(preference.system),
        static_cast<int>(destination.size()), destination.data(), emulator);
    return used > 0 && static_cast<std::size_t>(used) < capacity &&
           static_cast<std::size_t>(used) <= maximum_record;
}
// Missing/corrupt/mismatched records require an explicit route choice. A
// pinned but uninstalled emulator never falls through to another emulator.
inline Decision resolve(Source source, Input preference_input, const Preference& preference,
                        InstalledMenu installed, void* context) {
    Decision decision;
    if (!source_valid(source)) return decision;
    decision.resolution = Resolution::choose_route;
    if (preference_input != Input::valid || preference.system != source.system ||
        !preference_valid(preference)) return decision;
    if (preference.destination == ConsoleTileDestination::emulator &&
        (!installed || !installed(context, source.system, preference.emulator_title))) {
        decision.resolution = Resolution::stale_emulator;
        return decision;
    }
    decision.intent.system = source.system;
    decision.intent.destination = preference.destination;
    std::memcpy(decision.intent.emulator_title, preference.emulator_title,
                sizeof(decision.intent.emulator_title));
    decision.resolution = Resolution::ready;
    return decision;
}
} // namespace shelf::tile_route
