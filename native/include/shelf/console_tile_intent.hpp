// SPDX-License-Identifier: GPL-3.0-only
// Strict startup contract for an isShelf-owned console shortcut.
#pragma once
#include "catalog.generated.hpp"
#include "launch_protocol.hpp"
#include "console_tile_job_pin.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace shelf {
inline constexpr char console_tile_marker_path[] = "/app0/isshelf-console-tile.txt";
inline constexpr char console_tile_shared_assets[] = "/data/homebrew/PPSA99541";
inline constexpr std::size_t console_tile_message_capacity = 128;
inline constexpr std::string_view console_tile_header = "ISSHELF-CONSOLE-TILE-1\n";
inline constexpr std::string_view console_tile_arg_prefix = "--isshelf-console-tile=";
// A second argv token distinguishes a newly created fixed-Home route from
// legacy one-argument Home sources, which intentionally ask on every launch.
// It is a UX hint, never proof that a Home tile is owned or registered.
inline constexpr std::string_view console_tile_fixed_home_arg_prefix =
    "--isshelf-home-direct-v2=";
inline constexpr std::string_view console_tile_fixed_home_arg_option =
    "--isshelf-home-direct-v2";

enum class ConsoleTileDestination { library, home, emulator };
struct ConsoleTileIntent {
    std::size_t system = system_count;
    ConsoleTileDestination destination = ConsoleTileDestination::library;
    char emulator_title[10]{};
};
enum class ConsoleTileInput { absent, valid, invalid };
enum class FixedHomeInput { absent, valid, invalid };

