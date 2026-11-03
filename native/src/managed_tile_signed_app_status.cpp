// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/managed_tile_signed_app_status.hpp"

#include <cstring>

namespace shelf::managed_tile_signed_app_status {
namespace {
#if !defined(SHELF_PS5)

bool canonical_identity(const char (&value)[65]) {
    if (value[64] != 0) return false;
    bool nonzero = false;
    for (std::size_t i = 0; i < 64; ++i) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
        nonzero = nonzero || ch != '0';
    }
    return nonzero;
}

struct Guard {
    const Binding& binding;
    bool entered = false;
    explicit Guard(const Binding& value) : binding(value) {
        entered = binding.enter(binding.guard_context);
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    ~Guard() { if (entered) binding.leave(binding.guard_context); }
};

bool bound(const Binding& binding) {
    return binding.enabled && binding.service_identity_verified &&
           binding.capability_verified && binding.session_owned &&
           binding.service_epoch != 0 &&
           binding.service_epoch == binding.session_epoch &&
           canonical_identity(binding.service_sha256) &&
           canonical_identity(binding.session_service_sha256) &&
           std::memcmp(binding.service_sha256,
                       binding.session_service_sha256, 65) == 0 &&
           binding.session && binding.enter && binding.leave &&
           binding.transport.clock && binding.transport.port != 0;
}
#endif

} // namespace

Read read_status(const Binding& binding, std::size_t system) {
    Read result;
    if (system >= system_count) {
        result.state = State::invalid_request;
        return result;
    }
#if defined(SHELF_PS5)
    (void)binding;
    return result; // No PS5 STATUS socket, even with an injected binding.
#else
    if (!bound(binding)) return result;
    Guard guard(binding);
    if (!guard.entered || !binding.session->active ||
        binding.session->next_counter == 0) return result;
    managed_tile_signed_status::PendingStatus pending;
    if (!managed_tile_signed_status::prepare_status(*binding.session, system,
                                                    pending)) {
        result.state = State::unknown;
        return result;
    }
    result.counter = pending.counter;
    api::Status observed{};
    const auto outcome = http::exchange_status(binding.transport,
                                               *binding.session, pending,
                                               observed, &result.transport);
    if (outcome == managed_tile_signed_status::Outcome::valid) {
        result.state = State::observed;
        result.status = observed;
    } else if (outcome == managed_tile_signed_status::Outcome::conflict) {
        result.state = State::conflict;
        result.status = observed;
    } else {
        result.state = State::unknown;
    }
    return result;
#endif
}

} // namespace shelf::managed_tile_signed_app_status
