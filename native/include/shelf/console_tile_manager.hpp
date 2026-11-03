// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "console_tile_intent.hpp"
#include "console_tile_write_gate.hpp"
#include <array>
#include <cstddef>

namespace shelf {

enum class ConsoleTileStage {
    absent,
    registered,
    pending,
    deferred,
    removed_pending,
    conflict,
    unavailable,
    failed,
};

struct ConsoleTileOutcome {
    ConsoleTileStage stage = ConsoleTileStage::unavailable;
    bool owned = false;
    // The owned source is isolated, but ShadowMount must process the exact
    // title after isShelf has cooperatively closed.
    bool handoff_required = false;
    ConsoleTileIntent intent;
    char title_id[10]{};
    char message[192]{};
    // Bounded ownership/allocator evidence for startup.log; never shown as the UI notice.
    char diagnostic[1536]{};
};

// Result of the explicit in-app maintenance action. The operation first
// validates every owned source, refreshes installed shortcuts while preserving
// each shortcut's existing destination, identifies only stale PS5 Home
// artwork, then either completes immediately or prepares a bounded post-close
// registration handoff. A mid-run failure is safe to retry; `refreshed`
// records the durable source prefix and `registration_refreshes` records the
// exact number of stale registrations accepted or prepared for that handoff.
struct ConsoleTileBatchOutcome {
    bool complete = false;
    // ShadowMount intentionally refuses title uninstalls while isShelf is the
    // active big app. In that case the UI hands this bounded owned-title list
    // to the existing cooperative post-close helper.
    bool handoff_required = false;
    std::size_t installed = 0;
    std::size_t refreshed = 0;
    std::size_t registration_refreshes = 0;
    std::size_t removing = 0;
    std::size_t failed_system = static_cast<std::size_t>(-1);
    ConsoleTileStage failed_stage = ConsoleTileStage::absent;
    char handoff_titles[521]{};
    char message[192]{};
};

// Result of the explicit "add or repair emulator tiles" action. The caller
// supplies only emulator-menu intents that were derived from the completed
// installed-library scan. The manager validates the whole selected set before
// its first write, then records the exact durable prefix so a transient
// ShadowMount failure can be retried without guessing what changed.
struct ConsoleTileEmulatorBatchOutcome {
    bool complete = false;
    bool close_required = false;
    std::size_t selected = 0;
    std::size_t committed = 0;
    std::size_t created = 0;
    std::size_t updated = 0;
    std::size_t failed_system = static_cast<std::size_t>(-1);
    ConsoleTileStage failed_stage = ConsoleTileStage::absent;
    std::array<bool, system_count> committed_systems{};
    std::array<ConsoleTileIntent, system_count> intents{};
    char message[192]{};
};

struct ConsoleTileRemovalBatchOutcome {
    bool complete = false;
    bool handoff_required = false;
    // The app sandbox cannot see global residual registrations after their
    // source/recovery folders are gone. A post-close helper must validate all
    // fixed preferred slots before sending any uninstall request.
    bool residual_sweep = false;
    std::size_t found = 0;
    std::size_t queued = 0;
    std::size_t already_queued = 0;
    std::size_t failed_system = static_cast<std::size_t>(-1);
    ConsoleTileStage failed_stage = ConsoleTileStage::absent;
    bool queued_systems[system_count]{};
    char handoff_titles[521]{};
    char message[192]{};
};

enum class ConsoleTileAvailabilityReason {
    ready,
    hardware_qualification,
    invalid_config,
    bundle_asset_missing_or_unsafe,
    bundle_asset_integrity,
    api_transport,
    api_http_status,
    api_malformed_response,
    api_status,
    api_version,
    api_capabilities,
    api_missing_capability,
};

struct ConsoleTileAvailability {
    ConsoleTileAvailabilityReason reason = ConsoleTileAvailabilityReason::invalid_config;
    int http_status = 0;
    // Relative bundle asset or missing capability; never an arbitrary server string.
    char detail[64]{};
    // The verified bundle source, truncated only for diagnostics.
    char bundle_source[64]{};
    char message[192]{};
    bool ready() const { return reason == ConsoleTileAvailabilityReason::ready; }
};

// Returns HTTP status, or -1 for a transport error. `response` receives only
// the JSON response body and is NUL terminated. Tests may supply a local fake.
using ConsoleTilePost = int (*)(void* context, const char* route, const char* request,
                                char* response, std::size_t capacity);

struct ConsoleTileConfig {
    const char* homebrew_root = "/data/homebrew";
    const char* bundle_root = "/app0/assets/console-tiles";
    // The signed app's direct installed assets when /app0 is a virtual view.
    const char* bundle_fallback_root = "/data/homebrew/PPSA99541/assets/console-tiles";
    const char* user_app_root = "/user/app";
    const char* user_appmeta_root = "/user/appmeta";
    const char* retired_root = "/data/console-shelf/console-tile-retired";
    unsigned short api_port = 10101;
    ConsoleTilePost post = nullptr;
    void* post_context = nullptr;
#ifndef SHELF_PS5
    // Synthetic host fixtures only; never compiled into a PS5 build.
    bool test_skip_integrity = false;
#endif
};

// Owns only folders carrying isShelf's exact ownership marker. Operations are
// synchronous; run them off the render thread. `pending` and `deferred` mean a
// scan was accepted or a source exists, not that a home-screen tile is visible.
class ConsoleTileManager {
    ConsoleTileConfig config_;

  public:
    explicit ConsoleTileManager(ConsoleTileConfig config = {}) : config_(config) {}
    // Reports the first failed startup gate without writing to the console.
    ConsoleTileAvailability availability() const;
    // Read-only ShadowMount status for the fixed 52-title catalog. This does
    // not prove source ownership, title absence, or permission to create.
    ConsoleTileAvailability catalog_read_availability() const;
    ConsoleTileOutcome inspect_catalog_title(std::size_t system) const;
    bool available() const;
    ConsoleTileOutcome inspect(std::size_t system) const;
    ConsoleTileOutcome upsert(const ConsoleTileIntent& intent) const;
    // Adds missing emulator shortcuts and changes existing selected shortcuts
    // to their verified emulator menus. Non-selected systems are untouched.
    ConsoleTileEmulatorBatchOutcome upsert_emulators(
        const std::array<ConsoleTileIntent, system_count>& intents,
        const std::array<bool, system_count>& selected) const;
    // Refreshes only existing isShelf-owned shortcuts. It never creates a
    // missing shortcut and never changes a saved launch destination.
    ConsoleTileBatchOutcome refresh_installed() const;
    // Isolates every exact isShelf-owned shortcut source without requesting
    // foreground uninstalls. If the app-visible sources are all gone, PS5
    // builds prepare a fixed preferred-slot sweep whose global registration
    // validation happens only in the post-close helper. The returned bounded
    // list is processed once by ShadowMount only after isShelf closes.
    ConsoleTileRemovalBatchOutcome remove_installed() const;
    ConsoleTileOutcome remove(std::size_t system) const;
    ConsoleTileOutcome reconcile(std::size_t system) const;
    // Call only after the owner has confirmed physical home-screen removal.
    ConsoleTileOutcome confirm_removed(std::size_t system) const;
};

bool console_tile_title_id(unsigned number, char (&output)[10]);
unsigned console_tile_preferred_number(std::size_t system);

} // namespace shelf
