// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/console_tile_intent.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace {
void check(bool condition, const char* reason) {
    if (!condition) {
        std::fprintf(stderr, "FAIL console tile intent: %s\n", reason);
        std::exit(1);
    }
}
void write_file(const char* path, std::string_view body) {
    FILE* file = std::fopen(path, "wb");
    check(file != nullptr, "test marker can be opened");
    check(std::fwrite(body.data(), 1, body.size(), file) == body.size(), "test marker can be written");
    check(std::fclose(file) == 0, "test marker can be closed");
}
} // namespace

int main() {
    using namespace shelf;
    char marker[console_tile_message_capacity + 1], argument[console_tile_message_capacity + 1];
    const char* path = "console-tile-intent-test.txt";
    std::remove(path);
    check(catalog[0].id == "ps5" && catalog[1].id == "ps4" && catalog[2].id == "ps3" &&
              console_tile_isshelf_only(0) && console_tile_isshelf_only(1) &&
              !console_tile_isshelf_only(2),
          "only native PS5 and PS4 shortcuts are locked to isShelf choices");
    for (std::size_t system = 0; system < system_count; ++system)
        for (auto destination : {ConsoleTileDestination::library, ConsoleTileDestination::home}) {
            ConsoleTileIntent intent{system, destination, {}};
            ConsoleTileIntent parsed;
            check(encode_console_tile_marker(intent, marker, sizeof(marker)) &&
                      parse_console_tile_marker(marker, parsed) && console_tile_intents_equal(intent, parsed),
                  "all catalog systems round-trip through a marker for library and home");
            check(encode_console_tile_arg(intent, argument, sizeof(argument)) &&
                      parse_console_tile_arg(argument, parsed) && console_tile_intents_equal(intent, parsed),
                  "all catalog systems round-trip through a forwarder argument");
            char* single[] = {argument};
            bool marker_used = true;
            check(select_console_tile_intent(1, single, path, parsed, &marker_used) == ConsoleTileInput::valid &&
                      !marker_used && console_tile_intents_equal(intent, parsed),
                  "all catalog library and home shortcuts select correctly from PS5 argv[0]");
            if (destination == ConsoleTileDestination::home) {
                char fixed[console_tile_message_capacity + 1]{};
                std::size_t fixed_system = system_count;
                check(encode_console_tile_fixed_home_arg(system, fixed, sizeof(fixed)) &&
                          parse_console_tile_fixed_home_arg(fixed, fixed_system) &&
                          fixed_system == system,
                      "all catalog fixed-Home markers round-trip exactly");
                char* fixed_argv[] = {argument, fixed};
                check(select_console_tile_intent(2, fixed_argv, path, parsed) ==
                          ConsoleTileInput::valid &&
                          console_tile_intents_equal(intent, parsed),
                      "matching fixed-Home marker is accepted with its Home route");
            }
        }
    std::size_t emulator_cases = 0;
    for (std::size_t system = 0; system < system_count; ++system) {
        const auto id = catalog[system].id;
        const char* title = nullptr;
        if (id == "ps3") title = "PPSA99303";
        else if (id == "ps2") title = "PPSA99203";
        else if (id == "ps1") title = "PPSA99248";
        else if (id == "switch") title = "PPSA99008";
        else if (id == "wiiu") title = "PPSA99360";
        else if (!catalog[system].suggested_core.empty()) title = "PPSA99169";
        if (!title) continue;
        ++emulator_cases;
        ConsoleTileIntent intent{system, ConsoleTileDestination::emulator, {}};
        std::snprintf(intent.emulator_title, sizeof(intent.emulator_title), "%s", title);
        ConsoleTileIntent parsed;
        check(encode_console_tile_marker(intent, marker, sizeof(marker)) &&
                  parse_console_tile_marker(marker, parsed) && console_tile_intents_equal(intent, parsed),
              "applicable emulator shortcuts round-trip through ownership markers");
        check(encode_console_tile_arg(intent, argument, sizeof(argument)) &&
                  parse_console_tile_arg(argument, parsed) && console_tile_intents_equal(intent, parsed),
              "applicable emulator shortcuts round-trip through forwarder arguments");
        char* single[] = {argument};
        bool marker_used = true;
        check(select_console_tile_intent(1, single, path, parsed, &marker_used) == ConsoleTileInput::valid &&
                  !marker_used && console_tile_intents_equal(intent, parsed),
              "applicable emulator shortcuts select correctly from PS5 argv[0]");
    }
    check(emulator_cases == system_count - 3, "all applicable console emulator routes were exercised");
    ConsoleTileIntent ps1{4, ConsoleTileDestination::emulator, {}};
    std::strcpy(ps1.emulator_title, "PPSA99248");
    check(catalog[ps1.system].id == "ps1", "the emulator example points to PS1");
    check(encode_console_tile_marker(ps1, marker, sizeof(marker)), "emulator marker encodes");
    check(encode_console_tile_arg(ps1, argument, sizeof(argument)), "emulator argument encodes");
    ConsoleTileIntent parsed;
    check(parse_console_tile_marker(marker, parsed) && console_tile_intents_equal(ps1, parsed),
          "emulator marker pins its exact title");
    check(parse_console_tile_arg(argument, parsed) && console_tile_intents_equal(ps1, parsed),
          "emulator argument pins its exact title");
    check(!parse_console_tile_marker("ISSHELF-CONSOLE-TILE-1\nsystem=unknown\ndestination=library\nemulator=-\n", parsed),
          "unknown console IDs cannot alias a catalog entry");
    check(!parse_console_tile_marker("ISSHELF-CONSOLE-TILE-1\nsystem=ps1\ndestination=game\nemulator=-\n", parsed),
          "unknown destination is rejected");
    check(!parse_console_tile_marker("ISSHELF-CONSOLE-TILE-1\nsystem=ps1\ndestination=library\nemulator=PPSA99248\n", parsed),
          "a non-emulator destination cannot carry a title");
    check(!parse_console_tile_marker("ISSHELF-CONSOLE-TILE-1\nsystem=ps1\ndestination=emulator\nemulator=-\n", parsed),
          "an emulator destination needs a title");
    check(!parse_console_tile_marker("ISSHELF-CONSOLE-TILE-1\nsystem=ps1\ndestination=emulator\nemulator=PPSA99999\n", parsed),
          "unverified emulator titles are rejected");
    check(!parse_console_tile_marker("ISSHELF-CONSOLE-TILE-1\nsystem=ps3\ndestination=emulator\nemulator=PPSA99248\n", parsed),
          "a known emulator title for another console is rejected in a marker");
    check(!parse_console_tile_arg("--isshelf-console-tile=ps3,emulator,PPSA99248", parsed),
          "a known emulator title for another console is rejected in argv");
    check(parse_console_tile_arg("--isshelf-console-tile=ps1,emulator,PPSA97510", parsed) &&
              parsed.system == ps1.system &&
              std::strcmp(parsed.emulator_title, "PPSA97510") == 0,
          "a pinned historical PS1 menu remains valid without requiring installation at parse time");
    check(!parse_console_tile_marker("ISSHELF-CONSOLE-TILE-1\r\nsystem=ps1\ndestination=library\nemulator=-\n", parsed),
          "CRLF and other grammar variants are rejected");
    check(!parse_console_tile_marker("ISSHELF-CONSOLE-TILE-1\nsystem=ps1\ndestination=library\nemulator=-\nextra=1\n", parsed),
          "extra marker fields are rejected");
    check(!parse_console_tile_arg("--isshelf-console-tile=ps1,library,-,extra", parsed),
          "extra argument fields are rejected");
    check(!parse_console_tile_arg("--isshelf-console-tile=ps1,emulator,PPSA99999", parsed),
          "unverified emulator arguments are rejected");
    check(!encode_console_tile_marker(ps1, marker, 8), "short marker output buffers are rejected");

    char program[] = "isShelf";
    char matching[] = "--isshelf-console-tile=ps1,emulator,PPSA99248";
    char conflicting[] = "--isshelf-console-tile=ps2,home,-";
    char unrelated[] = "--system-option";
    char* no_shortcut[] = {program, unrelated};
    char* ps5_single_arg[] = {matching};
    char* one_shortcut[] = {program, matching};
    char* duplicate[] = {program, matching, matching};
    char* clash[] = {program, conflicting};
    bool marker_used = true;
    check(select_console_tile_intent(2, no_shortcut, path, parsed, &marker_used) == ConsoleTileInput::absent &&
              !marker_used, "ordinary unrelated startup arguments leave the app unchanged");
    check(select_console_tile_intent(1, ps5_single_arg, path, parsed, &marker_used) == ConsoleTileInput::valid &&
              !marker_used && console_tile_intents_equal(parsed, ps1),
          "PS5 launch argument works when the CRT passes it as argv[0]");
    check(select_console_tile_intent(2, one_shortcut, path, parsed, &marker_used) == ConsoleTileInput::valid &&
              !marker_used && console_tile_intents_equal(parsed, ps1), "forwarder argument works without a marker");
    check(select_console_tile_intent(3, duplicate, path, parsed) == ConsoleTileInput::invalid,
          "duplicate shortcut arguments are rejected");
    char ps2_home[] = "--isshelf-console-tile=ps2,home,-";
    char ps2_fixed[] = "--isshelf-home-direct-v2=ps2";
    char ps1_fixed[] = "--isshelf-home-direct-v2=ps1";
    char malformed_fixed[] = "--isshelf-home-direct-v2=ps2,extra";
    char no_equals_fixed[] = "--isshelf-home-direct-v2";
    char* mismatched_fixed[] = {ps2_home, ps1_fixed};
    char* duplicate_fixed[] = {ps2_home, ps2_fixed, ps2_fixed};
    char* malformed[] = {ps2_home, malformed_fixed};
    char* no_equals[] = {ps2_home, no_equals_fixed};
    char* marker_only[] = {ps2_fixed};
    char* non_home_fixed[] = {matching, ps1_fixed};
    check(select_console_tile_intent(2, mismatched_fixed, path, parsed) == ConsoleTileInput::invalid,
          "cross-console fixed-Home marker is rejected");
    check(select_console_tile_intent(3, duplicate_fixed, path, parsed) == ConsoleTileInput::invalid,
          "duplicate fixed-Home markers are rejected");
    check(select_console_tile_intent(2, malformed, path, parsed) == ConsoleTileInput::invalid &&
              select_console_tile_intent(2, no_equals, path, parsed) == ConsoleTileInput::invalid,
          "malformed fixed-Home markers are rejected");
    check(select_console_tile_intent(1, marker_only, path, parsed) == ConsoleTileInput::invalid &&
              select_console_tile_intent(2, non_home_fixed, path, parsed) == ConsoleTileInput::invalid,
          "a fixed-Home marker requires a matching Home shortcut");
    check(encode_console_tile_marker(ps1, marker, sizeof(marker)), "example marker re-encodes");
    write_file(path, marker);
    check(select_console_tile_intent(2, one_shortcut, path, parsed, &marker_used) == ConsoleTileInput::valid &&
              marker_used && console_tile_intents_equal(parsed, ps1), "matching marker and argument agree");
    check(select_console_tile_intent(2, clash, path, parsed) == ConsoleTileInput::invalid,
          "a conflicting marker and argument are rejected");
    char* marker_and_fixed[] = {ps2_home, ps2_fixed};
    check(select_console_tile_intent(2, marker_and_fixed, path, parsed) == ConsoleTileInput::invalid,
          "a fixed-Home argv marker cannot override an /app0 marker");
    write_file(path, std::string(console_tile_message_capacity + 1, 'x'));
    check(read_console_tile_marker(path, parsed) == ConsoleTileInput::invalid,
          "oversized marker is rejected without truncating it into a valid request");
    check(select_console_tile_intent(2, one_shortcut, path, parsed) == ConsoleTileInput::invalid,
          "an invalid marker cannot be bypassed with an argument");
    check(std::remove(path) == 0, "test marker is removed");
    std::puts("PASS: all console tile intents round-trip; malformed, duplicate and conflicting startup inputs fail closed.");
}
