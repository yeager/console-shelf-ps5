// SPDX-License-Identifier: GPL-3.0-only
#define SHELF_CONSOLE_TILE_HELPER_HOST_TEST
#define SHELF_CONSOLE_TILE_HELPER_TEST_PIN
#include "shelf/console_tile_job_bridge.hpp"
#include "shelf/console_tile_job_routes.hpp"
#include <cassert>
#include <cstring>
#include <map>
#include <string>

namespace {
struct Files {
    std::map<std::string, std::string> files;
    int sends = 0;
    static bool entropy(void*, unsigned char (&output)[16]) {
        std::memset(output, 0, sizeof(output));
        return true;
    }
    static shelf::tile_job::FileStatus read(void* raw, const char* path, char* output,
                                             std::size_t capacity, std::size_t& used) {
        auto& files = static_cast<Files*>(raw)->files;
        const auto item = files.find(path);
        if (item == files.end()) return shelf::tile_job::FileStatus::missing;
        if (item->second.size() >= capacity) return shelf::tile_job::FileStatus::error;
        used = item->second.size();
        std::memcpy(output, item->second.data(), used);
        return shelf::tile_job::FileStatus::present;
    }
    static bool create(void* raw, const char* path, std::string_view body) {
        return static_cast<Files*>(raw)->files.emplace(path, body).second;
    }
    shelf::tile_job::JobIo io() { return {this, entropy, read, create}; }
    static bool send(void* raw) {
        ++static_cast<Files*>(raw)->sends;
        return true;
    }
};

bool installed(const void*, std::size_t system, std::string_view title) {
    return system == 2 && title == "PPSA99303";
}
}

