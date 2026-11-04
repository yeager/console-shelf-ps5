// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/settings.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace shelf {
namespace {
constexpr std::array<const char*, 3> layout_names = {"grid", "swipe", "shelf"};
constexpr std::array<const char*, 2> look_names = {"icons", "cards"};
constexpr std::array<const char*, 3> scope_names = {"all", "available", "favorites"};
constexpr std::array<const char*, 8> sort_names = {"featured", "az",    "za",    "newest",
                                                   "oldest",   "brand", "games", "favorites"};
constexpr std::array<const char*, 5> filter_names = {"all", "emulators", "games", "built_in", "no_route"};
constexpr std::array<const char*, 4> icon_names = {"gaming", "classic", "jade", "vector"};
constexpr std::array<const char*, 8> tv_names = {"console", "intro", "picture", "clock", "message", "favorites", "off", "video"};
constexpr std::array<const char*, 4> noise_names = {"off", "emulator", "games", "either"};
constexpr std::array<const char*, 2> language_names = {"en", "sv"};
bool valid_text(const char* text, std::size_t capacity) {
    if (!std::memchr(text, 0, capacity)) return false;
    for (const unsigned char* at = reinterpret_cast<const unsigned char*>(text); *at; ++at)
        if (*at < 32 || *at == 127) return false;
    return true;
}
#ifdef _WIN32
bool wide_path(const char* path, wchar_t (&output)[4096]) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, output, 4096) > 0;
}
FILE* open_file(const char* path, const char* mode) {
    wchar_t wide[4096];
    if (!wide_path(path, wide))
        return nullptr;
    FILE* file = nullptr;
    if (_wfopen_s(&file, wide, std::strcmp(mode, "rb") == 0 ? L"rb" : L"wb") != 0)
        return nullptr;
    return file;
}
bool replace_file(const char* from, const char* to) {
    wchar_t source[4096], target[4096];
    return wide_path(from, source) && wide_path(to, target) &&
           MoveFileExW(source, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
#else
FILE* open_file(const char* path, const char* mode) {
    return std::fopen(path, mode);
}
bool replace_file(const char* from, const char* to) {
    return std::rename(from, to) == 0;
}
#endif
bool flush_file(FILE* file) {
    if (std::fflush(file) != 0)
        return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}
template <std::size_t N> int choice(const char* value, const std::array<const char*, N>& options) {
    for (std::size_t i = 0; i < N; ++i)
        if (std::strcmp(value, options[i]) == 0)
            return static_cast<int>(i);
    return -1;
}
bool valid_route(const char* value) {
    const auto length = std::strlen(value);
    if (length >= 48)
        return false;
    constexpr const char* retroarch = "retroarch:";
    if (std::strncmp(value, retroarch, 10) == 0) {
        if (length == 10)
            return false;
        for (std::size_t i = 10; i < length; ++i) {
            const auto c = value[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
                return false;
        }
        return true;
    }
    for (std::size_t i = 0; i < length; ++i) {
        const auto c = value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.'))
            return false;
    }
    return true;
}
bool valid(const Settings& settings) {
    const auto& p = settings.preferences;
    if (static_cast<unsigned>(p.layout) >= layout_names.size() ||
        static_cast<unsigned>(p.grid_look) >= look_names.size() ||
        static_cast<unsigned>(p.scope) >= scope_names.size() || static_cast<unsigned>(p.sort) >= sort_names.size() ||
        static_cast<unsigned>(p.filter) >= filter_names.size() || static_cast<unsigned>(p.icon) >= icon_names.size() ||
        static_cast<unsigned>(p.language) >= language_names.size() ||
        p.brand >= brands.size() || static_cast<unsigned>(p.tv_mode) >= tv_names.size() ||
        p.tv_period >= tv_periods.size() || static_cast<unsigned>(p.tv_noise) >= noise_names.size() ||
        !valid_text(settings.tv_text, sizeof(settings.tv_text)))
        return false;
    for (const auto& route : settings.preferred_emulator) {
        if (!std::memchr(route.data(), 0, route.size()) || !valid_route(route.data()))
            return false;
    }
    for (std::size_t i = 0; i < system_count; ++i) {
        const auto& slot = settings.shortcut_routes[i];
        if (slot.input != tile_route::Input::absent &&
            slot.input != tile_route::Input::valid &&
            slot.input != tile_route::Input::invalid)
            return false;
        if (slot.input == tile_route::Input::valid &&
            (slot.preference.system != i || !tile_route::preference_valid(slot.preference)))
            return false;
    }
    if (static_cast<std::size_t>(std::count(p.shelf_selected.begin(), p.shelf_selected.end(), true)) > shelf_capacity)
        return false;
    for (std::size_t i = 0; i < settings.game_favorites.size(); ++i)
        if (settings.game_favorites[i] && std::find(settings.game_favorites.begin(), settings.game_favorites.begin() + i,
                                                 settings.game_favorites[i]) != settings.game_favorites.begin() + i)
            return false;
    return true;
}
bool read_file(const char* path, Settings& output) {
    FILE* file = open_file(path, "rb");
    if (!file)
        return false;
    char checked_bytes[32769];
    const auto checked_size = std::fread(checked_bytes, 1, sizeof(checked_bytes), file);
    if (checked_size == sizeof(checked_bytes) || std::ferror(file) || std::memchr(checked_bytes, 0, checked_size)) {
        std::fclose(file);
        return false;
    }
    std::rewind(file);
    Settings parsed;
    constexpr std::size_t room_keys = 11 + system_count * 4;
    std::array<bool, room_keys + 6 + 64> seen{};
    std::array<bool, system_count> shortcut_route_seen{};
    char line[256];
    std::size_t bytes = 0;
    bool okay = true;
    while (std::fgets(line, sizeof(line), file)) {
        const auto length = std::strlen(line);
        bytes += length;
        if (bytes > 32768 || (!std::strchr(line, '\n') && !std::feof(file))) {
            okay = false;
            break;
        }
        line[std::strcspn(line, "\r\n")] = 0;
        if (line[0] == 0 || line[0] == '#')
            continue;
        char* equals = std::strchr(line, '=');
        if (!equals) {
            okay = false;
            break;
        }
        *equals = 0;
        const char* value = equals + 1;
        int key = -1;
        if (std::strcmp(line, "version") == 0) {
            key = 0;
            if (std::strcmp(value, "1") != 0)
                okay = false;
        } else if (std::strcmp(line, "layout") == 0) {
            key = 1;
            int n = choice(value, layout_names);
            if (n < 0)
                okay = false;
            else
                parsed.preferences.layout = static_cast<Layout>(n);
        } else if (std::strcmp(line, "look") == 0) {
            key = 2;
            int n = choice(value, look_names);
            if (n < 0)
                okay = false;
            else
                parsed.preferences.grid_look = static_cast<GridLook>(n);
        } else if (std::strcmp(line, "scope") == 0) {
            key = 3;
            int n = choice(value, scope_names);
            if (n < 0)
                okay = false;
            else
                parsed.preferences.scope = static_cast<Scope>(n);
        } else if (std::strcmp(line, "sort") == 0) {
            key = 4;
            int n = choice(value, sort_names);
            if (n < 0)
                okay = false;
            else
                parsed.preferences.sort = static_cast<Sort>(n);
        } else if (std::strcmp(line, "filter") == 0) {
            key = 5;
            int n = choice(value, filter_names);
            if (n < 0)
                okay = false;
            else
                parsed.preferences.filter = static_cast<SystemFilter>(n);
        } else if (std::strcmp(line, "icon") == 0) {
            key = 6;
            int n = choice(value, icon_names);
            if (n < 0)
                okay = false;
            else
                parsed.preferences.icon = static_cast<AppIcon>(n);
        } else if (std::strcmp(line, "monochrome_missing") == 0) {
            key = static_cast<int>(8 + system_count * 3); // Optional in older schema-1 settings.
            if (std::strcmp(value, "0") != 0 && std::strcmp(value, "1") != 0)
                okay = false;
            else
                parsed.preferences.monochrome_missing = value[0] == '1';
        } else if (std::strcmp(line, "shelf_single") == 0 || std::strcmp(line, "reduced_motion") == 0) {
            const bool single = std::strcmp(line, "shelf_single") == 0;
            key = static_cast<int>(9 + system_count * 3 + (single ? 0 : 1));
            if (std::strcmp(value, "0") != 0 && std::strcmp(value, "1") != 0)
                okay = false;
            else
                (single ? parsed.preferences.shelf_single : parsed.preferences.reduced_motion) = value[0] == '1';
        } else if (std::strcmp(line, "brand") == 0) {
            key = 7;
            bool found = false;
            for (std::size_t i = 0; i < brands.size(); ++i)
                if (brands[i] == value) {
                    parsed.preferences.brand = i;
                    found = true;
                    break;
                }
            if (!found)
                okay = false;
        } else if (std::strcmp(line, "language") == 0) {
            key = static_cast<int>(room_keys + 69); // Optional in existing schema-1 settings.
            const int n = choice(value, language_names);
            if (n < 0)
                okay = false;
            else
                parsed.preferences.language = static_cast<Language>(n);
        } else if (std::strcmp(line, "tv_mode") == 0) {
            key = static_cast<int>(room_keys);
            const int n = choice(value, tv_names);
            if (n < 0) okay = false;
            else parsed.preferences.tv_mode = static_cast<TvMode>(n);
        } else if (std::strcmp(line, "tv_noise") == 0) {
            key = static_cast<int>(room_keys + 68); // Optional: legacy schema-1 settings keep static off.
            const int n = choice(value, noise_names);
            if (n < 0) okay = false;
            else parsed.preferences.tv_noise = static_cast<TvNoise>(n);
        } else if (std::strcmp(line, "tv_period") == 0) {
            key = static_cast<int>(room_keys + 1);
            bool found = false;
            for (std::size_t i = 0; i < tv_periods.size(); ++i) {
                char period[8]; std::snprintf(period, sizeof(period), "%u", tv_periods[i]);
                if (std::strcmp(value, period) == 0) { parsed.preferences.tv_period = i; found = true; break; }
            }
            if (!found) okay = false;
        } else if (std::strcmp(line, "intro_all") == 0) {
            key = static_cast<int>(room_keys + 2);
            if (std::strcmp(value, "0") != 0 && std::strcmp(value, "1") != 0) okay = false;
            else parsed.preferences.intro_all = value[0] == '1';
        } else if (std::strcmp(line, "tv_text") == 0) {
            key = static_cast<int>(room_keys + 3);
            if (std::strlen(value) >= sizeof(parsed.tv_text) || !valid_text(value, std::strlen(value) + 1)) okay = false;
            else std::memcpy(parsed.tv_text, value, std::strlen(value) + 1);
        } else if (std::strncmp(line, "game_favorite.", 14) == 0) {
            char* end = nullptr;
            const auto slot = std::strtoul(line + 14, &end, 10);
            char canonical[8]; std::snprintf(canonical, sizeof(canonical), "%lu", slot);
            if (!line[14] || *end || slot >= parsed.game_favorites.size() || std::strcmp(line + 14, canonical) != 0) okay = false;
            else {
                key = static_cast<int>(room_keys + 4 + slot);
                const auto digits = std::strlen(value);
                if (digits != 16) okay = false;
                else {
                    std::uint64_t number = 0;
                    for (std::size_t i = 0; i < digits; ++i) {
                        const char c = value[i];
                        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { okay = false; break; }
                        number = number * 16 + static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
                    }
                    parsed.game_favorites[slot] = number;
                }
            }
        } else {
            const char* dot = std::strchr(line, '.');
            if (!dot)
                continue; // Unknown future keys never become console identities.
            const auto id = find_system(dot + 1);
            const auto prefix = static_cast<std::size_t>(dot - line);
            if (id == system_count) {
                if (prefix == 5 && std::strncmp(line, "route", 5) == 0)
                    okay = false;
                continue;
            }
            if ((prefix == 6 && std::strncmp(line, "hidden", 6) == 0) ||
                (prefix == 8 && std::strncmp(line, "favorite", 8) == 0)) {
                if (std::strcmp(value, "0") != 0 && std::strcmp(value, "1") != 0)
                    okay = false;
                const bool favorite = prefix == 8;
                key = static_cast<int>(8 + id + (favorite ? system_count : 0));
                (favorite ? parsed.preferences.favorite : parsed.preferences.hidden)[id] = value[0] == '1';
            } else if (prefix == 5 && std::strncmp(line, "shelf", 5) == 0) {
                key = static_cast<int>(11 + system_count * 3 + id);
                if (std::strcmp(value, "0") != 0 && std::strcmp(value, "1") != 0)
                    okay = false;
                else
                    parsed.preferences.shelf_selected[id] = value[0] == '1';
            } else if (prefix == 8 && std::strncmp(line, "emulator", 8) == 0) {
                key = static_cast<int>(8 + system_count * 2 + id);
                if (!valid_route(value))
                    okay = false;
                else
                    std::memcpy(parsed.preferred_emulator[id].data(), value, std::strlen(value) + 1);
            } else if (prefix == 5 && std::strncmp(line, "route", 5) == 0) {
                auto& slot = parsed.shortcut_routes[id];
                if (shortcut_route_seen[id]) {
                    slot.input = tile_route::Input::invalid;
                    slot.preference = {};
                } else {
                    shortcut_route_seen[id] = true;
                    tile_route::Preference preference;
                    if (tile_route::settings_codec::parse_value(id, value, preference)) {
                        slot.input = tile_route::Input::valid;
                        slot.preference = preference;
                    } else {
                        slot.input = tile_route::Input::invalid;
                    }
                }
            }
        }
        if (key >= 0) {
            if (seen[static_cast<std::size_t>(key)])
                okay = false;
            seen[static_cast<std::size_t>(key)] = true;
        }
        if (!okay)
            break;
    }
    if (std::ferror(file))
        okay = false;
    std::fclose(file);
    for (std::size_t i = 0; i < 8; ++i)
        if (!seen[i])
            okay = false;
    if (!okay || !valid(parsed))
        return false;
    output = parsed;
    return true;
}
bool copy_file(const char* from, const char* to) {
    FILE* source = open_file(from, "rb");
    if (!source)
        return false;
    FILE* target = open_file(to, "wb");
    if (!target) {
        std::fclose(source);
        return false;
    }
    char buffer[4096];
    bool okay = true;
    for (;;) {
        const auto size = std::fread(buffer, 1, sizeof(buffer), source);
        if (size && std::fwrite(buffer, 1, size, target) != size) {
            okay = false;
            break;
        }
        if (size < sizeof(buffer)) {
            if (std::ferror(source))
                okay = false;
            break;
        }
    }
    if (!flush_file(target))
        okay = false;
    if (std::fclose(target) != 0)
        okay = false;
    std::fclose(source);
    return okay;
}
} // namespace

SettingsLoad load_settings(const char* path, Settings& output) {
    output = Settings{};
    FILE* probe = open_file(path, "rb");
    const bool missing = !probe && errno == ENOENT;
    if (probe)
        std::fclose(probe);
    if (read_file(path, output))
        return SettingsLoad::loaded;
    char backup[4096];
    if (std::snprintf(backup, sizeof(backup), "%s.bak", path) < 0 || std::strlen(path) + 5 >= sizeof(backup))
        return SettingsLoad::invalid;
    if (read_file(backup, output))
        return SettingsLoad::backup;
    return missing ? SettingsLoad::missing : SettingsLoad::invalid;
}

bool save_settings(const char* path, const Settings& settings) {
    if (!valid(settings) || std::strlen(path) > 4000)
        return false;
    char temporary[4096], backup[4096], backup_temporary[4096];
    std::snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    std::snprintf(backup, sizeof(backup), "%s.bak", path);
    std::snprintf(backup_temporary, sizeof(backup_temporary), "%s.bak.tmp", path);
    FILE* file = open_file(temporary, "wb");
    if (!file)
        return false;
    const auto& p = settings.preferences;
    bool okay = std::fprintf(
                    file,
                    "# isShelf settings; schema 1; console keys use stable "
                    "IDs\nversion=1\nlayout=%s\nlook=%s\nscope=%s\nsort=%s\nfilter=%s\nicon=%s\nbrand=%.*s\nlanguage=%s\n",
                    layout_names[static_cast<std::size_t>(p.layout)], look_names[static_cast<std::size_t>(p.grid_look)],
                    scope_names[static_cast<std::size_t>(p.scope)], sort_names[static_cast<std::size_t>(p.sort)],
                    filter_names[static_cast<std::size_t>(p.filter)], icon_names[static_cast<std::size_t>(p.icon)],
                    static_cast<int>(brands[p.brand].size()), brands[p.brand].data(),
                    language_names[static_cast<std::size_t>(p.language)]) > 0;
    if (okay)
        okay = std::fprintf(file, "monochrome_missing=%d\nshelf_single=%d\nreduced_motion=%d\n",
                            p.monochrome_missing ? 1 : 0, p.shelf_single ? 1 : 0, p.reduced_motion ? 1 : 0) > 0;
    if (okay)
        okay = std::fprintf(file, "tv_mode=%s\ntv_period=%u\nintro_all=%d\ntv_text=%s\ntv_noise=%s\n",
                            tv_names[static_cast<std::size_t>(p.tv_mode)], tv_periods[p.tv_period], p.intro_all ? 1 : 0,
                            settings.tv_text, noise_names[static_cast<std::size_t>(p.tv_noise)]) > 0;
    for (std::size_t i = 0; i < settings.game_favorites.size() && okay; ++i)
        okay = std::fprintf(file, "game_favorite.%zu=%016llx\n", i,
                            static_cast<unsigned long long>(settings.game_favorites[i])) > 0;
    for (std::size_t i = 0; i < system_count && okay; ++i) {
        const auto id = catalog[i].id;
        okay =
            std::fprintf(file, "hidden.%.*s=%d\nfavorite.%.*s=%d\nemulator.%.*s=%s\nshelf.%.*s=%d\n", static_cast<int>(id.size()),
                         id.data(), p.hidden[i] ? 1 : 0, static_cast<int>(id.size()), id.data(), p.favorite[i] ? 1 : 0,
                         static_cast<int>(id.size()), id.data(), settings.preferred_emulator[i].data(),
                         static_cast<int>(id.size()), id.data(), p.shelf_selected[i] ? 1 : 0) > 0;
        if (okay && settings.shortcut_routes[i].input == tile_route::Input::valid) {
            char line[256];
            okay = tile_route::settings_codec::encode_line(
                settings.shortcut_routes[i].preference, line, sizeof(line)) &&
                std::fprintf(file, "%s", line) > 0;
        }
    }
    if (!flush_file(file))
        okay = false;
    if (std::fclose(file) != 0)
        okay = false;
    Settings verified;
    if (!okay || !read_file(temporary, verified))
        return false;
    Settings previous;
    if (read_file(path, previous) && (!copy_file(path, backup_temporary) || !replace_file(backup_temporary, backup)))
        return false;
    return replace_file(temporary, path);
}
} // namespace shelf
