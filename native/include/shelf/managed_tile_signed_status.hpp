// SPDX-License-Identifier: GPL-3.0-only
// Offline-only authenticated STATUS boundary for the managed tile candidate.
// No production source includes this header; it owns no socket or credential.
#pragma once

#include "managed_tile_api_v1.hpp"
#include "managed_tile_pairing_client.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace shelf::managed_tile_signed_status {

namespace api = managed_tile_api_v1;
namespace pair = managed_tile_pairing_client;

enum class Outcome { unknown, valid, conflict, held_unknown };

// One pending read owns one burned counter. It cannot be copied into another
// pending read. A failed transport/response consumes this object; the caller
// may prepare a fresh STATUS with the next counter.
struct PendingStatus {
    std::size_t system = system_count;
    std::uint64_t counter = 0;
    pair::Mac request_mac{};
    char body[128]{};
    std::size_t body_bytes = 0;
    char counter_header[21]{};
    char request_mac_header[65]{};
    char expected_create_nonce[33]{};
    ConsoleTileIntent expected_intent{};
    bool generation_specific = false;
    bool active = false;

    PendingStatus() = default;
    PendingStatus(const PendingStatus&) = delete;
    PendingStatus& operator=(const PendingStatus&) = delete;
    PendingStatus(PendingStatus&&) = delete;
    PendingStatus& operator=(PendingStatus&&) = delete;
};

inline void clear(PendingStatus& pending) {
    pending.system = system_count;
    pending.counter = 0;
    pair::clear(pending.request_mac.data(), pending.request_mac.size());
    std::memset(pending.body, 0, sizeof(pending.body));
    pending.body_bytes = 0;
    std::memset(pending.counter_header, 0, sizeof(pending.counter_header));
    std::memset(pending.request_mac_header, 0, sizeof(pending.request_mac_header));
    std::memset(pending.expected_create_nonce, 0, sizeof(pending.expected_create_nonce));
    pending.expected_intent = {};
    pending.generation_specific = false;
    pending.active = false;
}

inline void encode_mac_hex(const pair::Mac& mac, char (&output)[65]) {
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < mac.size(); ++i) {
        output[2 * i] = digits[mac[i] >> 4];
        output[2 * i + 1] = digits[mac[i] & 15];
    }
    output[64] = 0;
}

inline bool decode_mac_hex(const char* input, std::size_t bytes, pair::Mac& output) {
    output.fill(0);
    if (!input || bytes != 64) return false;
    for (std::size_t i = 0; i < output.size(); ++i) {
        unsigned digits[2]{};
        for (unsigned part = 0; part < 2; ++part) {
            const unsigned char ch = static_cast<unsigned char>(input[2 * i + part]);
            if (ch >= '0' && ch <= '9') digits[part] = ch - '0';
            else if (ch >= 'a' && ch <= 'f') digits[part] = ch - 'a' + 10u;
            else {
                output.fill(0);
                return false;
            }
        }
        output[i] = static_cast<unsigned char>((digits[0] << 4) | digits[1]);
    }
    return true;
}

// The returned body and header strings are exact, fixed-route transport
// inputs. The counter is reserved before a future caller may attempt I/O.
inline bool prepare_status(pair::Session& session, std::size_t system,
                           PendingStatus& pending,
                           std::string_view expected_create_nonce = {},
                           const ConsoleTileIntent* expected_intent = nullptr) {
    if (pending.active) return false;
    clear(pending);
    const bool generation_specific = !expected_create_nonce.empty();
    if (system >= system_count ||
        generation_specific != (expected_intent != nullptr) ||
        (generation_specific &&
         (!api::nonce_valid(expected_create_nonce) ||
          expected_intent->system != system ||
          !console_tile_intent_valid(*expected_intent))) ||
        !api::encode_status(system, pending.body, sizeof(pending.body))) {
        clear(pending);
        return false;
    }
    pending.body_bytes = std::strlen(pending.body);
    if (!pair::prepare_request(session, api::status_route,
                               sizeof(api::status_route) - 1,
                               pending.body, pending.body_bytes,
                               pending.counter, pending.request_mac)) {
        clear(pending);
        return false;
    }
    const int printed = std::snprintf(
        pending.counter_header, sizeof(pending.counter_header), "%llu",
        static_cast<unsigned long long>(pending.counter));
    if (printed <= 0 || static_cast<std::size_t>(printed) >=
                            sizeof(pending.counter_header)) {
        clear(pending); // The reserved counter still stays burned.
        return false;
    }
    encode_mac_hex(pending.request_mac, pending.request_mac_header);
    if (generation_specific) {
        std::memcpy(pending.expected_create_nonce,
                    expected_create_nonce.data(), expected_create_nonce.size());
        pending.expected_create_nonce[32] = 0;
        pending.expected_intent = *expected_intent;
    }
    pending.system = system;
    pending.generation_specific = generation_specific;
    pending.active = true;
    return true;
}

// Complete one pending read. A caller supplies the exact HTTP status, raw
// body bytes, and the value of X-IsShelf-Response-MAC. No response, unsigned
// response, bad MAC, non-200 status, or malformed signed JSON is unknown.
// Only a verified HTTP 200 body reaches parse_status.
inline Outcome consume_status(const pair::Session& session,
                              PendingStatus& pending, int http_status,
                              const void* body, std::size_t body_bytes,
                              const char* response_mac_hex,
                              std::size_t response_mac_hex_bytes,
                              api::Status& result) {
    result = {};
    if (!pending.active) return Outcome::unknown;
    const auto system = pending.system;
    const auto counter = pending.counter;
    pair::Mac request_mac = pending.request_mac;
    const bool generation_specific = pending.generation_specific;
    char expected_nonce[33]{};
    if (generation_specific)
        std::memcpy(expected_nonce, pending.expected_create_nonce, sizeof(expected_nonce));
    const ConsoleTileIntent expected_intent = pending.expected_intent;
    clear(pending); // A failed response cannot be replayed against this read.
    pair::Mac received_mac{};
    const bool authenticated =
        http_status == 200 && body && body_bytes != 0 &&
        body_bytes <= pair::max_response_bytes &&
        decode_mac_hex(response_mac_hex, response_mac_hex_bytes, received_mac) &&
        pair::verify_response(session, counter, request_mac, 200,
                              body, body_bytes, received_mac);
    pair::clear(received_mac.data(), received_mac.size());
    pair::clear(request_mac.data(), request_mac.size());
    if (!authenticated) {
        std::memset(expected_nonce, 0, sizeof(expected_nonce));
        return Outcome::unknown;
    }
    const auto parsed = api::parse_status(
        system,
        std::string_view(static_cast<const char*>(body), body_bytes),
        result,
        generation_specific ? std::string_view(expected_nonce, 32)
                            : std::string_view(),
        generation_specific ? &expected_intent : nullptr);
    std::memset(expected_nonce, 0, sizeof(expected_nonce));
    if (parsed == api::ParseResult::valid) return Outcome::valid;
    if (parsed == api::ParseResult::conflict) return Outcome::conflict;
    if (parsed == api::ParseResult::held_unknown) return Outcome::held_unknown;
    result = {};
    return Outcome::unknown;
}

} // namespace shelf::managed_tile_signed_status
