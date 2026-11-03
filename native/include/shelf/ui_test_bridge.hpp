// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "ui.hpp"
#include "console_tile_route_path_probe.hpp"
#include "console_tile_settings_path_probe.hpp"
#include <cstddef>
#include <cstdint>

namespace shelf {
namespace tile_job { struct JobRootProbeResult; }
// UI test timers are always shorter than half the 32-bit clock cycle. Within
// that bound, unsigned subtraction handles wrap, while a future start or stale
// loop sample is rejected instead of appearing to have elapsed for 49 days.
inline constexpr bool ui_test_elapsed(std::uint32_t now, std::uint32_t start, std::uint32_t interval) {
    const auto elapsed = static_cast<std::uint32_t>(now - start);
    return interval < 0x80000000u && elapsed < 0x80000000u && elapsed >= interval;
}
enum class UiTestOperation {
    snapshot, action, launch_confirm, stop, close, probe_paths, probe_writes_v4,
    probe_job_root, create_job_root, probe_app_source, create_n64_source,
    probe_ps2_source, probe_ps2_direct, probe_route_store, probe_route_path,
    probe_settings_path
};
struct UiTestRequest {
    std::uint64_t sequence = 0, revision = 0;
    UiTestOperation operation = UiTestOperation::snapshot;
    Action action = Action::up;
    char name[20]{};
};
// The synthetic sequence-zero receipt only makes the command channel ready.
// It must not synchronously encode a full-size PNG while L3 + R3 is being
// enabled. Explicit snapshots and ordinary command receipts retain their
// bound image; probe operations that already publish structured evidence do
// not need one.
inline constexpr bool ui_test_discrete_frame_required(const UiTestRequest& request) {
    if (request.sequence == 0)
        return false;
    return request.operation != UiTestOperation::probe_paths &&
           request.operation != UiTestOperation::probe_writes_v4 &&
           request.operation != UiTestOperation::probe_ps2_source &&
           request.operation != UiTestOperation::probe_ps2_direct &&
           request.operation != UiTestOperation::probe_route_store &&
           request.operation != UiTestOperation::probe_route_path &&
           request.operation != UiTestOperation::probe_settings_path;
}
// Portable read-only report. The PS5 adapter copies a bounded POSIX result;
// the transport does not need a source-path argument or a POSIX header.
struct UiTestPs2SourceNode {
    std::uint64_t device = 0, inode = 0, owner = 0, group = 0, mode = 0;
    std::int64_t size = 0;
};
struct UiTestPs2SourceReport {
    bool complete = false;
    const char* failure = "none";
    std::uint64_t uid = 0, euid = 0;
    std::size_t observed_nodes = 0, failed_node = 8;
    unsigned failed_pass = 0, files_read = 0;
    int system_error = 0;
    UiTestPs2SourceNode nodes[8]{};
};
struct UiTestPs2DirectReport {
    bool complete = false, target_opened = false, content_matched = false, mount_distinct = false;
    const char* failure = "none";
    int system_error = 0;
    std::uint64_t uid = 0, euid = 0, root_device = 0, system_device = 0;
    std::uint64_t target_device = 0, target_inode = 0, target_owner = 0, target_mode = 0;
    std::int64_t target_size = 0;
    std::size_t bytes_read = 0;
};
struct UiTestRouteStoreReport {
    const char* root = "unsafe";
    std::uint64_t uid = 0, euid = 0;
    int system_error = 0;
};
// Trial the same UI transition used by physical input, and refuse any action
// that would leave the app. Refused actions do not mutate the live state.
bool apply_ui_test_action(UiState& state, Action action);
// Separate one-shot path for a currently selected emulator menu or an exact
// queued-removal handoff. Ordinary remote Confirm remains unable to launch or
// leave isShelf.
bool apply_ui_test_launch_confirm(UiState& state);

// Private UI testing transport. The caller owns opt-in, expiry and dispatch;
// this class only exchanges bounded records in the app's own data directory.
// The session prevents stale replay and does not authenticate an FTP client.
class UiTestBridge {
    bool enabled_ = false;
    bool private_diagnostics_ = false;
    std::uint64_t next_sequence_ = 1;
    char session_[33]{};
    char directory_[2304]{}, request_path_[2304]{}, frame_new_path_[2304]{};

  public:
    // Public owner support accepts only bounded menu control, snapshots and
    // cooperative close. Development probes and emulator launch confirmation
    // require an explicitly private session.
    bool start(const char* data_root, const char* session32, bool private_diagnostics = false);
    void stop();
    bool active() const { return enabled_; }
    const char* session() const { return session_; }
    // Accepts exactly the next sequence and consumes it before returning.
    bool poll(UiTestRequest& request);
    const char* frame_path() const { return frame_new_path_; }
    // True means the response was published. frameAvailable in that response
    // separately records whether a complete matching PNG was published. A
    // stopped initialized session can publish its final inactive response.
    bool publish(const UiTestRequest& request, std::uint64_t revision, const char* result,
                 bool applied, bool saved, const UiState& state, bool captured,
                 std::size_t probe_bytes = 0, std::uint64_t probe_fnv = 0,
                 const tile_job::JobRootProbeResult* job_root = nullptr,
                 const UiTestPs2SourceReport* ps2_source = nullptr,
                 const UiTestPs2DirectReport* ps2_direct = nullptr,
                 const UiTestRouteStoreReport* route_store = nullptr,
                 const tile_route::RoutePathReport* route_path = nullptr,
                 const tile_route::SettingsPathReport* settings_path = nullptr);
};
} // namespace shelf
