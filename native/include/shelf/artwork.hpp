// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "library.hpp"
#include <cstdio>
namespace shelf {
// Resolve existing artwork only. Optional art never changes game discovery or launch capability.
void discover_artwork(const ReadOnlyFs& files, Library& library);
bool valid_artwork_source(std::string_view path);
void cache_artwork(const ReadOnlyFs& files, Library& library, const char* data_root,
                   bool (*cancelled)(void*) = nullptr, void* context = nullptr);
inline void artwork_name(const LibraryGame& game, char (&out)[32]) {
    std::snprintf(out, sizeof(out), "@cover/%016llx", static_cast<unsigned long long>(game_favorite_key(game)));
}
inline void spine_name(const LibraryGame& game, char (&out)[32]) {
    std::snprintf(out, sizeof(out), "@spine/%016llx", static_cast<unsigned long long>(game_favorite_key(game)));
}
} // namespace shelf
