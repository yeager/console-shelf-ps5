// SPDX-License-Identifier: GPL-3.0-only
// Host-only second half of the UI intent seam test. Deliberately no transport.
#define SHELF_TILE_WRITE_QUARANTINE_TEST
#include "shelf/managed_tile_signed_create.hpp"
#include "shelf/generic_app_tile_source.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

static_assert(!shelf::console_tile_writes_enabled,
              "The signed CREATE fixture must remain default-off");

namespace create = shelf::managed_tile_signed_create;
namespace pair = shelf::managed_tile_pairing_client;

namespace {

struct JournalFixture {
    shelf::ConsoleTileIntent intent{};
    std::string nonce;
    unsigned calls = 0;
};

bool journal(void* context, const shelf::ConsoleTileIntent& intent,
             const char nonce[33]) {
    auto& expected = *static_cast<JournalFixture*>(context);
    ++expected.calls;
    return shelf::console_tile_intents_equal(intent, expected.intent) &&
           expected.nonce == nonce;
}

pair::Session session() {
    pair::Session result{};
    for (std::size_t i = 0; i < result.key.size(); ++i)
        result.key[i] = static_cast<unsigned char>(0x30u + i);
    result.next_counter = 1;
    result.active = true;
    return result;
}

std::string hex(const char* bytes, std::size_t count) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        const auto value = static_cast<unsigned char>(bytes[i]);
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 15]);
    }
    return result;
}

} // namespace

int main() {
    static shelf::Library installed{};
    std::string line;
    std::size_t sequence = 0;
    std::size_t emulator_choices = 0;
    while (std::getline(std::cin, line)) {
        std::istringstream fields(line);
        std::size_t system = shelf::system_count;
        std::string route, emulator, extra;
        assert((fields >> system >> route >> emulator) && !(fields >> extra));
        shelf::ConsoleTileIntent intent{};
        intent.system = system;
        if (route == "library") intent.destination = shelf::ConsoleTileDestination::library;
        else if (route == "home") intent.destination = shelf::ConsoleTileDestination::home;
        else {
            assert(route == "emulator" && emulator != "-");
            intent.destination = shelf::ConsoleTileDestination::emulator;
            assert(emulator.size() < sizeof(intent.emulator_title));
            std::memcpy(intent.emulator_title, emulator.c_str(), emulator.size() + 1);
        }
        assert(system < shelf::system_count &&
               shelf::managed_tile_api_v1::mutable_title(system));
        const shelf::Library* library = nullptr;
        if (intent.destination == shelf::ConsoleTileDestination::emulator) {
            ++emulator_choices;
            installed.route_count = 1;
            installed.routes[0] = {};
            installed.routes[0].system = system;
            std::snprintf(installed.routes[0].title_id,
                          sizeof(installed.routes[0].title_id), "%s", emulator.c_str());
            std::snprintf(installed.routes[0].folder,
                          sizeof(installed.routes[0].folder),
                          "/data/homebrew/%s", emulator.c_str());
            library = &installed;
        } else assert(emulator == "-");
        char nonce[33]{};
        std::snprintf(nonce, sizeof(nonce), "%032zx", ++sequence);
        JournalFixture fixture{intent, nonce};
        auto opened = session();
        create::PendingCreate pending;
        assert(create::prepare_create(opened, intent, library, "invalid-nonce",
                                      journal, &fixture, pending) ==
               create::Prepare::invalid_request);
        assert(fixture.calls == 0 && opened.next_counter == 1 && !pending.ready);
        if (library) {
            installed.routes[0].system = (system + 1) % shelf::system_count;
            assert(create::prepare_create(opened, intent, library, nonce,
                                          journal, &fixture, pending) ==
                   create::Prepare::invalid_request);
            assert(fixture.calls == 0 && opened.next_counter == 1);
            installed.routes[0].system = system;
        }
        assert(create::prepare_create(opened, intent, library, nonce,
                                      journal, &fixture, pending) ==
               create::Prepare::ready);
        assert(fixture.calls == 1 && pending.ready && !pending.dispatched &&
               pending.generation.journaled &&
               shelf::console_tile_intents_equal(pending.generation.intent, intent) &&
               std::strcmp(pending.generation.nonce, nonce) == 0 &&
               std::strlen(pending.request_mac_header) == 64);
        const auto source = shelf::generic_app_tile_source::plan(intent);
        assert(source.valid && source.forwarder_bytes == std::strlen(source.forwarder) &&
               std::strcmp(source.title_id,
                           shelf::tile_job::owned_title_for(system)) == 0);
        std::cout << system << '\t'
                  << hex(pending.body, pending.body_bytes) << '\t'
                  << hex(source.forwarder, source.forwarder_bytes) << '\t'
                  << pending.request_mac_header << '\n';
        if (sequence == 1) {
            assert(create::begin_send(pending));
            create::Generation generation;
            shelf::managed_tile_api_v1::Operation receipt;
            assert(create::consume_create(opened, pending, -1, nullptr, 0,
                                          nullptr, 0, generation, receipt) ==
                   create::Outcome::uncertain);
            shelf::managed_tile_signed_status::PendingStatus read;
            assert(create::prepare_reconcile_status(opened, generation, read));
            auto changed = generation;
            changed.nonce[0] = changed.nonce[0] == '0' ? '1' : '0';
            assert(!create::prepare_reconcile_status(opened, changed, read));
            changed = generation;
            changed.intent.destination = shelf::ConsoleTileDestination::home;
            assert(!create::prepare_reconcile_status(opened, changed, read));
        }
    }
    assert(sequence == 103 && emulator_choices == 9);
    std::fprintf(stderr, "Signed fixture: %zu CREATE requests, %zu emulator choices\n",
                 sequence, emulator_choices);
}
