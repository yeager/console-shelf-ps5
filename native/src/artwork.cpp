// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/artwork.hpp"
#include "shelf/memory.hpp"
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace shelf {
namespace {
bool directory(const char* path) {
#ifdef _WIN32
    return _mkdir(path) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0700) == 0 || errno == EEXIST;
#endif
}
bool cached_image(const char* path) {
    int w = 0, h = 0, channels = 0;
    return stbi_info(path, &w, &h, &channels) && w > 0 && h > 0 && w <= 960 && h <= 960;
}
std::uint64_t identity(const char* path, std::uint64_t bytes, std::int64_t modified) {
    std::uint64_t value = 14695981039346656037ull;
    for (const char* c = path; *c; ++c) value = (value ^ static_cast<unsigned char>(*c)) * 1099511628211ull;
    value ^= bytes;
    value *= 1099511628211ull;
    return value ^ static_cast<std::uint64_t>(modified);
}
bool same_tag(const char* path, const char* wanted) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    char actual[32]{};
    const auto count = std::fread(actual, 1, sizeof(actual), file);
    std::fclose(file);
    return count == std::strlen(wanted) && std::memcmp(actual, wanted, count) == 0;
}
void quoted(FILE* file, const char* text) {
    std::fputc('"', file);
    for (const unsigned char* c = reinterpret_cast<const unsigned char*>(text); *c; ++c) {
        if (*c == '"' || *c == '\\') std::fputc('\\', file);
        if (*c < 32) std::fprintf(file, "\\u%04x", static_cast<unsigned>(*c));
        else std::fputc(*c, file);
    }
    std::fputc('"', file);
}
bool thumbnail(const unsigned char* encoded, std::size_t bytes, const char* path) {
    int w = 0, h = 0, channels = 0;
    if (!stbi_info_from_memory(encoded, static_cast<int>(bytes), &w, &h, &channels) ||
        w < 1 || h < 1 || w > 8192 || h > 8192 || static_cast<std::uint64_t>(w) * h > 16 * 1024 * 1024)
        return false;
    auto* input = stbi_load_from_memory(encoded, static_cast<int>(bytes), &w, &h, &channels, 4);
    if (!input) return false;
    const double scale = std::min(1.0, 960.0 / std::max(w, h));
    const int tw = std::max(1, static_cast<int>(w * scale)), th = std::max(1, static_cast<int>(h * scale));
    auto* pixels = static_cast<unsigned char*>(allocate(static_cast<std::size_t>(tw) * th * 4));
    if (!pixels) { stbi_image_free(input); return false; }
    for (int y = 0; y < th; ++y) {
        const double sy = std::max(0.0, (y + .5) * h / th - .5);
        const int y0 = static_cast<int>(sy), y1 = std::min(h - 1, y0 + 1);
        const double fy = sy - y0;
        for (int x = 0; x < tw; ++x) {
            const double sx = std::max(0.0, (x + .5) * w / tw - .5);
            const int x0 = static_cast<int>(sx), x1 = std::min(w - 1, x0 + 1);
            const double fx = sx - x0;
            for (int c = 0; c < 4; ++c) {
                const double a = input[(static_cast<std::size_t>(y0) * w + x0) * 4 + c] * (1 - fx) +
                                 input[(static_cast<std::size_t>(y0) * w + x1) * 4 + c] * fx;
                const double b = input[(static_cast<std::size_t>(y1) * w + x0) * 4 + c] * (1 - fx) +
                                 input[(static_cast<std::size_t>(y1) * w + x1) * 4 + c] * fx;
                pixels[(static_cast<std::size_t>(y) * tw + x) * 4 + c] = static_cast<unsigned char>(a * (1 - fy) + b * fy + .5);
            }
        }
    }
    const bool ok = stbi_write_png(path, tw, th, 4, pixels, tw * 4) != 0;
    release(pixels);
    stbi_image_free(input);
    return ok;
}
bool cache_spine(const ReadOnlyFs& fs, const LibraryGame& game, const char* folder, std::size_t& transferred,
                 bool (*cancelled)(void*), void* context) {
    if (game.artwork_kind != ArtworkKind::front || !game.artwork[0] || !fs.stat) return false;
    const char* dot = std::strrchr(game.artwork, '.');
    if (!dot) return false;
    char source[path_capacity];
    FileStat info;
    bool found = false;
    for (const char* ext : {".jpg", ".png", ".jpeg", ".JPG", ".PNG", ".JPEG"}) {
        const int used = std::snprintf(source, sizeof(source), "%.*s_spine%s", static_cast<int>(dot - game.artwork), game.artwork, ext);
        if (used < 1 || static_cast<std::size_t>(used) >= sizeof(source) || !valid_artwork_source(source)) continue;
        if (cancelled && cancelled(context)) return false;
        if (fs.stat(fs.context, source, info) == FsStatus::ok && !info.directory && info.size && info.size <= 8 * 1024 * 1024) {
            found = true; break;
        }
    }
    if (!found) return false;
    char path[2120], tag_path[2120], temporary[2120], tag[32];
    const auto key = static_cast<unsigned long long>(game_favorite_key(game));
    std::snprintf(path, sizeof(path), "%s/%016llx.spine.png", folder, key);
    std::snprintf(tag_path, sizeof(tag_path), "%s/%016llx.spine.tag", folder, key);
    std::snprintf(temporary, sizeof(temporary), "%s/%016llx.spine.tmp", folder, key);
    std::snprintf(tag, sizeof(tag), "%016llx\n", static_cast<unsigned long long>(identity(source, info.size, info.modified)));
    if (cached_image(path) && same_tag(tag_path, tag)) return true;
    if (transferred + info.size > 64 * 1024 * 1024) return false;
    auto* encoded = static_cast<unsigned char*>(allocate(static_cast<std::size_t>(info.size)));
    if (!encoded) return false;
    std::size_t used = 0;
    bool ok = fs.read(fs.context, source, 0, encoded, static_cast<std::size_t>(info.size), used) == FsStatus::ok && used == info.size;
    transferred += used;
    ok = ok && !(cancelled && cancelled(context)) && thumbnail(encoded, used, temporary);
    release(encoded);
    if (ok) {
        std::remove(path);
        ok = std::rename(temporary, path) == 0;
        if (ok) if (FILE* file = std::fopen(tag_path, "wb")) {
            std::fwrite(tag, 1, std::strlen(tag), file); std::fclose(file);
        }
    }
    if (!ok) std::remove(temporary);
    return ok;
}
} // namespace
void cache_artwork(const ReadOnlyFs& fs, Library& library, const char* data_root,
                   bool (*cancelled)(void*), void* context) {
    if (!data_root || !*data_root || std::strlen(data_root) > 2000 || !fs.read) return;
    char folder[2048];
    std::snprintf(folder, sizeof(folder), "%s/covers", data_root);
    if (!directory(folder)) return;
    std::size_t transferred = 0;
    for (std::size_t i = 0; i < library.game_count; ++i) {
        if (cancelled && cancelled(context)) return;
        auto& game = library.games[i];
        const auto key = game_favorite_key(game);
        char path[2120], tag_path[2120], temporary[2120], tag[32];
        std::snprintf(path, sizeof(path), "%s/%016llx.png", folder, static_cast<unsigned long long>(key));
        std::snprintf(tag_path, sizeof(tag_path), "%s/%016llx.tag", folder, static_cast<unsigned long long>(key));
        std::snprintf(temporary, sizeof(temporary), "%s/%016llx.tmp", folder, static_cast<unsigned long long>(key));
        std::snprintf(tag, sizeof(tag), "%016llx\n", static_cast<unsigned long long>(identity(game.artwork, game.artwork_bytes, game.artwork_modified)));
        if (cached_image(path) && (!game.artwork[0] || same_tag(tag_path, tag))) {
            game.artwork_ready = true;
            ++library.artwork.cached;
            continue;
        }
        if (!game.artwork[0]) { ++library.artwork.missing; continue; }
        if (!valid_artwork_source(game.artwork) || !game.artwork_bytes || game.artwork_bytes > 8 * 1024 * 1024 ||
            transferred + game.artwork_bytes > 64 * 1024 * 1024) { ++library.artwork.failed; continue; }
        const auto bytes = static_cast<std::size_t>(game.artwork_bytes);
        auto* encoded = static_cast<unsigned char*>(allocate(bytes));
        if (!encoded) { ++library.artwork.failed; continue; }
        std::size_t used = 0;
        bool ok = fs.read(fs.context, game.artwork, 0, encoded, bytes, used) == FsStatus::ok && used == bytes;
        transferred += used;
        if (ok && !(cancelled && cancelled(context))) ok = thumbnail(encoded, bytes, temporary);
        else ok = false;
        release(encoded);
        if (ok && cached_image(temporary)) {
            // Re-creatable cache only; the complete new image is checked before replacement.
            std::remove(path);
            ok = std::rename(temporary, path) == 0;
            if (ok) {
                if (FILE* file = std::fopen(tag_path, "wb")) {
                    std::fwrite(tag, 1, std::strlen(tag), file);
                    std::fclose(file);
                }
                game.artwork_ready = true;
                ++library.artwork.cached;
            }
        } else ok = false;
        if (!ok) { ++library.artwork.failed; std::remove(temporary); }
    }
    for (std::size_t i = 0; i < library.game_count; ++i) {
        if (cancelled && cancelled(context)) return;
        auto& game = library.games[i];
        game.artwork_spine_ready = game.artwork_ready && cache_spine(fs, game, folder, transferred, cancelled, context);
    }
    char index[2120];
    std::snprintf(index, sizeof(index), "%s/index.json", folder);
    if (FILE* file = std::fopen(index, "wb")) {
        std::fputs("{\"games\":[", file);
        for (std::size_t i = 0; i < library.game_count; ++i) {
            const auto& game = library.games[i];
            std::fprintf(file, "%s{\"key\":\"%016llx\",\"system\":", i ? "," : "", static_cast<unsigned long long>(game_favorite_key(game)));
            char system[40];
            std::snprintf(system, sizeof(system), "%.*s", static_cast<int>(catalog[game.system].id.size()), catalog[game.system].id.data());
            quoted(file, system);
            std::fputs(",\"name\":", file); quoted(file, game.name);
            std::fputs(",\"path\":", file); quoted(file, game.path);
            std::fputs(",\"source\":", file); quoted(file, game.artwork);
            std::fprintf(file, ",\"ready\":%s,\"kind\":%u,\"spineReady\":%s}", game.artwork_ready ? "true" : "false",
                         static_cast<unsigned>(game.artwork_kind), game.artwork_spine_ready ? "true" : "false");
        }
        std::fputs("]}\n", file);
        std::fclose(file);
    }
}
} // namespace shelf
