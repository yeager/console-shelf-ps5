// SPDX-License-Identifier: GPL-3.0-only
// App-facing boundary for the proposed managed tile service. Its transport,
// credential and create journal are deliberately unbound in the PS5 app.
#pragma once

#include "managed_tile_api_v1.hpp"
#include "console_tile_job_routes.hpp"
#include <cstddef>

namespace shelf::managed_tile_app_client {

// Enabling network requests needs a reviewed service identity, credential
// custody and (for CREATE) durable journal. This remains false in the app.
inline constexpr bool requests_enabled = false;

using Send = int (*)(void* context, const char* fixed_route,
                     const char* request, char* response,
                     std::size_t capacity, std::size_t& response_bytes);
using Nonce = bool (*)(void* context, char* output, std::size_t capacity);
using Journal = bool (*)(void* context, const ConsoleTileIntent& intent,
                         const char* nonce);

struct Binding {
    void* context = nullptr;
    // Send must be a separately qualified, authenticated fixed-loopback
    // transport. A bare ShadowMount HTTP callback is insufficient.
    Send send = nullptr;
    Nonce nonce = nullptr;
    Journal journal = nullptr;
    bool service_identity_verified = false;
    bool credential_bound = false;
    bool capability_verified = false;
};

enum class State {
    credential_unavailable,
    invalid_request,
    nonce_unavailable,
    journal_unavailable,
    transport_unknown,
    reply_unknown,
    observed,
    receipt_needs_status,
    pending,
    installed_reported,
    conflict,
};

struct StatusRead {
    State state = State::credential_unavailable;
    managed_tile_api_v1::Status status{};
};

struct CreateAttempt {
    State state = State::credential_unavailable;
    ConsoleTileIntent intent{};
    char nonce[33]{};
    bool journaled = false;
    bool may_have_sent = false;
    managed_tile_api_v1::Operation receipt{};
};

class Client {
public:
    explicit Client(bool enabled = requests_enabled, Binding binding = {})
        : enabled_(enabled), binding_(binding) {}

    bool admitted() const { return ready(); }

    // Status is valid for every fixed catalog title, including four pilots.
    // Unbound credentials cause no network call.
    StatusRead status(std::size_t system) const;

    // CREATE is valid only for the 48 new titles. The nonce and intent must
    // be durably journaled before the sole POST. A POST never reports install.
    CreateAttempt create(const ConsoleTileIntent& intent,
                         const Library* library) const;

    // A distinct status request must match the original nonce and route.
    // Installed here is a service report, not a PS5 Home or TV observation.
    StatusRead reconcile(const CreateAttempt& attempt) const;

private:
    bool ready() const;
    StatusRead read(std::size_t system, const char* nonce,
                    const ConsoleTileIntent* intent) const;
    bool enabled_;
    Binding binding_;
};

} // namespace shelf::managed_tile_app_client
