// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/json.hpp"
#include "shelf/library.hpp"
#include "shelf/artwork.hpp"
#include "shelf/game_case.hpp"
#include "shelf/ui.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#endif
namespace {
void check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL library: %s\n", message);
        std::exit(1);
    }
}
struct File {
    bool directory = false;
    std::uint64_t size = 0;
    std::vector<unsigned char> bytes;
};
struct Tree {
    std::map<std::string, File> files;
    std::string denied;
    std::string unreadable;
    void directory(std::string path) {
        if (path.empty())
            return;
        files[path].directory = true;
        const auto slash = path.rfind('/');
        if (slash != path.npos)
            directory(path.substr(0, slash));
    }
    void file(std::string path, std::string text, std::uint64_t size = 0) {
        directory(path.substr(0, path.rfind('/')));
        auto& f = files[path];
        f.bytes.assign(text.begin(), text.end());
        f.size = size ? size : text.size();
    }
    void app(const char* id) {
        std::string root = std::string("/user/app/") + id;
        file(root + "/eboot.bin", "PROGRAM");
        file(root + "/sce_sys/param.json", std::string("{\"titleId\":\"") + id + "\",\"titleName\":\"Test emulator\"}");
    }
    shelf::ReadOnlyFs fs() {
        return {
            this,
            [](void* raw, const char* path, shelf::EntryVisitor visit, void* dest) {
                auto& tree = *static_cast<Tree*>(raw);
                if (tree.denied == path)
                    return shelf::FsStatus::denied;
                const auto at = tree.files.find(path);
                if (at == tree.files.end())
                    return shelf::FsStatus::missing;
                if (!at->second.directory)
                    return shelf::FsStatus::invalid;
                const std::string prefix = std::string(path) + "/";
                for (const auto& pair : tree.files)
                    if (pair.first.compare(0, prefix.size(), prefix) == 0) {
                        const auto child = pair.first.substr(prefix.size());
                        if (child.find('/') != child.npos)
                            continue;
                        if (!visit(dest, child.c_str(), {pair.second.directory, pair.second.size, 0}))
                            return shelf::FsStatus::limit;
                    }
                return shelf::FsStatus::ok;
            },
            [](void* raw, const char* path, shelf::FileStat& info) {
                auto& tree = *static_cast<Tree*>(raw);
                if (tree.denied == path)
                    return shelf::FsStatus::denied;
                const auto at = tree.files.find(path);
                if (at == tree.files.end())
                    return shelf::FsStatus::missing;
                info = {at->second.directory, at->second.size, 0};
                return shelf::FsStatus::ok;
            },
            [](void* raw, const char* path, std::uint64_t offset, void* output, std::size_t cap, std::size_t& used) {
                used = 0;
                const auto& tree = *static_cast<Tree*>(raw);
                if (tree.unreadable == path)
                    return shelf::FsStatus::failed;
                const auto& files = tree.files;
                const auto at = files.find(path);
                if (at == files.end())
                    return shelf::FsStatus::missing;
                if (offset < at->second.bytes.size()) {
                    used = std::min<std::size_t>(cap, at->second.bytes.size() - static_cast<std::size_t>(offset));
                    std::memcpy(output, at->second.bytes.data() + offset, used);
                }
                return shelf::FsStatus::ok;
            }};
    }
};
struct SmallStackScan {
    shelf::ReadOnlyFs files;
    shelf::Library* library;
};
#ifdef _WIN32
DWORD WINAPI scan_small_stack(void* raw) {
#else
void* scan_small_stack(void* raw) {
#endif
    auto& scan = *static_cast<SmallStackScan*>(raw);
    shelf::discover_library(scan.files, *scan.library);
    return 0;
}
void discover_small_stack(shelf::ReadOnlyFs files, shelf::Library& library) {
    SmallStackScan scan{files, &library};
    constexpr std::size_t stack_bytes = 64 * 1024;
#ifdef _WIN32
    HANDLE worker = CreateThread(nullptr, stack_bytes, scan_small_stack, &scan,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    check(worker != nullptr, "create a worker with a 64 KiB stack reservation");
    check(WaitForSingleObject(worker, INFINITE) == WAIT_OBJECT_0, "join small-stack discovery worker");
    CloseHandle(worker);
#else
    pthread_attr_t attributes;
    check(pthread_attr_init(&attributes) == 0, "initialize small-stack worker attributes");
    check(pthread_attr_setstacksize(&attributes, stack_bytes) == 0, "limit discovery worker to 64 KiB");
    pthread_t worker;
    check(pthread_create(&worker, &attributes, scan_small_stack, &scan) == 0, "create small-stack discovery worker");
    pthread_attr_destroy(&attributes);
    check(pthread_join(worker, nullptr) == 0, "join small-stack discovery worker");
#endif
}
} // namespace
void test_library() {
    using namespace shelf;
    check(valid_virtual_path("/data/roms/Great Game (USA).z64"), "canonical path");
    for (const char* path : {"relative", "/data/../keys", "/data//games", "/data/games/", "/data/./x",
                             "/data/roms\nQUIT", "/data/a:b", "/data/a\\b", "/data/\xc0\xaf"})
        check(!valid_virtual_path(path), "unsafe path refused");
    char joined[path_capacity];
    check(join_media_path("/data/roms", "disc/a.cue", joined, sizeof(joined)), "nested relative path");
    check(!join_media_path("/data/roms", "../keys.bin", joined, sizeof(joined)), "traversal reference refused");
    check(!join_media_path("/data/roms", "/other/file", joined, sizeof(joined)), "absolute reference refused");
    JsonDocument json;
    const char* good = "{\"name\":\"Mario\"}";
    check(json.parse(good, std::strlen(good)), "JSON metadata parser");
    for (const char* value : {"{\"a\":1,\"a\":2}", "{\"a\":\"x\\u0000y\"}", "{} trailing", "{\"a\":\"\xc0\xaf\"}"})
        check(!json.parse(value, std::strlen(value)), "ambiguous metadata refused");
    Tree tree;
    tree.app("PPSA99541"); // Shelf must not list its own tile as a game.
    tree.app("PPSA99169");
    tree.app("PPSA99203");
    tree.app("PPSA97510");
    tree.file("/user/app/PPSA99169/cores/mupen64plus_next_libretro.so", "CORE");
    tree.file("/user/app/PPSA99169/cores/snes9x_libretro.info", "INFO IS NOT A CORE");
    tree.directory("/data/homebrew/PPSA99169");
    tree.file("/data/homebrew/PPSA99169/cores/mupen64plus_next_libretro.so", "CORE");
    tree.file("/data/homebrew/PPSA99169/cores/ps5sx2_libretro.so", "CORE");
    tree.file("/data/homebrew/PPSA99169/content/n64/Mario Party.z64", std::string("\x80\x37\x12\x40", 4), 32000000);
    tree.file("/data/homebrew/PPSA99169/content/n64/Bad.z64", "WRONG");
    tree.file("/data/homebrew/PPSA99169/content/n64/Partial.z64.part", "PARTIAL");
    tree.file("/data/homebrew/PPSA99169/content/n64/bios/Firmware.z64", std::string("\x80\x37\x12\x40", 4));
    std::string iso(0x8008, 0);
    iso.replace(0x8001, 5, "CD001");
    tree.file("/data/PCSX2/games/Example.iso", iso, 1234567);
    tree.file("/data/PSXS5/games/Two Discs/Disc 1.bin", "TRACK");
    tree.file("/data/PSXS5/games/Two Discs/Disc 2.bin", "TRACK");
    tree.file("/data/PSXS5/games/Two Discs/Disc 1.cue", "FILE \"Disc 1.bin\" BINARY\n  TRACK 01 MODE2/2352\n");
    tree.file("/data/PSXS5/games/Two Discs/Disc 2.cue", "FILE \"Disc 2.bin\" BINARY\n  TRACK 01 MODE2/2352\n");
    tree.file("/data/PSXS5/games/Two Discs/Collection.m3u", "# two discs\n\nDisc 1.cue\nDisc 2.cue\n");
    tree.file("/data/PSXS5/games/Orphan.cue", "FILE \"Missing.bin\" BINARY\n");
    tree.file("/data/PSXS5/games/Bad.m3u", "Orphan.cue\n");
    tree.file("/data/PSXS5/games/Secret.cue", "FILE \"../../keys.bin\" BINARY\n");
    std::string sfo(128, 0);
    const auto put = [&](std::size_t at, unsigned value, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i)
            sfo[at + i] = static_cast<char>((value >> (8 * i)) & 255);
    };
    sfo.replace(1, 3, "PSF");
    put(8, 52, 4);
    put(12, 80, 4);
    put(16, 2, 4);
    put(22, 0x0204, 2);
    put(24, 10, 4);
    put(28, 16, 4);
    put(36, 6, 2);
    put(38, 0x0204, 2);
    put(40, 10, 4);
    put(44, 16, 4);
    put(48, 16, 4);
    sfo.replace(52, 15, std::string("TITLE\0TITLE_ID\0", 15));
    sfo.replace(80, 10, std::string("Test Game\0", 10));
    sfo.replace(96, 10, std::string("BLES12345\0", 10));
    tree.file("/data/homebrew/PPSA99303/rpcs3/games/PS3 example/PS3_GAME/PARAM.SFO", sfo);
    tree.file("/data/homebrew/PPSA99303/rpcs3/games/PS3 example/PS3_GAME/USRDIR/EBOOT.BIN", "PROGRAM NOT READ");
    auto result = std::make_unique<Library>();
    discover_library(tree.fs(), *result);
    check(result->scan.complete, "bounded successful scan");
    check(result->game_count == 4, "games only; no tracks, dependencies, partial dumps or duplicates");
    check(result->inventory[find_system("n64")].game_count == 1 &&
              result->inventory[find_system("ps2")].game_count == 1 &&
              result->inventory[find_system("ps1")].game_count == 1,
          "system folder mapping");
    check(result->inventory[find_system("snes")].emulator_count == 0, "info file is not installation evidence");
    check(result->route_count == 4 && result->scan.missing_references == 3, "real routes and missing disc references");
    check(result->inventory[find_system("ps3")].game_count == 1 &&
              result->inventory[find_system("ps3")].emulator_count == 0,
          "SFO identity without invented emulator evidence");
    const auto* preferred = preferred_route(*result, find_system("ps2"), "");
    check(preferred && std::strcmp(preferred->id, "PPSA99203") == 0, "standalone route is the default");
    preferred = preferred_route(*result, find_system("ps2"), "retroarch:ps5sx2");
    check(preferred && std::strcmp(preferred->id, "retroarch:ps5sx2") == 0, "explicit core preference wins");
    for (std::size_t i = 0; i < result->route_count; ++i)
        check(result->routes[i].capability == LaunchCapability::unavailable, "discovery is not launch validation");
    Tree psswan;
    psswan.app("PPSA99248");
    psswan.file("/data/homebrew/PPSA99248/games/Own game.chd", "MComprHD", 123456);
    psswan.file("/data/psswanstation/games/Outside game.chd", "MComprHD", 123456);
    auto psswan_library = std::make_unique<Library>();
    discover_library(psswan.fs(), *psswan_library);
    const auto* psswan_route = preferred_route(*psswan_library, find_system("ps1"), "PPSA99248");
    check(psswan_route && std::strcmp(psswan_route->name, "PSSwanStation") == 0 &&
              psswan_library->route_count == 1 && psswan_library->game_count == 2 &&
              psswan_library->inventory[find_system("ps1")].game_count == 2,
          "PSSwanStation is a PS1 emulator, with app-local and external games, not a native game tile");
    Tree console_shortcuts;
    console_shortcuts.app("PPSA99704");
    console_shortcuts.app("PPSA99705");
    console_shortcuts.file("/data/homebrew/PPSA99704/isshelf-console-tile.json",
                           "{\"format\":\"isshelf-console-tile/1\",\"system\":\"ps1\",\"titleId\":\"PPSA99704\","
                           "\"target\":\"PPSA99541\",\"forwarderSource\":\"https://github.com/heydemoura/ps5-forwarder-format\","
                           "\"forwarderCommit\":\"22e8762ad7dbd01e18bd8c0e95f86461fdf8aaf4\"}");
    console_shortcuts.file("/data/homebrew/PPSA99704/forwarder.json",
                           "{\"format\":\"ps5-forwarder/1\",\"target\":\"PPSA99541\"}");
    auto shortcut_library = std::make_unique<Library>();
    discover_library(console_shortcuts.fs(), *shortcut_library);
    check(shortcut_library->game_count == 1 &&
              std::strcmp(shortcut_library->games[0].title_id, "PPSA99705") == 0,
          "verified isShelf shortcut is hidden from native games but a foreign title in the same range remains");
    console_shortcuts.file("/data/homebrew/PPSA99704/isshelf-console-tile.json", "{\"format\":\"wrong\"}");
    shortcut_library = std::make_unique<Library>();
    discover_library(console_shortcuts.fs(), *shortcut_library);
    check(shortcut_library->game_count == 2,
          "an invalid ownership marker cannot hide a registered PS5 title");
    Tree covers;
    covers.file("/data/PSXS5/games/Exact/Exact.cue", "FILE \"Exact.bin\" BINARY\n");
    covers.file("/data/PSXS5/games/Exact/Exact.bin", "TRACK");
    covers.file("/data/PSXS5/library.txt", "Exact\tSLES-12345\t1\t/data/PSXS5/games/Exact/Exact.cue\tExact\n");
    covers.file("/data/PSXS5/covers/3d/SLES-12345.png", "IMAGE");
    covers.file("/data/PSXS5/covers/boxart/SLES-12345.png", "FRONT");
    covers.file("/data/PSXS5/covers/3d/SLES-00000.png", "WRONG GAME");
    covers.file("/data/PCSX2/games/PS2.iso", iso);
    covers.file("/data/PCSX2/chd-serials.txt", "/data/PCSX2/games/PS2.iso\t32776\t1\tSLUS-12345\n");
    covers.file("/data/PCSX2/covers/SLUS-12345.jpg", "FRONT");
    auto cover_library = std::make_unique<Library>();
    discover_library(covers.fs(), *cover_library);
    check(cover_library->game_count == 2 && cover_library->artwork.found == 2 && cover_library->scan.complete,
          "serial-indexed PS1 and size-checked PS2 covers are optional read-only metadata");
    for (std::size_t i = 0; i < cover_library->game_count; ++i)
        check(std::strstr(cover_library->games[i].artwork, "12345") && !cover_library->games[i].artwork_ready,
              "only the exact game's artwork is selected; discovery does not claim decoded artwork");
    for (std::size_t i = 0; i < cover_library->game_count; ++i)
        if (cover_library->games[i].system == find_system("ps1"))
            check(std::strstr(cover_library->games[i].artwork, "/3d/") &&
                      cover_library->games[i].artwork_kind == ArtworkKind::case_render,
                  "an existing 3D PS1 case is preferred and is identified so the renderer never wraps it twice");
    LibraryGame shape;
    shape.system = find_system("n64");
    auto profile = game_case_profile(shape, 1.43f);
    check(profile.width > profile.height && profile.material == CaseMaterial::cardboard,
          "western N64 games use a wide cardboard box, not a DVD case");
    std::strcpy(shape.name, "Example (Japan)");
    profile = game_case_profile(shape, .7f);
    check(profile.width < profile.height, "Japanese N64 artwork uses the portrait packaging");
    shape.system = find_system("ps2");
    std::strcpy(shape.name, "Example (Europe)");
    profile = game_case_profile(shape, .71f);
    check(profile.width < profile.height && profile.red == 4 && profile.green == 5 && profile.blue == 7 &&
              profile.material == CaseMaterial::plastic,
          "all PS2 regions have DVD-case proportions and the owner's requested black plastic");
    for (std::size_t system = 0; system < system_count; ++system) {
        shape.system = system;
        const auto p = game_case_profile(shape, 1);
        check(p.width > 0 && p.height > 0 && p.depth > 0 && p.depth < p.height,
              "every catalog system has finite, positive case proportions or an explicit non-case presentation");
    }
    shape.system = find_system("c64");
    for (float aspect : {.01f, 100.f})
        check(game_case_profile(shape, aspect).material == CaseMaterial::none,
              "extreme banner or spine scans stay flat instead of crossing the case camera's near plane");
    const std::string index_row = "Exact\tSLES-12345\t1\t/data/PSXS5/games/Exact/Exact.cue\tExact\n";
    covers.file("/data/PSXS5/library.txt", index_row + "#" + std::string(65535 - index_row.size() - 1, ' '));
    discover_small_stack(covers.fs(), *cover_library);
    check(cover_library->scan.complete && cover_library->artwork.found == 2,
          "a maximum-size serial index fits a 64 KiB discovery worker without hiding games or covers");
    covers.file("/data/PSXS5/library.txt", index_row, 65536);
    discover_small_stack(covers.fs(), *cover_library);
    check(cover_library->game_count == 2 && cover_library->artwork.found == 1,
          "an oversized optional serial index is skipped while discovery still completes");
    covers.file("/data/PSXS5/library.txt", index_row, 65535);
    discover_small_stack(covers.fs(), *cover_library);
    check(cover_library->scan.complete && cover_library->artwork.found == 1,
          "a short optional index read releases its buffer and leaves the game library usable");
    covers.file("/data/PSXS5/library.txt", index_row);
    covers.file("/data/PCSX2/chd-serials.txt", "/data/PCSX2/games/PS2.iso\t1\t1\tSLUS-12345\n");
    discover_library(covers.fs(), *cover_library);
    check(cover_library->artwork.found == 1, "a stale PS2 serial row with the wrong game size cannot assign art");
    covers.file("/data/PSXS5/library.txt", "Exact\tSLES-12345\t1\t/data/PSXS5/games/Exact/Exact.cue\tExact\nExact\tSLES-23456\t1\t/data/PSXS5/games/Exact/Exact.cue\tExact\n");
    discover_library(covers.fs(), *cover_library);
    check(cover_library->artwork.found == 0, "conflicting game-to-serial rows cannot choose a cover");
    check(valid_artwork_source("/user/app/PPSA12345/sce_sys/icon0.png") &&
              valid_artwork_source("/data/homebrew/PPSA99303/rpcs3/games/Own Game/PS3_GAME/ICON0.PNG"),
          "registered native icons and PS3 game icons are eligible artwork");
    for (const char* path : {"/data/PSXS5/retroachievements.ini", "/data/PSXS5/covers/../profile.png",
                             "/data/PCSX2/settings/account.png", "/user/app/PPSA12345/other.png"})
        check(!valid_artwork_source(path), "artwork reads cannot reach account files or escape a cover directory");
    UiState state;
    state.library = result.get();
    state.inventory = result->inventory;
    state.selected_system = find_system("n64");
    state.page = Page::details;
    state.detail_focus = 2;
    input(state, Action::confirm);
    check(state.page == Page::library && state.game_focus == 0, "real library navigation");
    input(state, Action::down);
    check(state.game_focus == 0, "library selection remains in bounds");
    input(state, Action::confirm);
    check(state.notice[0] != 0, "untested launch gives an honest notice");
    const auto selected_games = games_for_system(*result, state.selected_system);
    check(selected_games.count > 0, "favourite test has a discovered game");
    const auto& favorite_game = result->games[selected_games.indices[0]];
    input(state, Action::favorite);
    check(state.settings.game_favorites[0] == game_favorite_key(favorite_game), "Triangle saves the selected game's stable identity");
    input(state, Action::favorite);
    check(state.settings.game_favorites[0] == 0, "Triangle removes the same favourite without affecting files");
    tree.unreadable = "/data/homebrew/PPSA99169/content/n64/Mario Party.z64";
    discover_library(tree.fs(), *result);
    check(!result->scan.complete, "header transport failure is not a complete scan");
    tree.unreadable.clear();
    tree.denied = "/data/PCSX2/games";
    discover_library(tree.fs(), *result);
    check(!result->scan.complete && result->scan.roots_denied == 1, "unreadable root remains partial");
    check(result->inventory[find_system("ps2")].game_count == 0, "rescan clears stale inventory");
    std::puts("PASS: read-only discovery, bounded metadata, path validation, real cores, disc references, partial "
              "scans and game navigation.");
}
