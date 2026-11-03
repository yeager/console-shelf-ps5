// SPDX-License-Identifier: GPL-3.0-only
#define SHELF_TILE_WRITE_QUARANTINE_TEST
#include "shelf/managed_tile_signed_create.hpp"
#include "shelf/generic_app_tile_source.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>

namespace create = shelf::managed_tile_signed_create;
namespace status = shelf::managed_tile_signed_status;
namespace pair = shelf::managed_tile_pairing_client;
namespace api = shelf::managed_tile_api_v1;

static_assert(!std::is_copy_constructible<create::PendingCreate>::value);
static_assert(!std::is_move_constructible<create::PendingCreate>::value);
static_assert(shelf::system_count == 52);

namespace {

constexpr char nonce[] = "0123456789abcdef0123456789abcdef";

pair::Session session() {
    pair::Session result{};
    for (std::size_t i = 0; i < result.key.size(); ++i)
        result.key[i] = static_cast<unsigned char>(0x30u + i);
    result.next_counter = 1;
    result.active = true;
    return result;
}

struct JournalFixture {
    shelf::ConsoleTileIntent expected{};
    const create::PendingCreate* pending = nullptr;
    unsigned calls = 0;
    bool succeed = true;
    bool mutate_intent = false;
    bool mutate_nonce = false;
};

bool journal(void* context, const shelf::ConsoleTileIntent& intent,
             const char supplied_nonce[33]) {
    auto& fixture = *static_cast<JournalFixture*>(context);
    ++fixture.calls;
    assert(shelf::console_tile_intents_equal(intent, fixture.expected));
    assert(std::strcmp(supplied_nonce, nonce) == 0);
    if (fixture.pending)
        assert(!fixture.pending->ready && fixture.pending->body[0] == 0 &&
               fixture.pending->counter == 0);
    if (fixture.mutate_intent)
        const_cast<shelf::ConsoleTileIntent&>(intent).destination =
            shelf::ConsoleTileDestination::home;
    if (fixture.mutate_nonce)
        const_cast<char*>(supplied_nonce)[0] = 'f';
    return fixture.succeed;
}

std::string operation(const char* title, const char* state,
                      bool replay = false) {
    return std::string("{\"status\":0,\"contract_version\":1,\"title_id\":\"") +
        title + "\",\"operation\":\"create\",\"nonce\":\"" + nonce +
        "\",\"state\":\"" + state + "\",\"replay\":" +
        (replay ? "true" : "false") + "}";
}

std::string installed(const shelf::ConsoleTileIntent& intent) {
    const char* title = shelf::tile_job::owned_title_for(intent.system);
    const auto route = shelf::console_tile_destination_name(intent.destination);
    const char* emulator = intent.destination == shelf::ConsoleTileDestination::emulator
                               ? intent.emulator_title : "-";
    return std::string("{\"status\":0,\"contract_version\":1,\"title_id\":\"") +
        title + "\",\"state\":\"installed\",\"create_nonce\":\"" + nonce +
        "\",\"route\":\"" + std::string(route) +
        "\",\"emulator_title_id\":\"" + emulator +
        "\",\"source_verified\":true,\"mount_verified\":true,"
        "\"registration_verified\":true,\"reason\":null}";
}

template <typename Pending>
void response_hex(const pair::Session& opened, const Pending& pending,
                  unsigned http, const std::string& body, char (&hex)[65]) {
    pair::Mac mac{};
    assert(pair::response_mac(opened, pending.counter, pending.request_mac,
                              http, body.data(), body.size(), mac));
    status::encode_mac_hex(mac, hex);
    pair::clear(mac.data(), mac.size());
}

void successful_route(const shelf::ConsoleTileIntent& intent,
                      const shelf::Library* library) {
    auto opened = session();
    create::PendingCreate pending;
    JournalFixture fixture{intent};
    fixture.pending = &pending;
    assert(create::prepare_create(opened, intent, library, nonce, journal,
                                  &fixture, pending) == create::Prepare::ready);
    assert(fixture.calls == 1 && pending.ready && !pending.dispatched &&
           pending.counter == 1 && opened.next_counter == 2 &&
           pending.generation.journaled &&
           !pending.generation.may_have_sent &&
           shelf::console_tile_intents_equal(pending.generation.intent, intent));
    const std::string expected_route =
        std::string("\"route\":\"") +
        std::string(shelf::console_tile_destination_name(intent.destination)) +
        "\"";
    assert(std::strcmp(pending.counter_header, "1") == 0 &&
           std::strlen(pending.request_mac_header) == 64 &&
           std::strstr(pending.body, shelf::tile_job::owned_title_for(intent.system)) &&
           std::strstr(pending.body, expected_route.c_str()) != nullptr);
    const bool psp_library_vector =
        std::strcmp(shelf::tile_job::owned_title_for(intent.system),
                    "PPSA99705") == 0 &&
        intent.destination == shelf::ConsoleTileDestination::library;
    if (psp_library_vector)
        assert(std::strcmp(pending.request_mac_header,
                           "e6caf30d5fe9e9b5413f9f8559f68d8"
                           "e56d48ec78bec46d86471f17de8f688a2") == 0);
    const auto source = shelf::generic_app_tile_source::plan(intent);
    assert(source.valid &&
           std::strstr(source.launch_arg,
                       std::string(shelf::catalog[intent.system].id).c_str()) &&
           std::strstr(source.launch_arg,
                       std::string(shelf::console_tile_destination_name(intent.destination)).c_str()));
    assert(create::begin_send(pending) && !create::begin_send(pending));
    const auto body = operation(shelf::tile_job::owned_title_for(intent.system),
                                "preparing");
    char mac[65]{};
    response_hex(opened, pending, 202u, body, mac);
    if (psp_library_vector)
        assert(std::strcmp(mac,
                           "c7a5532c2a75b1da8ae89941765926f"
                           "f518294b0fd1dfb82de4238f350b66546") == 0);
    create::Generation generation;
    api::Operation receipt;
    assert(create::consume_create(opened, pending, 202, body.data(), body.size(),
                                  mac, std::strlen(mac), generation, receipt) ==
           create::Outcome::receipt_needs_status);
    assert(!pending.ready && !pending.dispatched && pending.counter == 0 &&
           generation.journaled && generation.may_have_sent &&
           shelf::console_tile_intents_equal(generation.intent, intent) &&
           generation.create_counter == 1 &&
           std::strcmp(generation.nonce, nonce) == 0 &&
           receipt.receipt == api::ReceiptState::needs_status);
    assert(create::consume_create(opened, pending, 202, body.data(), body.size(),
                                  mac, std::strlen(mac), generation, receipt) ==
           create::Outcome::invalid);
    // Recreate the immutable generation after the consumed receipt. A caller
    // retains it before any second consume attempt.
}

void reconciliation(const shelf::ConsoleTileIntent& intent,
                    const shelf::Library* library) {
    auto opened = session();
    create::PendingCreate pending;
    JournalFixture fixture{intent};
    fixture.pending = &pending;
    assert(create::prepare_create(opened, intent, library, nonce, journal,
                                  &fixture, pending) == create::Prepare::ready);
    assert(create::begin_send(pending));
    create::Generation generation;
    api::Operation receipt;
    assert(create::consume_create(opened, pending, -1, nullptr, 0,
                                  nullptr, 0, generation, receipt) ==
           create::Outcome::uncertain);
    assert(generation.journaled && generation.may_have_sent &&
           shelf::console_tile_intents_equal(generation.intent, intent));
    status::PendingStatus read;
    assert(create::prepare_reconcile_status(opened, generation, read));
    const auto body = installed(intent);
    char mac[65]{};
    response_hex(opened, read, 200u, body, mac);
    api::Status observed;
    assert(create::consume_reconcile_status(
               opened, read, 200, body.data(), body.size(),
               mac, std::strlen(mac), observed) ==
           create::Reconcile::installed_reported);
    assert(shelf::console_tile_intents_equal(observed.intent, intent));
    auto changed = generation;
    changed.intent.destination =
        intent.destination == shelf::ConsoleTileDestination::home
            ? shelf::ConsoleTileDestination::library
            : shelf::ConsoleTileDestination::home;
    std::memset(changed.intent.emulator_title, 0,
                sizeof(changed.intent.emulator_title));
    assert(!create::prepare_reconcile_status(opened, changed, read));
    changed = generation;
    changed.nonce[0] = changed.nonce[0] == '0' ? '1' : '0';
    assert(!create::prepare_reconcile_status(opened, changed, read));
    changed = generation;
    changed.create_request_mac[0] ^= 1;
    assert(!create::prepare_reconcile_status(opened, changed, read));
    changed = generation;
    ++changed.create_counter;
    assert(!create::prepare_reconcile_status(opened, changed, read));
    assert(create::prepare_reconcile_status(opened, generation, read));
    auto wrong_route = intent;
    wrong_route.destination =
        intent.destination == shelf::ConsoleTileDestination::home
            ? shelf::ConsoleTileDestination::library
            : shelf::ConsoleTileDestination::home;
    std::memset(wrong_route.emulator_title, 0,
                sizeof(wrong_route.emulator_title));
    const auto wrong_body = installed(wrong_route);
    response_hex(opened, read, 200u, wrong_body, mac);
    assert(create::consume_reconcile_status(
               opened, read, 200, wrong_body.data(), wrong_body.size(),
               mac, std::strlen(mac), observed) ==
           create::Reconcile::unknown);
    assert(create::prepare_reconcile_status(opened, generation, read));
    response_hex(opened, read, 200u, body, mac);
    auto rekeyed = opened;
    rekeyed.key[0] ^= 1;
    assert(create::consume_reconcile_status(
               rekeyed, read, 200, body.data(), body.size(),
               mac, std::strlen(mac), observed) ==
           create::Reconcile::unknown);
}

void failures() {
    const shelf::ConsoleTileIntent library{0, shelf::ConsoleTileDestination::library, {}};
    auto opened = session();
    create::PendingCreate pending;
    JournalFixture fixture{library};
    fixture.pending = &pending;
    auto before = opened.next_counter;
    assert(create::prepare_create(opened, library, nullptr, "bad", journal,
                                  &fixture, pending) == create::Prepare::invalid_request);
    assert(opened.next_counter == before && fixture.calls == 0);
    opened.active = false;
    assert(create::prepare_create(opened, library, nullptr, nonce, journal,
                                  &fixture, pending) == create::Prepare::counter_unavailable);
    assert(fixture.calls == 0 && !pending.ready);
    opened = session();
    fixture.succeed = false;
    assert(create::prepare_create(opened, library, nullptr, nonce, journal,
                                  &fixture, pending) == create::Prepare::journal_unavailable);
    assert(opened.next_counter == 2 && !pending.ready &&
           !create::begin_send(pending));
    fixture.succeed = true;
    fixture.mutate_intent = true;
    assert(create::prepare_create(opened, library, nullptr, nonce, journal,
                                  &fixture, pending) == create::Prepare::journal_unavailable);
    fixture.mutate_intent = false;
    fixture.mutate_nonce = true;
    assert(create::prepare_create(opened, library, nullptr, nonce, journal,
                                  &fixture, pending) == create::Prepare::journal_unavailable);
    fixture.mutate_nonce = false;
    assert(opened.next_counter == 4 && !pending.ready);
    assert(create::prepare_create(opened, library, nullptr, nonce, journal,
                                  &fixture, pending) == create::Prepare::ready);
    assert(create::prepare_create(opened, library, nullptr, nonce, journal,
                                  &fixture, pending) == create::Prepare::invalid_request);
    create::PendingCreate empty_pending;
    assert(!create::begin_send(empty_pending));
    create::Generation generation;
    api::Operation receipt;
    assert(create::consume_create(opened, pending, -1, nullptr, 0,
                                  nullptr, 0, generation, receipt) ==
           create::Outcome::invalid && pending.ready);
    assert(create::begin_send(pending));
    const auto body = operation(shelf::tile_job::owned_title_for(library.system),
                                "preparing");
    char mac[65]{};
    response_hex(opened, pending, 202u, body, mac);
    mac[0] = mac[0] == '0' ? '1' : '0';
    assert(create::consume_create(opened, pending, 202, body.data(), body.size(),
                                  mac, std::strlen(mac), generation, receipt) ==
           create::Outcome::uncertain);
    assert(generation.journaled && generation.may_have_sent &&
           std::strcmp(generation.nonce, nonce) == 0);
    status::PendingStatus read;
    assert(create::prepare_reconcile_status(opened, generation, read));
    auto absent = std::string("{\"status\":0,\"contract_version\":1,\"title_id\":\"") +
        shelf::tile_job::owned_title_for(library.system) +
        "\",\"state\":\"absent\",\"create_nonce\":null,\"route\":null,"
        "\"emulator_title_id\":null,\"source_verified\":false,"
        "\"mount_verified\":false,\"registration_verified\":false,"
        "\"reason\":null}";
    response_hex(opened, read, 200u, absent, mac);
    api::Status observed;
    assert(create::consume_reconcile_status(
               opened, read, 200, absent.data(), absent.size(),
               mac, std::strlen(mac), observed) ==
           create::Reconcile::unknown);
    status::PendingStatus diagnostic;
    assert(status::prepare_status(opened, library.system, diagnostic));
    const auto installed_body = installed(library);
    response_hex(opened, diagnostic, 200u, installed_body, mac);
    observed.stage = api::Stage::installed; // Fabricated caller state.
    assert(create::consume_reconcile_status(
               opened, diagnostic, 200, installed_body.data(),
               installed_body.size(), mac, std::strlen(mac), observed) ==
           create::Reconcile::unknown &&
           observed.stage == api::Stage::held_unknown &&
           !diagnostic.active);
    generation.may_have_sent = false;
    assert(!create::prepare_reconcile_status(opened, generation, read));
    generation.may_have_sent = true;
    generation.journaled = false;
    assert(!create::prepare_reconcile_status(opened, generation, read));
}

} // namespace

