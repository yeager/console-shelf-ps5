// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/console_tile_route_adoption.hpp"
#include "shelf/console_tile_write_gate.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
void check(bool okay, const char* message) {
    if (!okay) {
        std::fprintf(stderr, "FAIL route adoption: %s\n", message);
        std::exit(1);
    }
}
bool installed(void* context, std::size_t system, std::string_view title) {
    const auto* choice = static_cast<const shelf::tile_route::Preference*>(context);
    return choice && choice->system == system && title == choice->emulator_title;
}
} // namespace

int main() {
    using namespace shelf;
    using namespace shelf::tile_route;
    using namespace shelf::tile_route::adoption;
#if defined(SHELF_PS5) || defined(SHELF_TILE_WRITE_QUARANTINE_TEST)
    static_assert(!console_tile_writes_enabled,
                  "Route adoption must not enable PS5 tile writes");
#endif
    unsigned pilot_count = 0, prepared_count = 0;
    for (std::size_t system = 0; system < system_count; ++system) {
        const bool pilot = generic_tile_create::reserved_existing_title(system);
        pilot_count += pilot ? 1 : 0;
        prepared_count += pilot ? 0 : 1;
        ConsoleTileDestination original_destination = ConsoleTileDestination::home;
        if (system == system_for("n64")) original_destination = ConsoleTileDestination::library;
        const ConsoleTileIntent original{system, original_destination, {}};
        char argument[console_tile_message_capacity + 1]{};
        check(encode_console_tile_arg(original, argument, sizeof(argument)),
              "pinned source argument encodes");
        char* argv[] = {argument};
        Preference empty;
        auto decision = resolve(ConsoleTileInput::valid, false, original, 1, argv,
                                Input::absent, empty, installed, nullptr);
        check(decision.state == State::original && applicable(decision) &&
                  console_tile_intents_equal(decision.intent, original),
              "all 52 absent preferences retain their exact source route");
        decision = require_launch_choice(ConsoleTileInput::valid, false, original, 1, argv);
        check(decision.state == State::choose_route && !applicable(decision),
              "all 52 owned v1 sources require an explicit choice while saving is unavailable");
        if (original_destination == ConsoleTileDestination::home) {
            char fixed_home[console_tile_message_capacity + 1]{};
            check(encode_console_tile_fixed_home_arg(system, fixed_home,
                                                     sizeof(fixed_home)),
                  "fixed-Home argument encodes");
            char* fixed_argv[] = {argument, fixed_home};
            decision = require_launch_choice(ConsoleTileInput::valid, false,
                                             original, 2, fixed_argv);
            check(pilot ? decision.state == State::invalid && !applicable(decision)
                        : decision.state == State::original && applicable(decision) &&
                          console_tile_intents_equal(decision.intent, original),
                  "only a new prepared Home source can bypass the legacy chooser");
        }
        for (const auto destination : {ConsoleTileDestination::library,
                                       ConsoleTileDestination::home}) {
            const Preference preference{system, destination, {}};
            decision = resolve(ConsoleTileInput::valid, false, original, 1, argv,
                               Input::valid, preference, installed, nullptr);
            check(decision.state == State::preferred && applicable(decision) &&
                      decision.intent.system == system &&
                      decision.intent.destination == destination &&
                      !decision.intent.emulator_title[0],
                  "all 52 routes adopt a valid Library or Home preference");
        }
        const Preference library{system, ConsoleTileDestination::library, {}};
        decision = resolve(ConsoleTileInput::valid, false, original, 1, argv,
                           Input::invalid, library, installed, nullptr);
        check(decision.state == State::choose_route && !applicable(decision),
              "corrupt preference cannot silently pick a route");
        char* duplicate[] = {argument, argument};
        decision = resolve(ConsoleTileInput::valid, false, original, 2, duplicate,
                           Input::valid, library, installed, nullptr);
        check(decision.state == State::invalid && !applicable(decision),
              "duplicate shortcut argument cannot select a route");
        decision = resolve(ConsoleTileInput::valid, true, original, 1, argv,
                           Input::valid, library, installed, nullptr);
        check(decision.state == State::original &&
                  console_tile_intents_equal(decision.intent, original),
              "a marker source keeps its own destination");
        decision = require_launch_choice(ConsoleTileInput::valid, true, original, 1, argv);
        check(decision.state == State::original && applicable(decision),
              "a marker source is not mistaken for an adopted v1 source");
    }
    check(pilot_count == 4 && prepared_count == 48,
          "exact four installed pilots and 48 prepared catalog sources");

    const std::size_t ps2 = system_for("ps2");
    const ConsoleTileIntent ps2_home{ps2, ConsoleTileDestination::home, {}};
    char ps2_argument[console_tile_message_capacity + 1]{};
    check(encode_console_tile_arg(ps2_home, ps2_argument, sizeof(ps2_argument)),
          "PS2 fixed Home source encodes");
    char* ps2_argv[] = {ps2_argument};
    const Preference ps2_library{ps2, ConsoleTileDestination::library, {}};
    auto decision = resolve(ConsoleTileInput::valid, false, ps2_home, 1, ps2_argv,
                            Input::valid, ps2_library, installed, nullptr);
    check(decision.state == State::preferred &&
              decision.intent.destination == ConsoleTileDestination::library,
          "the existing PS2 Home tile can enter its Library with a saved choice");

    Preference ps2_menu{ps2, ConsoleTileDestination::emulator, {}};
    std::snprintf(ps2_menu.emulator_title, sizeof(ps2_menu.emulator_title), "%s",
                  "PPSA99203");
    decision = resolve(ConsoleTileInput::valid, false, ps2_home, 1, ps2_argv,
                       Input::valid, ps2_menu, installed, &ps2_menu);
    check(decision.state == State::preferred &&
              decision.intent.destination == ConsoleTileDestination::emulator &&
              std::strcmp(decision.intent.emulator_title, "PPSA99203") == 0,
          "PS2 installed emulator choice selects only its pinned menu");
    decision = resolve(ConsoleTileInput::valid, false, ps2_home, 1, ps2_argv,
                       Input::valid, ps2_menu, installed, nullptr);
    check(decision.state == State::stale_emulator && !applicable(decision),
          "uninstalled PS2 emulator requires a fresh choice");
    decision = resolve(ConsoleTileInput::invalid, false, ps2_home, 1, ps2_argv,
                       Input::valid, ps2_library, installed, nullptr);
    check(decision.state == State::invalid && !applicable(decision),
          "invalid original selector result stays invalid");
    decision = require_launch_choice(ConsoleTileInput::invalid, false, ps2_home, 1, ps2_argv);
    check(decision.state == State::invalid && !applicable(decision),
          "malformed source cannot enter the launch chooser");
    char* duplicate[] = {ps2_argument, ps2_argument};
    decision = require_launch_choice(ConsoleTileInput::valid, false, ps2_home, 2, duplicate);
    check(decision.state == State::invalid && !applicable(decision),
          "duplicate source argument cannot enter the launch chooser");
    char ps2_fixed[] = "--isshelf-home-direct-v2=ps2";
    char ps1_fixed[] = "--isshelf-home-direct-v2=ps1";
    char malformed_fixed[] = "--isshelf-home-direct-v2=ps2,extra";
    char* mismatched[] = {ps2_argument, ps1_fixed};
    char* malformed[] = {ps2_argument, malformed_fixed};
    char* repeated[] = {ps2_argument, ps2_fixed, ps2_fixed};
    decision = require_launch_choice(ConsoleTileInput::valid, false, ps2_home, 2, mismatched);
    check(decision.state == State::invalid && !applicable(decision),
          "cross-console fixed marker fails closed in route adoption");
    decision = require_launch_choice(ConsoleTileInput::valid, false, ps2_home, 2, malformed);
    check(decision.state == State::invalid && !applicable(decision),
          "malformed fixed marker fails closed in route adoption");
    decision = require_launch_choice(ConsoleTileInput::valid, false, ps2_home, 3, repeated);
    check(decision.state == State::invalid && !applicable(decision),
          "duplicate fixed marker fails closed in route adoption");
    decision = resolve(ConsoleTileInput::absent, false, {}, 0, nullptr,
                       Input::absent, {}, installed, nullptr);
    check(decision.state == State::absent && !applicable(decision),
          "normal main app launch has no route adoption");
    decision = require_launch_choice(ConsoleTileInput::absent, false, {}, 0, nullptr);
    check(decision.state == State::absent && !applicable(decision),
          "normal main app launch never presents a shortcut chooser");
    std::puts("PASS: 52 console source routes compose with chosen Library, Home, or installed emulator destinations offline.");
}
