// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "model.hpp"
#include <cstdint>

namespace shelf {
constexpr std::size_t path_capacity = 768, max_games = 8192, max_routes = 256;
enum class FsStatus { ok, missing, denied, invalid, limit, failed };
struct FileStat {
    bool directory = false;
    std::uint64_t size = 0;
    std::int64_t modified = 0;
};
using EntryVisitor = bool (*)(void*, const char*, const FileStat&);
// All operations are read-only. The virtual path namespace uses absolute PS5 paths.
struct ReadOnlyFs {
    void* context = nullptr;
    FsStatus (*list)(void*, const char*, EntryVisitor, void*) = nullptr;
    FsStatus (*stat)(void*, const char*, FileStat&) = nullptr;
    FsStatus (*read)(void*, const char*, std::uint64_t, void*, std::size_t, std::size_t&) = nullptr;
    bool cached = false;
};
enum class MediaKind { cartridge, disc_image, cue, playlist, ps3_folder, native_title };
enum class ArtworkKind { front, case_render, icon, screenshot };
struct EmulatorRoute {
    std::size_t system = 0;
    char id[48]{}, name[96]{}, title_id[16]{}, folder[path_capacity]{}, core[path_capacity]{};
    // Discovery never upgrades this to a tested launch capability.
    LaunchCapability capability = LaunchCapability::unavailable;
};
struct LibraryGame {
    std::size_t system = 0;
    char name[256]{}, path[path_capacity]{}, title_id[32]{};
    MediaKind kind = MediaKind::cartridge;
    std::uint64_t size = 0;
    char artwork[path_capacity]{};
    std::uint64_t artwork_bytes = 0;
    std::int64_t artwork_modified = 0;
    bool artwork_ready = false;
    ArtworkKind artwork_kind = ArtworkKind::front;
    bool artwork_spine_ready = false;
};
struct ArtworkSummary {
    std::size_t found = 0, cached = 0, missing = 0, failed = 0;
};
// Stable local preference identity; this is not a security or file-integrity hash.
std::uint64_t game_favorite_key(const LibraryGame& game);
struct ScanSummary {
    std::size_t roots_read = 0, roots_denied = 0, files_examined = 0, rejected = 0, missing_references = 0;
    bool complete = true, limit_reached = false;
    bool cached = false;
};
struct Library {
    std::array<EmulatorRoute, max_routes> routes{};
    std::array<LibraryGame, max_games> games{};
    std::size_t route_count = 0, game_count = 0;
    Inventory inventory{};
    ScanSummary scan;
    ArtworkSummary artwork;
};
// No source directory is created or changed. Caller owns this comparatively large object.
void discover_library(const ReadOnlyFs& files, Library& result);
bool valid_virtual_path(std::string_view path);
bool join_media_path(std::string_view base, std::string_view reference, char* output, std::size_t capacity);
const EmulatorRoute* preferred_route(const Library& library, std::size_t system, std::string_view id);
struct GameList {
    std::array<std::size_t, max_games> indices{};
    std::size_t count = 0;
};
GameList games_for_system(const Library& library, std::size_t system);
} // namespace shelf
