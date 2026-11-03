// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "launch_protocol.hpp"
#include "library.hpp"
#include <cstdio>
#include <cstring>

namespace shelf {
enum class LaunchPlan { unavailable, menu, direct_game, native_game };
struct LaunchCaption {
    const char* action;
    const char* badge;
    const char* expectation;
};
inline LaunchCaption launch_caption(LaunchPlan plan) {
    switch (plan) {
    case LaunchPlan::menu:
        return {"Open emulator library", "LIBRARY ONLY",
                "Choose the game there. Circle returns to your emulator choices."};
    case LaunchPlan::direct_game:
        return {"Play game", "DIRECT PLAY", "The selected game opens directly in your chosen emulator."};
    case LaunchPlan::native_game:
        return {"Open game", "DIRECT PLAY", "The selected game opens directly on your PS5."};
    default:
        return {"No launch available", "UNAVAILABLE", "Choose a compatible installed emulator to play this game."};
    }
}
inline bool launch_route_folder(const EmulatorRoute& route) {
    if (!launch_bounded(route.title_id) || !launch_bounded(route.folder) || !launch_menu_known(route.title_id))
        return false;
    char root[path_capacity];
    for (const char* parent : {"/data/homebrew", "/user/app"}) {
        std::snprintf(root, sizeof(root), "%s/%s", parent, route.title_id);
        if (std::strcmp(root, route.folder) == 0)
            return true;
    }
    return false;
}
inline LaunchPlan plan_emulator(const Library& library, std::size_t system, std::string_view preference,
                                LaunchRequest& request) {
    const auto* route = preferred_route(library, system, preference);
    if (!route || !launch_route_folder(*route))
        return LaunchPlan::unavailable;
    request = {};
    std::snprintf(request.title, sizeof(request.title), "%s", route->title_id);
    return LaunchPlan::menu;
}
inline LaunchPlan plan_game(const Library& library, std::size_t index, std::string_view preference,
                            LaunchRequest& request) {
    if (index >= library.game_count)
        return LaunchPlan::unavailable;
    const auto& game = library.games[index];
    if (!launch_bounded(game.title_id) || !launch_bounded(game.path) || game.system >= system_count)
        return LaunchPlan::unavailable;
    if (game.kind == MediaKind::native_title) {
        if (!launch_title_valid(game.title_id))
            return LaunchPlan::unavailable;
        char path[path_capacity];
        std::snprintf(path, sizeof(path), "/user/app/%s", game.title_id);
        const auto system = find_system(std::string_view(game.title_id).substr(0, 4) == "PPSA" ? "ps5" : "ps4");
        if (game.system != system || std::strcmp(path, game.path) != 0)
            return LaunchPlan::unavailable;
        request = {};
        request.mode = LaunchMode::native_title;
        std::snprintf(request.title, sizeof(request.title), "%s", game.title_id);
        return LaunchPlan::native_game;
    }
    if (plan_emulator(library, game.system, preference, request) == LaunchPlan::unavailable)
        return LaunchPlan::unavailable;
    const auto* route = preferred_route(library, game.system, preference);
    if (std::strcmp(route->title_id, "PPSA99248") == 0) {
        if (game.system != find_system("ps1") || !launch_ps1_path(game.path))
            return LaunchPlan::unavailable;
        const int used = std::snprintf(request.rom, sizeof(request.rom), "%.*s%s",
                                       static_cast<int>(psswan_ftp_origin.size()), psswan_ftp_origin.data(), game.path);
        if (used <= 0 || static_cast<std::size_t>(used) >= sizeof(request.rom))
            return LaunchPlan::unavailable;
        request.mode = LaunchMode::psswan_game;
        return LaunchPlan::direct_game;
    }
    if (std::strcmp(route->title_id, "PPSA99169") != 0)
        return LaunchPlan::menu;
    if (!launch_bounded(route->core) || !launch_media_path(game.path))
        return LaunchPlan::unavailable;
    char prefix[path_capacity];
    std::snprintf(prefix, sizeof(prefix), "%s/cores/", route->folder);
    const std::string_view core_path(route->core), base(prefix);
    if (core_path.size() <= base.size() || core_path.substr(0, base.size()) != base)
        return LaunchPlan::unavailable;
    auto core = core_path.substr(base.size());
    constexpr std::string_view suffix = "_libretro.so";
    if (core.size() <= suffix.size() || core.substr(core.size() - suffix.size()) != suffix)
        return LaunchPlan::unavailable;
    core.remove_suffix(suffix.size());
    if (core.size() >= sizeof(request.core))
        return LaunchPlan::unavailable;
    for (char c : core)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return LaunchPlan::unavailable;
    request.mode = LaunchMode::retroarch_game;
    std::snprintf(request.rom, sizeof(request.rom), "%s", game.path);
    std::memcpy(request.core, core.data(), core.size());
    request.core[core.size()] = 0;
    return LaunchPlan::direct_game;
}
} // namespace shelf
