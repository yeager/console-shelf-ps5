// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "model.hpp"
#include "console_tile_settings_route_codec.hpp"
#include <cstdint>

namespace shelf {
struct Settings {
    Preferences preferences;
    std::array<std::array<char, 48>, system_count> preferred_emulator{};
    // Saved Home-shortcut destinations are untrusted UI choices only. They
    // never authorize a tile, helper, or database mutation.
    std::array<tile_route::settings_codec::Slot, system_count> shortcut_routes{};
    char tv_text[96] = "Welcome to your play room";
    std::array<std::uint64_t, 64> game_favorites{};
};
enum class SettingsLoad { loaded, missing, backup, invalid };
// Reads only the named owned settings file and its backup. Never writes during load.
SettingsLoad load_settings(const char* path, Settings& output);
// Same-directory temporary file, flush, backup of a valid previous file, atomic replace.
bool save_settings(const char* path, const Settings& settings);
} // namespace shelf
