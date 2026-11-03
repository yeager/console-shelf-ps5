// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>

namespace shelf {
// The updater helper may relaunch only isShelf, after the exact owner-approved
// transaction has finished. Swordpdf's PS5SX2 relaunch work establishes the
// post-exit settle and running-title confirmation shape. isShelf additionally
// requires a continuously idle Home interval before its one launch request.
// Physical v0.1.1 recovery testing showed that five seconds raced the console's
// roughly 30-second ShadowMount refresh cadence and opened the persistent
// "can't open this app" page. Wait one full cadence plus a five-second margin.
//
// Physical PS5 evidence showed why the request must never be repeated: a
// negative sceLncUtilLaunchApp return still caused ShadowMount to prepare the
// isShelf mount asynchronously. Reissuing the call tore that mount down and
// started another until the helper exhausted its retries. One request followed
// by a long observation window lets that asynchronous launch finish normally.
inline constexpr unsigned update_relaunch_rounds = 1;
inline constexpr unsigned update_relaunch_attempts_per_round = 1;
inline constexpr unsigned update_relaunch_settle_ms = 35000;
inline constexpr unsigned update_relaunch_observe_ms = 30000;
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
    const auto before_request = [&](UpdateRelaunchState state) {
        if (state == UpdateRelaunchState::target)
            outcome.result = UpdateRelaunchResult::launched;
        else if (state == UpdateRelaunchState::other_app)
            outcome.result = UpdateRelaunchResult::foreground_changed;
        else if (state != UpdateRelaunchState::idle)
            outcome.result = UpdateRelaunchResult::service_unavailable;
        else
            return false;
        return true;
    };
    if (before_request(ops.state(ops.context)))
        return outcome;

    // Poll throughout the settle. A single end-point sample could miss another
    // app briefly taking the foreground while the helper was sleeping.
    for (unsigned elapsed = 0; elapsed < update_relaunch_settle_ms;) {
        const unsigned remaining = update_relaunch_settle_ms - elapsed;
        const unsigned step = remaining < update_relaunch_poll_ms
            ? remaining : update_relaunch_poll_ms;
        ops.wait(ops.context, step);
        elapsed += step;
        if (before_request(ops.state(ops.context)))
            return outcome;
    }

    outcome.last_code = ops.launch(ops.context);
    outcome.attempts = 1;

    // Observe even a negative immediate return. ShadowMount may already be
    // completing the requested mount and launch asynchronously. Unknown state
    // samples are harmless here because no second request can be issued.
    for (unsigned elapsed = 0;;) {
        const auto state = ops.state(ops.context);
        if (state == UpdateRelaunchState::target) {
            outcome.result = UpdateRelaunchResult::launched;
            return outcome;
        }
        if (state == UpdateRelaunchState::other_app) {
            outcome.result = UpdateRelaunchResult::foreground_changed;
            return outcome;
        }
        if (elapsed >= update_relaunch_observe_ms)
            break;
        const unsigned remaining = update_relaunch_observe_ms - elapsed;
        const unsigned step = remaining < update_relaunch_poll_ms
            ? remaining : update_relaunch_poll_ms;
        ops.wait(ops.context, step);
        elapsed += step;
    }
    outcome.result = update_relaunch_accepted(outcome.last_code)
        ? UpdateRelaunchResult::not_observed
        : UpdateRelaunchResult::refused;
    return outcome;
}
} // namespace shelf
