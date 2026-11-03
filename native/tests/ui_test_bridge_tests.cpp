// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/ui_test_bridge.hpp"
#include "shelf/console_tile_startup.hpp"
#include "shelf/console_tile_job_root_probe.hpp"
#include "shelf/console_tile_path_probe.hpp"
#include "shelf/console_tile_write_probe.hpp"
#include "shelf/console_tile_write_probe_v4.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#endif
namespace {
void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL UI test bridge: %s\n", message);
        std::exit(1);
    }
}
void make_directory(const char* path) {
#ifdef _WIN32
    check(_mkdir(path) == 0, "create owned fixture directory");
#else
    check(mkdir(path, 0700) == 0, "create owned fixture directory");
#endif
}
void write(const std::string& path, const void* bytes, std::size_t count) {
    FILE* output = std::fopen(path.c_str(), "wb");
    check(output != nullptr, "open owned fixture");
    check(std::fwrite(bytes, 1, count, output) == count, "write owned fixture");
    check(std::fclose(output) == 0, "close owned fixture");
}
void write(const std::string& path, const std::string& text) { write(path, text.data(), text.size()); }
std::string read(const std::string& path) {
    FILE* input = std::fopen(path.c_str(), "rb");
    check(input != nullptr, "open response fixture");
    std::string text;
    char bytes[1024];
    std::size_t count;
    while ((count = std::fread(bytes, 1, sizeof(bytes), input)) != 0)
        text.append(bytes, count);
    check(!std::ferror(input) && std::fclose(input) == 0, "read complete response fixture");
    return text;
}
bool exists(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    std::fclose(file);
    return true;
}
bool directory_exists(const std::string& path) {
    struct stat info{};
    return stat(path.c_str(), &info) == 0 && (info.st_mode & S_IFMT) == S_IFDIR;
}
std::string command(const char* session, std::uint64_t sequence, std::uint64_t revision, const char* action) {
    char text[180];
    std::snprintf(text, sizeof(text), "ISSHELF_UI_TEST_1 %s %llu %llu %s\n", session,
                  static_cast<unsigned long long>(sequence), static_cast<unsigned long long>(revision), action);
    return text;
}
std::uint32_t crc(const unsigned char* bytes, std::size_t count) {
    std::uint32_t value = 0xffffffffu;
    for (std::size_t i = 0; i < count; ++i) {
        value ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0u);
    }
    return value ^ 0xffffffffu;
}
void be32(std::vector<unsigned char>& bytes, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<unsigned char>(value >> shift));
}
void chunk(std::vector<unsigned char>& bytes, const char* type, const std::vector<unsigned char>& content) {
    be32(bytes, static_cast<std::uint32_t>(content.size()));
    const auto begin = bytes.size();
    for (unsigned i = 0; i < 4; ++i)
        bytes.push_back(static_cast<unsigned char>(type[i]));
    bytes.insert(bytes.end(), content.begin(), content.end());
    be32(bytes, crc(bytes.data() + begin, 4 + content.size()));
}
std::vector<unsigned char> image(std::uint32_t width = 1) {
    std::vector<unsigned char> bytes{137, 80, 78, 71, 13, 10, 26, 10}, header;
    be32(header, width);
    be32(header, 1);
    header.insert(header.end(), {8, 6, 0, 0, 0});
    chunk(bytes, "IHDR", header);
    // A complete zlib stream containing one unfiltered RGBA pixel.
    chunk(bytes, "IDAT", {0x78, 0x01, 0x01, 0x05, 0x00, 0xfa, 0xff, 0x00,
                           0x11, 0x22, 0x33, 0xff, 0x02, 0x14, 0x01, 0x66});
    chunk(bytes, "IEND", {});
    return bytes;
}
std::string fnv(const std::vector<unsigned char>& bytes) {
    std::uint64_t value = 14695981039346656037ull;
    for (auto byte : bytes)
        value = (value ^ byte) * 1099511628211ull;
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}
void contains(const std::string& json, const char* text, const char* message) {
    check(json.find(text) != std::string::npos, message);
}
} // namespace
void test_ui_test_bridge() {
    using namespace shelf;
    check(!ui_test_elapsed(100, 101, 600000) && !ui_test_elapsed(100, 101, 250),
          "a later start cannot make a cadence timer fire immediately");
    check(!ui_test_elapsed(600100, 101, 600000) && ui_test_elapsed(600101, 101, 600000),
          "long cadence intervals respect their exact boundary");
    check(!ui_test_elapsed(350, 101, 250) && ui_test_elapsed(351, 101, 250),
          "request polling respects its exact quarter-second boundary");
    constexpr std::uint32_t wrap_start = 0xfffffff0u;
    constexpr auto expiry_after_wrap = static_cast<std::uint32_t>(wrap_start + 600000u);
    check(!ui_test_elapsed(expiry_after_wrap - 1, wrap_start, 600000) &&
              ui_test_elapsed(expiry_after_wrap, wrap_start, 600000),
          "long cadence intervals remain exact across 32-bit clock wrap");
    check(!ui_test_elapsed(0xe9u, wrap_start, 250) && ui_test_elapsed(0xeau, wrap_start, 250),
          "poll interval remains exact across 32-bit clock wrap");
    check(!ui_test_elapsed(100, 101, 0) && ui_test_elapsed(101, 101, 0) &&
              ui_test_elapsed(0x10u, wrap_start, 0),
          "capture deadlines wait for future starts and survive clock wrap");
    check(!ui_test_elapsed(0, 0, 0x80000000u), "ambiguous half-cycle timer intervals are refused");
    UiTestRequest capture_request;
    std::strcpy(capture_request.name, "snapshot");
    check(!ui_test_discrete_frame_required(capture_request),
          "synthetic ready receipt never blocks L3 plus R3 on a full PNG");
    capture_request.sequence = 1;
    check(ui_test_discrete_frame_required(capture_request),
          "an explicit snapshot retains bound PNG evidence");
    capture_request.operation = UiTestOperation::action;
    check(ui_test_discrete_frame_required(capture_request),
          "a discrete menu command retains bound PNG evidence");
    for (const auto operation : {UiTestOperation::probe_paths, UiTestOperation::probe_writes_v4,
                                 UiTestOperation::probe_ps2_source, UiTestOperation::probe_ps2_direct,
                                 UiTestOperation::probe_route_store, UiTestOperation::probe_route_path,
                                 UiTestOperation::probe_settings_path}) {
        capture_request.operation = operation;
        check(!ui_test_discrete_frame_required(capture_request),
              "structured probes never request a redundant screenshot");
    }
    constexpr const char* session = "0123456789abcdef0123456789abcdef";
    constexpr const char* other = "1123456789abcdef0123456789abcdef";
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    char root[128];
    std::snprintf(root, sizeof(root), "ui-test-bridge-%lld", static_cast<long long>(stamp));
    make_directory(root);
    const std::string directory = std::string(root) + "/";
    const auto request_file = directory + "ui-test-request.txt", response = directory + "ui-test-response.json";
    const auto published_frame = directory + "ui-test-frame.png";
    const auto legacy_directory = directory + "ui-test";
    UiTestBridge bridge;
    UiTestRequest request;
    check(!bridge.active() && !bridge.poll(request), "bridge starts disabled");
    check(!bridge.start(root, "short") && !bridge.start(root, "0123456789ABCDEF0123456789abcdef"),
          "invalid sessions cannot activate transport");
    check(!bridge.start((directory + "missing-root").c_str(), session) &&
              !directory_exists(directory + "missing-root"), "transport never creates a missing data root");
    check(bridge.start(root, session) && bridge.active(), "start uses existing owned data root");
    check(!directory_exists(legacy_directory), "transport never creates a nested ui-test directory");
    make_directory(legacy_directory.c_str());
    write(legacy_directory + "/response.json", "preserved-failed-trial-evidence");
    check(!exists(response) && !bridge.poll(request), "start has no stale response or command");
    check(std::string(bridge.frame_path()) == directory + "ui-test-frame.new.png", "capture path uses fixed flat prefix");
    UiState state;
    state.selected_system = find_system("ps2");
    state.focus = 7;
    state.game_focus = 2;
    state.tab = SettingsTab::room;
    std::strcpy(state.query, "private-query");
    std::strcpy(state.settings.tv_text, "private-room-text");
    std::strcpy(state.notice, "private-path/data/game.cue");
    UiTestRequest ready;
    std::strcpy(ready.name, "snapshot");
    check(bridge.publish(ready, 4, "ready", false, true, state, false), "initial ready response publishes");
    auto json = read(response);
    contains(json, "\"sequence\":0", "ready has synthetic sequence zero");
    contains(json, "\"nextSequence\":1", "ready names next expected command");
    contains(json, "\"selectedSystem\":\"ps2\"", "response uses stable console ID");
    contains(json, "\"frameAvailable\":false", "no capture never claims an image");
    contains(json, "\"sessionTimeoutMs\":0", "owner-enabled session reports no automatic timeout");
    check(json.find("private") == std::string::npos, "responses omit queries, messages, game paths and notices");
    check(!exists(directory + "ui-test-response.new.json") && json.back() == '\n', "response publishes complete then renames");

    const auto valid = command(session, 1, 4, "snapshot");
    std::vector<std::string> invalid{
        valid.substr(0, valid.size() - 1), valid + "\n", valid + "extra", command(other, 1, 4, "snapshot"),
        command(session, 0, 4, "snapshot"), command(session, 2, 4, "snapshot"), command(session, 1, 4, "unknown"),
        command(session, 1, 4, "../frame.png"), command(session, 1, 4, "right left"),
        command(session, 1, 4, "wait:100"), command(session, 1, 4, "circle-down"),
        command(session, 1, 4, "text:abc"), command(session, 1, 4, "RIGHT"),
        "ISSHELF_UI_TEST_1 " + std::string(session) + " 01 4 snapshot\n",
        "ISSHELF_UI_TEST_1 " + std::string(session) + " 1 04 snapshot\n",
        "ISSHELF_UI_TEST_1 " + std::string(session) + " 18446744073709551616 4 snapshot\n",
        "ISSHELF_UI_TEST_1 " + std::string(session) + " 1 18446744073709551616 snapshot\n",
        "ISSHELF_UI_TEST_1 " + std::string(session) + " 1 4  snapshot\n",
        std::string(180, 'x') + "\n"
    };
    auto zero = valid;
    zero[zero.size() - 2] = 0;
    invalid.push_back(zero);
    auto crlf = valid;
    crlf.insert(crlf.size() - 1, "\r");
    invalid.push_back(crlf);
    for (const auto& text : invalid) {
        write(request_file, text);
        check(!bridge.poll(request), "malformed, stale, replayable or unsupported commands never dispatch");
    }
    write(directory + "ui-test-request.new.txt", valid);
    check(!bridge.poll(request), "incomplete upload path is never read");
    check(std::remove(request_file.c_str()) == 0 &&
              std::rename((directory + "ui-test-request.new.txt").c_str(), request_file.c_str()) == 0,
          "fixture atomically publishes request");
    check(bridge.poll(request) && request.sequence == 1 && request.revision == 4 &&
              request.operation == UiTestOperation::snapshot, "exact next snapshot is accepted");
    check(!bridge.poll(request), "retained request never dispatches a second time");
    state.page = Page::shortcut_route;
    state.shortcut_route_system = find_system("ps2");
    check(bridge.publish(request, 4, "ok", false, true, state, false),
          "route-choice snapshot publishes through the existing bridge");
    contains(read(response), "\"page\":6", "route-choice page has its appended bridge value");
    state.page = Page::home;

    write(request_file, command(session, 2, 4, "right"));
    check(bridge.poll(request) && request.sequence == 2 && request.action == Action::right &&
              request.operation == UiTestOperation::action, "one discrete action is parsed");
    const auto pixels = image();
    write(bridge.frame_path(), pixels.data(), pixels.size());
    check(bridge.publish(request, 5, "applied", true, true, state, true), "matching PNG response publishes");
    json = read(response);
    contains(json, "\"nextSequence\":3", "command sequence is consumed before response");
    contains(json, "\"frameAvailable\":true", "complete validated PNG is available");
    contains(json, "\"actionApplied\":true", "successful action records applied outcome");
    const auto hash_field = "\"frameFNV64\":\"" + fnv(pixels) + "\"";
    contains(json, hash_field.c_str(), "response hash binds exact PNG bytes");
    check(read(published_frame) == std::string(reinterpret_cast<const char*>(pixels.data()), pixels.size()) &&
              !exists(bridge.frame_path()), "image is renamed completely before response");
    check(!bridge.poll(request), "response publication does not make action replayable");
    for (const auto& broken : {std::vector<unsigned char>(pixels.begin(), pixels.end() - 1), image(1921)}) {
        write(bridge.frame_path(), broken.data(), broken.size());
        check(bridge.publish(request, 5, "applied", true, false, state, true),
              "capture failure still publishes independent action result");
        json = read(response);
        contains(json, "\"actionApplied\":true", "failed capture does not deny an already applied action");
        contains(json, "\"settingsSaved\":false", "persistence status is independent");
        contains(json, "\"frameAvailable\":false", "incomplete or oversized frame is unavailable");
        contains(json, "\"frameBytes\":0", "failed capture has zero image size");
        contains(json, "\"frameFNV64\":\"0000000000000000\"", "failed capture carries no stale digest");
        check(!exists(published_frame) && !exists(bridge.frame_path()), "failed image cannot reuse stale frame");
    }
    auto corrupt = pixels;
    corrupt[corrupt.size() - 1] ^= 1;
    write(bridge.frame_path(), corrupt.data(), corrupt.size());
    check(bridge.publish(request, 5, "applied", true, true, state, true), "corrupt CRC returns response");
    contains(read(response), "\"frameAvailable\":false", "corrupt CRC is rejected");
    write(bridge.frame_path(), pixels.data(), pixels.size());
    check(bridge.publish(request, 5, "capture-failed", true, true, state, false), "explicit capture failure publishes");
    check(!exists(bridge.frame_path()) && !exists(published_frame), "explicit failed capture cannot expose previous files");
    const auto safe_response = read(response);
    check(!bridge.publish(request, 5, "bad\"result", true, true, state, false) && read(response) == safe_response,
          "untrusted response labels cannot inject JSON or replace response");

    std::uint64_t sequence = 3;
    constexpr const char* names[] = {"up", "down", "left", "right", "confirm", "back", "settings", "layout",
                                      "favorite", "previous-tab", "tab"};
    constexpr Action actions[] = {Action::up, Action::down, Action::left, Action::right, Action::confirm,
                                   Action::back, Action::settings, Action::layout, Action::favorite,
                                   Action::previous_tab, Action::next_tab};
    for (unsigned i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i) {
        write(request_file, command(session, sequence, 0, names[i]));
        check(bridge.poll(request) && request.sequence == sequence++ && request.revision == 0 &&
                  request.action == actions[i], "allowlist maps exactly one action and preserves revision for caller");
        check(!bridge.poll(request), "each allowlisted action is consumed once");
    }
    write(request_file, command(session, sequence, 5, "launch-confirm"));
    check(bridge.poll(request) && request.operation == UiTestOperation::launch_confirm &&
              request.sequence == sequence++ && request.revision == 5,
          "launch confirmation is a separate nonce-bound one-shot operation");
    check(!bridge.poll(request), "launch confirmation cannot dispatch twice");
    write(request_file, command(session, sequence, 5, "probe-paths"));
    check(bridge.poll(request) && request.operation == UiTestOperation::probe_paths &&
              request.sequence == sequence++ && request.revision == 5,
          "probe path request is session/sequence/revision bound and consumed once");
    check(!bridge.poll(request), "retained path probe cannot dispatch twice");
    check(!bridge.publish(request, 5, "probe-recorded", true, true, state, false),
          "probe recorded cannot be published without receipt byte evidence");
    check(bridge.publish(request, 5, "probe-unknown", true, true, state, false),
          "uncertain probe is acknowledged without inventing a complete record");
    write(request_file, command(session, sequence, 5, "probe-writes-v4"));
    check(bridge.poll(request) && request.operation == UiTestOperation::probe_writes_v4 &&
              request.sequence == sequence++ && request.revision == 5,
          "write probe is a separate nonce-bound one-shot action");
    check(!bridge.poll(request), "retained write probe cannot dispatch twice");
    check(!bridge.publish(request, 5, "probe-recorded", true, true, state, false),
          "write probe completion requires bound receipt byte evidence");
    check(bridge.publish(request, 5, "probe-unknown", true, true, state, false),
          "uncertain write probe remains unknown");
    write(request_file, command(session, sequence, 5, "probe-app-source"));
    check(bridge.poll(request) && request.operation == UiTestOperation::probe_app_source &&
              request.sequence == sequence++, "app-owned source action is nonce-bound and consumed once");
    check(!bridge.poll(request), "retained app-owned source action cannot run twice");
    check(!bridge.publish(request, 5, "source-recorded", true, true, state, false),
          "app-owned source completion requires bound receipt bytes");
    check(bridge.publish(request, 5, "source-held", true, true, state, false),
          "partial app-owned source attempt remains explicitly held");
    check(bridge.publish(request, 5, "source-recorded", true, true, state, false,
                         180, 0x7fe40d7566a8bd11ull),
          "app-owned source record publishes only with bounded checksum evidence");
    contains(read(response), "\"action\":\"probe-app-source\"", "source response names its action");
    contains(read(response), "\"probeBytes\":180", "source response binds receipt byte count");
    write(request_file, command(session, sequence, 5, "probe-ps2-source"));
    check(bridge.poll(request) && request.operation == UiTestOperation::probe_ps2_source &&
              request.sequence == sequence++, "protected PS2 source action is session-bound and consumed once");
    check(!bridge.poll(request), "retained protected PS2 source action cannot run twice");
    check(!bridge.publish(request, 5, "ps2-source-readable", false, true, state, false),
          "readable PS2 result requires a bounded identity report");
    UiTestPs2SourceReport ps2_report;
    ps2_report.complete = true;
    ps2_report.uid = 1;
    ps2_report.euid = 0;
    ps2_report.observed_nodes = ps2_report.failed_node = 8;
    ps2_report.files_read = 4;
    ps2_report.nodes[0] = {163, 1, 0, 0, 0040777, 512};
    ps2_report.nodes[1] = {215, 2, 0, 0, 0040755, 512};
    check(!bridge.publish(request, 5, "ps2-source-readable", true, true, state, false,
                          0, 0, nullptr, &ps2_report),
          "read-only protected PS2 action cannot claim applied mutation");
    check(bridge.publish(request, 5, "ps2-source-readable", false, true, state, false,
                         0, 0, nullptr, &ps2_report),
          "complete protected PS2 observation publishes only read-only identity data");
    json = read(response);
    contains(json, "\"action\":\"probe-ps2-source\"", "PS2 response names its action");
    contains(json, "\"actionApplied\":false", "PS2 observation never claims a write");
    contains(json, "\"ps2Source\":{\"complete\":true", "PS2 report is present");
    contains(json, "\"uid\":1,\"euid\":0", "PS2 report records app process identity");
    contains(json, "\"device\":215,\"inode\":2", "PS2 report records protected mount identity");
    const auto ps2_safe_response = json;
    ps2_report.failure = "bad\"failure";
    check(!bridge.publish(request, 5, "ps2-source-unavailable", false, true, state, false,
                          0, 0, nullptr, &ps2_report) && read(response) == ps2_safe_response,
          "PS2 failure label cannot inject response JSON");
    ps2_report.complete = false;
    ps2_report.failure = "open";
    ps2_report.observed_nodes = 3;
    ps2_report.failed_node = 3;
    ps2_report.failed_pass = 1;
    ps2_report.system_error = 13;
    ps2_report.files_read = 0;
    check(!bridge.publish(request, 5, "ps2-source-readable", false, true, state, false,
                          0, 0, nullptr, &ps2_report),
          "incomplete PS2 read cannot be labeled readable");
    check(bridge.publish(request, 5, "ps2-source-unavailable", false, true, state, false,
                         0, 0, nullptr, &ps2_report),
          "partial protected PS2 observation reports its failure without a write");
    json = read(response);
    contains(json, "\"observedNodes\":3,\"failedNode\":3,\"failedPass\":1,\"systemError\":13",
             "partial PS2 observation identifies its bounded failure");
    write(request_file, command(session, sequence, 5, "probe-ps2-direct"));
    check(bridge.poll(request) && request.operation == UiTestOperation::probe_ps2_direct &&
              request.sequence == sequence++, "direct PS2 action is session-bound and consumed once");
    check(!bridge.poll(request), "retained direct PS2 action cannot run twice");
    check(!bridge.publish(request, 5, "ps2-direct-matched", false, true, state, false),
          "direct PS2 match requires a report");
    UiTestPs2DirectReport direct_report;
    direct_report.complete = direct_report.target_opened = direct_report.content_matched =
        direct_report.mount_distinct = true;
    direct_report.uid = direct_report.euid = 1;
    direct_report.bytes_read = direct_report.target_size = 676;
    direct_report.root_device = 9;
    direct_report.system_device = direct_report.target_device = 7;
    direct_report.target_inode = 44;
    direct_report.target_mode = 0100644;
    check(!bridge.publish(request, 5, "ps2-direct-matched", true, true, state, false,
                          0, 0, nullptr, nullptr, &direct_report),
          "direct PS2 observation cannot claim mutation");
    check(bridge.publish(request, 5, "ps2-direct-matched", false, true, state, false,
                         0, 0, nullptr, nullptr, &direct_report),
          "direct PS2 match publishes bounded identities");
    json = read(response);
    contains(json, "\"ps2Direct\":{\"complete\":true", "direct report is present");
    contains(json, "\"bytesRead\":676,\"rootDevice\":9,\"systemDevice\":7",
             "direct report records exact file count and mount distinction");
    direct_report.complete = direct_report.mount_distinct = false;
    direct_report.failure = "mount-alias";
    direct_report.system_device = 9;
    check(!bridge.publish(request, 5, "ps2-direct-matched", false, true, state, false,
                          0, 0, nullptr, nullptr, &direct_report),
          "matching bytes through an alias cannot be labeled a direct match");
    check(bridge.publish(request, 5, "ps2-direct-unavailable", false, true, state, false,
                         0, 0, nullptr, nullptr, &direct_report),
          "matching bytes through an alias remain an unavailable observation");
    write(request_file, command(session, sequence, 5, "probe-route-store"));
    check(bridge.poll(request) && request.operation == UiTestOperation::probe_route_store &&
              request.sequence == sequence++, "route-store observation is session-bound and consumed once");
    check(!bridge.poll(request), "retained route-store observation cannot run twice");
    check(!bridge.publish(request, 5, "route-store-missing", false, true, state, false),
          "route-store observation requires its report");
    UiTestRouteStoreReport route_report;
    route_report.root = "missing";
    route_report.uid = route_report.euid = 1;
    route_report.system_error = 2;
    check(!bridge.publish(request, 5, "route-store-missing", true, true, state, false,
                          0, 0, nullptr, nullptr, nullptr, &route_report),
          "read-only route-store observation cannot claim mutation");
    check(!bridge.publish(request, 5, "route-store-present", false, true, state, false,
                          0, 0, nullptr, nullptr, nullptr, &route_report),
          "route-store result cannot contradict the root status");
    check(bridge.publish(request, 5, "route-store-missing", false, true, state, false,
                         0, 0, nullptr, nullptr, nullptr, &route_report),
          "missing route-store root publishes bounded read-only identity");
    json = read(response);
    contains(json, "\"routeStore\":{\"root\":\"missing\",\"uid\":1,\"euid\":1,\"systemError\":2}",
             "route-store report records missing root and process identity");
    route_report.root = "unsafe\"json";
    check(!bridge.publish(request, 5, "route-store-unsafe", false, true, state, false,
                          0, 0, nullptr, nullptr, nullptr, &route_report),
          "route-store root cannot inject response JSON");
    route_report.root = "present";
    route_report.system_error = 0;
    check(bridge.publish(request, 5, "route-store-present", false, true, state, false,
                         0, 0, nullptr, nullptr, nullptr, &route_report),
          "present route-store root can be reported without reading a preference");
    write(request_file, command(session, sequence, 5, "probe-route-path"));
    check(bridge.poll(request) && request.operation == UiTestOperation::probe_route_path &&
              request.sequence == sequence++, "route-path diagnostic is session-bound and consumed once");
    check(!bridge.poll(request), "retained route-path diagnostic cannot run twice");
    check(!bridge.publish(request, 5, "route-path-observed", false, true, state, false),
          "route-path diagnostic requires its report");
    tile_route::RoutePathReport path_report;
    path_report.uid = path_report.euid = 1;
    path_report.parent_before = {true, false, 1, 0, 0, 0, 0};
    check(!bridge.publish(request, 5, "route-path-observed", true, true, state, false,
                          0, 0, nullptr, nullptr, nullptr, nullptr, &path_report),
          "read-only route-path diagnostic cannot claim mutation");
    check(bridge.publish(request, 5, "route-path-observed", false, true, state, false,
                         0, 0, nullptr, nullptr, nullptr, nullptr, &path_report),
          "route-path diagnostic publishes bounded call results");
    contains(read(response), "\"routePath\":{\"uid\":1,\"euid\":1,\"parentBefore\":",
             "route-path report records process and parent identity");
    contains(read(response), "\"parentOpens\":[{\"name\":\"base-nonblock\"",
             "route-path report names its fixed open stages");
    check(read(response).size() <= 4096, "route-path response stays within client transfer bound");
    contains(read(response), "\"routeStore\":null", "route-path action cannot carry route-store result");
    write(request_file, command(session, sequence, 5, "probe-settings-path"));
    check(bridge.poll(request) && request.operation == UiTestOperation::probe_settings_path &&
              request.sequence == sequence++, "settings-path diagnostic is session-bound and consumed once");
    check(!bridge.poll(request), "retained settings-path diagnostic cannot run twice");
    check(!bridge.publish(request, 5, "settings-path-observed", false, true, state, false),
          "settings-path diagnostic requires its report");
    UiTestRequest wrong_settings_action = request;
    wrong_settings_action.operation = UiTestOperation::snapshot;
    check(!bridge.publish(wrong_settings_action, 5, "settings-path-observed", false, true, state, false),
          "settings-path result cannot be attached to another action");
    tile_route::SettingsPathReport settings_report;
    settings_report.uid = settings_report.euid = 1;
    settings_report.parent_open.flags = 0x00120104u;
    settings_report.parent_open.attempted = true;
    settings_report.parent_open.open_error = 1;
    for (auto& item : settings_report.files) {
        item.before.attempted = item.after.attempted = true;
        item.before.error = item.after.error = 1;
        item.direct.flags = 0x00100104u;
        item.direct.attempted = true;
        item.direct.open_error = 1;
    }
    check(!bridge.publish(request, 5, "settings-path-observed", true, true, state, false,
                          0, 0, nullptr, nullptr, nullptr, nullptr, nullptr, &settings_report),
          "read-only settings-path diagnostic cannot claim mutation");
    check(bridge.publish(request, 5, "settings-path-observed", false, true, state, false,
                         0, 0, nullptr, nullptr, nullptr, nullptr, nullptr, &settings_report),
          "settings-path diagnostic publishes bounded call results");
    contains(read(response), "\"settingsPath\":{\"uid\":1,\"euid\":1,\"parentOpen\":",
             "settings-path report records process and parent open result");
    contains(read(response), "\"files\":[{\"name\":\"settings.ini\"",
             "settings-path report names only the fixed settings files");
    check(read(response).size() <= 4096, "settings-path response stays within client transfer bound");
    contains(read(response), "\"routePath\":null", "settings-path action cannot carry route-path data");
    write(request_file, command(session, sequence, 5, "create-n64-source"));
    check(bridge.poll(request) && request.operation == UiTestOperation::create_n64_source &&
              request.sequence == sequence++, "N64 source-only action is nonce-bound and consumed once");
    check(!bridge.poll(request), "retained N64 source action cannot run twice");
    check(!bridge.publish(request, 5, "console-source-recorded", true, true, state, false),
          "N64 source completion requires bound receipt bytes");
    check(bridge.publish(request, 5, "console-source-held", true, true, state, false),
          "partial N64 source attempt remains held");
    check(bridge.publish(request, 5, "console-source-recorded", true, true, state, false,
                         290, 0x7fe40d7566a8bd11ull),
          "N64 source-only receipt requires checksum evidence");
    contains(read(response), "\"action\":\"create-n64-source\"", "N64 response names its action");
    contains(read(response), "\"ps2Source\":null", "PS2 identities are absent from another action");
    write(request_file, command(session, sequence, 5, "probe-job-root"));
    check(bridge.poll(request) && request.operation == UiTestOperation::probe_job_root &&
              request.sequence == sequence++, "read-only job root action is distinct and consumed once");
    tile_job::JobRootProbeResult root_report;
    root_report.ran = root_report.parent_valid = root_report.complete = true;
    root_report.uid = 101;
    root_report.euid = 202;
    root_report.parent = {7, 19, 0, 0777u, true, false};
    root_report.child_lookup_errno = 2;
    check(!bridge.publish(request, 5, "job-root-observed", true, true, state, false),
          "job root observation cannot publish without a bounded result");
    check(bridge.publish(request, 5, "unsupported", false, true, state, false),
          "disabled job-root action still publishes an acknowledgement");
    contains(read(response), "\"result\":\"unsupported\"", "disabled job-root result is explicit");
    contains(read(response), "\"jobRoot\":null", "disabled job-root has no identity report");
    check(bridge.publish(request, 5, "job-root-observed", true, true, state, false,
                         0, 0, &root_report), "read-only identity result publishes");
    json = read(response);
    contains(json, "\"uid\":101,\"euid\":202", "app UID and EUID are exposed");
    contains(json, "\"parentDevice\":7,\"parentInode\":19", "checked parent identity is exposed");
    contains(json, "\"mkdirAttempted\":false", "read-only action records no mkdir");
    write(request_file, command(session, sequence, 5, "create-job-root"));
    check(bridge.poll(request) && request.operation == UiTestOperation::create_job_root &&
              request.sequence == sequence++, "child create action is separately selected and consumed once");
    root_report.child = {7, 30, 202, 0700u, true, false};
    root_report.child_seen = root_report.child_valid = root_report.mkdir_attempted =
        root_report.created = true;
    root_report.child_lookup_errno = 0;
    check(bridge.publish(request, 5, "job-root-created", true, true, state, false,
                         0, 0, &root_report), "bounded post-identity result publishes");
    json = read(response);
    contains(json, "\"childOwner\":202,\"childMode\":448", "private child owner and mode are exposed");
    contains(json, "\"mkdirAttempted\":true,\"mkdirErrno\":0", "attempt and errno are explicit");
    write(request_file, command(session, sequence, 5, "probe-writes"));
    check(!bridge.poll(request), "consumed v3 write action is no longer dispatchable");
    write(request_file, command(session, sequence, 5, "close"));
    check(bridge.poll(request) && request.operation == UiTestOperation::close &&
              request.sequence == sequence++ && request.revision == 5,
          "close is distinct from Back and carries the observed revision");
    check(!bridge.poll(request), "a retained close request cannot replay");
    check(bridge.publish(request, 6, "busy", false, true, state, false) && bridge.active(),
          "a refused close keeps the owner-enabled bridge available");
    write(request_file, command(session, sequence, 5, "stop"));
    check(bridge.poll(request) && request.operation == UiTestOperation::stop, "stop is distinct from UI Back");
    check(bridge.publish(request, 6, "stopped", false, true, state, false) && !bridge.active(),
          "stop publishes inactive state and deactivates transport");
    contains(read(response), "\"active\":false", "final response remains available after stop");
    check(!bridge.poll(request), "stopped session cannot dispatch");
    write(published_frame, "old-frame");
    check(bridge.start(root, other) && !exists(response) && !exists(published_frame),
          "new session makes previous response and frame unavailable");
    check(!bridge.poll(request), "old session request cannot act after reopen");
    write(request_file, command(other, 1, 0, "snapshot"));
    check(bridge.poll(request) && request.sequence == 1, "new process session starts a fresh sequence");
    bridge.stop();
    check(!bridge.poll(request) && bridge.publish(request, 0, "closed", false, true, state, false),
          "stopped transport rejects commands but can publish its final response");
    contains(read(response), "\"active\":false", "explicit stop is reflected in terminal response");
    for (const char* terminal : {"owner-stopped", "expired"}) {
        check(bridge.publish(request, 1, terminal, false, true, state, false),
              "owner cancellation and expiry publish after stop");
        const auto terminal_response = read(response);
        contains(terminal_response, "\"active\":false", "terminal status cannot reactivate transport");
        contains(terminal_response, terminal, "terminal status preserves its exact disposition");
        check(!bridge.poll(request), "terminal publication never reenables request dispatch");
    }
    check(bridge.start(root, session), "close receipt starts from a fresh owner-enabled session");
    write(request_file, command(session, 1, 7, "close"));
    check(bridge.poll(request) && request.operation == UiTestOperation::close,
          "fresh close command is accepted once");
    bridge.stop();
    check(bridge.publish(request, 8, "cleanup-complete", true, true, state, false),
          "cooperative close publishes a terminal response without requiring a frame");
    contains(read(response), "\"action\":\"close\"", "terminal receipt names the close command");
    contains(read(response), "\"actionApplied\":true", "terminal receipt records completed cleanup");
    contains(read(response), "\"frameAvailable\":false", "terminal receipt cannot reuse a prior image");
    check(!bridge.poll(request), "closed session cannot replay its close request");
    check(!bridge.start(root, "invalid") && !bridge.publish(request, 0, "closed", false, true, state, false),
          "failed start cannot publish using an older session");
    write(request_file, "malformed-stale-request");
    check(bridge.start(root, session) && !bridge.poll(request),
          "a retained malformed old request stays inert after session start");
    write(request_file, command(session, 1, 0, "snapshot"));
    check(bridge.poll(request), "malformed old file cannot consume the fresh session sequence");
    bridge.stop();
    check(read(legacy_directory + "/response.json") == "preserved-failed-trial-evidence",
          "existing nested failed-trial evidence stays untouched through start, publish and stop");

    UiState home;
    check(apply_ui_test_action(home, Action::right) && home.focus == 1, "remote direction applies one menu step");
    check(apply_ui_test_action(home, Action::back) && !home.exit_requested && home.close_hint_requested,
          "remote Back never simulates a hold-to-close");
    check(apply_ui_test_action(home, Action::settings) && home.page == Page::settings,
          "ordinary UI transitions remain available");
    static Library library;
    const auto ps1 = find_system("ps1");
    library.route_count = 1;
    auto& route = library.routes[0];
    route.system = ps1;
    std::strcpy(route.id, "psswanstation");
    std::strcpy(route.title_id, "PPSA99248");
    std::strcpy(route.folder, "/data/homebrew/PPSA99248");
    library.scan.complete = true;
    UiState shortcut;
    shortcut.library = &library;
    shortcut.launch_connected = true;
    check(begin_console_tile_route_choice(shortcut, {ps1, ConsoleTileDestination::home, {}}) &&
              shortcut.page == Page::shortcut_route && shortcut.shortcut_route_focus == 1,
          "remote shortcut fixture enters the chooser on its installed emulator");
    check(apply_ui_test_action(shortcut, Action::down) && shortcut.shortcut_route_focus == 2 &&
              apply_ui_test_action(shortcut, Action::up) && shortcut.shortcut_route_focus == 1,
          "remote directions browse away from and back to the preferred emulator");
    check(!apply_ui_test_action(shortcut, Action::confirm) &&
              shortcut.page == Page::shortcut_route && !shortcut.launch_requested,
          "remote Confirm cannot launch an emulator from the route chooser");
    check(apply_ui_test_action(shortcut, Action::up) && shortcut.shortcut_route_focus == 0 &&
              apply_ui_test_action(shortcut, Action::confirm) && shortcut.page == Page::shortcut_route &&
              shortcut.route_save_requested && shortcut.route_save_apply_after &&
              shortcut.pending_route_save.destination == ConsoleTileDestination::library &&
              !shortcut.launch_requested,
          "remote Confirm may queue a Library choice but waits for saved readback");
    UiState launch;
    launch.library = &library;
    launch.launch_connected = true;
    launch.page = Page::details;
    launch.selected_system = ps1;
    launch.detail_focus = 1;
    std::strcpy(launch.notice, "kept-original-notice");
    check(!apply_ui_test_action(launch, Action::confirm) && !launch.launch_requested &&
              std::strcmp(launch.notice, "kept-original-notice") == 0,
          "emulator-menu launch attempt is refused without changing live state");
    check(apply_ui_test_launch_confirm(launch) && launch.launch_requested &&
              launch.pending_launch.mode == LaunchMode::emulator_menu &&
              std::strcmp(launch.pending_launch.title, "PPSA99248") == 0,
          "explicit one-shot confirmation admits only the selected emulator menu");
    UiState removal;
    removal.page = Page::console_tile;
    removal.selected_system = find_system("threedo");
    removal.tile_connected = true;
    removal.tile_removing[removal.selected_system] = true;
    removal.tile_removal_handoff[removal.selected_system] = true;
    removal.tile_removal_count = 2;
    removal.tile_focus = 0;
    std::strcpy(removal.tile_removal_titles, "PPSA99700,PPSA99701");
    check(!apply_ui_test_action(removal, Action::confirm) && !removal.launch_requested,
          "ordinary remote Confirm cannot start a queued-removal handoff");
    check(apply_ui_test_launch_confirm(removal) && removal.launch_requested &&
              removal.pending_launch.mode == LaunchMode::tile_remove &&
              std::strcmp(removal.pending_launch.title, "PPSA99700") == 0 &&
              std::strcmp(removal.pending_launch.rom, "PPSA99701") == 0,
          "explicit one-shot confirmation admits only the exact queued-removal handoff");
    UiState invalid_removal = removal;
    invalid_removal.launch_requested = false;
    invalid_removal.pending_launch = {};
    invalid_removal.tile_removal_count = 3;
    const auto invalid_before = invalid_removal;
    check(!apply_ui_test_launch_confirm(invalid_removal) &&
              !invalid_removal.launch_requested &&
              std::strcmp(invalid_removal.tile_removal_titles,
                          invalid_before.tile_removal_titles) == 0,
          "queued-removal confirmation refuses a mismatched count without changing live state");
    UiState residual_removal;
    residual_removal.page = Page::console_tile;
    residual_removal.selected_system = find_system("threedo");
    residual_removal.tile_connected = true;
    residual_removal.tile_removal_batch_ready = true;
    residual_removal.tile_residual_sweep = true;
    residual_removal.tile_focus = 0;
    std::string residual_titles;
    for (unsigned number = 99700; number < 99752; ++number) {
        char title[10];
        std::snprintf(title, sizeof(title), "PPSA%05u", number);
        if (!residual_titles.empty()) residual_titles.push_back(',');
        residual_titles += title;
    }
    check(queue_console_tile_removals(residual_removal,
                                       residual_titles.c_str(),
                                       residual_titles.size() + 1) &&
              apply_ui_test_launch_confirm(residual_removal) &&
              residual_removal.launch_requested &&
              residual_removal.pending_launch.mode == LaunchMode::tile_residual,
          "one-shot confirmation admits the complete canonical residual sweep");
    launch.launch_requested = false;
    library.game_count = 1;
    auto& game = library.games[0];
    game.system = ps1;
    game.kind = MediaKind::cue;
    std::strcpy(game.name, "Private test game");
    std::strcpy(game.path, "/data/PSXS5/games/private.cue");
    launch.page = Page::library;
    const auto direct_before = launch;
    check(!apply_ui_test_launch_confirm(launch) && !launch.launch_requested &&
              launch.page == direct_before.page,
          "one-shot menu confirmation cannot launch a selected game");
    check(!apply_ui_test_action(launch, Action::confirm) && !launch.launch_requested &&
              launch.page == direct_before.page && launch.game_focus == direct_before.game_focus &&
              std::strcmp(launch.notice, direct_before.notice) == 0,
          "selected PS1 direct launch is refused without changing live state");
    game.system = find_system("ps5");
    game.kind = MediaKind::native_title;
    std::strcpy(game.title_id, "PPSA12345");
    std::strcpy(game.path, "/user/app/PPSA12345");
    launch.selected_system = game.system;
    check(!apply_ui_test_action(launch, Action::confirm) && !launch.launch_requested,
          "native-title launch is also refused");
    launch.exit_requested = true;
    check(!apply_ui_test_action(launch, Action::right) && launch.exit_requested,
          "an already exiting state cannot be changed by remote input");
    char probe_request[96], probe_name[96], probe_request_path[128], probe_receipt_path[128];
    std::size_t probe_request_bytes = 0;
    check(console_tile_probe_request(session, probe_request, probe_request_bytes) &&
              std::string(probe_request, probe_request_bytes) ==
                  std::string("format=isshelf-console-tile-path-probe/2\nnonce=") + session + "\n" &&
              console_tile_probe_receipt_name(session, probe_name) &&
              std::string(probe_name) == std::string("console-tile-path-v2-") + session + ".txt",
          "path probe request and receipt names are fixed to the bridge session nonce");
    check(!console_tile_probe_nonce("../../different-file"), "path probe nonce cannot select a path");
    check(console_tile_probe_request_mode_allowed(0644u) &&
              console_tile_probe_request_mode_allowed(0444u) &&
              !console_tile_probe_request_mode_allowed(0600u) &&
              !console_tile_probe_request_mode_allowed(0666u) &&
              !console_tile_probe_request_mode_allowed(0646u),
          "probe request must be world-readable without group or world write permission");
    check(console_tile_probe_request_path(probe_request_path) &&
              std::string(probe_request_path) ==
                  "/data/console-shelf/console-tile-path-request-v2.txt" &&
              console_tile_probe_receipt_path(session, probe_receipt_path) &&
              std::string(probe_receipt_path) ==
                  std::string("/data/console-shelf/") + probe_name,
          "app probe files use only exact fixed absolute paths");
    char short_path[8];
    check(!console_tile_probe_request_path(short_path) &&
              !console_tile_probe_receipt_path(session, short_path) &&
              !console_tile_probe_receipt_path("../../different-file", probe_receipt_path),
          "path construction rejects truncation and invalid nonce");
    std::string body = std::string(probe_request, probe_request_bytes) +
                       "runtime=standard-sdk-v0.42\nrequest_unlinked=0\n";
    constexpr const char* probe_roots[] = {"/data/homebrew", "/user/app", "/user/appmeta"};
    constexpr const char* probe_paths[] = {
        "/data/homebrew/PPSA99541", "/data/homebrew/PPSA99704", "/user/app/PPSA99704",
        "/user/appmeta/PPSA99704", "/data/homebrew/PPSA99752", "/user/app/PPSA99752",
        "/user/appmeta/PPSA99752"};
    constexpr const char* probe_fields[] = {
        "lstat_errno", "lstat_type", "lstat_mode", "stat_errno", "stat_type", "stat_mode",
        "open_errno", "fstat_errno", "fstat_type", "fstat_mode", "fstat_same_lstat"};
    constexpr const char* directory_fields[] = {
        "dir_open_errno", "dir_read_errno", "dir_close_errno", "entries_seen", "complete",
        "seen_PPSA99541", "seen_PPSA99704", "seen_PPSA99752"};
    for (unsigned group = 0; group < 10; ++group) {
        const bool root_group = group < 3;
        const unsigned index = root_group ? group : group - 3;
        const std::string label = std::string(root_group ? "root_" : "path_") + std::to_string(index);
        body += label + "=" + (root_group ? probe_roots[index] : probe_paths[index]) + "\n";
        for (const char* field : probe_fields)
            body += label + "_" + field + "=" + (group == 2 && std::strcmp(field, "open_errno") == 0 ? "2" : "0") + "\n";
        if (root_group)
            for (const char* field : directory_fields)
                body += label + "_" + field + "=" +
                        (group != 2 && std::strcmp(field, "complete") == 0 ? "1" : "0") + "\n";
    }
    body += "title_root_write_attempted=0\ndatabase_open_attempted=0\n"
            "database_write_attempted=0\nnetwork_attempted=0\nrecord_complete=1\n";
    check(console_tile_probe_receipt_valid(body, session),
          "record shape is accepted even when root enumeration or open failed");
    check(!console_tile_probe_receipt_valid(body, other) &&
              !console_tile_probe_receipt_valid(body.substr(0, body.size() - 1), session),
          "wrong nonce or truncated receipt cannot be acknowledged");
    auto extra = body + "extra=1\n";
    check(!console_tile_probe_receipt_valid(extra, session), "extra receipt fields are refused");
    check(console_tile_probe_fnv(body) != console_tile_probe_fnv(extra), "receipt hash binds exact bytes");
    char write_request[96], write_receipt_path[128];
    std::size_t write_request_bytes = 0;
    check(console_tile_write_request(session, write_request, write_request_bytes) &&
              std::string(write_request, write_request_bytes) ==
                  std::string("format=isshelf-console-tile-write-probe/3\nnonce=") + session + "\n" &&
              console_tile_write_receipt_path(session, write_receipt_path) &&
              std::string(write_receipt_path) ==
                  std::string("/data/console-shelf/console-tile-write-v3-") + session + ".txt",
          "v3 request and receipt have distinct fixed nonce-bound paths");
    std::string write_body = std::string("format=isshelf-console-tile-write-probe/3\nnonce=") +
        session + "\nruntime=standard-sdk-v0.42\nscratch=/data/homebrew/.isshelf-write-probe-v3-" +
        session + "\nscratch_remaining=absent\nscratch_remaining_errno=2\n"
        "valid_title_id_write_attempted=0\nhomebrew_scratch_write_attempted=1\n"
        "homebrew_scratch_created=1\ndatabase_open_attempted=0\nnetwork_attempted=0\n";
    for (const char* stage : console_tile_write_stages)
        write_body += std::string("stage_") + stage + "_errno=0\n";
    write_body += "first_failure=none\nfirst_errno=0\ntransaction_complete=1\nrecord_complete=1\n";
    check(console_tile_write_receipt_valid(write_body, session), "v3 success requires every stage and cleanup");
    check(!console_tile_write_receipt_valid(write_body, other) &&
              !console_tile_write_receipt_valid(write_body.substr(0, write_body.size() - 1), session) &&
              !console_tile_write_receipt_valid(write_body + "extra=1\n", session),
          "v3 rejects wrong nonce, partial receipt and extra fields");
    char write_v4_request[96], write_v4_receipt_path[128];
    std::size_t write_v4_request_bytes = 0;
    check(console_tile_write_v4_request(session, write_v4_request, write_v4_request_bytes) &&
              std::string(write_v4_request, write_v4_request_bytes) ==
                  std::string("format=isshelf-console-tile-write-probe/4\nnonce=") + session + "\n" &&
              console_tile_write_v4_receipt_path(session, write_v4_receipt_path) &&
              std::string(write_v4_receipt_path) ==
                  std::string("/data/console-shelf/console-tile-write-v4-") + session + ".txt",
          "v4 request and receipt are distinct from consumed v3 paths");
    std::string write_v4_body = std::string("format=isshelf-console-tile-write-probe/4\nnonce=") +
        session + "\nruntime=standard-sdk-v0.42\nscratch=/data/homebrew/.isshelf-write-probe-v4-" +
        session + "\nscratch_remaining=absent\nscratch_remaining_errno=2\n"
        "valid_title_id_write_attempted=0\nhomebrew_scratch_write_attempted=1\n"
        "homebrew_scratch_created=1\ndatabase_open_attempted=0\nnetwork_attempted=0\n";
    for (const char* stage : console_tile_write_v4_stages)
        write_v4_body += std::string("stage_") + stage + "_errno=0\n";
    write_v4_body += "first_failure=none\nfirst_errno=0\ntransaction_complete=1\nrecord_complete=1\n";
    check(console_tile_write_v4_receipt_valid(write_v4_body, session) &&
              !console_tile_write_v4_receipt_valid(write_body, session) &&
              !console_tile_write_receipt_valid(write_v4_body, session),
          "v4 receipt accepts only its own exact nonce-bound format");
    std::puts("PASS: private UI bridge bounded protocol, session/replay rejection, atomic frame receipts, capture failures and launch suppression.");
}
