// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/managed_tile_signed_status.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>

namespace signed_status = shelf::managed_tile_signed_status;
namespace pair = shelf::managed_tile_pairing_client;
namespace api = shelf::managed_tile_api_v1;

static_assert(!std::is_copy_constructible<signed_status::PendingStatus>::value);
static_assert(!std::is_move_constructible<signed_status::PendingStatus>::value);
static_assert(shelf::system_count == 52);

namespace {

pair::Session session() {
    pair::Session result{};
    for (std::size_t i = 0; i < result.key.size(); ++i)
        result.key[i] = static_cast<unsigned char>(0x40u + i);
    result.next_counter = 1;
    result.active = true;
    return result;
}

std::size_t system_for(std::string_view name) {
    for (std::size_t i = 0; i < shelf::system_count; ++i)
        if (shelf::catalog[i].id == name) return i;
    std::abort();
}

std::string absent(const char* title) {
    return std::string("{\"status\":0,\"contract_version\":1,\"title_id\":\"") +
        title + "\",\"state\":\"absent\",\"create_nonce\":null,\"route\":null,"
        "\"emulator_title_id\":null,\"source_verified\":false,"
        "\"mount_verified\":false,\"registration_verified\":false,\"reason\":null}";
}

std::string installed(const char* title, const char* nonce) {
    return std::string("{\"status\":0,\"contract_version\":1,\"title_id\":\"") +
        title + "\",\"state\":\"installed\",\"create_nonce\":\"" + nonce +
        "\",\"route\":\"library\",\"emulator_title_id\":\"-\","
        "\"source_verified\":true,\"mount_verified\":true,"
        "\"registration_verified\":true,\"reason\":null}";
}

std::string special(const char* title, const char* state, const char* reason) {
    return std::string("{\"status\":0,\"contract_version\":1,\"title_id\":\"") +
        title + "\",\"state\":\"" + state +
        "\",\"create_nonce\":null,\"route\":null,\"emulator_title_id\":null,"
        "\"source_verified\":null,\"mount_verified\":null,"
        "\"registration_verified\":null,\"reason\":\"" + reason + "\"}";
}

void signed_mac(const pair::Session& opened,
                const signed_status::PendingStatus& pending,
                int status, const std::string& body, char (&hex)[65]) {
    pair::Mac mac{};
    assert(pair::response_mac(opened, pending.counter, pending.request_mac,
                              static_cast<unsigned>(status), body.data(),
                              body.size(), mac));
    signed_status::encode_mac_hex(mac, hex);
    pair::clear(mac.data(), mac.size());
}

signed_status::Outcome consume(pair::Session& opened,
                               signed_status::PendingStatus& pending,
                               int status, const std::string& body,
                               const char* response_hex, std::size_t hex_bytes,
                               api::Status& parsed) {
    return signed_status::consume_status(
        opened, pending, status, body.data(), body.size(),
        response_hex, hex_bytes, parsed);
}

void all_catalog_titles() {
    auto opened = session();
    for (std::size_t system = 0; system < shelf::system_count; ++system) {
        signed_status::PendingStatus pending;
        assert(signed_status::prepare_status(opened, system, pending));
        assert(pending.active && pending.system == system);
        assert(pending.counter == system + 1u);
        assert(std::strcmp(pending.counter_header,
                           std::to_string(system + 1u).c_str()) == 0);
        assert(std::strlen(pending.request_mac_header) == 64u);
        assert(std::strstr(pending.body,
                           shelf::tile_job::owned_title_for(system)) != nullptr);
        const auto body = absent(shelf::tile_job::owned_title_for(system));
        char response_hex[65]{};
        signed_mac(opened, pending, 200, body, response_hex);
        api::Status parsed{};
        assert(consume(opened, pending, 200, body, response_hex, 64u, parsed) ==
               signed_status::Outcome::valid);
        assert(parsed.stage == api::Stage::absent &&
               std::strcmp(parsed.title_id,
                           shelf::tile_job::owned_title_for(system)) == 0);
        assert(!pending.active && pending.counter == 0 &&
               pending.request_mac == pair::Mac{});
        assert(consume(opened, pending, 200, body, response_hex, 64u, parsed) ==
               signed_status::Outcome::unknown);
    }
    assert(opened.next_counter == shelf::system_count + 1u);
}

void pinned_server_status_vector() {
    // Exact bytes emitted by ShadowMount's isolated signed STATUS fixture.
    constexpr char key_hex[] =
        "301d255d53966941ab1d6fb6dfe46cc691fd976ba2d3988c27a9a10c0d9c6814";
    constexpr char request_hex[] =
        "e5b50536b6c87b70baa680c664da3ee8c5a678bb758d4e006cd8aeca6a3180c7";
    constexpr char response_hex[] =
        "4070e409d1e20e91068c657cfd9820d3c6802d84f735a3c4c2f3b73386dea3ef";
    constexpr char expected_request[] =
        "{\"contract_version\":1,\"title_id\":\"PPSA99705\"}";
    pair::Session opened{};
    assert(signed_status::decode_mac_hex(key_hex, 64u, opened.key));
    opened.next_counter = 1;
    opened.active = true;
    const auto system = system_for("psp");
    assert(std::strcmp(shelf::tile_job::owned_title_for(system), "PPSA99705") == 0);
    signed_status::PendingStatus pending;
    assert(signed_status::prepare_status(opened, system, pending));
    assert(pending.counter == 1u && std::strcmp(pending.counter_header, "1") == 0 &&
           std::strcmp(pending.body, expected_request) == 0 &&
           std::strcmp(pending.request_mac_header, request_hex) == 0);
    api::Status parsed{};
    const auto body = absent("PPSA99705");
    assert(signed_status::consume_status(
               opened, pending, 200, body.data(), body.size(), response_hex,
               64u, parsed) == signed_status::Outcome::valid);
    assert(parsed.stage == api::Stage::absent);
}

void malformed_or_uncertain() {
    constexpr char nonce[] = "0123456789abcdef0123456789abcdef";
    const auto system = system_for("snes");
    const auto* title = shelf::tile_job::owned_title_for(system);
    auto opened = session();
    signed_status::PendingStatus pending;
    api::Status parsed{};
    const auto body = absent(title);
    char response_hex[65]{};

    assert(!signed_status::prepare_status(opened, shelf::system_count, pending));
    assert(opened.next_counter == 1);
    assert(!signed_status::prepare_status(opened, system, pending, nonce));
    assert(opened.next_counter == 1);
    pair::Mac stale{};
    stale.fill(0xa5);
    assert(!signed_status::decode_mac_hex(nullptr, 64u, stale));
    assert(stale == pair::Mac{});
    shelf::ConsoleTileIntent wrong_intent{system_for("ps2"),
                                          shelf::ConsoleTileDestination::library, {}};
    assert(!signed_status::prepare_status(opened, system, pending, nonce,
                                          &wrong_intent));
    assert(opened.next_counter == 1);

    assert(signed_status::prepare_status(opened, system, pending));
    assert(!signed_status::prepare_status(opened, system, pending));
    assert(opened.next_counter == 2); // The pending read was not overwritten.
    signed_mac(opened, pending, 200, body, response_hex);
    assert(consume(opened, pending, 200, body, nullptr, 0, parsed) ==
           signed_status::Outcome::unknown);
    assert(parsed.stage == api::Stage::held_unknown && parsed.title_id[0] == 0);
    assert(!pending.active);

    // A disconnected or timed-out transport has no trustworthy STATUS body.
    assert(signed_status::prepare_status(opened, system, pending));
    assert(signed_status::consume_status(opened, pending, 0, nullptr, 0,
                                        nullptr, 0, parsed) ==
           signed_status::Outcome::unknown);
    assert(!pending.active && parsed.title_id[0] == 0);

    // Unknown results burn their counters; a fresh STATUS uses counter 3.
    assert(signed_status::prepare_status(opened, system, pending));
    assert(pending.counter == 3);
    signed_mac(opened, pending, 200, body, response_hex);
    std::string tampered_body = body;
    tampered_body[tampered_body.find("absent")] = 'A';
    assert(consume(opened, pending, 200, tampered_body, response_hex, 64u, parsed) ==
           signed_status::Outcome::unknown);
    assert(parsed.title_id[0] == 0);

    assert(signed_status::prepare_status(opened, system, pending));
    signed_mac(opened, pending, 200, body, response_hex);
    response_hex[0] = 'A'; // Noncanonical header, even when a hex letter.
    assert(consume(opened, pending, 200, body, response_hex, 64u, parsed) ==
           signed_status::Outcome::unknown);
    assert(parsed.title_id[0] == 0);

    assert(signed_status::prepare_status(opened, system, pending));
    signed_mac(opened, pending, 200, body, response_hex);
    assert(consume(opened, pending, 200, body, response_hex, 63u, parsed) ==
           signed_status::Outcome::unknown);

    assert(signed_status::prepare_status(opened, system, pending));
    signed_mac(opened, pending, 200, body, response_hex);
    pending.counter += 1u; // Response for another counter.
    assert(consume(opened, pending, 200, body, response_hex, 64u, parsed) ==
           signed_status::Outcome::unknown);

    assert(signed_status::prepare_status(opened, system, pending));
    signed_mac(opened, pending, 200, body, response_hex);
    pending.request_mac[0] ^= 1u; // Response for another request body/path.
    assert(consume(opened, pending, 200, body, response_hex, 64u, parsed) ==
           signed_status::Outcome::unknown);

    assert(signed_status::prepare_status(opened, system, pending));
    signed_mac(opened, pending, 503, body, response_hex);
    assert(consume(opened, pending, 503, body, response_hex, 64u, parsed) ==
           signed_status::Outcome::unknown);

    assert(signed_status::prepare_status(opened, system, pending));
    const std::string malformed = "{}";
    signed_mac(opened, pending, 200, malformed, response_hex);
    assert(consume(opened, pending, 200, malformed, response_hex, 64u, parsed) ==
           signed_status::Outcome::unknown);
    assert(parsed.title_id[0] == 0);

    assert(signed_status::prepare_status(opened, system, pending));
    const auto duplicate = body.substr(0, body.size() - 1) +
                           ",\"state\":\"installed\"}";
    signed_mac(opened, pending, 200, duplicate, response_hex);
    assert(consume(opened, pending, 200, duplicate, response_hex, 64u, parsed) ==
           signed_status::Outcome::unknown);
    assert(parsed.title_id[0] == 0);

    assert(signed_status::prepare_status(opened, system, pending));
    const auto other = absent(shelf::tile_job::owned_title_for(system_for("psp")));
    signed_mac(opened, pending, 200, other, response_hex);
    assert(consume(opened, pending, 200, other, response_hex, 64u, parsed) ==
           signed_status::Outcome::unknown);

    assert(signed_status::prepare_status(opened, system, pending));
    signed_mac(opened, pending, 200, body, response_hex);
    pair::clear(opened); // Session loss between request and response.
    assert(consume(opened, pending, 200, body, response_hex, 64u, parsed) ==
           signed_status::Outcome::unknown);
}

void generation_and_reported_errors() {
    constexpr char nonce[] = "0123456789abcdef0123456789abcdef";
    const auto system = system_for("snes");
    const auto* title = shelf::tile_job::owned_title_for(system);
    auto opened = session();
    shelf::ConsoleTileIntent intent{system,
                                    shelf::ConsoleTileDestination::library, {}};
    signed_status::PendingStatus pending;
    api::Status parsed{};
    char response_hex[65]{};

    assert(signed_status::prepare_status(opened, system, pending, nonce,
                                          &intent));
    const auto body = installed(title, nonce);
    signed_mac(opened, pending, 200, body, response_hex);
    assert(consume(opened, pending, 200, body, response_hex, 64u, parsed) ==
           signed_status::Outcome::valid);
    assert(parsed.stage == api::Stage::installed && parsed.has_intent &&
           std::strcmp(parsed.create_nonce, nonce) == 0);

    assert(signed_status::prepare_status(opened, system, pending, nonce,
                                          &intent));
    const auto foreign_nonce = installed(
        title, "fedcba9876543210fedcba9876543210");
    signed_mac(opened, pending, 200, foreign_nonce, response_hex);
    assert(consume(opened, pending, 200, foreign_nonce, response_hex, 64u,
                   parsed) == signed_status::Outcome::unknown);
    assert(parsed.title_id[0] == 0);

    assert(signed_status::prepare_status(opened, system, pending));
    const auto conflict = special(title, "conflict", "legacy_pilot");
    signed_mac(opened, pending, 200, conflict, response_hex);
    assert(consume(opened, pending, 200, conflict, response_hex, 64u, parsed) ==
           signed_status::Outcome::conflict);
    assert(parsed.stage == api::Stage::conflict);

    assert(signed_status::prepare_status(opened, system, pending));
    const auto held = special(title, "held_unknown", "journal_uncertain");
    signed_mac(opened, pending, 200, held, response_hex);
    assert(consume(opened, pending, 200, held, response_hex, 64u, parsed) ==
           signed_status::Outcome::held_unknown);
    assert(parsed.stage == api::Stage::held_unknown);
}

} // namespace

int main() {
    all_catalog_titles();
    pinned_server_status_vector();
    malformed_or_uncertain();
    generation_and_reported_errors();
    std::puts("signed STATUS client: 52 titles and authenticated parse gate passed");
}
