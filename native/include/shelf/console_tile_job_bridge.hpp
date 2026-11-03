// SPDX-License-Identifier: GPL-3.0-only
// Install-only app bridge. A PS5 build keeps this behind the disabled write
// gate until a reviewed generic helper asset is bundled.
#pragma once

#include "console_tile_job_dispatch.hpp"
#include "console_tile_helper_pin.hpp"
#include "console_tile_manager.hpp"
#include "pinned_helper_sha256.hpp"
#include <cstdio>

namespace shelf::tile_job {

// The caller can supply bytes, but cannot supply their reviewed identity.
// Only the exact byte count and SHA-256 compiled into the build admit them.
struct PinnedHelper {
    const unsigned char* data = nullptr;
    std::size_t bytes = 0;
    bool available() const {
        return detail::exact_helper_sha256(data, bytes,
                                           detail::expected_generic_helper_bytes,
                                           detail::expected_generic_helper_sha256);
    }
};

template <class LegacyProbe>
ConsoleTileAvailability install_availability(bool writes_enabled, PinnedHelper helper,
                                             LegacyProbe legacy_probe) {
    // The disabled PS5 gate retains the exact existing picker preview.
    if (!writes_enabled) return legacy_probe();

    ConsoleTileAvailability result;
    if (!helper.available()) {
        result.reason = ConsoleTileAvailabilityReason::bundle_asset_missing_or_unsafe;
        std::snprintf(result.message, sizeof(result.message),
                      "Reviewed shortcut helper is unavailable in this build.");
        return result;
    }
    result.reason = ConsoleTileAvailabilityReason::ready;
    std::snprintf(result.message, sizeof(result.message),
                  "One-shot shortcut sender is available; each tile still needs verification.");
    return result;
}

template <class LegacyUpsert, class SendOnce>
ConsoleTileOutcome run_install(bool writes_enabled, const ConsoleTileIntent& intent,
                               EmulatorRouteCheck installed_route, const void* route_context,
                               PinnedHelper helper,
                               LegacyUpsert legacy_upsert, SendOnce send_once) {
    // Preserve the existing preview path while the PS5 write gate is closed.
    if (!writes_enabled) return legacy_upsert(intent);

    ConsoleTileOutcome outcome;
    outcome.intent = intent;
    outcome.stage = ConsoleTileStage::unavailable;
    if (!catalog_pin_matches(intent.system) ||
        !intent_valid(intent, installed_route, route_context)) {
        outcome.stage = ConsoleTileStage::conflict;
        std::snprintf(outcome.message, sizeof(outcome.message),
                      "Shortcut route does not match the pinned console catalog.");
        return outcome;
    }
    if (!helper.available()) {
        std::snprintf(outcome.message, sizeof(outcome.message),
                      "Reviewed shortcut helper is unavailable in this build.");
        return outcome;
    }

    Dispatch job;
    const auto phase = send_once(job, Action::upsert, intent, helper.data, helper.bytes);
    if (phase == Phase::sent_unverified) {
        outcome.stage = ConsoleTileStage::pending;
        std::snprintf(outcome.message, sizeof(outcome.message),
                      "Shortcut request sent; waiting for a verified result.");
    } else {
        outcome.stage = ConsoleTileStage::deferred;
        std::snprintf(outcome.message, sizeof(outcome.message),
                      "Shortcut request held; no tile was confirmed.");
    }
    // A sent request is not evidence of a source, registration, or TV tile.
    outcome.owned = false;
    return outcome;
}

} // namespace shelf::tile_job
