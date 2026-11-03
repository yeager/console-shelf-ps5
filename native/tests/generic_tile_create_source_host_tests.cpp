// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/generic_tile_create_host.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <unistd.h>

using namespace shelf;
using namespace shelf::generic_tile_create;

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

static bool wiiu_menu(const void* context, std::size_t system,
                      std::string_view title) {
    return context && system == 8 && title == "PPSA99360";
}

static std::string read_file(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    assert(file);
    std::string content;
    char block[1024];
    for (std::size_t count; (count = std::fread(block, 1, sizeof(block), file)) != 0;)
        content.append(block, count);
    assert(!std::ferror(file) && std::fclose(file) == 0);
    return content;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    CatalogState state;
    std::set<std::string> titles;
    std::size_t eligible = 0;
    for (std::size_t system = 0; system < system_count; ++system) {
        const auto* item = state.get(system);
        assert(item && tile_job::catalog_pin_matches(system));
        assert(titles.insert(tile_job::owned_title_for(system)).second);
        if (reserved_existing_title(system)) {
            assert(item->phase == Phase::existing_title);
            assert(!state.select_route(ConsoleTileIntent{system, ConsoleTileDestination::library, {}}));
            assert(state.begin_source(system, "0123456789abcdef0123456789abcdef",
                                      clear_admission()) == Begin::existing_title);
        } else {
            ++eligible;
            assert(item->phase == Phase::inspect_required);
        }
    }
    assert(eligible == 48 && titles.size() == system_count);
    assert(state.get(system_count) == nullptr);

    constexpr char first_nonce[] = "0123456789abcdef0123456789abcdef";
    constexpr char second_nonce[] = "11111111111111111111111111111111";
    constexpr char third_nonce[] = "22222222222222222222222222222222";
    constexpr char fourth_nonce[] = "33333333333333333333333333333333";
    char root[] = "/tmp/isshelf-create-flow-XXXXXX";
    assert(::mkdtemp(root));

    ConsoleTileIntent ps5_home{0, ConsoleTileDestination::home, {}};
    assert(state.select_route(ps5_home));
    assert(state.get(0)->route_selected);
    assert(state.begin_source(0, first_nonce, Admission{}) == Begin::incomplete_inventory);
    assert(state.get(0)->phase == Phase::inspect_required);
    Admission occupied = clear_admission();
    occupied.user_app = Presence::present;
    assert(state.begin_source(0, first_nonce, occupied) == Begin::occupied);
    Admission no_writer = clear_admission();
    no_writer.exclusive_writer = false;
    assert(state.begin_source(0, first_nonce, no_writer) == Begin::history_or_writer_unproved);
    Admission no_history = clear_admission();
    no_history.durable_history_clear = false;
    assert(state.begin_source(0, first_nonce, no_history) == Begin::history_or_writer_unproved);
    assert(state.begin_source(0, fourth_nonce, clear_admission()) == Begin::started);
    assert(!state.cancel_selection(0));
    // The host adapter cannot create a second source after the title is claimed.
    const auto already_claimed = run_host_source_once(state, 0, first_nonce,
        clear_admission(), root, argv[1]);
    assert(already_claimed.begin == Begin::duplicate_or_unresolved &&
           !already_claimed.writer.attempted);
    // Complete the first claim with the exact reviewed source writer.
    const auto first = generic_app_tile_source::run_once(root, argv[1], fourth_nonce,
                                                        state.get(0)->route);
    assert(first.attempted && first.complete);
    assert(state.finish_source(0, first.complete, first.receipt_bytes, first.receipt_fnv));
    assert(state.get(0)->phase == Phase::source_ready);
    assert(std::strcmp(state.get(0)->nonce, fourth_nonce) == 0);
    assert(state.get(0)->route.destination == ConsoleTileDestination::home);
    const std::string ps5_source = std::string(root) + "/" +
        generic_app_tile_source::folder_prefix + fourth_nonce + "/PPSA99700";
    assert(read_file(ps5_source + "/forwarder.json").find(",home,-") != std::string::npos);
    assert(!state.select_route(ConsoleTileIntent{0, ConsoleTileDestination::library, {}}));
    assert(!state.note_registration_attempt(0, false));
    assert(state.note_registration_attempt(0, true));
    assert(state.get(0)->phase == Phase::registration_unknown);
    assert(!state.observe_registration(0, RegistrationObservation{}));
    assert(state.get(0)->phase == Phase::registration_unknown);
    RegistrationObservation manual_without_install{true, false, false, false, false,
                                                   false, ManualEntry::exact};
    assert(!state.observe_registration(0, manual_without_install));
    assert(state.get(0)->phase == Phase::registration_unknown);
    RegistrationObservation installed{true, true, true, true, true, true,
                                      ManualEntry::unknown};
    assert(!state.observe_registration(0, installed));
    assert(state.get(0)->phase == Phase::registration_unknown);
    installed.manual = ManualEntry::absent;
    assert(state.observe_registration(0, installed));
    assert(state.get(0)->phase == Phase::managed_without_manual);
    assert(!state.note_home_observation(0, true, true));
    installed.manual = ManualEntry::exact;
    assert(state.observe_registration(0, installed));
    assert(state.get(0)->phase == Phase::registered_home_unverified);
    assert(state.note_home_observation(0, true, false));
    assert(state.get(0)->phase == Phase::registered_home_unverified);
    assert(state.note_home_observation(0, true, true));
    assert(state.get(0)->phase == Phase::home_launch_confirmed);

    CatalogState switch_state;
    ConsoleTileIntent switch_library{7, ConsoleTileDestination::library, {}};
    assert(switch_state.select_route(switch_library));
    const auto switch_source = run_host_source_once(switch_state, 7, second_nonce,
        clear_admission(), root, argv[1]);
    assert(switch_source.begin == Begin::started && switch_source.writer.complete);
    assert(switch_state.get(7)->phase == Phase::source_ready);
    const std::string switch_path = std::string(root) + "/" +
        generic_app_tile_source::folder_prefix + second_nonce + "/PPSA99707";
    assert(read_file(switch_path + "/forwarder.json").find(",library,-") != std::string::npos);
    assert(switch_state.note_registration_attempt(7, true));
    RegistrationObservation conflicting{true, true, true, false, true, true,
                                        ManualEntry::exact};
    assert(switch_state.observe_registration(7, conflicting));
    assert(switch_state.get(7)->phase == Phase::conflict);

    CatalogState manual_conflict;
    assert(manual_conflict.select_route(switch_library));
    assert(manual_conflict.begin_source(7, first_nonce, clear_admission()) == Begin::started);
    assert(manual_conflict.finish_source(7, true, 1, 1));
    assert(manual_conflict.note_registration_attempt(7, true));
    RegistrationObservation wrong_manual{true, true, true, true, true, true,
                                         ManualEntry::conflict};
    assert(manual_conflict.observe_registration(7, wrong_manual));
    assert(manual_conflict.get(7)->phase == Phase::conflict);

    CatalogState wiiu_state;
    ConsoleTileIntent wiiu_menu_route{8, ConsoleTileDestination::emulator, {}};
    std::strcpy(wiiu_menu_route.emulator_title, "PPSA99360");
    assert(!wiiu_state.select_route(wiiu_menu_route));
    assert(wiiu_state.select_route(wiiu_menu_route, wiiu_menu, root));
    assert(wiiu_state.begin_source(8, third_nonce, clear_admission()) == Begin::no_route);
    const auto wiiu_source = run_host_source_once(wiiu_state, 8, third_nonce,
        clear_admission(), root, argv[1], wiiu_menu, root);
    assert(wiiu_source.begin == Begin::started && wiiu_source.writer.complete);
    const std::string wiiu_path = std::string(root) + "/" +
        generic_app_tile_source::folder_prefix + third_nonce + "/PPSA99708";
    assert(read_file(wiiu_path + "/forwarder.json").find(",emulator,PPSA99360") !=
           std::string::npos);
    assert(std::strcmp(wiiu_state.get(8)->route.emulator_title, "PPSA99360") == 0);

    CatalogState interrupted;
    assert(interrupted.select_route(switch_library));
    assert(interrupted.begin_source(7, first_nonce, clear_admission()) == Begin::started);
    assert(interrupted.finish_source(7, false, 0, 0));
    assert(interrupted.get(7)->phase == Phase::source_incomplete);
    assert(interrupted.begin_source(7, fourth_nonce, clear_admission()) ==
           Begin::duplicate_or_unresolved);
    assert(std::strstr(phase_text(Phase::registration_unknown), "unknown"));
    assert(std::strstr(phase_text(Phase::managed_without_manual), "tracking"));

    assert(std::filesystem::remove_all(root) > 0);
    return 0;
}
