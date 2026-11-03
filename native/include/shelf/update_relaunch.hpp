// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>

namespace shelf {
// The updater helper may relaunch only isShelf, after the exact owner-approved
// transaction has finished.  This bounded retry/confirmation shape follows
// Swordpdf's PS5SX2 relaunch work, adapted to isShelf's stricter handoff.
inline constexpr unsigned update_relaunch_rounds = 2;
inline constexpr unsigned update_relaunch_attempts_per_round = 5;
// ShadowMount can still classify an immediate same-title start as the closing
// app's handoff. PS5SX2's console-tested relaunch path therefore enforces a
// minimum post-exit interval before its first LaunchApp call. Shelf already
// observes a full second of stable Home before committing the update; this
// additional settle keeps the first launch beyond that handoff window without
// making a speculative launch attempt.
inline constexpr unsigned update_relaunch_settle_ms = 1600;
inline constexpr unsigned update_relaunch_retry_ms = 800;
inline constexpr unsigned update_relaunch_observe_ms = 8000;
inline constexpr unsigned update_relaunch_poll_ms = 100;
inline constexpr std::uint32_t update_relaunch_already_running = 0x8094000cu;

enum class UpdateRelaunchState {
    idle = 0,
    target = 1,
    other_app = 2,
    unknown = -1,
};

enum class UpdateRelaunchResult {
    launched,
    foreground_changed,
    service_unavailable,
    refused,
    not_observed,
    invalid,
};

struct UpdateRelaunchOps {
    void* context = nullptr;
    UpdateRelaunchState (*state)(void*) = nullptr;
    int (*launch)(void*) = nullptr;
    void (*wait)(void*, unsigned) = nullptr;
};

struct UpdateRelaunchOutcome {
    UpdateRelaunchResult result = UpdateRelaunchResult::invalid;
    int last_code = 0;
    unsigned attempts = 0;
};

inline bool update_relaunch_accepted(int code) {
    return code >= 0 || static_cast<std::uint32_t>(code) == update_relaunch_already_running;
}

inline UpdateRelaunchOutcome relaunch_updated_shelf(const UpdateRelaunchOps& ops) {
    UpdateRelaunchOutcome outcome;
    if (!ops.state || !ops.launch || !ops.wait)
        return outcome;
    auto state = ops.state(ops.context);
    if (state == UpdateRelaunchState::target) {
        outcome.result = UpdateRelaunchResult::launched;
        return outcome;
    }
    if (state == UpdateRelaunchState::other_app) {
        outcome.result = UpdateRelaunchResult::foreground_changed;
        return outcome;
    }
    if (state != UpdateRelaunchState::idle) {
        outcome.result = UpdateRelaunchResult::service_unavailable;
        return outcome;
    }
    ops.wait(ops.context, update_relaunch_settle_ms);
    state = ops.state(ops.context);
    if (state == UpdateRelaunchState::target) {
        outcome.result = UpdateRelaunchResult::launched;
        return outcome;
    }
    if (state == UpdateRelaunchState::other_app) {
        outcome.result = UpdateRelaunchResult::foreground_changed;
        return outcome;
    }
    if (state != UpdateRelaunchState::idle) {
        outcome.result = UpdateRelaunchResult::service_unavailable;
        return outcome;
    }
    bool accepted_once = false;
    for (unsigned round = 0; round < update_relaunch_rounds; ++round) {
        bool accepted = false;
        for (unsigned attempt = 0; attempt < update_relaunch_attempts_per_round; ++attempt) {
            const auto state = ops.state(ops.context);
            if (state == UpdateRelaunchState::target) {
                outcome.result = UpdateRelaunchResult::launched;
                return outcome;
            }
            if (state == UpdateRelaunchState::other_app) {
                outcome.result = UpdateRelaunchResult::foreground_changed;
                return outcome;
            }
            if (state != UpdateRelaunchState::idle) {
                outcome.result = UpdateRelaunchResult::service_unavailable;
                return outcome;
            }
            outcome.last_code = ops.launch(ops.context);
            ++outcome.attempts;
            if (update_relaunch_accepted(outcome.last_code)) {
                accepted = accepted_once = true;
                break;
            }
            const bool final_attempt = round + 1 == update_relaunch_rounds &&
                                       attempt + 1 == update_relaunch_attempts_per_round;
            if (!final_attempt)
                ops.wait(ops.context, update_relaunch_retry_ms);
        }
        if (!accepted)
            continue;
        for (unsigned elapsed = 0; elapsed < update_relaunch_observe_ms;
             elapsed += update_relaunch_poll_ms) {
            const auto state = ops.state(ops.context);
            if (state == UpdateRelaunchState::target) {
                outcome.result = UpdateRelaunchResult::launched;
                return outcome;
            }
            if (state == UpdateRelaunchState::other_app) {
                outcome.result = UpdateRelaunchResult::foreground_changed;
                return outcome;
            }
            ops.wait(ops.context, update_relaunch_poll_ms);
        }
    }
    outcome.result = accepted_once ? UpdateRelaunchResult::not_observed
                                   : UpdateRelaunchResult::refused;
    return outcome;
}
} // namespace shelf
