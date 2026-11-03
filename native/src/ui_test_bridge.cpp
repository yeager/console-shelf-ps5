// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/ui_test_bridge.hpp"
#include "shelf/console_tile_job_root_probe.hpp"
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sys/stat.h>

namespace shelf {
namespace {
constexpr std::size_t request_limit = 160, frame_limit = 16 * 1024 * 1024;
constexpr std::uint64_t fnv_offset = 14695981039346656037ull, fnv_prime = 1099511628211ull;
struct RouteJson {
    char bytes[3072]{};
    std::size_t used = 0;
    bool append(const char* format, ...) {
        if (used >= sizeof(bytes)) return false;
        va_list args;
        va_start(args, format);
        const int count = std::vsnprintf(bytes + used, sizeof(bytes) - used, format, args);
        va_end(args);
        if (count <= 0 || static_cast<std::size_t>(count) >= sizeof(bytes) - used) return false;
        used += static_cast<std::size_t>(count);
        return true;
    }
    bool stat(const tile_route::RoutePathStat& node) {
        return append("{\"attempted\":%s,\"ok\":%s,\"errno\":%d,\"device\":%llu,\"inode\":%llu,"
                      "\"owner\":%llu,\"mode\":%llu}",
                      node.attempted ? "true" : "false", node.ok ? "true" : "false", node.error,
                      static_cast<unsigned long long>(node.device),
                      static_cast<unsigned long long>(node.inode),
                      static_cast<unsigned long long>(node.owner),
                      static_cast<unsigned long long>(node.mode));
    }
    bool opened(const char* name, const tile_route::RoutePathOpen& call) {
        return append("{\"name\":\"%s\",\"flags\":%u,\"attempted\":%s,\"opened\":%s,"
                      "\"openErrno\":%d,\"fdStat\":", name, call.flags,
                      call.attempted ? "true" : "false", call.opened ? "true" : "false",
                      call.open_error) && stat(call.fd_stat) &&
               append(",\"closeErrno\":%d}", call.close_error);
    }
};
bool path(char (&out)[2304], const char* directory, const char* name) {
    const int used = std::snprintf(out, sizeof(out), "%s/%s", directory, name);
    return used > 0 && static_cast<std::size_t>(used) < sizeof(out);
}
bool discard(const char* file) {
    return std::remove(file) == 0 || errno == ENOENT;
}
bool replace(const char* source, const char* target) {
    // Windows rename does not replace. No reader sees a partially written file;
    // readers may briefly see no file between removal and rename.
    return discard(target) && std::rename(source, target) == 0;
}
bool existing_directory(const char* name) {
    struct stat info{};
    return stat(name, &info) == 0 && (info.st_mode & S_IFMT) == S_IFDIR;
}
bool valid_session(const char* value) {
    if (!value || std::strlen(value) != 32)
        return false;
    for (unsigned i = 0; i < 32; ++i)
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f')))
            return false;
    return true;
}
bool decimal(const char*& at, const char* end, std::uint64_t& value) {
    if (at == end || *at < '0' || *at > '9')
        return false;
    const char* first = at;
    value = 0;
    while (at != end && *at >= '0' && *at <= '9') {
        const unsigned digit = static_cast<unsigned>(*at++ - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    // A canonical decimal form makes accidental extra fields unambiguous.
    return !(at - first > 1 && *first == '0') && at != end && *at++ == ' ';
}
bool operation(const char* name, UiTestRequest& request) {
    if (std::strcmp(name, "snapshot") == 0) {
        request.operation = UiTestOperation::snapshot;
        return true;
    }
    if (std::strcmp(name, "stop") == 0) {
        request.operation = UiTestOperation::stop;
        return true;
    }
    if (std::strcmp(name, "close") == 0) {
        request.operation = UiTestOperation::close;
        return true;
    }
    if (std::strcmp(name, "probe-paths") == 0) {
        request.operation = UiTestOperation::probe_paths;
        return true;
    }
    if (std::strcmp(name, "probe-writes-v4") == 0) {
        request.operation = UiTestOperation::probe_writes_v4;
        return true;
    }
    if (std::strcmp(name, "probe-job-root") == 0) {
        request.operation = UiTestOperation::probe_job_root;
        return true;
    }
    if (std::strcmp(name, "create-job-root") == 0) {
        request.operation = UiTestOperation::create_job_root;
        return true;
    }
    if (std::strcmp(name, "probe-app-source") == 0) {
        request.operation = UiTestOperation::probe_app_source;
        return true;
    }
    if (std::strcmp(name, "create-n64-source") == 0) {
        request.operation = UiTestOperation::create_n64_source;
        return true;
    }
    if (std::strcmp(name, "probe-ps2-source") == 0) {
        request.operation = UiTestOperation::probe_ps2_source;
        return true;
    }
    if (std::strcmp(name, "probe-ps2-direct") == 0) {
        request.operation = UiTestOperation::probe_ps2_direct;
        return true;
    }
    if (std::strcmp(name, "probe-route-store") == 0) {
        request.operation = UiTestOperation::probe_route_store;
        return true;
    }
    if (std::strcmp(name, "probe-route-path") == 0) {
        request.operation = UiTestOperation::probe_route_path;
        return true;
    }
    if (std::strcmp(name, "probe-settings-path") == 0) {
        request.operation = UiTestOperation::probe_settings_path;
        return true;
    }
    if (std::strcmp(name, "launch-confirm") == 0) {
        request.operation = UiTestOperation::launch_confirm;
        return true;
    }
    constexpr const char* names[] = {"up", "down", "left", "right", "confirm", "back", "settings", "layout",
                                      "favorite", "previous-tab", "tab"};
    constexpr Action actions[] = {Action::up, Action::down, Action::left, Action::right, Action::confirm,
                                   Action::back, Action::settings, Action::layout, Action::favorite,
                                   Action::previous_tab, Action::next_tab};
    for (unsigned i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i)
        if (std::strcmp(name, names[i]) == 0) {
            request.operation = UiTestOperation::action;
            request.action = actions[i];
            return true;
        }
    return false;
}
bool parse(const char* bytes, std::size_t size, const char* expected_session, std::uint64_t next,
           UiTestRequest& request) {
    constexpr char prefix[] = "ISSHELF_UI_TEST_1 ";
    if (!size || size > request_limit || bytes[size - 1] != '\n' || size < sizeof(prefix) + 32 + 6)
        return false;
    for (std::size_t i = 0; i + 1 < size; ++i)
        if (static_cast<unsigned char>(bytes[i]) < 32 || static_cast<unsigned char>(bytes[i]) > 126)
            return false;
    if (std::memcmp(bytes, prefix, sizeof(prefix) - 1) != 0)
        return false;
    const char* at = bytes + sizeof(prefix) - 1;
    const char* end = bytes + size - 1;
    if (std::memcmp(at, expected_session, 32) != 0 || at[32] != ' ')
        return false;
    at += 33;
    UiTestRequest value;
    if (!decimal(at, end, value.sequence) || !decimal(at, end, value.revision) ||
        value.sequence != next || value.sequence == std::numeric_limits<std::uint64_t>::max())
        return false;
    const auto remaining = static_cast<std::size_t>(end - at);
    if (!remaining || remaining >= sizeof(value.name))
        return false;
    std::memcpy(value.name, at, remaining);
    if (!operation(value.name, value))
        return false;
    request = value;
    return true;
}
std::uint32_t be32(const unsigned char* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) | (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
}
std::uint32_t crc(std::uint32_t value, const unsigned char* bytes, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        value ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0u);
    }
    return value;
}
struct PngReader {
    FILE* file;
    std::size_t bytes = 0;
    std::uint64_t fnv = fnv_offset;
    bool read(unsigned char* out, std::size_t count) {
        if (count > frame_limit - bytes || std::fread(out, 1, count, file) != count)
            return false;
        bytes += count;
        for (std::size_t i = 0; i < count; ++i)
            fnv = (fnv ^ out[i]) * fnv_prime;
        return true;
    }
};
bool png(const char* file, std::size_t& bytes, std::uint64_t& hash) {
    FILE* input = std::fopen(file, "rb");
    if (!input)
        return false;
    PngReader reader{input};
    unsigned char buffer[4096];
    constexpr unsigned char signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    bool okay = reader.read(buffer, 8) && std::memcmp(buffer, signature, 8) == 0;
    bool data = false, finished = false;
    for (unsigned chunk = 0; okay && !finished && chunk < 4096; ++chunk) {
        okay = reader.read(buffer, 8);
        if (!okay)
            break;
        const auto length = be32(buffer);
        const bool header = std::memcmp(buffer + 4, "IHDR", 4) == 0;
        const bool image = std::memcmp(buffer + 4, "IDAT", 4) == 0;
        const bool end = std::memcmp(buffer + 4, "IEND", 4) == 0;
        if (length > frame_limit || (chunk == 0 ? !header || length != 13 : header) ||
            (end && (length != 0 || !data))) {
            okay = false;
            break;
        }
        std::uint32_t check = crc(0xffffffffu, buffer + 4, 4);
        std::uint32_t left = length;
        while (okay && left) {
            const auto count = left < sizeof(buffer) ? static_cast<std::size_t>(left) : sizeof(buffer);
            okay = reader.read(buffer, count);
            if (okay && header) {
                const auto width = be32(buffer), height = be32(buffer + 4);
                okay = width && height && width <= 1920 && height <= 1080 && buffer[8] == 8 &&
                       (buffer[9] == 2 || buffer[9] == 6) && buffer[10] == 0 && buffer[11] == 0 && buffer[12] == 0;
            }
            if (okay)
                check = crc(check, buffer, count);
            left -= static_cast<std::uint32_t>(count);
        }
        okay = okay && reader.read(buffer, 4) && be32(buffer) == (check ^ 0xffffffffu);
        if (image && length)
            data = true;
        if (end)
            finished = true;
    }
    okay = okay && finished && std::fgetc(input) == EOF && !std::ferror(input);
    if (std::fclose(input) != 0)
        okay = false;
    if (okay) {
        bytes = reader.bytes;
        hash = reader.fnv;
    }
    return okay;
}
bool result_label(const char* value) {
    if (!value)
        return false;
    const auto size = std::strlen(value);
    if (!size || size > 64)
        return false;
    for (std::size_t i = 0; i < size; ++i)
        if (!((value[i] >= 'a' && value[i] <= 'z') || (value[i] >= 'A' && value[i] <= 'Z') ||
              (value[i] >= '0' && value[i] <= '9') || value[i] == '-' || value[i] == '_'))
            return false;
    return true;
}
} // namespace

bool apply_ui_test_action(UiState& state, Action action) {
    UiState candidate = state;
    input(candidate, action);
    if (candidate.launch_requested || candidate.exit_requested ||
        (candidate.route_save_requested && candidate.route_save_apply_after &&
         candidate.pending_route_save.destination == ConsoleTileDestination::emulator))
        return false;
    state = candidate;
    return true;
}
bool apply_ui_test_launch_confirm(UiState& state) {
    if (state.launch_requested || state.launch_busy || state.exit_requested ||
        state.tile_requested || state.tile_busy || state.route_save_requested ||
        state.route_save_busy)
        return false;
    UiState candidate = state;
    if (state.page == Page::console_tile) {
        const bool queued = state.tile_removal_batch_ready ||
                            (state.selected_system < system_count &&
                             state.tile_removal_handoff[state.selected_system]);
        if (!state.tile_connected || !queued || state.tile_focus != 0 ||
            !state.tile_removal_count)
            return false;
        input(candidate, Action::confirm);
        const auto& request = candidate.pending_launch;
        std::size_t count = 0;
        const bool residual = request.mode == LaunchMode::tile_residual;
        const bool titles_valid = residual
            ? launch_tile_residual_titles_valid(request.title, request.rom, &count)
            : launch_tile_refresh_titles_valid(request.title, request.rom, &count);
        if (!candidate.launch_requested || candidate.exit_requested ||
            (request.mode != LaunchMode::tile_remove && !residual) ||
            residual != state.tile_residual_sweep || request.core[0] ||
            !titles_valid ||
            count != state.tile_removal_count)
            return false;
        state = candidate;
        return true;
    }
    if (state.page != Page::details || state.detail_focus != 1)
        return false;
    input(candidate, Action::confirm);
    const auto& request = candidate.pending_launch;
    if (!candidate.launch_requested || candidate.exit_requested ||
        request.mode != LaunchMode::emulator_menu || !launch_menu_known(request.title) ||
        request.rom[0] || request.core[0])
        return false;
    state = candidate;
    return true;
}
bool UiTestBridge::start(const char* data_root, const char* session32) {
    stop();
    session_[0] = directory_[0] = request_path_[0] = frame_new_path_[0] = 0;
    if (!data_root || !*data_root || std::strlen(data_root) > 2000 || !valid_session(session32))
        return false;
    for (const unsigned char* at = reinterpret_cast<const unsigned char*>(data_root); *at; ++at)
        if (*at < 32 || *at == 127)
            return false;
    char response[2304], pending[2304], frame[2304];
    if (!existing_directory(data_root))
        return false;
    std::memcpy(directory_, data_root, std::strlen(data_root) + 1);
    if (!path(request_path_, directory_, "ui-test-request.txt") ||
        !path(frame_new_path_, directory_, "ui-test-frame.new.png") ||
        !path(response, directory_, "ui-test-response.json") ||
        !path(pending, directory_, "ui-test-response.new.json") ||
        !path(frame, directory_, "ui-test-frame.png") || !discard(response) || !discard(pending) ||
        !discard(frame) || !discard(frame_new_path_))
        return false;
    std::memcpy(session_, session32, sizeof(session_));
    next_sequence_ = 1;
    enabled_ = true;
    return true;
}
void UiTestBridge::stop() { enabled_ = false; }
bool UiTestBridge::poll(UiTestRequest& request) {
    if (!enabled_)
        return false;
    FILE* input = std::fopen(request_path_, "rb");
    if (!input)
        return false;
    char bytes[request_limit + 1];
    const auto size = std::fread(bytes, 1, sizeof(bytes), input);
    const bool okay = !std::ferror(input);
    const bool closed = std::fclose(input) == 0;
    if (!okay || !closed || !parse(bytes, size, session_, next_sequence_, request))
        return false;
    ++next_sequence_;
    return true;
}
bool UiTestBridge::publish(const UiTestRequest& request, std::uint64_t revision, const char* result,
                           bool applied, bool saved, const UiState& state, bool captured,
                           std::size_t probe_bytes, std::uint64_t probe_fnv,
                           const tile_job::JobRootProbeResult* job_root,
                           const UiTestPs2SourceReport* ps2_source,
                           const UiTestPs2DirectReport* ps2_direct,
                           const UiTestRouteStoreReport* route_store,
                           const tile_route::RoutePathReport* route_path,
                           const tile_route::SettingsPathReport* settings_path) {
    if (!session_[0] || !result_label(result))
        return false;
    const bool source_probe = request.operation == UiTestOperation::probe_app_source;
    const bool n64_source = request.operation == UiTestOperation::create_n64_source;
    const bool payload_probe = request.operation == UiTestOperation::probe_paths ||
                               request.operation == UiTestOperation::probe_writes_v4;
    if (std::strcmp(result, "probe-recorded") == 0 &&
        (!payload_probe || !applied || !probe_bytes || probe_bytes > 8192))
        return false;
    if (std::strcmp(result, "source-recorded") == 0 &&
        (!source_probe || !applied || !probe_bytes || probe_bytes > 8192 || !probe_fnv))
        return false;
    if (std::strcmp(result, "console-source-recorded") == 0 &&
        (!n64_source || !applied || !probe_bytes || probe_bytes > 8192 || !probe_fnv))
        return false;
    if (!payload_probe && !source_probe && !n64_source && (probe_bytes || probe_fnv))
        return false;
    const bool job_root_action = request.operation == UiTestOperation::probe_job_root ||
                                 request.operation == UiTestOperation::create_job_root;
    if (job_root_action && !job_root && std::strcmp(result, "unsupported") != 0)
        return false;
    const bool ps2_source_action = request.operation == UiTestOperation::probe_ps2_source;
    if ((ps2_source && (!ps2_source_action || applied)) ||
        (ps2_source_action && applied) ||
        (ps2_source_action &&
         (std::strcmp(result, "ps2-source-readable") == 0 ||
          std::strcmp(result, "ps2-source-unavailable") == 0) && !ps2_source))
        return false;
    const bool ps2_direct_action = request.operation == UiTestOperation::probe_ps2_direct;
    if ((ps2_direct && (!ps2_direct_action || applied)) || (ps2_direct_action && applied) ||
        (ps2_direct_action &&
         (std::strcmp(result, "ps2-direct-matched") == 0 ||
          std::strcmp(result, "ps2-direct-unavailable") == 0) && !ps2_direct))
        return false;
    const bool route_store_action = request.operation == UiTestOperation::probe_route_store;
    if ((route_store && (!route_store_action || applied)) || (route_store_action && applied) ||
        (route_store_action &&
         (std::strcmp(result, "route-store-present") == 0 ||
          std::strcmp(result, "route-store-missing") == 0 ||
          std::strcmp(result, "route-store-unsafe") == 0) && !route_store))
        return false;
    char route_store_json[192]{};
    const char* route_store_value = "null";
    if (route_store_action && route_store) {
        if (!route_store->root) return false;
        const char* expected = std::strcmp(route_store->root, "present") == 0 ? "route-store-present" :
                               std::strcmp(route_store->root, "missing") == 0 ? "route-store-missing" :
                               std::strcmp(route_store->root, "unsafe") == 0 ? "route-store-unsafe" : nullptr;
        if (!expected || std::strcmp(result, expected) != 0 || route_store->system_error < 0 ||
            route_store->system_error > 65535 || captured || probe_bytes || probe_fnv ||
            (std::strcmp(route_store->root, "present") == 0 && route_store->system_error != 0))
            return false;
        const int length = std::snprintf(route_store_json, sizeof(route_store_json),
            "{\"root\":\"%s\",\"uid\":%llu,\"euid\":%llu,\"systemError\":%d}",
            route_store->root, static_cast<unsigned long long>(route_store->uid),
            static_cast<unsigned long long>(route_store->euid), route_store->system_error);
        if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(route_store_json)) return false;
        route_store_value = route_store_json;
    }
    const bool route_path_action = request.operation == UiTestOperation::probe_route_path;
    const bool route_path_observed = std::strcmp(result, "route-path-observed") == 0;
    if ((route_path && (!route_path_action || applied || captured || probe_bytes || probe_fnv ||
                        !route_path_observed)) ||
        (route_path_action && (applied || route_path_observed != (route_path != nullptr))))
        return false;
    RouteJson route_path_json;
    const char* route_path_value = "null";
    if (route_path_action && route_path) {
        constexpr const char* variants[4] = {"base-nonblock", "directory", "nofollow-nonblock", "full"};
        if (!route_path_json.append("{\"uid\":%llu,\"euid\":%llu,\"parentBefore\":",
                                    static_cast<unsigned long long>(route_path->uid),
                                    static_cast<unsigned long long>(route_path->euid)) ||
            !route_path_json.stat(route_path->parent_before) ||
            !route_path_json.append(",\"parentOpens\":[")) return false;
        for (unsigned i = 0; i < 4; ++i)
            if ((i && !route_path_json.append(",")) ||
                !route_path_json.opened(variants[i], route_path->parent_open[i])) return false;
        if (!route_path_json.append("],\"parentAfter\":") ||
            !route_path_json.stat(route_path->parent_after) ||
            !route_path_json.append(",\"parentExpected\":%s,\"parentMatch\":%s,\"childBefore\":",
                                    route_path->parent_expected ? "true" : "false",
                                    route_path->parent_match ? "true" : "false") ||
            !route_path_json.stat(route_path->child_before) ||
            !route_path_json.append(",\"childOpen\":") ||
            !route_path_json.opened("full", route_path->child_open) ||
            !route_path_json.append(",\"childAfter\":") ||
            !route_path_json.stat(route_path->child_after) ||
            !route_path_json.append(",\"childMatch\":%s}",
                                    route_path->child_match ? "true" : "false")) return false;
        route_path_value = route_path_json.bytes;
    }
    const bool settings_path_action = request.operation == UiTestOperation::probe_settings_path;
    const bool settings_path_observed = std::strcmp(result, "settings-path-observed") == 0;
    if ((settings_path_observed && !settings_path_action) ||
        (settings_path && (!settings_path_action || applied || captured || probe_bytes || probe_fnv ||
                           !settings_path_observed)) ||
        (settings_path_action && (applied || settings_path_observed != (settings_path != nullptr))))
        return false;
    RouteJson settings_path_json;
    const char* settings_path_value = "null";
    if (settings_path_action && settings_path) {
        if (!settings_path_json.append("{\"uid\":%llu,\"euid\":%llu,\"parentOpen\":",
                                       static_cast<unsigned long long>(settings_path->uid),
                                       static_cast<unsigned long long>(settings_path->euid)) ||
            !settings_path_json.opened("directory-nofollow-nonblock", settings_path->parent_open) ||
            !settings_path_json.append(",\"files\":[")) return false;
        constexpr const char* names[2] = {"settings.ini", "settings.ini.bak"};
        for (unsigned i = 0; i < 2; ++i) {
            const auto& file = settings_path->files[i];
            if ((i && !settings_path_json.append(",")) ||
                !settings_path_json.append("{\"name\":\"%s\",\"before\":", names[i]) ||
                !settings_path_json.stat(file.before) ||
                !settings_path_json.append(",\"direct\":") ||
                !settings_path_json.opened("nofollow-nonblock", file.direct) ||
                !settings_path_json.append(",\"after\":") ||
                !settings_path_json.stat(file.after) ||
                !settings_path_json.append(",\"namedMatch\":%s}",
                                           file.named_match ? "true" : "false")) return false;
        }
        if (!settings_path_json.append("]}")) return false;
        settings_path_value = settings_path_json.bytes;
    }
    char direct_json[768]{};
    const char* direct_value = "null";
    if (ps2_direct_action && ps2_direct) {
        if ((std::strcmp(result, "ps2-direct-matched") != 0 &&
             std::strcmp(result, "ps2-direct-unavailable") != 0) ||
            !result_label(ps2_direct->failure) || ps2_direct->system_error < 0 ||
            ps2_direct->system_error > 65535 || ps2_direct->bytes_read > 676 ||
            (ps2_direct->complete != (std::strcmp(result, "ps2-direct-matched") == 0)) ||
            (ps2_direct->complete && (std::strcmp(ps2_direct->failure, "none") != 0 ||
                                      !ps2_direct->target_opened || !ps2_direct->content_matched ||
                                      !ps2_direct->mount_distinct || ps2_direct->bytes_read != 676 ||
                                      ps2_direct->target_size != 676 || ps2_direct->system_error != 0)) ||
            (!ps2_direct->complete && std::strcmp(ps2_direct->failure, "none") == 0))
            return false;
        const int length = std::snprintf(direct_json, sizeof(direct_json),
            "{\"complete\":%s,\"targetOpened\":%s,\"contentMatched\":%s,\"mountDistinct\":%s,"
            "\"failure\":\"%s\",\"systemError\":%d,\"uid\":%llu,\"euid\":%llu,"
            "\"bytesRead\":%zu,\"rootDevice\":%llu,\"systemDevice\":%llu,"
            "\"targetDevice\":%llu,\"targetInode\":%llu,\"targetOwner\":%llu,"
            "\"targetMode\":%llu,\"targetSize\":%lld}",
            ps2_direct->complete ? "true" : "false", ps2_direct->target_opened ? "true" : "false",
            ps2_direct->content_matched ? "true" : "false", ps2_direct->mount_distinct ? "true" : "false",
            ps2_direct->failure, ps2_direct->system_error,
            static_cast<unsigned long long>(ps2_direct->uid),
            static_cast<unsigned long long>(ps2_direct->euid), ps2_direct->bytes_read,
            static_cast<unsigned long long>(ps2_direct->root_device),
            static_cast<unsigned long long>(ps2_direct->system_device),
            static_cast<unsigned long long>(ps2_direct->target_device),
            static_cast<unsigned long long>(ps2_direct->target_inode),
            static_cast<unsigned long long>(ps2_direct->target_owner),
            static_cast<unsigned long long>(ps2_direct->target_mode),
            static_cast<long long>(ps2_direct->target_size));
        if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(direct_json)) return false;
        direct_value = direct_json;
    }
    char ps2_json[3072]{};
    const char* ps2_value = "null";
    if (ps2_source_action && ps2_source) {
        if ((std::strcmp(result, "ps2-source-readable") == 0 && !ps2_source->complete) ||
            (std::strcmp(result, "ps2-source-unavailable") == 0 && ps2_source->complete))
            return false;
        if (!result_label(ps2_source->failure) || ps2_source->observed_nodes > 8 ||
            ps2_source->failed_node > 8 || ps2_source->failed_pass > 2 ||
            ps2_source->system_error < 0 || ps2_source->system_error > 65535 ||
            ps2_source->files_read > 4 ||
            (ps2_source->complete &&
             (std::strcmp(ps2_source->failure, "none") != 0 ||
              ps2_source->observed_nodes != 8 || ps2_source->failed_node != 8 ||
              ps2_source->failed_pass != 0 || ps2_source->files_read != 4)))
            return false;
        if (!ps2_source->complete && std::strcmp(ps2_source->failure, "none") == 0)
            return false;
        int count = std::snprintf(ps2_json, sizeof(ps2_json),
            "{\"complete\":%s,\"failure\":\"%s\",\"uid\":%llu,\"euid\":%llu,"
            "\"observedNodes\":%zu,\"failedNode\":%zu,\"failedPass\":%u,"
            "\"systemError\":%d,\"filesRead\":%u,\"nodes\":[",
            ps2_source->complete ? "true" : "false", ps2_source->failure,
            static_cast<unsigned long long>(ps2_source->uid),
            static_cast<unsigned long long>(ps2_source->euid),
            ps2_source->observed_nodes, ps2_source->failed_node,
            ps2_source->failed_pass, ps2_source->system_error, ps2_source->files_read);
        if (count <= 0 || static_cast<std::size_t>(count) >= sizeof(ps2_json)) return false;
        std::size_t used = static_cast<std::size_t>(count);
        for (std::size_t i = 0; i < ps2_source->observed_nodes; ++i) {
            const auto& node = ps2_source->nodes[i];
            count = std::snprintf(ps2_json + used, sizeof(ps2_json) - used,
                "%s{\"device\":%llu,\"inode\":%llu,\"owner\":%llu,"
                "\"group\":%llu,\"mode\":%llu,\"size\":%lld}",
                i ? "," : "", static_cast<unsigned long long>(node.device),
                static_cast<unsigned long long>(node.inode),
                static_cast<unsigned long long>(node.owner),
                static_cast<unsigned long long>(node.group),
                static_cast<unsigned long long>(node.mode),
                static_cast<long long>(node.size));
            if (count <= 0 || static_cast<std::size_t>(count) >= sizeof(ps2_json) - used)
                return false;
            used += static_cast<std::size_t>(count);
        }
        count = std::snprintf(ps2_json + used, sizeof(ps2_json) - used, "]}");
        if (count <= 0 || static_cast<std::size_t>(count) >= sizeof(ps2_json) - used)
            return false;
        ps2_value = ps2_json;
    }
    char job_root_json[1024]{};
    const char* job_root_value = "null";
    if (job_root_action && job_root) {
        const int length = std::snprintf(job_root_json, sizeof(job_root_json),
            "{\"ran\":%s,\"uid\":%llu,\"euid\":%llu,\"parentValid\":%s,"
            "\"parentDevice\":%llu,\"parentInode\":%llu,\"parentOwner\":%llu,\"parentMode\":%u,"
            "\"childSeen\":%s,\"childValid\":%s,\"childDevice\":%llu,\"childInode\":%llu,"
            "\"childOwner\":%llu,\"childMode\":%u,\"mkdirAttempted\":%s,\"mkdirErrno\":%d,"
            "\"pathErrno\":%d,\"childLookupErrno\":%d,\"syncErrno\":%d,\"closeErrno\":%d,"
            "\"created\":%s,\"complete\":%s}",
            job_root->ran ? "true" : "false",
            static_cast<unsigned long long>(job_root->uid),
            static_cast<unsigned long long>(job_root->euid),
            job_root->parent_valid ? "true" : "false",
            static_cast<unsigned long long>(job_root->parent.device),
            static_cast<unsigned long long>(job_root->parent.inode),
            static_cast<unsigned long long>(job_root->parent.owner), job_root->parent.mode,
            job_root->child_seen ? "true" : "false", job_root->child_valid ? "true" : "false",
            static_cast<unsigned long long>(job_root->child.device),
            static_cast<unsigned long long>(job_root->child.inode),
            static_cast<unsigned long long>(job_root->child.owner), job_root->child.mode,
            job_root->mkdir_attempted ? "true" : "false", job_root->mkdir_errno,
            job_root->path_errno, job_root->child_lookup_errno,
            job_root->sync_errno, job_root->close_errno,
            job_root->created ? "true" : "false", job_root->complete ? "true" : "false");
        if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(job_root_json)) return false;
        job_root_value = job_root_json;
    }
    UiTestRequest checked;
    if (!std::memchr(request.name, 0, sizeof(request.name)) || !operation(request.name, checked))
        return false;
    char frame[2304], response[2304], pending[2304];
    if (!path(frame, directory_, "ui-test-frame.png") || !path(response, directory_, "ui-test-response.json") ||
        !path(pending, directory_, "ui-test-response.new.json"))
        return false;
    std::size_t bytes = 0;
    std::uint64_t hash = 0;
    const bool available = captured && png(frame_new_path_, bytes, hash) && replace(frame_new_path_, frame);
    if (!available) {
        bytes = 0;
        hash = 0;
        discard(frame_new_path_);
        discard(frame);
    }
    const bool remain_active = enabled_ && request.operation != UiTestOperation::stop;
    if (request.operation == UiTestOperation::stop)
        stop();
    FILE* output = std::fopen(pending, "wb");
    if (!output)
        return false;
    const auto selected = state.selected_system < system_count ? catalog[state.selected_system].id : std::string_view{};
    const int written = std::fprintf(output,
        "{\"protocol\":1,\"sessionTimeoutMs\":0,\"active\":%s,\"session\":\"%s\",\"nextSequence\":%llu,\"sequence\":%llu,"
        "\"revision\":%llu,\"action\":\"%s\",\"result\":\"%s\",\"actionApplied\":%s,\"settingsSaved\":%s,"
        "\"frameAvailable\":%s,\"frameBytes\":%zu,\"frameFNV64\":\"%016llx\","
        "\"probeBytes\":%zu,\"probeFNV64\":\"%016llx\",\"jobRoot\":%s,\"ps2Source\":%s,\"ps2Direct\":%s,\"routeStore\":%s,\"routePath\":%s,\"settingsPath\":%s,\"page\":%d,"
        "\"selectedSystem\":\"%.*s\",\"focus\":%d,\"gameFocus\":%d,\"tab\":%d}\n",
        remain_active ? "true" : "false", session_, static_cast<unsigned long long>(next_sequence_),
        static_cast<unsigned long long>(request.sequence), static_cast<unsigned long long>(revision), request.name, result,
        applied ? "true" : "false", saved ? "true" : "false", available ? "true" : "false", bytes,
        static_cast<unsigned long long>(hash), probe_bytes, static_cast<unsigned long long>(probe_fnv),
        job_root_value, ps2_value, direct_value, route_store_value, route_path_value, settings_path_value,
        static_cast<int>(state.page), static_cast<int>(selected.size()),
        selected.data() ? selected.data() : "", state.focus, state.game_focus, static_cast<int>(state.tab));
    const bool flushed = written > 0 && std::fflush(output) == 0 && !std::ferror(output);
    const bool closed = std::fclose(output) == 0;
    const bool published = flushed && closed && replace(pending, response);
    if (!published)
        discard(pending);
    return published;
}
} // namespace shelf
