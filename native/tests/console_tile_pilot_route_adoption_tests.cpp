// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/console_tile_pilot_route_adoption.hpp"
#include "shelf/console_tile_write_gate.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
void check(bool okay, const char* reason) {
    if (!okay) { std::fprintf(stderr, "FAIL pilot route: %s\n", reason); std::exit(1); }
}
bool installed(void* context, std::size_t system, std::string_view title) {
    const auto* selected = static_cast<const shelf::tile_route::Preference*>(context);
    return selected && selected->system == system && title == selected->emulator_title;
}
const char* menu_for(std::string_view system) {
    if (system == "ps1") return "PPSA99248";
    if (system == "ps2") return "PPSA99203";
    if (system == "ps3") return "PPSA99303";
    return "PPSA99169";
}
} // namespace

int main() {
    using namespace shelf;
    using namespace shelf::tile_route;
    using namespace shelf::tile_route::pilot;
#if defined(SHELF_PS5) || defined(SHELF_TILE_WRITE_QUARANTINE_TEST)
    static_assert(!console_tile_writes_enabled, "Pilot adoption must not enable PS5 tile writes");
#endif
    for (const auto& fixed : fixed_sources) {
        const auto system = system_for(fixed.system);
        check(system < system_count &&
              std::strcmp(tile_job::owned_title_for(system), fixed.title) == 0,
              "pilot title and catalog remain pinned");
        ConsoleTileIntent original{system, fixed.destination, {}};
        check(console_tile_intent_valid(original), "pilot original intent is valid");
        std::string token(fixed.argument);
        char* argv[] = {token.data()};
        Preference empty;
        auto decision = pilot::resolve(ConsoleTileInput::valid, false, original, 1, argv,
                                       Input::absent, empty, installed, nullptr);
        check(decision.state == State::original &&
              console_tile_intents_equal(decision.intent, original),
              "missing preference retains exact pilot destination");
        for (auto route : {ConsoleTileDestination::library, ConsoleTileDestination::home}) {
            Preference choice{system, route, {}};
            char body[maximum_record + 1]{};
            Preference parsed;
            check(encode_preference(choice, body, sizeof(body)) &&
                  parse_preference(body, parsed), "choice round-trips strict record");
            decision = pilot::resolve(ConsoleTileInput::valid, false, original, 1, argv,
                                      Input::valid, parsed, installed, nullptr);
            check(decision.state == State::chosen && decision.intent.system == system &&
                  decision.intent.destination == route && !decision.intent.emulator_title[0],
                  "library and Home preferences override only the applied route");
        }
        Preference menu{system, ConsoleTileDestination::emulator, {}};
        std::snprintf(menu.emulator_title, sizeof(menu.emulator_title), "%s", menu_for(fixed.system));
        if (console_tile_emulator_menu_selects_system(menu.emulator_title)) {
            check(preference_valid(menu), "standalone emulator choice is pinned to the console");
            decision = pilot::resolve(ConsoleTileInput::valid, false, original, 1, argv,
                                      Input::valid, menu, installed, &menu);
            check(decision.state == State::chosen &&
                  decision.intent.destination == ConsoleTileDestination::emulator &&
                  std::strcmp(decision.intent.emulator_title, menu.emulator_title) == 0,
                  "installed standalone emulator menu is selected");
            decision = pilot::resolve(ConsoleTileInput::valid, false, original, 1, argv,
                                      Input::valid, menu, installed, nullptr);
            check(decision.state == State::stale_emulator,
                  "missing standalone emulator menu does not fall through");
        } else {
            check(!preference_valid(menu),
                  "a shared RetroArch menu cannot be saved as a selected-console preference");
        }
        decision = pilot::resolve(ConsoleTileInput::valid, false, original, 1, argv,
                                  Input::invalid, menu, installed, &menu);
        check(decision.state == State::choose_route,
              "corrupt preference requires an explicit choice");
        Preference wrong{system_for(fixed.system == "ps1" ? "ps2" : "ps1"),
                         ConsoleTileDestination::library, {}};
        decision = pilot::resolve(ConsoleTileInput::valid, false, original, 1, argv,
                                  Input::valid, wrong, installed, &menu);
        check(decision.state == State::choose_route,
              "another console cannot supply a pilot preference");
        decision = pilot::resolve(ConsoleTileInput::valid, true, original, 1, argv,
                                  Input::valid, menu, installed, &menu);
        check(decision.state == State::not_pilot,
              "a tile process with its own marker is not adopted");
        decision = pilot::resolve(ConsoleTileInput::invalid, false, original, 1, argv,
                                  Input::valid, menu, installed, &menu);
        check(decision.state == State::invalid_source,
              "invalid original selector result cannot be adopted");
        char* twice[] = {token.data(), token.data()};
        decision = pilot::resolve(ConsoleTileInput::valid, false, original, 2, twice,
                                  Input::absent, empty, installed, nullptr);
        check(decision.state == State::invalid_source,
              "duplicate route arguments fail closed");
        ConsoleTileIntent mismatched = original;
        mismatched.destination = original.destination == ConsoleTileDestination::home
                                     ? ConsoleTileDestination::library : ConsoleTileDestination::home;
        decision = pilot::resolve(ConsoleTileInput::valid, false, mismatched, 1, argv,
                                  Input::absent, empty, installed, nullptr);
        check(decision.state == State::invalid_source,
              "raw pilot argument and parsed original must agree");
        check(token == fixed.argument, "adoption never edits the source argument");
    }
    ConsoleTileIntent other{system_for("switch"), ConsoleTileDestination::library, {}};
    std::string token = "--isshelf-console-tile=switch,library,-";
    char* argv[] = {token.data()};
    Preference none;
    check(pilot::resolve(ConsoleTileInput::valid, false, other, 1, argv,
                         Input::absent, none, nullptr, nullptr).state == State::not_pilot,
          "other fixed routes remain outside pilot adoption");
    std::puts("PASS: four pilot routes retain defaults and accept bounded opt-in choices offline.");
}
