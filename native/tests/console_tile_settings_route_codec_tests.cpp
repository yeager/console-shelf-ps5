// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/console_tile_settings_route_codec.hpp"
#include "shelf/console_tile_write_gate.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
void check(bool okay, const char* reason) {
    if (!okay) {
        std::fprintf(stderr, "FAIL settings route codec: %s\n", reason);
        std::exit(1);
    }
}
} // namespace

int main() {
    using namespace shelf;
    using namespace shelf::tile_route;
    using namespace shelf::tile_route::settings_codec;
#if defined(SHELF_PS5) || defined(SHELF_TILE_WRITE_QUARANTINE_TEST)
    static_assert(!console_tile_writes_enabled, "Codec cannot enable PS5 tile writes");
#endif
    check(system_count == 52, "catalog count is pinned");
    std::string complete = "# existing settings.ini schema 1\nversion=1\nlayout=shelf\n";
    for (std::size_t system = 0; system < system_count; ++system) {
        const Preference wanted{system, system & 1 ? ConsoleTileDestination::home :
                                                ConsoleTileDestination::library, {}};
        char line[128]{};
        check(encode_line(wanted, line, sizeof(line)), "each console route encodes");
        complete += line;
    }
    const auto all = scan_routes(complete);
    check(all.file_valid, "schema 1 with 52 optional route lines scans");
    for (std::size_t system = 0; system < system_count; ++system)
        check(all.routes[system].input == Input::valid &&
                  all.routes[system].preference.system == system &&
                  all.routes[system].preference.destination ==
                      (system & 1 ? ConsoleTileDestination::home : ConsoleTileDestination::library),
              "every route remains bound to its catalog system");
    check(complete.size() < maximum_settings_bytes,
          "52 route lines fit the current settings.ini byte limit");

    const auto ps2 = system_for("ps2");
    Preference menu{ps2, ConsoleTileDestination::emulator, {}};
    std::memcpy(menu.emulator_title, "PPSA99203", 10);
    char line[128]{};
    check(encode_line(menu, line, sizeof(line)) &&
          std::string(line) == "route.ps2=emulator:PPSA99203\n",
          "PS2 emulator line uses a pinned exact title ID");
    auto one = scan_routes(std::string("version=1\n") + line);
    check(one.file_valid && one.routes[ps2].input == Input::valid &&
          one.routes[ps2].preference.destination == ConsoleTileDestination::emulator &&
          std::strcmp(one.routes[ps2].preference.emulator_title, "PPSA99203") == 0,
          "pinned emulator route round-trips");
    check(one.routes[system_for("ps3")].input == Input::absent,
          "missing console route remains absent");

    const auto threedo = system_for("threedo");
    one = scan_routes("version=1\nroute.threedo=emulator:PPSA99169\n");
    check(one.file_valid && one.routes[threedo].input == Input::valid &&
              one.routes[threedo].preference.destination ==
                  ConsoleTileDestination::library &&
              !one.routes[threedo].preference.emulator_title[0],
          "a legacy shared RetroArch route migrates to its matching isShelf library");
    one = scan_routes("version=1\nroute.switch2=emulator:PPSA99169\n");
    check(one.file_valid && one.routes[system_for("switch2")].input == Input::invalid,
          "an unsupported console cannot borrow the RetroArch migration path");

    one = scan_routes("version=1\nroute.ps2=library\nroute.ps2=home\n");
    check(one.file_valid && one.routes[ps2].input == Input::invalid &&
          one.routes[ps2].preference.system == system_count,
          "duplicate route cannot let either value win");
    one = scan_routes("version=1\nroute.ps2=emulator:PPSA99203\nroute.ps2=home\n");
    check(one.file_valid && one.routes[ps2].input == Input::invalid,
          "duplicate cannot override an emulator route");
    one = scan_routes("version=1\nroute.ps2=invalid\nroute.ps2=library\n");
    check(one.file_valid && one.routes[ps2].input == Input::invalid,
          "a later value cannot repair an invalid duplicate");
    one = scan_routes("version=1\nroute.ps2=emulator:PPSA99303\n");
    check(one.file_valid && one.routes[ps2].input == Input::invalid,
          "emulator title pinned to another console is rejected");
    one = scan_routes("version=1\nroute.ps2=emulator:PPSA99203-extra\n");
    check(one.file_valid && one.routes[ps2].input == Input::invalid,
          "overlong emulator identity is rejected");
    one = scan_routes("version=1\nroute.ps2=library\r\n");
    check(one.file_valid && one.routes[ps2].input == Input::valid,
          "canonical CRLF line is accepted like settings.ini");
    one = scan_routes("version=1\nroute.ps2=home");
    check(one.file_valid && one.routes[ps2].input == Input::valid,
          "last line without newline is accepted");

    check(!scan_routes("version=1\nroute.unknown=home\n").file_valid,
          "unknown console ID invalidates route scan");
    check(!scan_routes("version=1\nroute.PS2=home\n").file_valid,
          "noncanonical console ID invalidates route scan");
    check(!scan_routes("version=1\nroute.ps2 =home\n").file_valid,
          "spaced route key cannot alias catalog ID");
    check(!scan_routes("version=2\nroute.ps2=home\n").file_valid &&
          !scan_routes("route.ps2=home\n").file_valid &&
          !scan_routes("version=1\nversion=1\nroute.ps2=home\n").file_valid,
          "version must be exactly one schema-1 line");
    std::string embedded_nul = "version=1\nroute.ps2=home";
    embedded_nul.push_back('\0');
    embedded_nul.push_back('x');
    check(!scan_routes(embedded_nul).file_valid,
          "embedded NUL invalidates route scan");
    check(!scan_routes(std::string("version=1\n#") + std::string(254, 'x') + "\n").file_valid,
          "line beyond the existing parser limit is rejected");
    check(!scan_routes(std::string("version=1\n") + std::string(32768, 'x')).file_valid,
          "file beyond the existing byte limit is rejected");
    check(!scan_routes("version=1\nroute.ps2=home\rjunk\n").file_valid,
          "bare CR cannot change a route value");
    char short_line[8]{};
    check(!encode_line(menu, short_line, sizeof(short_line)),
          "truncated route line is never accepted");
    std::puts("PASS: optional settings route codec validates 52 catalog routes, duplicates and malformed values");
}
