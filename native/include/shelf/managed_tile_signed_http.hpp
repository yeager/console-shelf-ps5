// SPDX-License-Identifier: GPL-3.0-only
// Unwired fixed-loopback transport for the signed managed-tile candidates.
// It never owns a pairing secret, journal, or production capability.
#pragma once

#include "managed_tile_signed_create.hpp"
#include "managed_tile_signed_status.hpp"

#include <cstddef>
#include <cstdint>

namespace shelf::managed_tile_signed_http {

using Clock = bool (*)(void* context, std::uint64_t& monotonic_ms);
using Cancel = bool (*)(void* context);

struct Options {
    unsigned short port = 10101;
    unsigned timeout_ms = 3000;
    Clock clock = nullptr;
    Cancel cancel = nullptr;
    void* context = nullptr;
};

enum class Transport { invalid, cancelled, timed_out, io_unknown,
                       protocol_unknown, complete };

struct Reply {
    int http_status = -1;
    char body[managed_tile_pairing_client::max_response_bytes]{};
    std::size_t body_bytes = 0;
    char response_mac_hex[65]{};
    bool complete = false;
};

// Fixed POST paths only. Requires a caller-supplied, independently qualified
// monotonic clock; this module cannot attest the service or the pairing session.
// A complete result is still untrusted until a signed consume_* call succeeds.
Transport post(const Options& options, const char* route, const char* body,
               std::size_t body_bytes, const char* counter_header,
               const char* request_mac_hex, Reply& reply);

// These consume their pending object once a network attempt starts. They do
// not report installation on their own; a dispatched CREATE always returns a
// generation that needs a later exact signed STATUS, including on a lost
// response. CREATE networking is compiled out on PS5 and outside host fixture
// builds while the production write gate remains closed.
managed_tile_signed_status::Outcome exchange_status(
    const Options& options, const managed_tile_pairing_client::Session& session,
    managed_tile_signed_status::PendingStatus& pending,
    managed_tile_api_v1::Status& status,
    Transport* transport = nullptr);

managed_tile_signed_create::Outcome exchange_create(
    const Options& options, const managed_tile_pairing_client::Session& session,
    managed_tile_signed_create::PendingCreate& pending,
    managed_tile_signed_create::Generation& generation,
    managed_tile_api_v1::Operation& receipt,
    Transport* transport = nullptr);

// Only this generation-specific path may classify a later signed STATUS as
// installed_reported. A CREATE receipt alone never does so.
managed_tile_signed_create::Reconcile exchange_reconcile_status(
    const Options& options, const managed_tile_pairing_client::Session& session,
    managed_tile_signed_status::PendingStatus& pending,
    managed_tile_api_v1::Status& status,
    Transport* transport = nullptr);

} // namespace shelf::managed_tile_signed_http
