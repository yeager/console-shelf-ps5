// SPDX-License-Identifier: GPL-3.0-only
// Exercise the exact CREATE forwarder bytes through the main app's argv
// selector, route adoption, and startup destination. Host-only: no PS5 I/O.
#include "shelf/generic_app_tile_source.hpp"
#include "shelf/console_tile_route_adoption.hpp"
#include "shelf/console_tile_startup.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {
struct ForwardedArgs {
    std::string first, second;
    int count = 0;
};
ForwardedArgs arguments_from_forwarder(const shelf::generic_app_tile_source::Plan& source) {
    assert(source.valid);
    const std::string body(source.forwarder, source.forwarder_bytes);
    constexpr const char* prefix = "\"args\":[\"";
    const auto begin = body.find(prefix);
    assert(begin != std::string::npos);
    const auto value = begin + std::strlen(prefix);
    const auto first_end = body.find('"', value);
    assert(first_end != std::string::npos);
    ForwardedArgs args;
    args.first = body.substr(value, first_end - value);
    args.count = 1;
    if (body.compare(first_end, 2, "\"]") == 0) return args;
    assert(body.compare(first_end, 3, "\",\"") == 0);
    const auto second_start = first_end + 3;
    const auto second_end = body.find("\"]", second_start);
    assert(second_end != std::string::npos &&
           body.find('"', second_start) == second_end);
    args.second = body.substr(second_start, second_end - second_start);
    args.count = 2;
    return args;
}

shelf::ConsoleTileIntent selected_from_source(const shelf::generic_app_tile_source::Plan& source,
                                               const char* absent_marker) {
    auto args = arguments_from_forwarder(source);
    char* argv[] = {args.first.data(), args.second.data()};
    shelf::ConsoleTileIntent selected;
    assert(shelf::select_console_tile_intent(args.count, argv, absent_marker, selected) ==
           shelf::ConsoleTileInput::valid);
    assert(selected.system == shelf::find_system(source.system_id));
    return selected;
}

shelf::tile_route::adoption::Decision adopted_from_source(
    const shelf::generic_app_tile_source::Plan& source, const char* absent_marker) {
    auto args = arguments_from_forwarder(source);
    char* argv[] = {args.first.data(), args.second.data()};
    shelf::ConsoleTileIntent selected;
    const auto input = shelf::select_console_tile_intent(args.count, argv, absent_marker, selected);
    assert(input == shelf::ConsoleTileInput::valid);
    return shelf::tile_route::adoption::require_launch_choice(
        input, false, selected, args.count, argv);
}
} // namespace

int main() {
    using namespace shelf;
    using namespace shelf::generic_app_tile_source;
    using namespace shelf::tile_route::adoption;
    char root[] = "/tmp/isshelf-first-add-route-XXXXXX";
    assert(::mkdtemp(root));
    const std::string absent_marker = std::string(root) + "/absent-marker.json";
    static Library library;
    std::size_t eligible = 0, emulator_routes = 0;

    for (std::size_t system = 0; system < system_count; ++system) {
        if (reserved_existing_title(system)) continue;
        ++eligible;

        const ConsoleTileIntent library_intent{system, ConsoleTileDestination::library, {}};
        const auto library_source = plan(library_intent);
        assert(library_source.valid);
        const auto selected_library = selected_from_source(library_source, absent_marker.c_str());
        assert(console_tile_intents_equal(selected_library, library_intent));
        const auto adopted_library = adopted_from_source(library_source, absent_marker.c_str());
        assert(adopted_library.state == State::original && applicable(adopted_library) &&
               console_tile_intents_equal(adopted_library.intent, library_intent));
        UiState library_state;
        assert(apply_console_tile_intent(library_state, adopted_library.intent) ==
                   ConsoleTileApply::library &&
               library_state.page == Page::library && library_state.selected_system == system &&
               !library_state.launch_requested);

        const ConsoleTileIntent home_intent{system, ConsoleTileDestination::home, {}};
        const auto home_source = plan(home_intent);
        assert(home_source.valid);
        assert(arguments_from_forwarder(home_source).count == 2);
        const auto adopted_home = adopted_from_source(home_source, absent_marker.c_str());
        assert(adopted_home.state == State::original && applicable(adopted_home));
        UiState home_state;
        assert(apply_console_tile_intent(home_state, adopted_home.intent) ==
                   ConsoleTileApply::home && home_state.page == Page::home &&
               home_state.selected_system == system);
        char legacy_home[console_tile_message_capacity + 1]{};
        assert(encode_console_tile_arg(home_intent, legacy_home,
                                       sizeof(legacy_home)));
        char* legacy_argv[] = {legacy_home};
        const auto legacy_decision = require_launch_choice(
            ConsoleTileInput::valid, false, home_intent, 1, legacy_argv);
        assert(legacy_decision.state == State::choose_route &&
               !applicable(legacy_decision));

        for (const auto& menu : isshelf_badge::generic_tile::menus) {
            if (menu.system != system) continue;
            ++emulator_routes;
            ConsoleTileIntent emulator_intent{system, ConsoleTileDestination::emulator, {}};
            std::snprintf(emulator_intent.emulator_title,
                          sizeof(emulator_intent.emulator_title), "%s", menu.title_id);
            const auto emulator_source = plan(emulator_intent);
            assert(emulator_source.valid);
            const auto selected_menu = selected_from_source(emulator_source, absent_marker.c_str());
            assert(console_tile_intents_equal(selected_menu, emulator_intent));
            const auto adopted_menu = adopted_from_source(emulator_source, absent_marker.c_str());
            assert(adopted_menu.state == State::original && applicable(adopted_menu) &&
                   console_tile_intents_equal(adopted_menu.intent, emulator_intent));

            library.route_count = 1;
            auto& installed = library.routes[0];
            installed = {};
            installed.system = system;
            std::snprintf(installed.title_id, sizeof(installed.title_id), "%s", menu.title_id);
            std::snprintf(installed.folder, sizeof(installed.folder),
                          "/data/homebrew/%s", menu.title_id);
            UiState ready;
            ready.library = &library;
            ready.launch_connected = true;
            assert(apply_console_tile_intent(ready, adopted_menu.intent) ==
                       ConsoleTileApply::emulator &&
                   ready.launch_requested && ready.selected_system == system &&
                   std::strcmp(ready.pending_launch.title, menu.title_id) == 0);

            library.route_count = 0;
            UiState stale;
            stale.library = &library;
            stale.launch_connected = true;
            assert(apply_console_tile_intent(stale, adopted_menu.intent) ==
                       ConsoleTileApply::emulator_unavailable &&
                   stale.page == Page::details && !stale.launch_requested);
        }
    }
    assert(eligible == 48 && emulator_routes > 0);
    assert(::rmdir(root) == 0);
    std::puts("PASS: 48 new CREATE source arguments reach fixed Library/Home/emulator startup; legacy Home still chooses per launch");
}
