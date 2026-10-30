// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "catalog.generated.hpp"
#include "localization.hpp"

namespace shelf {
enum class Layout { grid, swipe, shelf };
inline constexpr std::array<std::string_view, 3> layout_labels = {"Grid", "Swipe", "Shelf"};
inline constexpr Layout next_layout(Layout layout, int direction = 1) {
    return static_cast<Layout>((static_cast<int>(layout) + direction % 3 + 3) % 3);
}
enum class GridLook { icons, cards };
enum class Scope { all, available, favorites };
inline constexpr std::size_t shelf_capacity = 8;
enum class Sort { featured, name_asc, name_desc, newest, oldest, brand, games, favorites };
enum class LaunchCapability { unavailable, emulator_menu, direct_game };
enum class SystemFilter { all, emulators, games, built_in, no_route };
enum class AppIcon { gaming, classic, jade, vector, count };
enum class TvMode { console, intro, picture, clock, message, favorites, off, video };
inline constexpr std::array<std::string_view, 8> tv_labels = {
    "Selected console", "Console intro picture", "My picture", "Clock & Shelf logo", "My message", "Favourite games", "Screen off", "Console intro video"};
enum class TvNoise { off, emulator, games, either };
inline constexpr std::array<std::string_view, 4> noise_labels = {
    "Off", "No emulator installed", "No games found", "No emulator or no games"};
inline constexpr std::array<unsigned, 4> tv_periods = {10, 20, 30, 60};

// Filled only by runtime adapters. A suggested core is never evidence of installation.
struct Availability {
    bool built_in = false;
    std::size_t emulator_count = 0;
    std::size_t game_count = 0;
    LaunchCapability launch = LaunchCapability::unavailable;
};
using Inventory = std::array<Availability, system_count>;

struct Preferences {
    Language language = Language::english;
    Layout layout = Layout::grid;
    GridLook grid_look = GridLook::icons;
    Scope scope = Scope::all;
    Sort sort = Sort::featured;
    SystemFilter filter = SystemFilter::all;
    AppIcon icon = AppIcon::vector;
    bool monochrome_missing = true;
    bool shelf_single = false;
    bool reduced_motion = false;
    TvMode tv_mode = TvMode::console;
    TvNoise tv_noise = TvNoise::off;
    bool intro_all = false;
    std::size_t tv_period = 1;
    std::size_t brand = 0;
    // These arrays are in-memory state. Persistence must use stable system IDs.
    std::array<bool, system_count> hidden{};
    std::array<bool, system_count> favorite{};
    std::array<bool, system_count> shelf_selected{};
};

struct BrowseRequest {
    std::string_view query;
    std::string_view brand;
};
inline bool tv_noise_for(const Preferences& p, const Availability& a) {
    const bool no_emulator = !a.built_in && !a.emulator_count, no_games = !a.game_count;
    return p.tv_mode != TvMode::off &&
           ((p.tv_noise == TvNoise::emulator && no_emulator) || (p.tv_noise == TvNoise::games && no_games) ||
            (p.tv_noise == TvNoise::either && (no_emulator || no_games)));
}
struct BrowseResult {
    std::array<std::size_t, system_count> indices{};
    std::size_t count = 0;
};

// Returns system_count when absent. Unknown identities must never alias the first system.
std::size_t find_system(std::string_view id);
bool is_available(const Availability& evidence);
bool use_monochrome(const Preferences& preferences, const Availability& evidence, std::string_view system_id = {});
BrowseResult browse(const Preferences& preferences, const Inventory& inventory, BrowseRequest request = {});
} // namespace shelf
