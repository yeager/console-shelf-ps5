// SPDX-License-Identifier: GPL-3.0-only
// Unwired, STATUS-only bridge from an explicitly bound app session to the
// authenticated fixed-loopback transport. PS5 requests stay compiled out.
#pragma once

#include "managed_tile_signed_http.hpp"

#include <cstddef>
#include <cstdint>

namespace shelf::managed_tile_signed_app_status {

namespace api = managed_tile_api_v1;
namespace http = managed_tile_signed_http;
namespace pair = managed_tile_pairing_client;

using Enter = bool (*)(void* context);
using Leave = void (*)(void* context);

struct Binding {
    bool enabled = false;
    bool service_identity_verified = false;
    bool capability_verified = false;
    bool session_owned = false;
    // These are caller-attested, nonsecret service identities. A future
    // integrator must derive both from the same running service instance.
    char service_sha256[65]{};
    char session_service_sha256[65]{};
    std::uint64_t service_epoch = 0;
    std::uint64_t session_epoch = 0;
    pair::Session* session = nullptr;
    Enter enter = nullptr;
    Leave leave = nullptr;
    void* guard_context = nullptr;
    http::Options transport{};
};

enum class State { unbound, invalid_request, unknown, observed, conflict };

struct Read {
    State state = State::unbound;
    api::Status status{};
    http::Transport transport = http::Transport::invalid;
    std::uint64_t counter = 0;
};

// Reserves a session counter under the caller's serialization guard, sends
// exactly one signed STATUS request, and accepts only a verified signed
// response. No CREATE route or unsigned managed_tile_app_client callback is
// reachable from this interface. No production caller is bound.
Read read_status(const Binding& binding, std::size_t system);

} // namespace shelf::managed_tile_signed_app_status
