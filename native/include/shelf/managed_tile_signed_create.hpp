// SPDX-License-Identifier: GPL-3.0-only
// Offline authenticated CREATE boundary. No production callsite or transport.
#pragma once

#include "console_tile_job_routes.hpp"
#include "console_tile_write_gate.hpp"
#include "managed_tile_signed_status.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace shelf::managed_tile_signed_create {

namespace api = managed_tile_api_v1;
namespace pair = managed_tile_pairing_client;
namespace signed_status = managed_tile_signed_status;

static_assert(!console_tile_writes_enabled,
              "This unwired CREATE candidate must not enable PS5 tile writes");

// A future implementation must durably record the exact generation before
// the POST. A true return is a claim by the injected journal, not proof that
// the PS5 filesystem supplied a safe journal implementation.
using Journal = bool (*)(void* context, const ConsoleTileIntent& intent,
                         const char nonce[33]);

struct Generation {
    ConsoleTileIntent intent{};
    char nonce[33]{};
    std::uint64_t create_counter = 0;
    pair::Mac create_request_mac{};
    bool journaled = false;
    bool may_have_sent = false;
};

struct PendingCreate {
    Generation generation{};
    std::uint64_t counter = 0;
    pair::Mac request_mac{};
    char body[256]{};
    std::size_t body_bytes = 0;
    char counter_header[21]{};
    char request_mac_header[65]{};
    bool ready = false;
    bool dispatched = false;

    PendingCreate() = default;
    PendingCreate(const PendingCreate&) = delete;
    PendingCreate& operator=(const PendingCreate&) = delete;
    PendingCreate(PendingCreate&&) = delete;
    PendingCreate& operator=(PendingCreate&&) = delete;
};

enum class Prepare { invalid_request, counter_unavailable, journal_unavailable,
                     ready };
enum class Outcome { invalid, uncertain, receipt_needs_status };
enum class Reconcile { unknown, pending, installed_reported, conflict,
                       held_unknown };

inline void clear(PendingCreate& pending) {
    pair::clear(&pending.generation, sizeof(pending.generation));
    pending.counter = 0;
    pair::clear(pending.request_mac.data(), pending.request_mac.size());
    pair::clear(pending.body, sizeof(pending.body));
    pending.body_bytes = 0;
    pair::clear(pending.counter_header, sizeof(pending.counter_header));
    pair::clear(pending.request_mac_header,
                sizeof(pending.request_mac_header));
    pending.ready = false;
    pending.dispatched = false;
}

