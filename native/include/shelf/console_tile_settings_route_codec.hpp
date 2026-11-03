// SPDX-License-Identifier: GPL-3.0-only
// Codec for optional console Home shortcut routes in settings.ini.
// It does not open, save, or authorize changes to a settings file or tile.
#pragma once
#include "console_tile_route_preference.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace shelf::tile_route::settings_codec {
inline constexpr std::string_view key_prefix = "route.";
inline constexpr std::size_t maximum_settings_bytes = 32768;
inline constexpr std::size_t maximum_settings_line = 255;

struct Slot {
    Input input = Input::absent;
    Preference preference{};
};
struct Scan {
    bool file_valid = false;
    std::array<Slot, system_count> routes{};
};

inline bool parse_value(std::size_t system, std::string_view value,
                        Preference& output) {
    Preference parsed;
    parsed.system = system;
    if (value == "library") parsed.destination = ConsoleTileDestination::library;
    else if (value == "home") parsed.destination = ConsoleTileDestination::home;
    else {
        constexpr std::string_view emulator_prefix = "emulator:";
        if (value.substr(0, emulator_prefix.size()) != emulator_prefix)
            return false;
        value.remove_prefix(emulator_prefix.size());
        if (value.empty() || value.size() >= sizeof(parsed.emulator_title))
            return false;
        parsed.destination = ConsoleTileDestination::emulator;
        std::memcpy(parsed.emulator_title, value.data(), value.size());
        // UI98-UI101 could save a console-level RetroArch destination even
        // though RetroArch cannot select a system without a game.  Read that
        // one legacy value as the matching isShelf library so the next normal
        // settings save completes the migration without losing other choices.
        if (!console_tile_emulator_menu_selects_system(parsed.emulator_title)) {
            ConsoleTileIntent legacy{system, ConsoleTileDestination::emulator, {}};
            std::memcpy(legacy.emulator_title, parsed.emulator_title,
                        sizeof(legacy.emulator_title));
            if (!console_tile_intent_valid(legacy)) return false;
            parsed.destination = ConsoleTileDestination::library;
            parsed.emulator_title[0] = 0;
        }
    }
    if (!preference_valid(parsed)) return false;
    output = parsed;
    return true;
}

inline bool encode_line(const Preference& preference, char* output,
                        std::size_t capacity) {
    if (!output || !capacity || !preference_valid(preference)) return false;
    const auto id = catalog[preference.system].id;
    const auto destination = console_tile_destination_name(preference.destination);
    const int used = preference.destination == ConsoleTileDestination::emulator
        ? std::snprintf(output, capacity, "route.%.*s=%.*s:%s\n",
                        static_cast<int>(id.size()), id.data(),
                        static_cast<int>(destination.size()), destination.data(),
                        preference.emulator_title)
        : std::snprintf(output, capacity, "route.%.*s=%.*s\n",
                        static_cast<int>(id.size()), id.data(),
                        static_cast<int>(destination.size()), destination.data());
    return used > 0 && static_cast<std::size_t>(used) < capacity &&
           static_cast<std::size_t>(used) <= maximum_settings_line;
}

// Call only after the existing load_settings has accepted the same file.
// Other schema-1 keys remain the existing parser's responsibility. A damaged
// route value invalidates that console's choice; a malformed file/unknown
// route identity invalidates the entire route scan. A duplicate never wins.
inline Scan scan_routes(std::string_view text) {
    Scan scan;
    if (text.size() > maximum_settings_bytes ||
        text.find('\0') != std::string_view::npos) return scan;
    bool version_seen = false;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto end = text.find('\n', offset);
        const auto count = end == std::string_view::npos ? text.size() - offset
                                                        : end - offset + 1;
        if (count > maximum_settings_line) return scan;
        auto line = text.substr(offset, count);
        offset += count;
        if (!line.empty() && line.back() == '\n') line.remove_suffix(1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.find('\r') != std::string_view::npos) return scan;
        if (line.empty() || line.front() == '#') continue;
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) return scan;
        const auto key = line.substr(0, equals);
        const auto value = line.substr(equals + 1);
        if (key == "version") {
            if (version_seen || value != "1") return scan;
            version_seen = true;
            continue;
        }
        if (key.substr(0, key_prefix.size()) != key_prefix) continue;
        const auto system = system_for(key.substr(key_prefix.size()));
        if (system >= system_count || !source_valid({system})) return scan;
        auto& slot = scan.routes[system];
        if (slot.input != Input::absent) {
            slot.input = Input::invalid;
            slot.preference = Preference{};
            continue;
        }
        Preference parsed;
        if (!parse_value(system, value, parsed)) {
            slot.input = Input::invalid;
            continue;
        }
        slot.input = Input::valid;
        slot.preference = parsed;
    }
    scan.file_valid = version_seen;
    return scan;
}
} // namespace shelf::tile_route::settings_codec
