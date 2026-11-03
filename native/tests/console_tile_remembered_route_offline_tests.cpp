// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/console_tile_remembered_route_offline.hpp"
#include "shelf/console_tile_write_gate.hpp"
#include "shelf/settings.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {
using namespace shelf;
using namespace shelf::tile_route;
using namespace shelf::tile_route::remembered_offline;

void check(bool okay, const char* label) {
    if (!okay) { std::fprintf(stderr, "FAIL remembered route: %s\n", label); std::exit(1); }
}

struct Fake {
    std::string stored, trace;
    bool write_ok = true, read_ok = true, tamper = false, apply_ok = true;
    ConsoleTileIntent applied{};
    unsigned apply_count = 0;
};
bool write(void* context, std::string_view value) {
    auto& fake = *static_cast<Fake*>(context);
    fake.trace += 'W';
    if (!fake.write_ok) return false;
    fake.stored.assign(value);
    return true;
}
bool readback(void* context, std::string& value) {
    auto& fake = *static_cast<Fake*>(context);
    fake.trace += 'R';
    if (!fake.read_ok) return false;
    value = fake.stored;
    if (fake.tamper) value += "# changed after save\n";
    return true;
}
bool apply(void* context, const ConsoleTileIntent& value) {
    auto& fake = *static_cast<Fake*>(context);
    fake.trace += 'A';
    fake.applied = value;
    ++fake.apply_count;
    return fake.apply_ok;
}
bool installed(void* context, std::size_t system, std::string_view title) {
    return *static_cast<bool*>(context) && system == system_for("ps2") && title == "PPSA99203";
}
std::string read_file(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    check(file != nullptr, "canonical settings file opens");
    std::string result;
    char bytes[4096];
    for (std::size_t n; (n = std::fread(bytes, 1, sizeof(bytes), file)) != 0;)
        result.append(bytes, n);
    check(!std::ferror(file) && std::fclose(file) == 0, "canonical settings file reads");
    return result;
}
ConsoleTileIntent choice(std::size_t system, ConsoleTileDestination destination,
                         const char* emulator = "") {
    ConsoleTileIntent intent{};
    intent.system = system;
    intent.destination = destination;
    std::snprintf(intent.emulator_title, sizeof(intent.emulator_title), "%s", emulator);
    return intent;
}
} // namespace