// Reserve the session counter first, then call the injected durable journal.
// This may leave a harmless counter gap when journaling fails. It never
// exposes a request for transport until the journal callback succeeds and
// returns the exact unmodified route/nonce. The caller supplies a fresh,
// CSPRNG-generated lowercase hex nonce; this module validates its shape only.
inline Prepare prepare_create(pair::Session& session,
                              const ConsoleTileIntent& selected,
                              const Library* installed_routes,
                              std::string_view nonce, Journal journal,
                              void* journal_context,
                              PendingCreate& pending) {
    if (pending.ready) return Prepare::invalid_request;
    clear(pending);
    const ConsoleTileIntent intent = selected;
    char request_body[256]{};
    if (!api::mutable_title(intent.system) ||
        !tile_job::intent_valid(intent, tile_job::installed_emulator_route,
                                installed_routes) ||
        !api::nonce_valid(nonce) || !journal ||
        !api::encode_create(intent, nonce, request_body,
                            sizeof(request_body))) {
        pair::clear(request_body, sizeof(request_body));
        clear(pending);
        return Prepare::invalid_request;
    }
    const auto request_bytes = std::strlen(request_body);
    std::uint64_t reserved_counter = 0;
    pair::Mac request_mac{};
    if (!pair::prepare_request(session, api::create_route,
                               sizeof(api::create_route) - 1,
                               request_body, request_bytes,
                               reserved_counter, request_mac)) {
        pair::clear(request_body, sizeof(request_body));
        clear(pending);
        return Prepare::counter_unavailable;
    }
    char counter_header[21]{};
    char request_mac_header[65]{};
    const int printed = std::snprintf(
        counter_header, sizeof(counter_header), "%llu",
        static_cast<unsigned long long>(reserved_counter));
    if (printed <= 0 || static_cast<std::size_t>(printed) >=
                            sizeof(counter_header)) {
        pair::clear(request_body, sizeof(request_body));
        pair::clear(request_mac.data(), request_mac.size());
        clear(pending); // The reserved counter remains burned.
        return Prepare::counter_unavailable;
    }
    signed_status::encode_mac_hex(request_mac, request_mac_header);
    Generation generation{};
    generation.intent = intent;
    std::memcpy(generation.nonce, nonce.data(), nonce.size());
    generation.nonce[32] = 0;
    generation.create_counter = reserved_counter;
    generation.create_request_mac = request_mac;
    ConsoleTileIntent journal_intent = intent;
    char journal_nonce[33]{};
    std::memcpy(journal_nonce, generation.nonce, sizeof(journal_nonce));
    const bool journaled = journal(journal_context, journal_intent,
                                   journal_nonce) &&
        journal_intent.system == intent.system &&
        journal_intent.destination == intent.destination &&
        std::memcmp(journal_intent.emulator_title, intent.emulator_title,
                    sizeof(intent.emulator_title)) == 0 &&
        std::memcmp(journal_nonce, generation.nonce,
                    sizeof(journal_nonce)) == 0;
    pair::clear(journal_nonce, sizeof(journal_nonce));
    if (!journaled) {
        pair::clear(request_body, sizeof(request_body));
        pair::clear(request_mac.data(), request_mac.size());
        pair::clear(&generation, sizeof(generation));
        pair::clear(counter_header, sizeof(counter_header));
        pair::clear(request_mac_header, sizeof(request_mac_header));
        clear(pending);
        return Prepare::journal_unavailable;
    }
    generation.journaled = true;
    pending.generation = generation;
    pending.counter = reserved_counter;
    pending.request_mac = request_mac;
    std::memcpy(pending.body, request_body, request_bytes + 1);
    pending.body_bytes = request_bytes;
    std::memcpy(pending.counter_header, counter_header,
                sizeof(counter_header));
    std::memcpy(pending.request_mac_header, request_mac_header,
                sizeof(request_mac_header));
    pending.ready = true;
    pair::clear(request_body, sizeof(request_body));
    pair::clear(request_mac.data(), request_mac.size());
    pair::clear(&generation, sizeof(generation));
    pair::clear(counter_header, sizeof(counter_header));
    pair::clear(request_mac_header, sizeof(request_mac_header));
    return Prepare::ready;
}

// Call once immediately before any transport attempt. Even if the transport
// returns without a response, reconciliation must use the original generation.
inline bool begin_send(PendingCreate& pending) {
    if (!pending.ready || pending.dispatched) return false;
    pending.dispatched = true;
    return true;
}

// Every dispatched CREATE produces a generation for subsequent signed STATUS,
// whether the receipt is accepted, lost, unsigned, or malformed. A verified
// receipt is only needs_status; it never proves installation.
inline Outcome consume_create(const pair::Session& session,
                              PendingCreate& pending, int http_status,
                              const void* body, std::size_t body_bytes,
                              const char* response_mac_hex,
                              std::size_t response_mac_hex_bytes,
                              Generation& generation,
                              api::Operation& receipt) {
    generation = {};
    receipt = {};
    if (!pending.ready || !pending.dispatched) return Outcome::invalid;
    generation = pending.generation;
    generation.may_have_sent = true;
    const auto counter = pending.counter;
    pair::Mac request_mac = pending.request_mac;
    clear(pending);
    pair::Mac received_mac{};
    const bool authenticated =
        http_status >= 100 && http_status <= 599 && body &&
        body_bytes != 0 && body_bytes <= pair::max_response_bytes &&
        signed_status::decode_mac_hex(response_mac_hex,
                                      response_mac_hex_bytes,
                                      received_mac) &&
        pair::verify_response(session, counter, request_mac,
                              static_cast<unsigned>(http_status),
                              body, body_bytes, received_mac);
    pair::clear(received_mac.data(), received_mac.size());
    pair::clear(request_mac.data(), request_mac.size());
    if (!authenticated) return Outcome::uncertain;
    const auto parsed = api::parse_operation(
        generation.intent.system, generation.nonce, generation.nonce, false,
        http_status,
        std::string_view(static_cast<const char*>(body), body_bytes),
        receipt);
    if (parsed == api::ParseResult::needs_status)
        return Outcome::receipt_needs_status;
    receipt = {};
    return Outcome::uncertain;
}

