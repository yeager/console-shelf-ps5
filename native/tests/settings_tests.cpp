// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/settings.hpp"
#include "shelf/console_tile_remembered_route_runtime.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif
namespace {
void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL settings: %s\n", message);
        std::exit(1);
    }
}
void write(const char* path, const char* text) {
    FILE* file = std::fopen(path, "wb");
    check(file != nullptr, "test file open");
    check(std::fwrite(text, 1, std::strlen(text), file) == std::strlen(text), "test file write");
    std::fclose(file);
}
} // namespace
void test_settings() {
    using namespace shelf;
    check(localize(Language::swedish, "Language") == "Språk", "Swedish catalogue translates a UI key");
    check(localize(Language::swedish, "Uncatalogued fallback") == "Uncatalogued fallback",
          "new source text remains usable before its catalogue entry exists");
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    char directory[128], path[160];
    std::snprintf(directory, sizeof(directory), "settings-test-%lld", static_cast<long long>(stamp));
#ifdef _WIN32
    check(_mkdir(directory) == 0, "owned test directory");
#else
    check(mkdir(directory, 0700) == 0, "owned test directory");
#endif
    std::snprintf(path, sizeof(path), "%s/settings.ini", directory);
    Settings settings;
    check(load_settings(path, settings) == SettingsLoad::missing, "first run uses defaults");
    check(settings.shortcut_routes[find_system("ps2")].input == tile_route::Input::absent,
          "first run has no invented Home shortcut destination");
    check(settings.preferences.icon == AppIcon::vector, "new settings use the vector logo");
    for (const auto logo : {AppIcon::gaming, AppIcon::classic, AppIcon::jade, AppIcon::vector}) {
        settings.preferences.icon = logo;
        check(save_settings(path, settings), "each old and new logo can be saved");
        Settings reopened;
        check(load_settings(path, reopened) == SettingsLoad::loaded && reopened.preferences.icon == logo,
              "each old and new logo survives reopening");
    }
    settings.preferences.hidden[find_system("ps2")] = true;
    settings.preferences.favorite[find_system("n64")] = true;
    settings.preferences.sort = Sort::favorites;
    settings.preferences.language = Language::swedish;
    settings.preferences.brand = 2;
    std::strcpy(settings.preferred_emulator[find_system("ps2")].data(), "ps5sx2");
    std::strcpy(settings.preferred_emulator[find_system("ps1")].data(), "retroarch:swanstation");
    std::strcpy(settings.preferred_emulator[find_system("n64")].data(), "retroarch:mupen64plus_next");
    settings.shortcut_routes[find_system("ps2")].input = tile_route::Input::valid;
    settings.shortcut_routes[find_system("ps2")].preference =
        {find_system("ps2"), ConsoleTileDestination::home, {}};
    settings.shortcut_routes[find_system("ps1")].input = tile_route::Input::valid;
    settings.shortcut_routes[find_system("ps1")].preference.system = find_system("ps1");
    settings.shortcut_routes[find_system("ps1")].preference.destination = ConsoleTileDestination::emulator;
    std::strcpy(settings.shortcut_routes[find_system("ps1")].preference.emulator_title, "PPSA99248");
    check(save_settings(path, settings), "initial atomic save");
    Settings read;
    check(load_settings(path, read) == SettingsLoad::loaded, "reload valid settings");
    check(read.preferences.hidden[find_system("ps2")] && read.preferences.favorite[find_system("n64")],
          "stable console IDs survive reload");
    check(read.preferences.sort == Sort::favorites && read.preferences.brand == 2 && read.preferences.language == Language::swedish,
          "sort, brand and language persist");
    check(std::strcmp(read.preferred_emulator[find_system("ps2")].data(), "ps5sx2") == 0,
          "preferred emulator persists");
    check(std::strcmp(read.preferred_emulator[find_system("ps1")].data(), "retroarch:swanstation") == 0 &&
              std::strcmp(read.preferred_emulator[find_system("n64")].data(), "retroarch:mupen64plus_next") == 0,
          "real discovered RetroArch route IDs survive atomic save and reload");
    check(read.shortcut_routes[find_system("ps2")].input == tile_route::Input::valid &&
              read.shortcut_routes[find_system("ps2")].preference.destination == ConsoleTileDestination::home &&
              read.shortcut_routes[find_system("ps1")].input == tile_route::Input::valid &&
              std::strcmp(read.shortcut_routes[find_system("ps1")].preference.emulator_title,
                          "PPSA99248") == 0,
          "optional per-console Home destinations survive atomic save and reload");
    Settings remembered;
    const ConsoleTileIntent ps2_library{find_system("ps2"), ConsoleTileDestination::library, {}};
    check(tile_route::remember_shortcut_route(path, read, ps2_library, nullptr, remembered) &&
              remembered.shortcut_routes[find_system("ps2")].preference.destination ==
                  ConsoleTileDestination::library,
          "a selected destination is durable and read back before use");
    const ConsoleTileIntent ps1_menu{find_system("ps1"), ConsoleTileDestination::emulator, "PPSA99248"};
    Settings unchanged = remembered;
    check(!tile_route::remember_shortcut_route(path, remembered, ps1_menu, nullptr, unchanged) &&
              unchanged.shortcut_routes[find_system("ps1")].preference.destination ==
                  ConsoleTileDestination::emulator,
          "an emulator choice cannot save without an installed matching menu");
    static Library installed;
    installed.route_count = 2;
    installed.routes[0].system = find_system("ps1");
    std::snprintf(installed.routes[0].title_id, sizeof(installed.routes[0].title_id), "PPSA99248");
    std::snprintf(installed.routes[0].folder, sizeof(installed.routes[0].folder),
                  "/data/homebrew/PPSA99248");
    installed.routes[1].system = find_system("ps2");
    std::snprintf(installed.routes[1].title_id, sizeof(installed.routes[1].title_id), "PPSA99203");
    std::snprintf(installed.routes[1].folder, sizeof(installed.routes[1].folder),
                  "/data/homebrew/PPSA99203");
    check(tile_route::remember_shortcut_route(path, remembered, ps1_menu, &installed, remembered) &&
              std::strcmp(remembered.shortcut_routes[find_system("ps1")].preference.emulator_title,
                          "PPSA99248") == 0,
          "the installed pinned emulator choice is read back before launch");
    std::array<ConsoleTileIntent, system_count> batch_intents{};
    std::array<bool, system_count> batch_selected{};
    const auto ps1 = find_system("ps1");
    const auto ps2 = find_system("ps2");
    batch_intents[ps1] = {ps1, ConsoleTileDestination::emulator, "PPSA99248"};
    batch_intents[ps2] = {ps2, ConsoleTileDestination::emulator, "PPSA99203"};
    batch_selected[ps1] = true;
    batch_selected[ps2] = true;
    Settings batch_remembered;
    check(tile_route::remember_shortcut_routes(path, remembered, batch_intents,
                                               batch_selected, &installed,
                                               batch_remembered) &&
              std::strcmp(batch_remembered.shortcut_routes[ps1].preference.emulator_title,
                          "PPSA99248") == 0 &&
              std::strcmp(batch_remembered.shortcut_routes[ps2].preference.emulator_title,
                          "PPSA99203") == 0,
          "a verified emulator batch is saved once and fully read back");
    auto invalid_batch = batch_intents;
    invalid_batch[ps2].system = ps1;
    Settings rejected_batch = batch_remembered;
    check(!tile_route::remember_shortcut_routes(path, batch_remembered, invalid_batch,
                                                batch_selected, &installed,
                                                rejected_batch) &&
              std::strcmp(rejected_batch.shortcut_routes[ps2].preference.emulator_title,
                          "PPSA99203") == 0,
          "an invalid emulator batch cannot partially replace remembered routes");
    ConsoleTileOutcome updated;
    updated.stage = ConsoleTileStage::registered;
    updated.owned = true;
    updated.intent = ps2_library;
    Settings synchronized;
    check(tile_route::remember_upsert_route(path, remembered, ps2_library, updated,
                                           &installed, synchronized) ==
              tile_route::UpsertRouteSync::saved &&
              synchronized.shortcut_routes[find_system("ps2")].preference.destination ==
                  ConsoleTileDestination::library,
          "a committed tile update synchronizes the separately remembered launch choice");
    const ConsoleTileIntent ps2_home{find_system("ps2"), ConsoleTileDestination::home, {}};
    check(tile_route::remember_upsert_route(path, synchronized, ps2_home, updated,
                                           &installed, unchanged) ==
              tile_route::UpsertRouteSync::not_applicable,
          "a mismatched manager readback cannot change the remembered launch choice");
    updated.stage = ConsoleTileStage::failed;
    updated.intent = ps2_home;
    check(tile_route::remember_upsert_route(path, synchronized, ps2_home, updated,
                                           &installed, unchanged) ==
              tile_route::UpsertRouteSync::not_applicable,
          "a failed tile update cannot change the remembered launch choice");
    updated.stage = ConsoleTileStage::registered;
    check(tile_route::remember_upsert_route("missing-parent/route/settings.ini", synchronized,
                                           ps2_home, updated, &installed, unchanged) ==
              tile_route::UpsertRouteSync::failed,
          "a committed source with an unsaved preference reports synchronization failure");
    check(!tile_route::remember_shortcut_route("missing-parent/route/settings.ini", remembered,
                                              ps2_library, nullptr, unchanged),
          "a failed route save cannot report success");
    Settings every_console;
    for (std::size_t i = 0; i < system_count; ++i) {
        every_console.shortcut_routes[i].input = tile_route::Input::valid;
        every_console.shortcut_routes[i].preference = {i, ConsoleTileDestination::home, {}};
    }
    char all_path[160];
    std::snprintf(all_path, sizeof(all_path), "%s/all-routes.ini", directory);
    Settings every_readback;
    check(save_settings(all_path, every_console) &&
              load_settings(all_path, every_readback) == SettingsLoad::loaded,
          "the ordinary settings writer accepts all 52 remembered destinations");
    for (std::size_t i = 0; i < system_count; ++i)
        check(every_readback.shortcut_routes[i].input == tile_route::Input::valid &&
                  every_readback.shortcut_routes[i].preference.system == i &&
                  every_readback.shortcut_routes[i].preference.destination == ConsoleTileDestination::home,
              "each catalog console keeps its own saved destination");
    for (const char* bad : {"retroarch:", "retroarch:../other", "other:core", "retroarch:core:other"}) {
        Settings rejected = settings;
        std::strcpy(rejected.preferred_emulator[find_system("ps1")].data(), bad);
        check(!save_settings(path, rejected), "malformed namespaced route IDs cannot replace valid settings");
    }
    settings.preferences.layout = Layout::swipe;
    check(save_settings(path, settings), "replacement preserves a valid backup");
    check(load_settings(path, read) == SettingsLoad::loaded && read.preferences.layout == Layout::swipe,
          "latest save is current");
    write(path, "version=999\nlayout=grid\n");
    check(load_settings(path, read) == SettingsLoad::backup && read.preferences.layout == Layout::grid,
          "damaged/unsupported current file recovers last valid backup");
    FILE* file = std::fopen(path, "rb");
    char original[64]{};
    check(file != nullptr, "corrupt source retained");
    std::fread(original, 1, 63, file);
    std::fclose(file);
    check(std::strstr(original, "version=999") != nullptr, "load never rewrites damaged data");
    check(save_settings(path, read), "explicit subsequent save can replace damaged current file");
    std::strcpy(read.preferred_emulator[find_system("ps2")].data(), "../invalid");
    check(!save_settings(path, read), "invalid route identifier cannot enter settings");
    check(load_settings(path, read) == SettingsLoad::loaded, "rejected save preserves current settings");
    read.preferences.layout = Layout::shelf;
    check(save_settings(path, read), "save third layout without changing the settings schema");
    Settings displayed;
    check(load_settings(path, displayed) == SettingsLoad::loaded && displayed.preferences.layout == Layout::shelf &&
              displayed.preferences.favorite[find_system("n64")], "Shelf layout and existing choices survive reopen");
    displayed.preferences.monochrome_missing = false;
    displayed.preferences.shelf_single = true;
    displayed.preferences.shelf_selected[find_system("n64")] = true;
    displayed.preferences.reduced_motion = true;
    displayed.preferences.tv_mode = TvMode::message;
    displayed.preferences.tv_noise = TvNoise::either;
    displayed.preferences.tv_period = 3;
    displayed.preferences.intro_all = true;
    std::strcpy(displayed.tv_text, "Iswebro's play room = home");
    displayed.game_favorites[3] = 0x0123456789abcdefull;
    check(save_settings(path, displayed) && load_settings(path, read) == SettingsLoad::loaded &&
              !read.preferences.monochrome_missing, "the full-colour override survives an atomic save and reopen");
    check(read.preferences.shelf_single && read.preferences.shelf_selected[find_system("n64")] &&
          read.preferences.reduced_motion, "curated selection and motion choice survive reopen");
    check(read.preferences.tv_mode == TvMode::message && read.preferences.tv_noise == TvNoise::either && read.preferences.tv_period == 3 &&
          read.preferences.intro_all && std::strcmp(read.tv_text, "Iswebro's play room = home") == 0 &&
          read.game_favorites[3] == 0x0123456789abcdefull, "room display, text, timing and game identities survive atomic save/reopen");
    auto injected = read; std::strcpy(injected.tv_text, "name\nlayout=swipe");
    check(!save_settings(path, injected), "personal text cannot inject settings lines");
    auto duplicate = read; duplicate.game_favorites[4] = duplicate.game_favorites[3];
    check(!save_settings(path, duplicate), "duplicate game identities cannot corrupt a slideshow");
    auto video = read; video.preferences.tv_mode = TvMode::video;
    check(save_settings(path, video) && load_settings(path, read) == SettingsLoad::loaded && read.preferences.tv_mode == TvMode::video,
          "video mode survives an atomic save/reopen");
    video.preferences.tv_noise = static_cast<TvNoise>(99);
    check(!save_settings(path, video), "unknown static policy cannot replace saved preferences");
    auto overfull = read;
    overfull.preferences.shelf_selected.fill(true);
    check(!save_settings(path, overfull), "an overfull curated display cannot replace saved settings");
    char legacy[160];
    std::snprintf(legacy, sizeof(legacy), "%s/legacy.ini", directory);
    write(legacy, "version=1\nlayout=grid\nlook=icons\nscope=all\nsort=featured\nfilter=all\nicon=gaming\nbrand=\n");
    Settings old_settings;
    check(load_settings(legacy, old_settings) == SettingsLoad::loaded && old_settings.preferences.monochrome_missing,
          "old settings remain valid and receive the default missing-emulator style");
    check(old_settings.shortcut_routes[find_system("ps2")].input == tile_route::Input::absent,
          "old schema-1 settings load without a fabricated Home shortcut destination");
    check(!old_settings.preferences.shelf_single && !old_settings.preferences.reduced_motion,
          "legacy settings use paged browsing and smooth motion without losing their choices");
    check(old_settings.preferences.tv_mode == TvMode::console && old_settings.preferences.tv_noise == TvNoise::off && old_settings.game_favorites[0] == 0,
          "legacy settings acquire room defaults without acquiring fabricated favourite games");
    write(legacy, "version=1\nlayout=grid\nlook=icons\nscope=all\nsort=featured\nfilter=all\nicon=gaming\nbrand=\nmonochrome_missing=2\n");
    check(load_settings(legacy, old_settings) == SettingsLoad::invalid, "malformed optional styles are not accepted");
    write(legacy, "version=1\nlayout=grid\nlook=icons\nscope=all\nsort=featured\nfilter=all\nicon=gaming\nbrand=\nroute.ps2=unrecognized\n");
    check(load_settings(legacy, old_settings) == SettingsLoad::loaded &&
              old_settings.shortcut_routes[find_system("ps2")].input == tile_route::Input::invalid,
          "malformed Home choice becomes an explicit chooser without rejecting other settings");
    write(legacy, "version=1\nlayout=grid\nlook=icons\nscope=all\nsort=featured\nfilter=all\nicon=gaming\nbrand=\nroute.threedo=emulator:PPSA99169\n");
    const auto threedo = find_system("threedo");
    check(load_settings(legacy, old_settings) == SettingsLoad::loaded &&
              old_settings.shortcut_routes[threedo].input == tile_route::Input::valid &&
              old_settings.shortcut_routes[threedo].preference.destination ==
                  ConsoleTileDestination::library,
          "a legacy shared RetroArch preference migrates to its matching isShelf library");
    write(legacy, "version=1\nlayout=grid\nlook=icons\nscope=all\nsort=featured\nfilter=all\nicon=gaming\nbrand=\nroute.switch2=emulator:PPSA99169\n");
    check(load_settings(legacy, old_settings) == SettingsLoad::loaded &&
              old_settings.shortcut_routes[find_system("switch2")].input ==
                  tile_route::Input::invalid,
          "an unsupported console cannot use the RetroArch migration path");
    write(legacy, "version=1\nlayout=grid\nlook=icons\nscope=all\nsort=featured\nfilter=all\nicon=gaming\nbrand=\nroute.ps2=home\nroute.ps2=library\n");
    check(load_settings(legacy, old_settings) == SettingsLoad::loaded &&
              old_settings.shortcut_routes[find_system("ps2")].input == tile_route::Input::invalid,
          "duplicate Home choices never pick a winner");
    check(!save_settings("missing-parent/does-not-exist/settings.ini", read), "unwritable save reports failure");
    char invalid[160];
    std::snprintf(invalid, sizeof(invalid), "%s/invalid.ini", directory);
    write(invalid, "version=1\nlayout=grid\nlayout=swipe\nlook=icons\nscope=all\nsort=featured\nfilter=all\nicon="
                   "gaming\nbrand=\n");
    check(load_settings(invalid, read) == SettingsLoad::invalid, "duplicate keys are corruption, not missing settings");
    check(read.preferences.layout == Layout::grid && !read.preferences.hidden[find_system("ps2")],
          "unrecoverable corruption returns clean defaults");
    check(save_settings(invalid, Settings{}), "create a clean file for binary-corruption check");
    FILE* binary = std::fopen(invalid, "ab");
    check(binary != nullptr, "binary-corruption fixture");
    const char zero = 0;
    check(std::fwrite(&zero, 1, 1, binary) == 1, "binary-corruption fixture write");
    std::fclose(binary);
    check(load_settings(invalid, read) == SettingsLoad::invalid,
          "embedded NUL bytes cannot be accepted as text settings");
    std::puts("PASS: native settings roundtrip, stable IDs, atomic replacement, backup recovery, invalid data "
              "preservation and save failures.");
}