int main() {
    shelf::ConsoleTileIntent intent;
    intent.system = 2; // PS3, fixed PPSA99702.
    intent.destination = shelf::ConsoleTileDestination::home;
    Files files;
    int old_calls = 0, new_calls = 0;
    auto old = [&](const shelf::ConsoleTileIntent&) {
        ++old_calls;
        shelf::ConsoleTileOutcome result;
        result.stage = shelf::ConsoleTileStage::unavailable;
        std::strcpy(result.message, "old preview");
        return result;
    };
    auto newer = [&](shelf::tile_job::Dispatch& job, shelf::tile_job::Action action,
                     const shelf::ConsoleTileIntent& selected, const unsigned char*,
                     std::size_t) {
        ++new_calls;
        assert(action == shelf::tile_job::Action::upsert);
        assert(selected.system == 2);
        assert(job.begin(files.io(), action, selected, installed, nullptr));
        assert(job.attempt_once(files.io(), Files::send, &files));
        return job.phase();
    };
    const unsigned char helper[] = {'a', 'b', 'c'};
    const unsigned char wrong_helper[] = {'a', 'b', 'd'};
    const shelf::tile_job::PinnedHelper reviewed_helper{helper, sizeof(helper)};
    const shelf::tile_job::PinnedHelper unreviewed_helper{wrong_helper, sizeof(wrong_helper)};
    assert(reviewed_helper.available() && !unreviewed_helper.available());
    auto availability = shelf::tile_job::install_availability(
        false, reviewed_helper, [] {
            shelf::ConsoleTileAvailability preview;
            preview.reason = shelf::ConsoleTileAvailabilityReason::api_transport;
            std::strcpy(preview.message, "old preview");
            return preview;
        });
    assert(availability.reason == shelf::ConsoleTileAvailabilityReason::api_transport &&
           std::strcmp(availability.message, "old preview") == 0);
    int legacy_probes = 0;
    availability = shelf::tile_job::install_availability(
        true, unreviewed_helper, [&] {
            ++legacy_probes;
            return shelf::ConsoleTileAvailability{};
        });
    assert(!availability.ready() && legacy_probes == 0);
    availability = shelf::tile_job::install_availability(
        true, reviewed_helper, [&] {
            ++legacy_probes;
            return shelf::ConsoleTileAvailability{};
        });
    assert(availability.ready() && legacy_probes == 0);
    auto outcome = shelf::tile_job::run_install(false, intent, installed, nullptr,
                                                reviewed_helper, old, newer);
    assert(outcome.stage == shelf::ConsoleTileStage::unavailable);
    assert(old_calls == 1 && new_calls == 0 && files.sends == 0);

    outcome = shelf::tile_job::run_install(true, intent, installed, nullptr,
                                           unreviewed_helper, old, newer);
    assert(outcome.stage == shelf::ConsoleTileStage::unavailable);
    assert(old_calls == 1 && new_calls == 0 && files.files.empty());

    outcome = shelf::tile_job::run_install(true, intent, installed, nullptr,
                                           reviewed_helper, old, newer);
    assert(outcome.stage == shelf::ConsoleTileStage::pending && !outcome.owned);
    assert(old_calls == 1 && new_calls == 1 && files.sends == 1);
    const std::string nonce(32, '0');
    const auto request = files.files.at(std::string(shelf::tile_job::job_root) +
                                       "/console-tile-job-" + nonce + ".txt");
    assert(request == "format=isshelf-console-tile-job/1\nnonce=" + nonce +
                      "\naction=upsert\nsystem=ps3\ndestination=home\nemulator=-\n");
    assert(request.find("title=") == std::string::npos);
    assert(request.find("path=") == std::string::npos);
    assert(request.find("platform=") == std::string::npos);
    assert(files.files.count(std::string(shelf::tile_job::job_root) +
                             "/console-tile-sent-" + nonce + ".txt") == 1);

    intent.destination = shelf::ConsoleTileDestination::emulator;
    std::strcpy(intent.emulator_title, "PPSA99303");
    Files emulator_files;
    auto emulator_send = [&](shelf::tile_job::Dispatch& job, shelf::tile_job::Action action,
                             const shelf::ConsoleTileIntent& selected, const unsigned char*,
                             std::size_t) {
        assert(job.begin(emulator_files.io(), action, selected, installed, nullptr));
        return job.phase();
    };
    outcome = shelf::tile_job::run_install(true, intent, installed, nullptr,
                                           reviewed_helper, old, emulator_send);
    assert(outcome.stage == shelf::ConsoleTileStage::deferred);
    assert(emulator_files.files.begin()->second.find("emulator=PPSA99303\n") != std::string::npos);
    std::strcpy(intent.emulator_title, "PPSA99203"); // PS2 route on PS3 request.
    outcome = shelf::tile_job::run_install(true, intent, installed, nullptr,
                                           reviewed_helper, old, newer);
    assert(outcome.stage == shelf::ConsoleTileStage::conflict);
    assert(old_calls == 1 && new_calls == 1);

    assert(shelf::system_count == 52);
    int generic_calls = 0;
    auto generic_send = [&](shelf::tile_job::Dispatch&, shelf::tile_job::Action action,
                            const shelf::ConsoleTileIntent& selected,
                            const unsigned char* bytes, std::size_t length) {
        assert(action == shelf::tile_job::Action::upsert);
        assert(shelf::tile_job::catalog_pin_matches(selected.system));
        assert(bytes == helper && length == sizeof(helper));
        ++generic_calls;
        return shelf::tile_job::Phase::held;
    };
    for (std::size_t system = 0; system < shelf::system_count; ++system)
        for (const auto destination : {shelf::ConsoleTileDestination::library,
                                       shelf::ConsoleTileDestination::home}) {
            shelf::ConsoleTileIntent selected{system, destination, {}};
            outcome = shelf::tile_job::run_install(true, selected, nullptr, nullptr,
                                                    reviewed_helper, old, generic_send);
            assert(outcome.stage == shelf::ConsoleTileStage::deferred && !outcome.owned);
        }
    assert(generic_calls == 104 && old_calls == 1);

    static shelf::Library route_library;
    // Synthetic one-route Library fixtures exercise policy for each candidate;
    // they do not attest installed emulators on a PS5.
    std::size_t synthetic_route_cases = 0;
    std::size_t shared_retroarch_rejections = 0;
    for (std::size_t system = 0; system < shelf::system_count; ++system) {
        const auto id = shelf::catalog[system].id;
        const char* title = nullptr;
        if (id == "ps3") title = "PPSA99303";
        else if (id == "ps2") title = "PPSA99203";
        else if (id == "ps1") title = "PPSA99248";
        else if (id == "switch") title = "PPSA99008";
        else if (id == "wiiu") title = "PPSA99360";
        else if (!shelf::catalog[system].suggested_core.empty()) title = "PPSA99169";
        if (!title) continue;
        route_library.route_count = 1;
        auto& route = route_library.routes[0];
        route = {};
        route.system = system;
        std::snprintf(route.id, sizeof(route.id), "available-route");
        std::snprintf(route.title_id, sizeof(route.title_id), "%s", title);
        std::snprintf(route.folder, sizeof(route.folder), "/data/homebrew/%s", title);
        shelf::ConsoleTileIntent selected{system, shelf::ConsoleTileDestination::emulator, {}};
        std::snprintf(selected.emulator_title, sizeof(selected.emulator_title), "%s", title);
        outcome = shelf::tile_job::run_install(
            true, selected, shelf::tile_job::installed_emulator_route, &route_library,
            reviewed_helper, old, generic_send);
        if (!shelf::console_tile_emulator_menu_selects_system(title)) {
            assert(outcome.stage == shelf::ConsoleTileStage::conflict && !outcome.owned);
            ++shared_retroarch_rejections;
            continue;
        }
        assert(outcome.stage == shelf::ConsoleTileStage::deferred && !outcome.owned);
        ++synthetic_route_cases;
    }
    assert(synthetic_route_cases == 5 && shared_retroarch_rejections == 44 &&
           generic_calls == 109 && old_calls == 1);
    return 0;
}