inline bool prepare_reconcile_status(pair::Session& session,
                                     const Generation& generation,
                                     signed_status::PendingStatus& pending) {
    if (!generation.journaled || !generation.may_have_sent ||
        !session.active || generation.create_counter == 0 ||
        generation.create_counter >= session.next_counter ||
        !api::mutable_title(generation.intent.system) ||
        generation.nonce[32] != 0 ||
        !api::nonce_valid(std::string_view(generation.nonce, 32)) ||
        !console_tile_intent_valid(generation.intent)) return false;
    char original_body[256]{};
    if (!api::encode_create(generation.intent, generation.nonce,
                            original_body, sizeof(original_body))) return false;
    // Recompute the original CREATE MAC from the retained counter, without
    // reserving a new counter in the live session. A changed route, nonce, or
    // re-paired session cannot be mistaken for the user's CREATE generation.
    pair::Session replay = session;
    replay.next_counter = generation.create_counter;
    std::uint64_t check_counter = 0;
    pair::Mac check_mac{};
    const bool bound = pair::prepare_request(
        replay, api::create_route, sizeof(api::create_route) - 1,
        original_body, std::strlen(original_body), check_counter, check_mac) &&
        check_counter == generation.create_counter &&
        pair::equal_mac(check_mac, generation.create_request_mac);
    pair::clear(replay);
    pair::clear(check_mac.data(), check_mac.size());
    pair::clear(original_body, sizeof(original_body));
    return bound && signed_status::prepare_status(
               session, generation.intent.system, pending,
               std::string_view(generation.nonce, 32),
               &generation.intent);
}

// Keep verification and classification in one call. A separately supplied
// "valid" outcome must never be paired with an unrelated installed Status.
inline Reconcile consume_reconcile_status(
    const pair::Session& session, signed_status::PendingStatus& pending,
    int http_status, const void* body, std::size_t body_bytes,
    const char* response_mac_hex, std::size_t response_mac_hex_bytes,
    api::Status& status) {
    status = {};
    if (!pending.active || !pending.generation_specific) {
        signed_status::clear(pending);
        return Reconcile::unknown;
    }
    const ConsoleTileIntent expected_intent = pending.expected_intent;
    char expected_nonce[33]{};
    std::memcpy(expected_nonce, pending.expected_create_nonce,
                sizeof(expected_nonce));
    const auto outcome = signed_status::consume_status(
        session, pending, http_status, body, body_bytes,
        response_mac_hex, response_mac_hex_bytes, status);
    const bool exact_generation = status.has_intent &&
        console_tile_intents_equal(status.intent, expected_intent) &&
        std::memcmp(status.create_nonce, expected_nonce,
                    sizeof(expected_nonce)) == 0;
    pair::clear(expected_nonce, sizeof(expected_nonce));
    if (outcome == signed_status::Outcome::valid && !exact_generation) {
        status = {};
        return Reconcile::unknown;
    }
    if (outcome == signed_status::Outcome::conflict)
        return Reconcile::conflict;
    if (outcome == signed_status::Outcome::held_unknown)
        return Reconcile::held_unknown;
    if (outcome != signed_status::Outcome::valid)
        return Reconcile::unknown;
    if (status.stage == api::Stage::installed &&
        status.source_verified == api::Proof::yes &&
        status.mount_verified == api::Proof::yes &&
        status.registration_verified == api::Proof::yes)
        return Reconcile::installed_reported;
    if (status.stage == api::Stage::preparing ||
        status.stage == api::Stage::registering)
        return Reconcile::pending;
    return Reconcile::unknown;
}

} // namespace shelf::managed_tile_signed_create