int main() {
    using namespace shelf;
    using namespace shelf::tile_route;
    using namespace shelf::tile_route::remembered_offline;
    static_assert(!console_tile_writes_enabled, "remembering routes cannot enable tile writes");
    char temporary[] = "/tmp/isshelf-remembered-route-XXXXXX";
    check(mkdtemp(temporary) != nullptr, "temporary settings folder created");
    const std::string path = std::string(temporary) + "/settings.ini";
    check(save_settings(path.c_str(), Settings{}), "real settings serializer makes base");
    const std::string base = read_file(path);
    check(settings_codec::scan_routes(base).file_valid,
          "real schema-1 settings output is a valid route base");
    Table routes{};
    Fake io;
    bool menu = true;
    check(system_count == 52, "all catalog systems represented");
    for (std::size_t system = 0; system < system_count; ++system) {
        const auto destination = system & 1 ? ConsoleTileDestination::home
                                            : ConsoleTileDestination::library;
        const auto selected = choice(system, destination);
        io.trace.clear();
        check(remember_then_apply(base, routes, selected, installed, &menu,
                                  write, readback, apply, &io) == Result::saved_and_applied,
              "every console choice saves and applies");
        check(io.trace == "WRA", "save and exact readback precede apply");
        const auto scan = settings_codec::scan_routes(io.stored);
        check(scan.file_valid && scan.routes[system].input == Input::valid,
              "each saved route reloads");
        routes = scan.routes;
    }
    check(io.stored.size() < settings_codec::maximum_settings_bytes,
          "all 52 choices fit schema-1 settings limit");
    const auto ps2 = system_for("ps2");
    const auto ps3 = system_for("ps3");
    const auto initial_ps3 = routes[ps3].preference.destination;
    io.trace.clear();
    check(remember_then_apply(base, routes,
                              choice(ps2, ConsoleTileDestination::emulator, "PPSA99203"),
                              installed, &menu, write, readback, apply, &io) ==
              Result::saved_and_applied && io.trace == "WRA",
          "emulator route saves before launch callback");
    routes = settings_codec::scan_routes(io.stored).routes;
    check(routes[ps2].preference.destination == ConsoleTileDestination::emulator &&
              std::strcmp(routes[ps2].preference.emulator_title, "PPSA99203") == 0 &&
              routes[ps3].preference.destination == initial_ps3,
          "one console choice does not rewrite another");
    menu = false;
    const auto stale = tile_route::resolve(Source{ps2}, routes[ps2].input,
                                            routes[ps2].preference, installed, &menu);
    check(stale.resolution == Resolution::stale_emulator,
          "removed emulator needs chooser, never another menu");
    io.trace.clear();
    check(remember_then_apply(base, routes,
                              choice(ps2, ConsoleTileDestination::emulator, "PPSA99203"),
                              installed, &menu, write, readback, apply, &io) ==
              Result::stale_emulator && io.trace.empty(),
          "stale emulator cannot save or launch");
    menu = true;
    io.trace.clear(); io.write_ok = false;
    check(remember_then_apply(base, routes, choice(ps2, ConsoleTileDestination::home),
                              installed, &menu, write, readback, apply, &io) ==
              Result::write_failed && io.trace == "W",
          "failed save cannot launch");
    io.write_ok = true; io.trace.clear(); io.tamper = true;
    check(remember_then_apply(base, routes, choice(ps2, ConsoleTileDestination::home),
                              installed, &menu, write, readback, apply, &io) ==
              Result::readback_uncertain && io.trace == "WR",
          "changed readback cannot launch");
    io.tamper = false; io.trace.clear(); io.apply_ok = false;
    check(remember_then_apply(base, routes, choice(ps2, ConsoleTileDestination::home),
                              installed, &menu, write, readback, apply, &io) ==
              Result::saved_apply_failed && io.trace == "WRA",
          "an unavailable post-save route is distinct from a save failure");
    std::string encoded;
    check(!append_routes("version=1\nroute.ps2=home\n", routes, encoded),
          "cannot silently duplicate an existing route line");
    auto invalid = routes;
    invalid[ps2].preference.system = ps3;
    check(!append_routes(base, invalid, encoded), "cross-console preference rejected");
    invalid = routes;
    invalid[ps2].input = Input::invalid;
    check(!append_routes(base, invalid, encoded), "corrupt route rejected");
    // The production schema-1 reader and writer now preserve optional route
    // keys. Their values remain untrusted UI input, never tile authority.
    FILE* file = std::fopen(path.c_str(), "wb");
    check(file && std::fwrite(io.stored.data(), 1, io.stored.size(), file) ==
                      io.stored.size() && std::fclose(file) == 0,
          "candidate settings text written in temporary host folder");
    Settings loaded;
    check(load_settings(path.c_str(), loaded) == SettingsLoad::loaded &&
              loaded.shortcut_routes[ps2].input == Input::valid &&
              loaded.shortcut_routes[ps2].preference.destination == ConsoleTileDestination::home,
          "production reader accepts saved route lines");
    check(save_settings(path.c_str(), loaded), "production writer saves route lines");
    const auto saved_scan = settings_codec::scan_routes(read_file(path));
    check(saved_scan.file_valid && saved_scan.routes[ps2].input == Input::valid &&
              saved_scan.routes[ps2].preference.destination == ConsoleTileDestination::home,
          "production writer preserves saved route on round trip");
    std::remove(path.c_str());
    std::remove((path + ".bak").c_str());
    rmdir(temporary);
    std::puts("PASS: 52 remembered choices, exact readback before apply, stale emulator and failure stops");
}