inline std::string_view console_tile_destination_name(ConsoleTileDestination destination) {
    switch (destination) {
    case ConsoleTileDestination::library: return "library";
    case ConsoleTileDestination::home: return "home";
    case ConsoleTileDestination::emulator: return "emulator";
    }
    return {};
}
// PS5 and PS4 already belong to the console's native game hub. Their isShelf
// shortcuts may open isShelf, but must never be repurposed as emulator-menu
// launchers even if a future catalog accidentally associates a menu with them.
inline bool console_tile_isshelf_only(std::size_t system) {
    return system < system_count &&
           (catalog[system].id == "ps5" || catalog[system].id == "ps4");
}
inline bool console_tile_intent_valid(const ConsoleTileIntent& intent) {
    if (intent.system >= system_count || !launch_bounded(intent.emulator_title))
        return false;
    if (intent.destination == ConsoleTileDestination::emulator)
        return !console_tile_isshelf_only(intent.system) &&
               launch_title_valid(intent.emulator_title) && launch_menu_known(intent.emulator_title) &&
               tile_job::pinned_emulator_menu(intent.system, intent.emulator_title);
    return (intent.destination == ConsoleTileDestination::library || intent.destination == ConsoleTileDestination::home) &&
           !intent.emulator_title[0];
}
// RetroArch's PS5 forwarder contract can select a core only together with
// content.  A console-level Home tile has no game to supply, so opening
// PPSA99169 can only reach RetroArch's generic menu; it cannot truthfully open
// with this console selected.  Keep legacy routes parseable for ownership and
// migration, but never create or save a new direct system-menu route to it.
inline constexpr char console_tile_shared_retroarch_title[] = "PPSA99169";
inline bool console_tile_emulator_menu_selects_system(std::string_view title) {
    return title != console_tile_shared_retroarch_title;
}
inline bool console_tile_creation_intent_valid(const ConsoleTileIntent& intent) {
    return console_tile_intent_valid(intent) &&
           (intent.destination != ConsoleTileDestination::emulator ||
            console_tile_emulator_menu_selects_system(intent.emulator_title));
}
inline bool normalize_console_tile_intent(const ConsoleTileIntent& source,
                                          ConsoleTileIntent& output) {
    if (!console_tile_intent_valid(source)) return false;
    output = source;
    if (source.destination == ConsoleTileDestination::emulator &&
        !console_tile_emulator_menu_selects_system(source.emulator_title)) {
        output.destination = ConsoleTileDestination::library;
        output.emulator_title[0] = 0;
    }
    return console_tile_creation_intent_valid(output);
}
inline bool console_tile_intents_equal(const ConsoleTileIntent& left, const ConsoleTileIntent& right) {
    return left.system == right.system && left.destination == right.destination &&
           std::strcmp(left.emulator_title, right.emulator_title) == 0;
}
inline bool console_tile_fields(std::string_view system, std::string_view destination, std::string_view emulator,
                                ConsoleTileIntent& result) {
    ConsoleTileIntent parsed;
    for (std::size_t i = 0; i < system_count; ++i)
        if (catalog[i].id == system) {
            parsed.system = i;
            break;
        }
    if (destination == "library") parsed.destination = ConsoleTileDestination::library;
    else if (destination == "home") parsed.destination = ConsoleTileDestination::home;
    else if (destination == "emulator") parsed.destination = ConsoleTileDestination::emulator;
    else return false;
    if (parsed.destination == ConsoleTileDestination::emulator) {
        if (emulator.size() >= sizeof(parsed.emulator_title))
            return false;
        std::memcpy(parsed.emulator_title, emulator.data(), emulator.size());
    } else if (emulator != "-")
        return false;
    if (!console_tile_intent_valid(parsed))
        return false;
    result = parsed;
    return true;
}
inline bool console_tile_take_field(std::string_view& input, std::string_view prefix, std::string_view& value) {
    if (input.size() < prefix.size() || std::memcmp(input.data(), prefix.data(), prefix.size()) != 0)
        return false;
    input.remove_prefix(prefix.size());
    const auto end = input.find('\n');
    if (end == input.npos)
        return false;
    value = std::string_view(input.data(), end);
    input.remove_prefix(end + 1);
    return true;
}
inline bool parse_console_tile_marker(std::string_view input, ConsoleTileIntent& result) {
    if (input.size() > console_tile_message_capacity || input.size() < console_tile_header.size() ||
        std::memcmp(input.data(), console_tile_header.data(), console_tile_header.size()) != 0)
        return false;
    input.remove_prefix(console_tile_header.size());
    std::string_view system, destination, emulator;
    return console_tile_take_field(input, "system=", system) &&
           console_tile_take_field(input, "destination=", destination) &&
           console_tile_take_field(input, "emulator=", emulator) && input.empty() &&
           console_tile_fields(system, destination, emulator, result);
}
inline bool parse_console_tile_arg(std::string_view input, ConsoleTileIntent& result) {
    if (input.size() > console_tile_message_capacity || input.size() < console_tile_arg_prefix.size() ||
        std::memcmp(input.data(), console_tile_arg_prefix.data(), console_tile_arg_prefix.size()) != 0)
        return false;
    input.remove_prefix(console_tile_arg_prefix.size());
    const auto first = input.find(',');
    if (first == input.npos) return false;
    const auto second = input.find(',', first + 1);
    if (second == input.npos || input.find(',', second + 1) != input.npos) return false;
    return console_tile_fields(std::string_view(input.data(), first),
                               std::string_view(input.data() + first + 1, second - first - 1),
                               std::string_view(input.data() + second + 1, input.size() - second - 1), result);
}
inline bool encode_console_tile_marker(const ConsoleTileIntent& intent, char* output, std::size_t capacity) {
    if (!output || !capacity || !console_tile_intent_valid(intent))
        return false;
    const auto id = catalog[intent.system].id;
    const auto destination = console_tile_destination_name(intent.destination);
    const char* emulator = intent.destination == ConsoleTileDestination::emulator ? intent.emulator_title : "-";
    const int used = std::snprintf(output, capacity,
                                   "ISSHELF-CONSOLE-TILE-1\nsystem=%.*s\ndestination=%.*s\nemulator=%s\n",
                                   static_cast<int>(id.size()), id.data(), static_cast<int>(destination.size()),
                                   destination.data(), emulator);
    return used > 0 && static_cast<std::size_t>(used) < capacity &&
           static_cast<std::size_t>(used) <= console_tile_message_capacity;
}
inline bool encode_console_tile_arg(const ConsoleTileIntent& intent, char* output, std::size_t capacity) {
    if (!output || !capacity || !console_tile_intent_valid(intent))
        return false;
    const auto id = catalog[intent.system].id;
    const auto destination = console_tile_destination_name(intent.destination);
    const char* emulator = intent.destination == ConsoleTileDestination::emulator ? intent.emulator_title : "-";
    const int used = std::snprintf(output, capacity, "--isshelf-console-tile=%.*s,%.*s,%s",
                                   static_cast<int>(id.size()), id.data(), static_cast<int>(destination.size()),
                                   destination.data(), emulator);
    return used > 0 && static_cast<std::size_t>(used) < capacity &&
           static_cast<std::size_t>(used) <= console_tile_message_capacity;
}
inline bool encode_console_tile_fixed_home_arg(std::size_t system, char* output,
                                               std::size_t capacity) {
    if (!output || !capacity || system >= system_count ||
        !tile_job::catalog_pin_matches(system)) return false;
    const auto id = catalog[system].id;
    const int used = std::snprintf(output, capacity, "%.*s%.*s",
        static_cast<int>(console_tile_fixed_home_arg_prefix.size()),
        console_tile_fixed_home_arg_prefix.data(), static_cast<int>(id.size()), id.data());
    return used > 0 && static_cast<std::size_t>(used) < capacity &&
           static_cast<std::size_t>(used) <= console_tile_message_capacity;
}
inline bool parse_console_tile_fixed_home_arg(std::string_view input,
                                              std::size_t& system) {
    if (input.size() > console_tile_message_capacity ||
        input.size() <= console_tile_fixed_home_arg_prefix.size() ||
        input.substr(0, console_tile_fixed_home_arg_prefix.size()) !=
            console_tile_fixed_home_arg_prefix) return false;
    const auto id = input.substr(console_tile_fixed_home_arg_prefix.size());
    for (std::size_t i = 0; i < system_count; ++i)
        if (catalog[i].id == id && tile_job::catalog_pin_matches(i)) {
            system = i;
            return true;
        }
    return false;
}
inline FixedHomeInput select_console_tile_fixed_home_arg(int argc, char* const argv[],
                                                           std::size_t& system) {
    system = system_count;
    if (argc < 0 || argc > 64 || (argc > 0 && !argv)) return FixedHomeInput::invalid;
    bool found = false;
    for (int i = 0; i < argc; ++i) {
        if (!argv[i]) return FixedHomeInput::invalid;
        if (std::strncmp(argv[i], console_tile_fixed_home_arg_option.data(),
                         console_tile_fixed_home_arg_option.size()) != 0) continue;
        if (found) return FixedHomeInput::invalid;
        const void* end = std::memchr(argv[i], 0, console_tile_message_capacity + 1);
        if (!end || !parse_console_tile_fixed_home_arg(
                std::string_view(argv[i], static_cast<const char*>(end) - argv[i]),
                system)) return FixedHomeInput::invalid;
        found = true;
    }
    return found ? FixedHomeInput::valid : FixedHomeInput::absent;
}
inline ConsoleTileInput read_console_tile_marker(const char* path, ConsoleTileIntent& result) {
    if (!path) return ConsoleTileInput::invalid;
    FILE* file = std::fopen(path, "rb");
    if (!file) return errno == ENOENT ? ConsoleTileInput::absent : ConsoleTileInput::invalid;
    char contents[console_tile_message_capacity + 1];
    const auto used = std::fread(contents, 1, sizeof(contents), file);
    const bool readable = !std::ferror(file);
    const bool closed = std::fclose(file) == 0;
    const bool okay = readable && closed;
    if (!okay || used > console_tile_message_capacity ||
        !parse_console_tile_marker(std::string_view(contents, used), result))
        return ConsoleTileInput::invalid;
    return ConsoleTileInput::valid;
}
inline ConsoleTileInput select_console_tile_intent(int argc, char* const argv[], const char* marker_path,
                                                   ConsoleTileIntent& result, bool* marker_used = nullptr) {
    ConsoleTileIntent marker, argument;
    const auto file = read_console_tile_marker(marker_path, marker);
    if (marker_used) *marker_used = false;
    if (argc < 0 || argc > 64 || (argc > 0 && !argv))
        return ConsoleTileInput::invalid;
    std::size_t fixed_home_system = system_count;
    const auto fixed_home = select_console_tile_fixed_home_arg(argc, argv,
                                                               fixed_home_system);
    if (fixed_home == FixedHomeInput::invalid) return ConsoleTileInput::invalid;
    bool has_argument = false;
    constexpr std::string_view option = "--isshelf-console-tile";
    // The PS5 CRT can pass the first launch argument as argv[0], without a
    // conventional executable name. Host builds may still put one there.
    for (int i = 0; i < argc; ++i) {
        if (!argv[i]) return ConsoleTileInput::invalid;
        if (std::strncmp(argv[i], option.data(), option.size()) != 0)
            continue;
        if (has_argument) return ConsoleTileInput::invalid;
        const void* end = std::memchr(argv[i], 0, console_tile_message_capacity + 1);
        if (!end || !parse_console_tile_arg(std::string_view(argv[i],
                                             static_cast<const char*>(end) - argv[i]), argument))
            return ConsoleTileInput::invalid;
        has_argument = true;
    }
    if (file == ConsoleTileInput::invalid ||
        (file == ConsoleTileInput::valid && has_argument && !console_tile_intents_equal(marker, argument)) ||
        (fixed_home == FixedHomeInput::valid &&
         (file != ConsoleTileInput::absent || !has_argument ||
          argument.destination != ConsoleTileDestination::home ||
          argument.system != fixed_home_system)))
        return ConsoleTileInput::invalid;
    if (file == ConsoleTileInput::valid) {
        result = marker;
        if (marker_used) *marker_used = true;
    }
    else if (has_argument) result = argument;
    else return ConsoleTileInput::absent;
    return ConsoleTileInput::valid;
}
} // namespace shelf
