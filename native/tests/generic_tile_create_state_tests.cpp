// SPDX-License-Identifier: GPL-3.0-only
// Portable state-machine regression; no source writer or PS5 API.
#include "shelf/generic_tile_create_state.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

using namespace shelf;
using namespace shelf::generic_tile_create;

static void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL generic CREATE state: %s\n", message);
        std::exit(1);
    }
}

static Admission clear_admission() {
    Admission result;
    result.hidden_source = Presence::absent;
    result.homebrew_source = Presence::absent;
    result.user_app = Presence::absent;
    result.user_appmeta = Presence::absent;
    result.game_info = Presence::absent;
    result.manual_list = Presence::absent;
    result.durable_history_clear = true;
    result.exclusive_writer = true;
    return result;
}

static bool switch_menu(const void* context, std::size_t system,
                        std::string_view title) {
    return context && system == 7 && title == "PPSA99008";
}

static CatalogState registration_unknown(std::size_t system,
                                         ConsoleTileDestination destination) {
    constexpr char nonce[] = "0123456789abcdef0123456789abcdef";
    CatalogState state;
    ConsoleTileIntent route;
    route.system = system;
    route.destination = destination;
    check(state.select_route(route), "selected route is catalog-valid");
    check(state.begin_source(system, nonce, clear_admission()) == Begin::started,
          "one source claim begins with complete absence and writer proof");
    check(state.finish_source(system, true, 27, 42), "source receipt is retained");
    check(state.note_registration_attempt(system, true), "durable send marker precedes uncertainty");
    return state;
}

int main() {
    CatalogState catalog_state;
    std::set<std::string> title_ids;
    std::size_t eligible = 0;
    constexpr char nonce[] = "0123456789abcdef0123456789abcdef";
    for (std::size_t system = 0; system < system_count; ++system) {
        const auto* record = catalog_state.get(system);
        check(record && tile_job::catalog_pin_matches(system), "every catalog row is pinned");
        check(title_ids.insert(tile_job::owned_title_for(system)).second,
              "all 52 title IDs are unique");
        if (reserved_existing_title(system)) {
            check(record->phase == Phase::existing_title,
                  "PS1, PS2, PS3 and N64 remain existing titles");
            check(!catalog_state.select_route(
                      ConsoleTileIntent{system, ConsoleTileDestination::library, {}}) &&
                      catalog_state.begin_source(system, nonce, clear_admission()) ==
                          Begin::existing_title,
                  "existing titles cannot start CREATE");
        } else {
            ++eligible;
            check(record->phase == Phase::inspect_required, "other titles need fresh inspection");
        }
    }
    check(eligible == 48 && title_ids.size() == system_count, "48 unique CREATE candidates");

    CatalogState preview;
    ConsoleTileIntent home{0, ConsoleTileDestination::home, {}};
    check(preview.select_route(home), "Home route selected");
    check(preview.cancel_selection(0) && !preview.get(0)->route_selected,
          "Circle may cancel before source work");
    check(preview.select_route(home), "Home route can be chosen again before claim");
    check(preview.begin_source(0, nonce, Admission{}) == Begin::incomplete_inventory,
          "unknown title occupancy blocks CREATE");
    Admission occupied = clear_admission();
    occupied.manual_list = Presence::present;
    check(preview.begin_source(0, nonce, occupied) == Begin::occupied,
          "existing manual entry blocks CREATE");
    Admission no_writer = clear_admission();
    no_writer.exclusive_writer = false;
    check(preview.begin_source(0, nonce, no_writer) == Begin::history_or_writer_unproved,
          "unproved exclusive writer blocks CREATE");
    check(preview.begin_source(0, nonce, clear_admission()) == Begin::started,
          "one complete admission claims the title");
    check(preview.begin_source(0, nonce, clear_admission()) == Begin::duplicate_or_unresolved &&
              !preview.select_route(ConsoleTileIntent{0, ConsoleTileDestination::library, {}}) &&
              !preview.cancel_selection(0),
          "in-flight title cannot be duplicated or rerouted");
    check(preview.finish_source(0, false, 0, 0) &&
              preview.get(0)->phase == Phase::source_incomplete &&
              preview.begin_source(0, nonce, clear_admission()) == Begin::duplicate_or_unresolved,
          "partial source remains unresolved without retry");

    CatalogState switch_tile;
    ConsoleTileIntent emulator{7, ConsoleTileDestination::emulator, {}};
    std::strcpy(emulator.emulator_title, "PPSA99008");
    check(!switch_tile.select_route(emulator), "emulator route needs installed-menu proof");
    check(switch_tile.select_route(emulator, switch_menu, &switch_tile),
          "installed pinned emulator route is saved");
    check(switch_tile.begin_source(7, nonce, clear_admission()) == Begin::no_route,
          "emulator availability is rechecked at dispatch");
    check(switch_tile.begin_source(7, nonce, clear_admission(), switch_menu,
                                   &switch_tile) == Begin::started,
          "unchanged installed emulator route passes dispatch");

    auto unknown = registration_unknown(7, ConsoleTileDestination::library);
    RegistrationObservation managed{true, true, true, true, true, true,
                                    ManualEntry::unknown};
    check(!unknown.observe_registration(7, managed) &&
              unknown.get(7)->phase == Phase::registration_unknown,
          "unknown manual tracking cannot be called absent");
    managed.manual = ManualEntry::absent;
    check(unknown.observe_registration(7, managed) &&
              unknown.get(7)->phase == Phase::managed_without_manual,
          "only explicitly absent manual tracking gets missing status");
    managed.manual = ManualEntry::unknown;
    check(!unknown.observe_registration(7, managed) &&
              unknown.get(7)->phase == Phase::managed_without_manual,
          "later unknown read does not claim a repair");
    managed.manual = ManualEntry::exact;
    check(unknown.observe_registration(7, managed) &&
              unknown.get(7)->phase == Phase::registered_home_unverified,
          "exact manual tracking permits registered status");
    check(unknown.note_home_observation(7, true, false) &&
              unknown.get(7)->phase == Phase::registered_home_unverified,
          "Home icon alone does not prove route");
    check(unknown.note_home_observation(7, true, true) &&
              unknown.get(7)->phase == Phase::home_launch_confirmed,
          "Home and route observations are distinct evidence");

    auto wrong_manual = registration_unknown(7, ConsoleTileDestination::library);
    managed.manual = ManualEntry::conflict;
    check(wrong_manual.observe_registration(7, managed) &&
              wrong_manual.get(7)->phase == Phase::conflict,
          "conflicting manual source is Check required, never missing tracking");
    auto wrong_source = registration_unknown(7, ConsoleTileDestination::library);
    managed.manual = ManualEntry::exact;
    managed.exact_source = false;
    check(wrong_source.observe_registration(7, managed) &&
              wrong_source.get(7)->phase == Phase::conflict,
          "conflicting installed source is Check required");

    std::puts("PASS generic CREATE state (52 titles, routes, duplicate guard, manual states)");
    return 0;
}
