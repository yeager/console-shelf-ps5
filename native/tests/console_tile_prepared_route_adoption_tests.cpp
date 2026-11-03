// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/console_tile_prepared_route_adoption.hpp"
#include "shelf/console_tile_write_gate.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
void check(bool okay, const char* message) {
    if (!okay) { std::fprintf(stderr, "FAIL prepared route: %s\n", message); std::exit(1); }
}
bool installed(void* context, std::size_t system, std::string_view title) {
    const auto* selected = static_cast<const shelf::tile_route::Preference*>(context);
    return selected && selected->system == system && title == selected->emulator_title;
}
} // namespace

int main() {
    using namespace shelf;
    using namespace shelf::tile_route;
    using namespace shelf::tile_route::prepared;
#if defined(SHELF_PS5) || defined(SHELF_TILE_WRITE_QUARANTINE_TEST)
    static_assert(!console_tile_writes_enabled,
                  "Prepared route adoption must not enable PS5 tile writes");
#endif
    unsigned count = 0;
    for (std::size_t system = 0; system < system_count; ++system) {
        if (!owned_prepared(system)) continue;
        ++count;
        ConsoleTileIntent original{system, ConsoleTileDestination::home, {}};
        char argument[console_tile_message_capacity + 1]{};
        check(encode_console_tile_arg(original, argument, sizeof(argument)),
              "exact v1 Home argument encodes");
        char* argv[] = {argument};
        Preference empty;
        auto decision = prepared::resolve(ConsoleTileInput::valid, false,
                                          original, 1, argv, Input::absent,
                                          empty, installed, nullptr);
        check(decision.state == State::original &&
              console_tile_intents_equal(decision.intent, original),
              "missing preference keeps pinned Home route");
        Preference library{system, ConsoleTileDestination::library, {}};
        decision = prepared::resolve(ConsoleTileInput::valid, false, original,
                                     1, argv, Input::valid, library,
                                     installed, nullptr);
        check(decision.state == State::chosen &&
              decision.intent.destination == ConsoleTileDestination::library,
              "valid Library choice overrides only applied route");
        decision = prepared::resolve(ConsoleTileInput::valid, false, original,
                                     1, argv, Input::invalid, library,
                                     installed, nullptr);
        check(decision.state == State::choose_route,
              "corrupt preference requires choice");
        char* duplicate[] = {argument, argument};
        decision = prepared::resolve(ConsoleTileInput::valid, false, original,
                                     2, duplicate, Input::absent, empty,
                                     installed, nullptr);
        check(decision.state == State::invalid_source,
              "duplicate route token fails closed");
        check(std::string_view(argument).find(",home,-") !=
              std::string_view::npos, "source argument is unchanged");
    }
    check(count == 48, "exactly 48 non-reserved prepared titles");
    unsigned library_origins = 0;
    for (std::size_t system = 0; system < system_count; ++system) {
        if (!owned_prepared(system)) continue;
        ++library_origins;
        ConsoleTileIntent source{system, ConsoleTileDestination::library, {}};
        char argument[console_tile_message_capacity + 1]{};
        check(encode_console_tile_arg(source, argument, sizeof(argument)),
              "prepared Library source encodes");
        char* argv[] = {argument};
        auto decision = prepared::resolve(ConsoleTileInput::valid, false, source,
                                          1, argv, Input::absent, {}, installed,
                                          nullptr);
        check(decision.state == State::original &&
                  console_tile_intents_equal(decision.intent, source),
              "missing preference retains exact Library source");
        const Preference home{system, ConsoleTileDestination::home, {}};
        decision = prepared::resolve(ConsoleTileInput::valid, false, source,
                                     1, argv, Input::valid, home, installed,
                                     nullptr);
        check(decision.state == State::chosen &&
                  decision.intent.destination == ConsoleTileDestination::home,
              "saved Home overrides exact Library source");
        decision = prepared::resolve(ConsoleTileInput::valid, false, source,
                                     1, argv, Input::invalid, home, installed,
                                     nullptr);
        check(decision.state == State::choose_route,
              "invalid saved choice asks from Library source");
        ConsoleTileIntent wrong{system, ConsoleTileDestination::home, {}};
        char wrong_argument[console_tile_message_capacity + 1]{};
        check(encode_console_tile_arg(wrong, wrong_argument,
                                      sizeof(wrong_argument)),
              "wrong Home argument encodes");
        char* wrong_argv[] = {wrong_argument};
        check(prepared::resolve(ConsoleTileInput::valid, false, source, 1,
                                wrong_argv, Input::valid, home, installed,
                                nullptr).state == State::invalid_source,
              "mismatched original Library argument refuses preference");
    }
    check(library_origins == 48, "all 48 Library origins covered");

    unsigned emulator_origins = 0, direct_origins = 0, shared_origins = 0;
    for (const auto& menu_pin : isshelf_badge::generic_tile::menus) {
        if (!owned_prepared(menu_pin.system)) continue;
        ++emulator_origins;
        ConsoleTileIntent source{menu_pin.system,
                                 ConsoleTileDestination::emulator, {}};
        std::snprintf(source.emulator_title, sizeof(source.emulator_title),
                      "%s", menu_pin.title_id);
        char argument[console_tile_message_capacity + 1]{};
        check(encode_console_tile_arg(source, argument, sizeof(argument)),
              "prepared Emulator source encodes");
        char* argv[] = {argument};
        Preference installed_menu{menu_pin.system,
                                  ConsoleTileDestination::emulator, {}};
        std::snprintf(installed_menu.emulator_title,
                      sizeof(installed_menu.emulator_title), "%s",
                      menu_pin.title_id);
        auto decision = prepared::resolve(ConsoleTileInput::valid, false, source,
                                          1, argv, Input::absent, {}, installed,
                                          &installed_menu);
        const bool direct = console_tile_emulator_menu_selects_system(
            menu_pin.title_id);
        if (direct) {
            ++direct_origins;
            check(decision.state == State::original &&
                      console_tile_intents_equal(decision.intent, source),
                  "installed standalone Emulator menu remains direct");
        } else {
            ++shared_origins;
            check(decision.state == State::original &&
                      decision.intent.system == source.system &&
                      decision.intent.destination == ConsoleTileDestination::library,
                  "a prepared RetroArch origin normalizes to its isShelf library");
        }
        decision = prepared::resolve(ConsoleTileInput::valid, false, source,
                                     1, argv, Input::absent, {}, installed,
                                     nullptr);
        check(direct ? decision.state == State::stale_emulator
                     : decision.state == State::original &&
                           decision.intent.destination == ConsoleTileDestination::library,
              "only an unavailable standalone menu is stale");
        const Preference home{menu_pin.system,
                              ConsoleTileDestination::home, {}};
        decision = prepared::resolve(ConsoleTileInput::valid, false, source,
                                     1, argv, Input::valid, home, installed,
                                     nullptr);
        check(decision.state == State::chosen &&
                  decision.intent.destination == ConsoleTileDestination::home,
              "saved Home overrides exact Emulator source");
        char fixed_home[console_tile_message_capacity + 1]{};
        check(encode_console_tile_fixed_home_arg(menu_pin.system, fixed_home,
                                                  sizeof(fixed_home)),
              "fixed Home token encodes");
        char* wrong_argv[] = {argument, fixed_home};
        check(prepared::resolve(ConsoleTileInput::valid, false, source, 2,
                                wrong_argv, Input::valid, home, installed,
                                nullptr).state == State::invalid_source,
              "Emulator source cannot carry Home-only token");
    }
    check(emulator_origins == 52 && direct_origins == 9 && shared_origins == 43,
          "all prepared direct and shared RetroArch origins are covered");
    const auto wiiu = system_for("wiiu");
    check(wiiu == 8 && owned_prepared(wiiu), "Wii U title is prepared");
    ConsoleTileIntent original{wiiu, ConsoleTileDestination::home, {}};
    char argument[console_tile_message_capacity + 1]{};
    check(encode_console_tile_arg(original, argument, sizeof(argument)),
          "Wii U argument encodes");
    char* argv[] = {argument};
    char fixed_home[console_tile_message_capacity + 1]{};
    check(encode_console_tile_fixed_home_arg(wiiu, fixed_home,
                                              sizeof(fixed_home)),
          "prepared fixed Home token encodes");
    char* fixed_argv[] = {argument, fixed_home};
    check(prepared::resolve(ConsoleTileInput::valid, false, original, 2,
                            fixed_argv, Input::absent, {}, installed,
                            nullptr).state == State::original,
          "exact prepared Home source accepts its fixed-Home token");
    Preference menu{wiiu, ConsoleTileDestination::emulator, {}};
    std::snprintf(menu.emulator_title, sizeof(menu.emulator_title), "%s", "PPSA99360");
    check(preference_valid(menu), "Wii U standalone emulator menu is pinned");
    auto decision = prepared::resolve(ConsoleTileInput::valid, false,
                                      original, 1, argv, Input::valid,
                                      menu, installed, &menu);
    check(decision.state == State::chosen &&
          decision.intent.destination == ConsoleTileDestination::emulator,
          "installed Wii U menu is chosen");
    decision = prepared::resolve(ConsoleTileInput::valid, false, original,
                                 1, argv, Input::valid, menu,
                                 installed, nullptr);
    check(decision.state == State::stale_emulator,
          "unavailable Wii U menu is stale, not silently replaced");
    ConsoleTileIntent pilot{system_for("ps2"), ConsoleTileDestination::home, {}};
    char pilot_argument[console_tile_message_capacity + 1]{};
    check(encode_console_tile_arg(pilot, pilot_argument, sizeof(pilot_argument)),
          "pilot argument encodes");
    char* pilot_argv[] = {pilot_argument};
    check(prepared::resolve(ConsoleTileInput::valid, false, pilot, 1, pilot_argv,
                            Input::absent, {}, installed, nullptr).state ==
          State::not_prepared, "existing pilot is never adopted here");
    std::puts("PASS: 48 prepared Home routes retain defaults and accept bounded opt-in choices offline.");
}
