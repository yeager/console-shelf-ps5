// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/console_tile_route_preference.hpp"
#include "shelf/console_tile_write_gate.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
void check(bool okay, const char* reason) {
    if (!okay) { std::fprintf(stderr, "FAIL route preference: %s\n", reason); std::exit(1); }
}
bool installed(void* context, std::size_t system, std::string_view title) {
    const auto* available = static_cast<const shelf::tile_route::Preference*>(context);
    return available && system == available->system && title == available->emulator_title;
}
const char* menu_title(std::size_t system) {
    const auto id = shelf::catalog[system].id;
    if (id == "ps3") return "PPSA99303";
    if (id == "ps2") return "PPSA99203";
    if (id == "ps1") return "PPSA99248";
    if (id == "switch") return "PPSA99008";
    if (id == "wiiu") return "PPSA99360";
    return shelf::catalog[system].suggested_core.empty() ? nullptr : "PPSA99169";
}
} // namespace

int main() {
    using namespace shelf;
    using namespace shelf::tile_route;
#if defined(SHELF_PS5) || defined(SHELF_TILE_WRITE_QUARANTINE_TEST)
    static_assert(!console_tile_writes_enabled, "This prototype must not open the PS5 CREATE gate");
#endif
    check(system_count == 52, "catalog size is pinned");
    std::size_t direct_menus = 0;
    std::size_t shared_retroarch_menus = 0;
    for (std::size_t system = 0; system < system_count; ++system) {
        Source source{system}, selected;
        char marker[maximum_record + 1]{}, argument[console_tile_message_capacity + 1]{};
        check(encode_source(source, marker, sizeof(marker)) &&
              encode_argument(source, argument, sizeof(argument)),
              "each catalog source encodes in bounded marker and argument");
        check(parse_source(marker, selected) && selected.system == system,
              "each marker pins a catalog title");
        char* argv[] = {argument};
        check(select_source(marker, 1, argv, selected) == Input::valid &&
              selected.system == system, "PS5 argv[0] selects one source");
        ConsoleTileIntent old;
        check(!parse_console_tile_arg(argument, old),
              "unintegrated source is rejected by the existing live selector");
        const std::string immutable_marker = marker, immutable_arg = argument;
        for (auto destination : {ConsoleTileDestination::library, ConsoleTileDestination::home}) {
            Preference preference{system, destination, {}};
            char record[maximum_record + 1]{};
            Preference parsed;
            check(encode_preference(preference, record, sizeof(record)) &&
                  parse_preference(record, parsed) && parsed.system == system &&
                  parsed.destination == destination,
                  "library and Home choices round-trip for all 52 consoles");
            const auto decision = resolve(source, Input::valid, parsed, nullptr, nullptr);
            check(decision.resolution == Resolution::ready &&
                  console_tile_intent_valid(decision.intent) &&
                  decision.intent.system == system && decision.intent.destination == destination,
                  "a saved route yields a validated existing startup intent");
            check(immutable_marker == marker && immutable_arg == argument,
                  "changing a preference never changes the source marker or argument");
        }
        Preference missing;
        check(resolve(source, Input::absent, missing, nullptr, nullptr).resolution ==
                  Resolution::choose_route,
              "missing preference requires a choice");
        check(resolve(source, Input::invalid, missing, nullptr, nullptr).resolution ==
                  Resolution::choose_route,
              "corrupt preference requires a choice");
        if (const char* title = menu_title(system)) {
            Preference preference{system, ConsoleTileDestination::emulator, {}};
            std::snprintf(preference.emulator_title, sizeof(preference.emulator_title), "%s", title);
            if (!console_tile_emulator_menu_selects_system(title)) {
                ++shared_retroarch_menus;
                char rejected[maximum_record + 1]{};
                check(!preference_valid(preference) &&
                          !encode_preference(preference, rejected, sizeof(rejected)),
                      "a shared RetroArch menu cannot be saved as a selected-console route");
                continue;
            }
            ++direct_menus;
            char record[maximum_record + 1]{};
            Preference parsed;
            check(encode_preference(preference, record, sizeof(record)) &&
                  parse_preference(record, parsed),
                  "pinned emulator choice round-trips");
            check(resolve(source, Input::valid, parsed, installed, &parsed).resolution ==
                  Resolution::ready,
                  "matching installed emulator menu may launch");
            check(resolve(source, Input::valid, parsed, nullptr, nullptr).resolution ==
                  Resolution::stale_emulator,
                  "absent service cannot launch a stale emulator");
            Preference unavailable = parsed;
            unavailable.emulator_title[0] = 0;
            check(resolve(source, Input::valid, unavailable, installed, &parsed).resolution ==
                  Resolution::choose_route,
                  "invalid emulator record cannot launch");
        }
    }
    check(direct_menus == 5 && shared_retroarch_menus == 44,
          "five standalone menus and 44 shared RetroArch systems were covered");
    Source ps1{system_for("ps1")};
    char marker[maximum_record + 1]{}, argument[console_tile_message_capacity + 1]{};
    check(encode_source(ps1, marker, sizeof(marker)) &&
          encode_argument(ps1, argument, sizeof(argument)), "PS1 source encodes");
    Source selected;
    char* one[] = {argument};
    char* duplicate[] = {argument, argument};
    char mismatch[console_tile_message_capacity + 1] = "--isshelf-console-tile=ps2,preference,-";
    char* conflicting[] = {mismatch};
    check(select_source(marker, 2, duplicate, selected) == Input::invalid,
          "duplicate launch argument is rejected");
    check(select_source(marker, 1, conflicting, selected) == Input::invalid,
          "marker and argument console mismatch is rejected");
    check(select_source("", 1, one, selected) == Input::invalid,
          "a dynamic argument without the owned marker is rejected");
    std::string altered = marker;
    const auto at = altered.find("PPSA99704");
    check(at != std::string::npos, "PS1 title pin present");
    altered.replace(at, 9, "PPSA99703");
    check(select_source(altered, 1, one, selected) == Input::invalid,
          "wrong title pin cannot be selected");
    check(!parse_source(std::string(marker) + "extra=1\n", selected),
          "extra source field rejected");
    check(!parse_argument("--isshelf-console-tile=ps1,home,-", selected),
          "legacy fixed route is not a dynamic source");
    Preference ps1_route{ps1.system, ConsoleTileDestination::library, {}};
    char record[maximum_record + 1]{};
    check(encode_preference(ps1_route, record, sizeof(record)), "route record encodes");
    std::string wrong_record = record;
    const auto title = wrong_record.find("PPSA99704");
    wrong_record.replace(title, 9, "PPSA99703");
    Preference parsed;
    check(!parse_preference(wrong_record, parsed), "wrong title record rejected");
    check(!parse_preference(std::string(record) + "extra=1\n", parsed),
          "extra route record field rejected");
    Preference ps2_route{system_for("ps2"), ConsoleTileDestination::home, {}};
    check(resolve(ps1, Input::valid, ps2_route, nullptr, nullptr).resolution ==
              Resolution::choose_route, "another title's preference cannot cross consoles");
    for (auto fixed : {ConsoleTileIntent{system_for("ps1"), ConsoleTileDestination::home, {}},
                       ConsoleTileIntent{system_for("ps2"), ConsoleTileDestination::home, {}},
                       ConsoleTileIntent{system_for("ps3"), ConsoleTileDestination::home, {}},
                       ConsoleTileIntent{system_for("n64"), ConsoleTileDestination::library, {}}}) {
        char fixed_arg[console_tile_message_capacity + 1]{};
        ConsoleTileIntent restored;
        check(encode_console_tile_arg(fixed, fixed_arg, sizeof(fixed_arg)) &&
              parse_console_tile_arg(fixed_arg, restored) &&
              console_tile_intents_equal(fixed, restored),
              "four installed pilot contracts remain fixed and valid");
    }
    std::puts("PASS: 52 immutable sources, three launch choices, strict pins, stale-menu and pilot isolation.");
}
