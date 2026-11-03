// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/model.hpp"
#include <cstdio>
#include <cstdlib>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
bool has(const shelf::BrowseResult& result, std::size_t id) {
    for (std::size_t i = 0; i < result.count; ++i)
        if (result.indices[i] == id)
            return true;
    return false;
}
} // namespace

void test_settings();
void test_ui();
void test_library();
void test_navigation();
void test_launch();
void test_ui_test_bridge();
void test_console_tile_startup();
void test_pairing_code_entry();
void test_console_tile_relay();
void test_console_tile_forwarder_config();
void test_ui_test_preview();
int main() {
    test_console_tile_forwarder_config();
    test_console_tile_relay();
    test_pairing_code_entry();
    test_console_tile_startup();
    using namespace shelf;
    Preferences preferences;
    Inventory inventory{};
    require(system_count == 52, "catalogue size");
    Availability missing;
    require(!use_monochrome(preferences, missing, "ps4") && !use_monochrome(preferences, missing, "ps5"),
            "native systems stay in colour even when their installed game list is empty");
    require(use_monochrome(preferences, missing), "missing emulators are monochrome by default");
    missing.game_count = 3;
    require(use_monochrome(preferences, missing), "game files alone do not imply an installed emulator");
    missing.emulator_count = 1;
    require(!use_monochrome(preferences, missing), "an installed emulator keeps its system in colour");
    missing.emulator_count = 0;
    missing.built_in = true;
    require(!use_monochrome(preferences, missing), "built-in PS4/PS5 do not need an emulator");
    Preferences full_colour;
    full_colour.monochrome_missing = false;
    require(!use_monochrome(full_colour, Availability{}), "the full-colour override is independent of installation");
    const auto ps5 = find_system("ps5"), n64 = find_system("n64"), ps3 = find_system("ps3");
    require(ps5 < system_count && n64 < system_count && ps3 < system_count, "known system identities");
    require(find_system("not-a-console") == system_count, "unknown identities stay absent");
    require(browse(preferences, inventory).count == 52, "all mode works before discovery");
    preferences.scope = Scope::available;
    require(browse(preferences, inventory).count == 0, "catalogue hints never pretend emulators are installed");
    inventory[ps5].built_in = true;
    inventory[n64] = {false, 1, 10, LaunchCapability::direct_game};
    inventory[ps3].game_count = 2;
    auto result = browse(preferences, inventory);
    require(result.count == 3 && has(result, ps5) && has(result, n64) && has(result, ps3),
            "built-in, emulator and games each count as availability");
    require(inventory[ps3].launch == LaunchCapability::unavailable, "game files do not imply a launch route");
    preferences.filter = SystemFilter::games;
    require(browse(preferences, inventory).count == 2, "game filter uses discovered content");
    preferences.filter = SystemFilter::emulators;
    require(browse(preferences, inventory).count == 1, "emulator filter uses actual installed evidence");
    preferences.filter = SystemFilter::built_in;
    require(browse(preferences, inventory).count == 1, "built-in routes can be selected independently");
    preferences.filter = SystemFilter::no_route;
    require(browse(preferences, inventory).count == 2, "visible content without a launch route remains identifiable");
    preferences.filter = SystemFilter::all;
    preferences.hidden[n64] = true;
    result = browse(preferences, inventory);
    require(result.count == 2 && !has(result, n64), "individual visibility composes with automatic availability");
    preferences.scope = Scope::all;
    require(browse(preferences, inventory).count == 51, "individual visibility also applies in all mode");
    preferences.hidden.fill(true);
    require(browse(preferences, inventory).count == 0, "an empty personal shelf is valid");
    preferences.hidden.fill(false);
    result = browse(preferences, inventory, {"64", "Nintendo"});
    require(result.count == 1 && result.indices[0] == n64, "brand and name search compose");
    require(browse(preferences, inventory, {"PLAYSTATION", ""}).count == 6, "ASCII search ignores case");
    require(browse(preferences, inventory, {"mednafen_wswan", ""}).count == 2, "suggested cores remain searchable");
    require(browse(preferences, inventory, {"no matches", ""}).count == 0, "empty search results");
    preferences.sort = Sort::name_asc;
    result = browse(preferences, inventory);
    require(catalog[result.indices[0]].id == "threedo", "alphabetical order");
    preferences.sort = Sort::newest;
    result = browse(preferences, inventory);
    require(catalog[result.indices[0]].id == "switch2", "newest hardware order does not claim support");
    preferences.sort = Sort::games;
    result = browse(preferences, inventory);
    require(result.indices[0] == n64 && result.indices[1] == ps3, "actual discovered game counts determine game order");
    preferences.favorite[ps3] = true;
    preferences.sort = Sort::favorites;
    result = browse(preferences, inventory);
    require(result.indices[0] == ps3, "favorites sort first");
    require(inventory[n64].game_count == 10 && inventory[ps3].launch == LaunchCapability::unavailable,
            "browsing preserves discovery evidence");
    std::puts("PASS: native catalogue, real-availability filtering, personal visibility, search, sorting and "
              "launch-capability separation.");
    test_settings();
    test_ui();
    test_navigation();
    test_library();
    test_launch();
    test_ui_test_bridge();
    test_ui_test_preview();
    return 0;
}
