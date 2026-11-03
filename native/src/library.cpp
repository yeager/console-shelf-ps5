// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/library.hpp"
#include "shelf/artwork.hpp"
#include "shelf/console_tile_job_pin.hpp"
#include "shelf/json.hpp"
#include "shelf/memory.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace shelf {
namespace {
bool starts(std::string_view a, std::string_view b) {
    return a.size() >= b.size() && a.substr(0, b.size()) == b;
}
char lower(char ch) {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + 32) : ch;
}
bool equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i]))
            return false;
    return true;
}
std::string_view basename(std::string_view path) {
    const auto at = path.rfind('/');
    return at == path.npos ? path : std::string_view(path.data() + at + 1, path.size() - at - 1);
}
std::string_view extension(std::string_view path) {
    auto name = basename(path);
    const auto at = name.rfind('.');
    return at == name.npos ? std::string_view{} : std::string_view(name.data() + at, name.size() - at);
}
bool one_of(std::string_view text, const char* list) {
    std::string_view values(list);
    while (!values.empty()) {
        const auto at = values.find('|');
        if (equal(text, values.substr(0, at)))
            return true;
        if (at == values.npos)
            break;
        values.remove_prefix(at + 1);
    }
    return false;
}
template <std::size_t N> void copy(char (&output)[N], std::string_view value) {
    if (value.size() >= N)
        value = value.substr(0, N - 1);
    while (!value.empty() && !valid_utf8(value))
        value.remove_suffix(1);
    std::memcpy(output, value.data(), value.size());
    output[value.size()] = 0;
}
std::string_view folder(std::string_view system) {
    struct Alias {
        const char* id;
        const char* directory;
    };
    static const Alias aliases[] = {{"gamecube", "gc"},
                                    {"3ds", "n3ds"},
                                    {"ds", "nds"},
                                    {"ps1", "psx"},
                                    {"pokemonmini", "pokemini"},
                                    {"sg1000", "sg-1000"},
                                    {"neogeopocket", "ngp"},
                                    {"neogeopocketcolor", "ngpc"},
                                    {"wonderswan", "wswan"},
                                    {"wonderswancolor", "wswanc"},
                                    {"threedo", "3do"},
                                    {"arcadefbneo", "fbneo"},
                                    {"arcademame", "mame"}};
    for (const auto& alias : aliases)
        if (system == alias.id)
            return alias.directory;
    return system;
}
bool game_extension(std::string_view system, std::string_view ext) {
    struct Formats {
        const char* ids;
        const char* extensions;
    };
    static const Formats formats[] = {
        {"n64", ".z64|.v64|.n64"},
        {"ps1|ps2|psp|saturn|dreamcast|segacd|pcfx|neogeocd|threedo", ".iso|.chd|.cso|.pbp|.cue|.m3u"},
        {"gamecube|wii", ".iso|.gcm|.rvz|.wbfs"},
        {"nes", ".nes|.unf|.unif"},
        {"fds", ".fds"},
        {"snes", ".sfc|.smc"},
        {"gb", ".gb"},
        {"gbc", ".gbc"},
        {"gba", ".gba"},
        {"ds", ".nds"},
        {"3ds", ".3ds|.cci|.cxi"},
        {"genesis", ".md|.gen|.smd|.bin"},
        {"sega32x", ".32x"},
        {"mastersystem|sg1000", ".sms|.sg|.bin"},
        {"gamegear", ".gg"},
        {"pcengine|supergrafx", ".pce|.sgx|.cue|.chd|.m3u"},
        {"pokemonmini", ".min"},
        {"virtualboy", ".vb|.vboy"},
        {"atarijaguar", ".j64|.jag|.rom"},
        {"atarilynx", ".lnx"},
        {"atari7800", ".a78"},
        {"atari5200", ".a52"},
        {"atari2600", ".a26|.bin"},
        {"neogeopocket", ".ngp"},
        {"neogeopocketcolor", ".ngc"},
        {"wonderswan", ".ws"},
        {"wonderswancolor", ".wsc"},
        {"arcadefbneo|arcademame|naomi|atomiswave", ".zip|.7z"},
        {"amiga", ".adf|.hdf|.lha|.ipf|.m3u"},
        {"c64", ".d64|.d81|.crt|.prg|.t64|.m3u"},
        {"dos", ".dosz|.zip|.exe"},
        {"scummvm", ".scummvm"}};
    for (const auto& format : formats)
        if (one_of(system, format.ids))
            return one_of(ext, format.extensions);
    // Switch packages require title-type parsing (base/update/DLC). Do not guess.
    return false;
}
bool skipped(std::string_view name) {
    return name.empty() || name.front() == '.' ||
           one_of(name, "bios|firmware|keys|saves|save|savestates|logs|cache|shaders|assets|cores|downloads|updates|"
                        "dlc|screenshots|textures|system");
}
void problem(Library& out, FsStatus status) {
    if (status == FsStatus::ok || status == FsStatus::missing)
        return;
    out.scan.complete = false;
    if (status == FsStatus::denied)
        ++out.scan.roots_denied;
    if (status == FsStatus::limit)
        out.scan.limit_reached = true;
}
bool is_file(const ReadOnlyFs& fs, const char* path, FileStat* info = nullptr) {
    FileStat local;
    return fs.stat(fs.context, path, info ? *info : local) == FsStatus::ok &&
           !(info ? info->directory : local.directory);
}
bool read_exact(const ReadOnlyFs& fs, const char* path, std::uint64_t offset, void* data, std::size_t cap) {
    std::size_t size = 0;
    return fs.read(fs.context, path, offset, data, cap, size) == FsStatus::ok && size == cap;
}
bool valid_header(const ReadOnlyFs& fs, const char* path, std::string_view system, ScanSummary& scan) {
    const auto ext = extension(path);
    unsigned char bytes[8]{};
    const auto header = [&](std::uint64_t offset, std::size_t cap) {
        std::size_t used = 0;
        const auto status = fs.read(fs.context, path, offset, bytes, cap, used);
        if (status != FsStatus::ok)
            scan.complete = false;
        return status == FsStatus::ok && used == cap;
    };
    if (system == "n64") {
        if (!header(0, 4))
            return false;
        return std::memcmp(bytes, "\x80\x37\x12\x40", 4) == 0 || std::memcmp(bytes, "\x37\x80\x40\x12", 4) == 0 ||
               std::memcmp(bytes, "\x40\x12\x37\x80", 4) == 0;
    }
    if (equal(ext, ".chd"))
        return header(0, 8) && std::memcmp(bytes, "MComprHD", 8) == 0;
    if (equal(ext, ".iso") && one_of(system, "ps1|ps2|psp|saturn|dreamcast|segacd|pcfx|neogeocd|threedo"))
        return header(0x8000, 8) && std::memcmp(bytes + 1, "CD001", 5) == 0;
    // Other formats are candidates, not an integrity or compatibility certification.
    return true;
}
void add_game(Library& out, std::size_t system, const char* path, const char* label, MediaKind kind, std::uint64_t size,
              const char* title_id = "") {
    for (std::size_t i = 0; i < out.game_count; ++i)
        if (std::strcmp(out.games[i].path, path) == 0 ||
            (*title_id && out.games[i].system == system && std::strcmp(out.games[i].title_id, title_id) == 0))
            return;
    if (out.game_count == max_games) {
        out.scan.complete = false;
        out.scan.limit_reached = true;
        return;
    }
    auto& game = out.games[out.game_count++];
    game = {};
    game.system = system;
    game.kind = kind;
    game.size = size;
    copy(game.path, path);
    copy(game.title_id, title_id);
    auto title = label && *label ? std::string_view(label) : basename(path);
    if (!label || !*label) {
        const auto ext = extension(title);
        title.remove_suffix(ext.size());
    }
    copy(game.name, title);
}
void add_route(Library& out, std::size_t system, const char* id, const char* name, const char* title, const char* path,
               const char* core = "") {
    for (std::size_t i = 0; i < out.route_count; ++i)
        if (out.routes[i].system == system && std::strcmp(out.routes[i].id, id) == 0)
            return;
    if (out.route_count == max_routes) {
        out.scan.complete = false;
        out.scan.limit_reached = true;
        return;
    }
    auto& route = out.routes[out.route_count++];
    route = {};
    route.system = system;
    copy(route.id, id);
    copy(route.name, name);
    copy(route.title_id, title);
    copy(route.folder, path);
    copy(route.core, core);
    ++out.inventory[system].emulator_count;
}
unsigned u16(const unsigned char* p) {
    return p[0] | (static_cast<unsigned>(p[1]) << 8);
}
unsigned u32(const unsigned char* p) {
    return u16(p) | (u16(p + 2) << 16);
}
bool sfo_string(const unsigned char* data, std::size_t size, const char* field, char* output, std::size_t capacity) {
    if (size < 20 || std::memcmp(data, "\0PSF", 4))
        return false;
    const auto keys = u32(data + 8), values = u32(data + 12), count = u32(data + 16);
    if (count > (size - 20) / 16 || keys >= size || values >= size)
        return false;
    for (unsigned i = 0; i < count; ++i) {
        const auto* item = data + 20 + i * 16;
        const auto key = static_cast<std::uint64_t>(keys) + u16(item),
                   value = static_cast<std::uint64_t>(values) + u32(item + 12);
        const auto length = u32(item + 4);
        if (key >= size || value >= size || length == 0 || length > size - value ||
            !std::memchr(data + key, 0, size - key))
            return false;
        if (std::strcmp(reinterpret_cast<const char*>(data + key), field))
            continue;
        if (u16(item + 2) != 0x0204 || length > capacity || data[value + length - 1] != 0 ||
            std::memchr(data + value, 0, length - 1))
            return false;
        if (!valid_utf8(std::string_view(reinterpret_cast<const char*>(data + value), length - 1)))
            return false;
        std::memcpy(output, data + value, length);
        return true;
    }
    return false;
}
bool read_sfo(const ReadOnlyFs& fs, const char* path, char* title, std::size_t title_size, char* id,
              std::size_t id_size) {
    FileStat st;
    if (!is_file(fs, path, &st) || !st.size || st.size > 65536)
        return false;
    auto* data = static_cast<unsigned char*>(allocate(static_cast<std::size_t>(st.size)));
    if (!data)
        return false;
    const bool ok = read_exact(fs, path, 0, data, static_cast<std::size_t>(st.size)) &&
                    sfo_string(data, static_cast<std::size_t>(st.size), "TITLE", title, title_size) &&
                    sfo_string(data, static_cast<std::size_t>(st.size), "TITLE_ID", id, id_size);
    release(data);
    return ok;
}
bool title_identity(std::string_view id) {
    if (id.size() != 9)
        return false;
    for (std::size_t i = 0; i < id.size(); ++i)
        if (!(i < 4 ? id[i] >= 'A' && id[i] <= 'Z' : id[i] >= '0' && id[i] <= '9'))
            return false;
    return true;
}
struct FileCandidate {
    char path[path_capacity]{};
    std::uint64_t size = 0;
    bool suppress = false;
};
struct Scanner {
    const ReadOnlyFs& fs;
    Library& out;
    FileCandidate* candidates = nullptr;
    std::size_t count = 0, system = 0;
    Scanner(const ReadOnlyFs& f, Library& o) : fs(f), out(o) {
        candidates = static_cast<FileCandidate*>(allocate_zero(max_games, sizeof(FileCandidate)));
    }
    ~Scanner() {
        release(candidates);
    }
    void walk(const char* root, unsigned depth = 0) {
        if (!candidates) {
            problem(out, FsStatus::failed);
            return;
        }
        struct Context {
            Scanner* scan;
            const char* root;
            unsigned depth;
        } context{this, root, depth};
        const auto status = fs.list(
            fs.context, root,
            [](void* raw, const char* name, const FileStat& stat) {
                auto& c = *static_cast<Context*>(raw);
                auto& scan = *c.scan;
                if (skipped(name))
                    return true;
                char path[path_capacity];
                if (!join_media_path(c.root, name, path, sizeof(path))) {
                    ++scan.out.scan.rejected;
                    return true;
                }
                if (stat.directory) {
                    if (c.depth < 4)
                        scan.walk(path, c.depth + 1);
                    else
                        problem(scan.out, FsStatus::limit);
                    return true;
                }
                ++scan.out.scan.files_examined;
                if (!stat.size || !game_extension(catalog[scan.system].id, extension(path)))
                    return true;
                if (scan.count == max_games)
                    return false;
                auto& candidate = scan.candidates[scan.count++];
                candidate = {};
                copy(candidate.path, path);
                candidate.size = stat.size;
                return true;
            },
            &context);
        if (status == FsStatus::ok)
            ++out.scan.roots_read;
        problem(out, status);
    }
    bool references(const FileCandidate& candidate, bool playlist) {
        if (candidate.size > 65536)
            return false;
        auto* data = static_cast<char*>(allocate(static_cast<std::size_t>(candidate.size) + 1));
        if (!data)
            return false;
        if (!read_exact(fs, candidate.path, 0, data, static_cast<std::size_t>(candidate.size)) ||
            std::memchr(data, 0, static_cast<std::size_t>(candidate.size))) {
            release(data);
            return false;
        }
        data[candidate.size] = 0;
        const std::string_view parent(candidate.path, std::string_view(candidate.path).rfind('/'));
        bool valid = true;
        std::size_t refs = 0;
        for (char* line = data; line && line < data + candidate.size;) {
            char* next = std::strchr(line, '\n');
            if (next)
                *next++ = 0;
            std::string_view value(line);
            while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '\t'))
                value.remove_suffix(1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                value.remove_prefix(1);
            if (playlist) {
                if (!value.empty() && value.front() == '#')
                    value = {};
            } else {
                if (value.size() >= 5 && equal(value.substr(0, 5), "FILE ")) {
                    value.remove_prefix(5);
                    while (!value.empty() && value.front() == ' ')
                        value.remove_prefix(1);
                    if (!value.empty() && value.front() == '"') {
                        value.remove_prefix(1);
                        const auto end = value.find('"');
                        if (end == value.npos) {
                            valid = false;
                            break;
                        }
                        value = value.substr(0, end);
                    } else {
                        const auto end = value.find_first_of(" \t");
                        value = value.substr(0, end);
                    }
                } else
                    value = {};
            }
            if (!value.empty()) {
                char target[path_capacity];
                FileStat info;
                if (value.front() == '/' || !join_media_path(parent, value, target, sizeof(target)) ||
                    !is_file(fs, target, &info) || !info.size ||
                    !(playlist ? game_extension(catalog[system].id, extension(target))
                               : one_of(extension(target), ".bin|.wav|.iso|.img|.mp3|.ogg"))) {
                    valid = false;
                    break;
                }
                if (playlist) {
                    const auto ext = extension(target);
                    if (equal(ext, ".m3u")) {
                        valid = false;
                        break;
                    }
                    if (equal(ext, ".cue")) {
                        FileCandidate referenced{};
                        copy(referenced.path, target);
                        referenced.size = info.size;
                        if (!references(referenced, false)) {
                            valid = false;
                            break;
                        }
                    } else if (!valid_header(fs, target, catalog[system].id, out.scan)) {
                        valid = false;
                        break;
                    }
                }
                ++refs;
            }
            line = next;
        }
        // Suppress only after every reference has been checked; a broken playlist must not hide valid discs.
        if (valid && refs && playlist) {
            char* line = data;
            while (line < data + candidate.size) {
                auto value = std::string_view(line);
                const auto n = std::strlen(line);
                line += n + 1;
                while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '\t'))
                    value.remove_suffix(1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                    value.remove_prefix(1);
                if (value.empty() || value.front() == '#')
                    continue;
                char target[path_capacity];
                if (!join_media_path(parent, value, target, sizeof(target)))
                    continue;
                for (std::size_t i = 0; i < count; ++i)
                    if (std::strcmp(candidates[i].path, target) == 0)
                        candidates[i].suppress = true;
            }
        }
        release(data);
        return valid && refs;
    }
    void scan(const char* root, std::size_t index) {
        system = index;
        count = 0;
        walk(root);
        // CUE sheets first, then M3U manifests; finally standalone images.
        for (int pass = 0; pass < 3; ++pass)
            for (std::size_t i = 0; i < count; ++i) {
                auto& candidate = candidates[i];
                const auto ext = extension(candidate.path);
                const int category = equal(ext, ".cue") ? 0 : equal(ext, ".m3u") ? 1 : 2;
                if (category != pass)
                    continue;
                if (category < 2) {
                    if (!references(candidate, category == 1)) {
                        candidate.suppress = true;
                        ++out.scan.missing_references;
                    }
                } else if (!valid_header(fs, candidate.path, catalog[system].id, out.scan)) {
                    candidate.suppress = true;
                    ++out.scan.rejected;
                }
            }
        for (std::size_t i = 0; i < count; ++i)
            if (!candidates[i].suppress) {
                const auto ext = extension(candidates[i].path);
                const auto kind = equal(ext, ".cue")                              ? MediaKind::cue
                                  : equal(ext, ".m3u")                            ? MediaKind::playlist
                                  : one_of(ext, ".iso|.chd|.cso|.pbp|.rvz|.wbfs") ? MediaKind::disc_image
                                                                                  : MediaKind::cartridge;
                add_game(out, system, candidates[i].path, "", kind, candidates[i].size);
            }
    }
};
void ps3_folder(const ReadOnlyFs& fs, Library& out, const char* root) {
    struct Context {
        const ReadOnlyFs* fs;
        Library* out;
        const char* root;
    } context{&fs, &out, root};
    const auto status = fs.list(
        fs.context, root,
        [](void* raw, const char* name, const FileStat& info) {
            auto& c = *static_cast<Context*>(raw);
            if (!info.directory || skipped(name))
                return true;
            char game[path_capacity], path[path_capacity], title[256]{}, id[32]{};
            if (!join_media_path(c.root, name, game, sizeof(game)) ||
                !join_media_path(game, "PS3_GAME/USRDIR/EBOOT.BIN", path, sizeof(path)) || !is_file(*c.fs, path))
                return true;
            if (join_media_path(game, "PS3_GAME/PARAM.SFO", path, sizeof(path)) &&
                read_sfo(*c.fs, path, title, sizeof(title), id, sizeof(id)) && title_identity(id))
                add_game(*c.out, find_system("ps3"), game, title, MediaKind::ps3_folder, 0, id);
            else
                ++c.out->scan.rejected;
            return true;
        },
        &context);
    if (status == FsStatus::ok)
        ++out.scan.roots_read;
    problem(out, status);
}
struct App {
    const char* id;
    const char* name;
    const char* systems;
};
static const App apps[] = {{"PPSA99169", "RetroArch", ""},
                           {"PPSA99203", "PS5SX2", "ps2"},
                           {"PPSA97510", "PSXS5", "ps1"},
                           {"PPSA99303", "PS5SX3", "ps3"},
                           {"PPSA99008", "ProsperoEden", "switch"},
                           {"PPSA99764", "Porpoise", "gamecube|wii"},
                           {"PPSA99360", "PS5-ECAM", "wiiu|3ds|ds"},
                           {"PPSA99064", "Mupen64", "n64"},
                           {"PPSA99247", "Flycast", "dreamcast|naomi|atomiswave"},
                           {"PPSA98510", "SwanStationPS5", "ps1"},
                           {"PPSA99248", "PSSwanStation", "ps1"}};