int main() {
    std::size_t mutable_count = 0;
    for (std::size_t system = 0; system < shelf::system_count; ++system) {
        const shelf::ConsoleTileIntent library{
            system, shelf::ConsoleTileDestination::library, {}};
        auto opened = session();
        create::PendingCreate pending;
        JournalFixture fixture{library};
        if (!api::mutable_title(system)) {
            assert(create::prepare_create(opened, library, nullptr, nonce,
                                          journal, &fixture, pending) ==
                   create::Prepare::invalid_request);
            assert(fixture.calls == 0 && opened.next_counter == 1);
            continue;
        }
        ++mutable_count;
        successful_route(library, nullptr);
        reconciliation(library, nullptr);
        const shelf::ConsoleTileIntent home{
            system, shelf::ConsoleTileDestination::home, {}};
        successful_route(home, nullptr);
        reconciliation(home, nullptr);
    }
    assert(mutable_count == 48);
    static shelf::Library installed_routes;
    std::size_t emulator_routes = 0;
    for (const auto& menu : isshelf_badge::generic_tile::menus) {
        if (!api::mutable_title(menu.system)) continue;
        shelf::ConsoleTileIntent emulator{
            menu.system, shelf::ConsoleTileDestination::emulator, {}};
        std::snprintf(emulator.emulator_title, sizeof(emulator.emulator_title),
                      "%s", menu.title_id);
        installed_routes.route_count = 1;
        installed_routes.routes[0] = {};
        installed_routes.routes[0].system = menu.system;
        std::snprintf(installed_routes.routes[0].title_id,
                      sizeof(installed_routes.routes[0].title_id), "%s",
                      menu.title_id);
        std::snprintf(installed_routes.routes[0].folder,
                      sizeof(installed_routes.routes[0].folder),
                      "/data/homebrew/%s", menu.title_id);
        successful_route(emulator, &installed_routes);
        reconciliation(emulator, &installed_routes);
        ++emulator_routes;
    }
    assert(emulator_routes > 0);
    failures();
    std::printf("PASS: %zu new titles and %zu emulator routes preserve signed CREATE generation for STATUS\n",
                mutable_count, emulator_routes);
}
