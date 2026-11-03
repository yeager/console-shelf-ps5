// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/artwork.hpp"
#include "shelf/memory.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace shelf {
namespace {
using View = std::string_view;
View part(View value, std::size_t start, std::size_t count = View::npos) {
    // No throwing substr dependency in the console's minimal C++ runtime.
    if (start >= value.size()) return {};
    const auto available = value.size() - start;
    return View(value.data() + start, count < available ? count : available);
}
View base(View path) {
    const auto slash = path.rfind('/');
    return part(path, slash == path.npos ? 0 : slash + 1);
}
View stem(View path) {
    path = base(path);
    const auto dot = path.rfind('.');
    return part(path, 0, dot);
}
bool serial(View value) {
    if (value.size() != 10 || value[4] != '-') return false;
    for (std::size_t i = 0; i < value.size(); ++i)
        if (i != 4 && !(i < 4 ? value[i] >= 'A' && value[i] <= 'Z' : value[i] >= '0' && value[i] <= '9'))
            return false;
    return true;
}
bool image(const ReadOnlyFs& fs, LibraryGame& game, View path) {
    if (!valid_artwork_source(path)) return false;
    char name[path_capacity];
    std::snprintf(name, sizeof(name), "%.*s", static_cast<int>(path.size()), path.data());
    FileStat info;
    if (fs.stat(fs.context, name, info) != FsStatus::ok || info.directory || !info.size || info.size > 8 * 1024 * 1024)
        return false;
    std::memcpy(game.artwork, name, path.size() + 1);
    game.artwork_bytes = info.size;
    game.artwork_modified = info.modified;
    game.artwork_kind = path.find("/covers/3d/") != path.npos ? ArtworkKind::case_render
        : path.find("/sce_sys/icon0.png") != path.npos || path.find("/PS3_GAME/ICON0.PNG") != path.npos
            ? ArtworkKind::icon
        : path.find("/Named_Titles/") != path.npos || path.find("/Named_Snaps/") != path.npos
            ? ArtworkKind::screenshot : ArtworkKind::front;
    return true;
}
bool named(const ReadOnlyFs& fs, LibraryGame& game, View directory, View name) {
    if (name.empty() || name.find_first_of("/\\\r\n") != name.npos) return false;
    char path[path_capacity];
    for (const char* ext : {".png", ".jpg", ".jpeg", ".PNG", ".JPG", ".JPEG"}) {
        const int count = std::snprintf(path, sizeof(path), "%.*s/%.*s%s", static_cast<int>(directory.size()),
                                        directory.data(), static_cast<int>(name.size()), name.data(), ext);
        if (count > 0 && static_cast<std::size_t>(count) < sizeof(path) && image(fs, game, path)) return true;
    }
    return false;
}
void serial_index(const ReadOnlyFs& fs, Library& library, const char* path, std::size_t system, bool ps2) {
    FileStat info;
    if (fs.stat(fs.context, path, info) != FsStatus::ok || info.directory || !info.size || info.size >= 65536) return;
    // The console's default worker stack cannot hold a complete serial index.
    // Match the other metadata readers: allocate only after validating its bound.
    auto* text = static_cast<char*>(allocate(static_cast<std::size_t>(info.size)));
    if (!text) return;
    std::size_t used = 0;
    if (fs.read(fs.context, path, 0, text, static_cast<std::size_t>(info.size), used) != FsStatus::ok || used != info.size) {
        release(text);
        return;
    }
    for (std::size_t start = 0; start < used;) {
        std::size_t end = start;
        while (end < used && text[end] != '\n') ++end;
        View line(text + start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        std::array<View, 6> fields{};
        std::size_t count = 0;
        while (count < fields.size()) {
            const auto tab = line.find('\t');
            fields[count++] = part(line, 0, tab);
            if (tab == line.npos) break;
            line.remove_prefix(tab + 1);
        }
        if (count < (ps2 ? 4u : 5u)) continue;
        const View game_path = fields[ps2 ? 0 : 3], id = fields[ps2 ? 3 : 1];
        if (!valid_virtual_path(game_path) || !serial(id)) continue;
        std::uint64_t bytes = 0;
        if (ps2) {
            bool valid = !fields[1].empty();
            for (char c : fields[1]) {
                if (c < '0' || c > '9' || bytes > (UINT64_MAX - 9u) / 10u) { valid = false; break; }
                bytes = bytes * 10u + static_cast<unsigned>(c - '0');
            }
            if (!valid) continue;
        }
        for (std::size_t i = 0; i < library.game_count; ++i) {
            auto& game = library.games[i];
            if (game.system != system || game_path != game.path || (ps2 && bytes != game.size)) continue;
            // Conflicting cache rows cannot choose an unrelated game's cover.
            if (!game.title_id[0]) std::snprintf(game.title_id, sizeof(game.title_id), "%.*s", static_cast<int>(id.size()), id.data());
            else if (id != game.title_id) std::strcpy(game.title_id, "ambiguous");
        }
    }
    release(text);
}
const char* database(View id) {
    struct Mapping { const char* id; const char* name; };
    static const Mapping names[] = {
        {"ps1", "Sony - PlayStation"}, {"ps2", "Sony - PlayStation 2"}, {"psp", "Sony - PlayStation Portable"},
        {"n64", "Nintendo - Nintendo 64"}, {"snes", "Nintendo - Super Nintendo Entertainment System"},
        {"nes", "Nintendo - Nintendo Entertainment System"}, {"gb", "Nintendo - Game Boy"},
        {"gbc", "Nintendo - Game Boy Color"}, {"gba", "Nintendo - Game Boy Advance"},
        {"ds", "Nintendo - Nintendo DS"}, {"3ds", "Nintendo - Nintendo 3DS"}, {"gamecube", "Nintendo - GameCube"},
        {"wii", "Nintendo - Wii"}, {"wiiu", "Nintendo - Wii U"}, {"virtualboy", "Nintendo - Virtual Boy"},
        {"pokemonmini", "Nintendo - Pokemon Mini"}, {"fds", "Nintendo - Family Computer Disk System"},
        {"genesis", "Sega - Mega Drive - Genesis"}, {"mastersystem", "Sega - Master System - Mark III"},
        {"gamegear", "Sega - Game Gear"}, {"segacd", "Sega - Mega-CD - Sega CD"}, {"sega32x", "Sega - 32X"},
        {"saturn", "Sega - Saturn"}, {"dreamcast", "Sega - Dreamcast"}, {"naomi", "Sega - Naomi"},
        {"atomiswave", "Sammy - Atomiswave"}, {"pcengine", "NEC - PC Engine - TurboGrafx 16"},
        {"pcfx", "NEC - PC-FX"}, {"supergrafx", "NEC - PC Engine SuperGrafx"},
        {"atarijaguar", "Atari - Jaguar"}, {"atarilynx", "Atari - Lynx"}, {"atari2600", "Atari - 2600"},
        {"atari5200", "Atari - 5200"}, {"atari7800", "Atari - 7800"}, {"threedo", "The 3DO Company - 3DO"},
        {"neogeocd", "SNK - Neo Geo CD"}, {"neogeopocket", "SNK - Neo Geo Pocket"},
        {"neogeopocketcolor", "SNK - Neo Geo Pocket Color"}, {"wonderswan", "Bandai - WonderSwan"},
        {"wonderswancolor", "Bandai - WonderSwan Color"}, {"amiga", "Commodore - Amiga"},
        {"c64", "Commodore - 64"}, {"arcadefbneo", "FBNeo - Arcade Games"}, {"arcademame", "MAME"}
    };
    for (const auto& item : names) if (id == item.id) return item.name;
    return nullptr;
}
bool thumbnail(const ReadOnlyFs& fs, LibraryGame& game, const char* kind) {
    const char* db = database(catalog[game.system].id);
    if (!db) return false;
    char directory[path_capacity];
    std::snprintf(directory, sizeof(directory), "/data/homebrew/PPSA99169/thumbnails/%s/%s", db, kind);
    return named(fs, game, directory, stem(game.path)) || named(fs, game, directory, game.name);
}
} // namespace
bool valid_artwork_source(View path) {
    if (!valid_virtual_path(path)) return false;
    const auto dot = path.rfind('.');
    const View ext = dot == path.npos ? View{} : part(path, dot);
    if (ext != ".png" && ext != ".PNG" && ext != ".jpg" && ext != ".JPG" && ext != ".jpeg" && ext != ".JPEG") return false;
    for (View prefix : {"/data/PSXS5/covers/", "/data/PCSX2/covers/", "/data/SwanStationPS5/covers/",
                        "/data/psswanstation/covers/", "/data/homebrew/PPSA99248/covers/",
                        "/data/homebrew/PPSA99169/thumbnails/"})
        if (path.size() > prefix.size() && part(path, 0, prefix.size()) == prefix) return true;
    constexpr View native = "/user/app/", icon = "/sce_sys/icon0.png";
    if (part(path, 0, native.size()) == native && path.size() == native.size() + 9 + icon.size() &&
        part(path, path.size() - icon.size()) == icon)
        return true;
    constexpr View ps3 = "/data/homebrew/PPSA99303/rpcs3/games/", ps3_icon = "/PS3_GAME/ICON0.PNG";
    if (part(path, 0, ps3.size()) == ps3 &&
        path.size() > ps3.size() + ps3_icon.size() && part(path, path.size() - ps3_icon.size()) == ps3_icon) return true;
    for (View prefix : {"/data/PSXS5/games/", "/data/PCSX2/games/", "/data/SwanStationPS5/games/",
                        "/data/homebrew/PPSA99248/games/", "/data/psswanstation/games/",
                        "/data/homebrew/PPSA99169/content/"})
        if (part(path, 0, prefix.size()) == prefix && (stem(path) == "cover" || stem(path) == "front")) return true;
    return false;
}
void discover_artwork(const ReadOnlyFs& fs, Library& library) {
    library.artwork = {};
    if (!fs.stat || !fs.read) return;
    serial_index(fs, library, "/data/PSXS5/library.txt", find_system("ps1"), false);
    serial_index(fs, library, "/data/PCSX2/chd-serials.txt", find_system("ps2"), true);
    for (std::size_t i = 0; i < library.game_count; ++i) {
        auto& game = library.games[i];
        game.artwork[0] = 0; game.artwork_ready = false;
        game.artwork_kind = ArtworkKind::front; game.artwork_spine_ready = false;
        game.artwork_bytes = 0; game.artwork_modified = 0;
        const auto system = catalog[game.system].id;
        char path[path_capacity];
        bool found = false;
        if (game.kind == MediaKind::native_title)
            found = join_media_path(game.path, "sce_sys/icon0.png", path, sizeof(path)) && image(fs, game, path);
        else if (game.kind == MediaKind::ps3_folder)
            found = join_media_path(game.path, "PS3_GAME/ICON0.PNG", path, sizeof(path)) && image(fs, game, path);
        if (!found && system == "ps2" && serial(game.title_id))
            found = named(fs, game, "/data/PCSX2/covers/3d", game.title_id) || named(fs, game, "/data/PCSX2/covers", game.title_id);
        if (!found && system == "ps1" && serial(game.title_id))
            found = named(fs, game, "/data/PSXS5/covers/3d", game.title_id) ||
                    named(fs, game, "/data/PSXS5/covers/default", game.title_id) || named(fs, game, "/data/PSXS5/covers/boxart", game.title_id);
        if (!found && system == "ps1")
            for (const char* directory : {"/data/PSXS5/covers/3d", "/data/PSXS5/covers/boxart", "/data/PSXS5/covers/default"}) {
                found = named(fs, game, directory, stem(game.path));
                if (found) break;
            }
        if (!found && system == "ps1")
            for (const char* directory : {"/data/homebrew/PPSA99248/covers", "/data/psswanstation/covers", "/data/SwanStationPS5/covers"}) {
                found = named(fs, game, directory, stem(game.path)) || (serial(game.title_id) && named(fs, game, directory, game.title_id));
                if (found) break;
            }
        if (!found) found = thumbnail(fs, game, "Named_Boxarts");
        if (!found && system == "ps1" && serial(game.title_id))
            found = named(fs, game, "/data/PSXS5/covers/3d", game.title_id) || named(fs, game, "/data/PSXS5/covers", game.title_id);
        if (!found && game.kind != MediaKind::native_title) {
            const View folder = part(View(game.path), 0, View(game.path).rfind('/'));
            if (base(folder) != "games" && base(folder) != "psx" && base(folder) != "ps1")
                found = named(fs, game, folder, "cover") || named(fs, game, folder, "front");
        }
        if (!found) found = thumbnail(fs, game, "Named_Titles") || thumbnail(fs, game, "Named_Snaps");
        if (found) ++library.artwork.found;
    }
}
} // namespace shelf