bool known_utility(std::string_view title) {
    if (one_of(title, "PPSA99000|PPSA90210|PPSA77305|PPSA99995|PPSA99310|PPSA98001|PPSA75837|PPSA99841|PPSA99109|"
                      "PPSA99350|PPSA99230|PPSA99058|PPSA99541"))
        return true;
    for (const auto& app : apps)
        if (title == app.id)
            return true;
    return false;
}
bool owned_console_shortcut(const ReadOnlyFs& fs, const char* title) {
    if (!tile_job::owned_title_known(title))
        return false;
    char root[path_capacity], path[path_capacity];
    if (!join_media_path("/data/homebrew", title, root, sizeof(root)) ||
        !join_media_path(root, "isshelf-console-tile.json", path, sizeof(path)))
        return false;
    JsonDocument marker;
    if (!marker.read(fs, path, 4096) ||
        std::strcmp(json_string(marker.root, "format"), "isshelf-console-tile/1") ||
        std::strcmp(json_string(marker.root, "titleId"), title) ||
        std::strcmp(json_string(marker.root, "target"), "PPSA99541") ||
        std::strcmp(json_string(marker.root, "forwarderSource"),
                    "https://github.com/heydemoura/ps5-forwarder-format") ||
        std::strcmp(json_string(marker.root, "forwarderCommit"),
                    "22e8762ad7dbd01e18bd8c0e95f86461fdf8aaf4") ||
        find_system(json_string(marker.root, "system")) == system_count)
        return false;
    if (!join_media_path(root, "forwarder.json", path, sizeof(path)))
        return false;
    JsonDocument forwarder;
    return forwarder.read(fs, path, 4096) &&
           !std::strcmp(json_string(forwarder.root, "format"), "ps5-forwarder/1") &&
           !std::strcmp(json_string(forwarder.root, "target"), "PPSA99541");
}
void native_titles(const ReadOnlyFs& fs, Library& out) {
    struct Context {
        const ReadOnlyFs* fs;
        Library* out;
    } context{&fs, &out};
    const auto status = fs.list(
        fs.context, "/user/app",
        [](void* raw, const char* name, const FileStat& info) {
            auto& c = *static_cast<Context*>(raw);
            if (!info.directory || !title_identity(name) || known_utility(name))
                return true;
            const bool ps5 = starts(name, "PPSA"), ps4 = starts(name, "CUSA");
            if (!ps5 && !ps4)
                return true;
            char root[path_capacity], path[path_capacity], title[256]{}, id[32]{};
            if (!join_media_path("/user/app", name, root, sizeof(root)))
                return true;
            // An isShelf console shortcut is a launcher, not a native PS5 game.
            // Verify its owned source and forwarder metadata; never exclude a
            // title merely because its ID falls in our preferred allocation range.
            if (ps5 && owned_console_shortcut(*c.fs, name))
                return true;
            if (ps5) {
                if (!join_media_path(root, "sce_sys/param.json", path, sizeof(path)))
                    return true;
                JsonDocument metadata;
                if (!metadata.read(*c.fs, path, 65536) || std::strcmp(json_string(metadata.root, "titleId"), name))
                    return true;
                const char* label = json_string(metadata.root, "titleName");
                if (!*label) {
                    const auto* localized = cJSON_GetObjectItemCaseSensitive(metadata.root, "localizedParameters");
                    label = json_string(cJSON_GetObjectItemCaseSensitive(localized, "en-US"), "titleName");
                    if (!*label) {
                        const auto* language =
                            cJSON_GetObjectItemCaseSensitive(localized, json_string(localized, "defaultLanguage"));
                        label = json_string(language, "titleName");
                    }
                }
                if (!*label)
                    return true;
                copy(title, label);
                copy(id, name);
            } else {
                if (!join_media_path(root, "sce_sys/param.sfo", path, sizeof(path)) ||
                    !read_sfo(*c.fs, path, title, sizeof(title), id, sizeof(id)) || std::strcmp(id, name))
                    return true;
            }
            // A registered tile can still fail to mount. This identifies a title, not playability.
            const char* evidence[] = {"eboot.bin", "app.pkg", "mount.lnk"};
            bool registered_program = false;
            for (const char* entry : evidence)
                if (join_media_path(root, entry, path, sizeof(path))) {
                    FileStat st;
                    if (is_file(*c.fs, path, &st) && st.size)
                        registered_program = true;
                }
            if (!registered_program)
                return true;
            const auto system = find_system(ps5 ? "ps5" : "ps4");
            add_game(*c.out, system, root, title, MediaKind::native_title, 0, id);
            c.out->inventory[system].built_in = true;
            return true;
        },
        &context);
    if (status == FsStatus::ok)
        ++out.scan.roots_read;
    problem(out, status);
}
void discover_apps(const ReadOnlyFs& fs, Library& out, Scanner& scan) {
    // Registered app directories plus an actual program are evidence; a suggested app name is not.
    for (const auto& app : apps) {
        char root[path_capacity], path[path_capacity];
        std::snprintf(root, sizeof(root), "/user/app/%s", app.id);
        join_media_path(root, "eboot.bin", path, sizeof(path));
        if (!is_file(fs, path)) {
            FileStat registration;
            if (fs.stat(fs.context, root, registration) != FsStatus::ok || !registration.directory)
                continue;
            std::snprintf(root, sizeof(root), "/data/homebrew/%s", app.id);
            join_media_path(root, "eboot.bin", path, sizeof(path));
            if (!is_file(fs, path))
                continue;
        }
        join_media_path(root, "sce_sys/param.json", path, sizeof(path));
        JsonDocument metadata;
        if (!metadata.read(fs, path, 65536) || std::strcmp(json_string(metadata.root, "titleId"), app.id))
            continue;
        if (std::strcmp(app.id, "PPSA99169") == 0) {
            // Use the source directory, not the restricted app0 mount, for ROM paths.
            char source[path_capacity];
            std::snprintf(source, sizeof(source), "/data/homebrew/%s", app.id);
            FileStat present;
            if (fs.stat(fs.context, source, present) == FsStatus::ok && present.directory)
                copy(root, source);
            for (std::size_t i = 0; i < system_count; ++i) {
                if (catalog[i].suggested_core.empty())
                    continue;
                char core[path_capacity], id[48], name[96];
                std::snprintf(core, sizeof(core), "%s/cores/%.*s_libretro.so", root,
                              static_cast<int>(catalog[i].suggested_core.size()), catalog[i].suggested_core.data());
                if (!is_file(fs, core, &present) || !present.size)
                    continue;
                std::snprintf(id, sizeof(id), "retroarch:%.*s", static_cast<int>(catalog[i].suggested_core.size()),
                              catalog[i].suggested_core.data());
                std::snprintf(name, sizeof(name), "RetroArch / %.*s",
                              static_cast<int>(catalog[i].suggested_core.size()), catalog[i].suggested_core.data());
                add_route(out, i, id, name, app.id, root, core);
            }
            for (std::size_t i = 0; i < system_count; ++i) {
                const auto sub = folder(catalog[i].id);
                std::snprintf(path, sizeof(path), "%s/content/%.*s", root, static_cast<int>(sub.size()), sub.data());
                scan.scan(path, i);
            }
        } else {
            for (std::size_t i = 0; i < system_count; ++i)
                if (one_of(catalog[i].id, app.systems))
                    add_route(out, i, app.id, app.name, app.id, root);
        }
    }
}
} // namespace

bool valid_virtual_path(std::string_view path) {
    if (path.empty() || path.size() >= path_capacity || path.front() != '/' || !valid_utf8(path) ||
        path.find_first_of("\\:\r\n") != path.npos)
        return false;
    if (path == "/")
        return true;
    std::size_t start = 1;
    for (std::size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/') {
            const std::string_view part(path.data() + start, i - start);
            if (part.empty() || part == "." || part == "..")
                return false;
            start = i + 1;
        }
    return true;
}
bool join_media_path(std::string_view base, std::string_view reference, char* output, std::size_t capacity) {
    if (!output || !capacity || !valid_virtual_path(base) || reference.empty() || reference.front() == '/' ||
        !valid_utf8(reference))
        return false;
    const auto length = base.size() + (base == "/" ? 0 : 1) + reference.size();
    if (length >= capacity || length >= path_capacity)
        return false;
    std::memcpy(output, base.data(), base.size());
    std::size_t at = base.size();
    if (base != "/")
        output[at++] = '/';
    std::memcpy(output + at, reference.data(), reference.size());
    output[length] = 0;
    return valid_virtual_path(output);
}
void discover_library(const ReadOnlyFs& fs, Library& out) {
    out.route_count = out.game_count = 0;
    out.inventory.fill({});
    out.scan = {};
    out.scan.cached = fs.cached;
    if (!fs.list || !fs.read || !fs.stat) {
        out.scan.complete = false;
        return;
    }
    struct Observed {
        const ReadOnlyFs* source;
        ScanSummary* scan;
    } observed{&fs, &out.scan};
    const ReadOnlyFs checked{
        &observed,
        [](void* context, const char* path, EntryVisitor visit, void* destination) {
            const auto& c = *static_cast<Observed*>(context);
            return c.source->list(c.source->context, path, visit, destination);
        },
        [](void* context, const char* path, FileStat& info) {
            const auto& c = *static_cast<Observed*>(context);
            return c.source->stat(c.source->context, path, info);
        },
        [](void* context, const char* path, std::uint64_t offset, void* bytes, std::size_t cap, std::size_t& used) {
            const auto& c = *static_cast<Observed*>(context);
            const auto status = c.source->read(c.source->context, path, offset, bytes, cap, used);
            if (status != FsStatus::ok)
                c.scan->complete = false;
            return status;
        },
        fs.cached};
    Scanner scanner(checked, out);
    discover_apps(checked, out, scanner);
    native_titles(checked, out);
    scanner.scan("/data/PCSX2/games", find_system("ps2"));
    scanner.scan("/data/PSXS5/games", find_system("ps1"));
    scanner.scan("/data/SwanStationPS5/games", find_system("ps1"));
    scanner.scan("/data/homebrew/PPSA99248/games", find_system("ps1"));
    scanner.scan("/data/psswanstation/games", find_system("ps1"));
    ps3_folder(checked, out, "/data/homebrew/PPSA99303/rpcs3/games");
    discover_artwork(fs, out);
    for (std::size_t i = 0; i < out.game_count; ++i)
        ++out.inventory[out.games[i].system].game_count;
    std::sort(out.games.begin(), out.games.begin() + out.game_count, [](const LibraryGame& a, const LibraryGame& b) {
        if (a.system != b.system)
            return a.system < b.system;
        const int name = std::strcmp(a.name, b.name);
        return name ? name < 0 : std::strcmp(a.path, b.path) < 0;
    });
}
const EmulatorRoute* preferred_route(const Library& library, std::size_t system, std::string_view id) {
    const EmulatorRoute* fallback = nullptr;
    for (std::size_t i = 0; i < library.route_count; ++i)
        if (library.routes[i].system == system) {
            if (!fallback || (fallback->core[0] && !library.routes[i].core[0]))
                fallback = &library.routes[i];
            if (id == library.routes[i].id)
                return &library.routes[i];
        }
    return fallback;
}
GameList games_for_system(const Library& library, std::size_t system) {
    GameList result;
    for (std::size_t i = 0; i < library.game_count; ++i)
        if (library.games[i].system == system)
            result.indices[result.count++] = i;
    return result;
}
std::uint64_t game_favorite_key(const LibraryGame& game) {
    if (game.system >= system_count) return 0;
    std::uint64_t key = 14695981039346656037ull;
    for (unsigned char c : catalog[game.system].id) key = (key ^ c) * 1099511628211ull;
    key = (key ^ 255u) * 1099511628211ull;
    for (const char* at = game.path; *at; ++at) key = (key ^ static_cast<unsigned char>(*at)) * 1099511628211ull;
    return key ? key : 1;
}
} // namespace shelf
