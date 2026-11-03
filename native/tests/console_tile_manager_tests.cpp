// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/console_tile_manager.hpp"
#include "shelf/model.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace {
void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL console tile manager: %s\n", message); std::exit(1); }
}
void write(const fs::path& path, const std::string& data) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    check(static_cast<bool>(output), "create fixture file");
    output << data;
    check(static_cast<bool>(output), "write fixture file");
}
std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "open fixture file");
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}
struct FakeApi {
    std::map<std::string, std::string> registered;
    std::map<std::string, std::string> info_overrides;
    bool deferred = true;
    int scans = 0, uninstall_requests = 0;
    int version_status = 200;
    int manual_lists = 0, manual_removes = 0;
    bool manual_remove_changes = true;
    bool malformed_uninstall_receipt = false;
    bool game_active_uninstall = false;
    std::vector<std::string> manual_paths;
    std::string missing_body = "{\"status\":2}";
    std::string version_body =
        "{\"api_version\":1,\"status\":0,\"capabilities\":[\"game_info\",\"rescan\",\"uninstall_game\",\"list_manual_sources\",\"remove_manual_source\"]}";
};
int post(void* context, const char* route, const char* request, char* reply, std::size_t capacity) {
    auto& api = *static_cast<FakeApi*>(context);
    std::string body;
    int status = 200;
    if (std::strcmp(route, "/api/v1/version") == 0) {
        body = api.version_body;
        status = api.version_status;
    }
    else if (std::strcmp(route, "/api/v1/games/info") == 0) {
        const std::string query(request);
        const auto at = query.find("PPSA");
        check(at != std::string::npos && at + 9 <= query.size(), "bounded info query");
        const auto id = query.substr(at, 9);
        const auto override = api.info_overrides.find(id);
        const auto found = api.registered.find(id);
        if (override != api.info_overrides.end()) body = override->second;
        else if (found == api.registered.end()) { status = 404; body = api.missing_body; }
        else body = "{\"status\":0,\"title_id\":\"" + id + "\",\"path\":\"" + found->second +
                    "\",\"installed\":true}";
    } else if (std::strcmp(route, "/api/v1/scan") == 0) {
        ++api.scans;
        body = api.deferred ? "{\"status\":0,\"scan_queued\":true,\"scan_deferred\":true}"
                            : "{\"status\":0,\"scan_queued\":true,\"scan_deferred\":false}";
    } else if (std::strcmp(route, "/api/v1/games/uninstall") == 0) {
        const std::string query(request);
        const auto at = query.find("PPSA");
        check(at != std::string::npos && at + 9 <= query.size(), "bounded uninstall query");
        const auto id = query.substr(at, 9);
        ++api.uninstall_requests;
        if (api.game_active_uninstall) {
            status = 409;
            body = "{\"status\":16,\"error\":\"a game is active or awaiting exit; close the game and try again\",\"error_reason\":\"game_active\"}";
        } else if (!api.malformed_uninstall_receipt) {
            api.registered.erase(id);
            body = "{\"status\":0,\"title_id\":\"" + id +
                   "\",\"uninstall_requested\":true}";
        } else {
            body = "{\"status\":0,\"title_id\":\"PPSA00000\",\"uninstall_requested\":true}";
        }
    } else if (std::strcmp(route, "/api/v1/manual/list") == 0) {
        ++api.manual_lists;
        body = "{\"status\":0,\"count\":" + std::to_string(api.manual_paths.size()) + ",\"paths\":[";
        for (std::size_t i = 0; i < api.manual_paths.size(); ++i) {
            if (i) body += ',';
            body += "\"" + api.manual_paths[i] + "\"";
        }
        body += "]}";
    } else if (std::strcmp(route, "/api/v1/manual/remove") == 0) {
        ++api.manual_removes;
        const std::string query(request), prefix = "{\"path\":\"";
        check(query.rfind(prefix, 0) == 0 && query.size() > prefix.size() + 2 &&
                  query.substr(query.size() - 2) == "\"}", "bounded manual remove query");
        const auto path = query.substr(prefix.size(), query.size() - prefix.size() - 2);
        if (api.manual_remove_changes) {
            api.manual_paths.erase(std::remove(api.manual_paths.begin(), api.manual_paths.end(), path),
                                   api.manual_paths.end());
        }
        body = "{\"status\":0,\"path\":\"" + path +
               "\",\"present\":false,\"changed\":" +
               (api.manual_remove_changes ? "true" : "false") + "}";
    } else { status = 404; body = "{\"status\":2}"; }
    check(body.size() < capacity, "fake response bound");
    std::memcpy(reply, body.c_str(), body.size() + 1);
    return status;
}
struct CatalogFake {
    enum class Info { missing, installed, pending, wrong_title, malformed } info = Info::missing;
    enum class Manual { empty, exact, other_source, duplicate, malformed, wrong_count } manual = Manual::empty;
    std::string version = "{\"api_version\":1,\"status\":0,\"capabilities\":[\"game_info\",\"list_manual_sources\"]}";
    std::string requested_title;
    int version_calls = 0, info_calls = 0, manual_calls = 0;
};
int catalog_post(void* context, const char* route, const char* request,
                 char* reply, std::size_t capacity) {
    auto& api = *static_cast<CatalogFake*>(context);
    std::string body;
    int status = 200;
    if (std::strcmp(route, "/api/v1/version") == 0) {
        ++api.version_calls;
        body = api.version;
    } else if (std::strcmp(route, "/api/v1/games/info") == 0) {
        ++api.info_calls;
        const std::string query(request);
        const auto at = query.find("PPSA");
        check(at != std::string::npos && at + 9 <= query.size(), "catalog info pins a title ID");
        api.requested_title = query.substr(at, 9);
        if (api.info == CatalogFake::Info::missing) {
            status = 404;
            body = "{\"status\":2,\"error\":\"No such file or directory\"}";
        } else if (api.info == CatalogFake::Info::malformed) {
            body = "{";
        } else {
            const auto title = api.info == CatalogFake::Info::wrong_title ? "PPSA00000" : api.requested_title;
            const auto path = "/system_data/isshelf-tiles/test/" + api.requested_title;
            body = "{\"status\":0,\"title_id\":\"" + title + "\",\"path\":\"" + path +
                   "\",\"installed\":" + (api.info == CatalogFake::Info::pending ? "false" : "true") + "}";
        }
    } else if (std::strcmp(route, "/api/v1/manual/list") == 0) {
        ++api.manual_calls;
        const auto exact = "/system_data/isshelf-tiles/test/" + api.requested_title;
        const auto other = "/different/source/" + api.requested_title;
        switch (api.manual) {
        case CatalogFake::Manual::empty: body = "{\"status\":0,\"count\":0,\"paths\":[]}"; break;
        case CatalogFake::Manual::exact:
            body = "{\"status\":0,\"count\":1,\"paths\":[\"" + exact + "\"]}"; break;
        case CatalogFake::Manual::other_source:
            body = "{\"status\":0,\"count\":1,\"paths\":[\"" + other + "\"]}"; break;
        case CatalogFake::Manual::duplicate:
            body = "{\"status\":0,\"count\":2,\"paths\":[\"" + exact + "\",\"" + exact + "\"]}"; break;
        case CatalogFake::Manual::malformed: body = "{"; break;
        case CatalogFake::Manual::wrong_count:
            body = "{\"status\":0,\"count\":2,\"paths\":[\"" + exact + "\"]}"; break;
        }
    } else {
        check(false, "catalog inspection uses only read-only ShadowMount routes");
    }
    check(body.size() < capacity, "catalog fake response bound");
    std::memcpy(reply, body.c_str(), body.size() + 1);
    return status;
}
void catalog_read_tests() {
    using namespace shelf;
    CatalogFake api;
    ConsoleTileConfig config;
    config.bundle_root = nullptr; // Status reads must not require write-side assets.
    config.post = catalog_post;
    config.post_context = &api;
    ConsoleTileManager manager(config);
    check(manager.catalog_read_availability().ready(), "catalog status reader checks only read capabilities");
    for (std::size_t system = 0; system < system_count; ++system) {
        const auto result = manager.inspect_catalog_title(system);
        char expected[10]{};
        std::snprintf(expected, sizeof(expected), "PPSA%05zu", 99700 + system);
        check(std::strcmp(result.title_id, expected) == 0 &&
                  api.requested_title == expected && !result.owned &&
                  result.stage == ConsoleTileStage::unavailable &&
                  std::strstr(result.message, "source and Home checks") != nullptr,
              "all 52 fixed titles report only bounded API absence, never CREATE admission");
    }
    check(manager.inspect_catalog_title(system_count).stage == ConsoleTileStage::failed,
          "out-of-catalog title is rejected before API contact");
    const auto ps2 = find_system("ps2");
    api.info = CatalogFake::Info::installed;
    api.manual = CatalogFake::Manual::exact;
    auto result = manager.inspect_catalog_title(ps2);
    check(result.stage == ConsoleTileStage::registered && !result.owned &&
              std::strstr(result.message, "verify ownership") != nullptr,
          "installed manual title is observed without claiming ownership or Home visibility");
    api.manual = CatalogFake::Manual::empty;
    result = manager.inspect_catalog_title(ps2);
    check(result.stage == ConsoleTileStage::registered && !result.owned &&
              std::strstr(result.message, "tracking is not shown") != nullptr,
          "installed title without manual tracking remains observed but unowned");
    api.manual = CatalogFake::Manual::other_source;
    result = manager.inspect_catalog_title(ps2);
    check(result.stage == ConsoleTileStage::conflict && !result.owned,
          "different manual source with same title basename is a conflict");
    api.manual = CatalogFake::Manual::duplicate;
    check(manager.inspect_catalog_title(ps2).stage == ConsoleTileStage::unavailable,
          "duplicate manual paths fail the observation");
    api.manual = CatalogFake::Manual::wrong_count;
    check(manager.inspect_catalog_title(ps2).stage == ConsoleTileStage::unavailable,
          "manual count mismatch fails the observation");
    api.manual = CatalogFake::Manual::malformed;
    check(manager.inspect_catalog_title(ps2).stage == ConsoleTileStage::unavailable,
          "malformed manual response fails the observation");
    api.manual = CatalogFake::Manual::exact;
    api.info = CatalogFake::Info::pending;
    check(manager.inspect_catalog_title(ps2).stage == ConsoleTileStage::pending,
          "known source without installed status is pending");
    api.info = CatalogFake::Info::missing;
    check(manager.inspect_catalog_title(ps2).stage == ConsoleTileStage::pending,
          "manual source with missing title record remains unresolved");
    api.info = CatalogFake::Info::wrong_title;
    check(manager.inspect_catalog_title(ps2).stage == ConsoleTileStage::unavailable,
          "wrong title in game info is rejected");
    api.info = CatalogFake::Info::malformed;
    check(manager.inspect_catalog_title(ps2).stage == ConsoleTileStage::unavailable,
          "malformed game info is rejected");
    api.version = "{\"api_version\":1.5,\"status\":0,\"capabilities\":[\"game_info\",\"list_manual_sources\"]}";
    check(manager.catalog_read_availability().reason == ConsoleTileAvailabilityReason::api_version,
          "fractional API version is rejected");
    api.version = "{\"api_version\":1,\"status\":0,\"capabilities\":[\"game_info\"]}";
    check(manager.catalog_read_availability().reason == ConsoleTileAvailabilityReason::api_missing_capability,
          "manual-list capability is required");
}
void loopback_probe(const shelf::ConsoleTileConfig& config) {
#ifdef _WIN32
    WSADATA winsock{};
    check(WSAStartup(MAKEWORD(2, 2), &winsock) == 0, "start loopback test sockets");
    using Socket = SOCKET;
    constexpr Socket invalid = INVALID_SOCKET;
#else
    using Socket = int;
    constexpr Socket invalid = -1;
#endif
    const auto close_socket = [](Socket fd) {
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
    };
    const Socket listener = socket(AF_INET, SOCK_STREAM, 0);
    check(listener != invalid, "create loopback test socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    check(bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
              listen(listener, 1) == 0,
          "bind loopback test socket");
#ifdef _WIN32
    int address_length = sizeof(address);
#else
    socklen_t address_length = sizeof(address);
#endif
    check(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_length) == 0,
          "discover loopback test port");
    const unsigned short port = ntohs(address.sin_port);
    bool answered = false;
    std::thread server([&]() {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        timeval timeout{5, 0};
#ifdef _WIN32
        const int ready = select(0, &readable, nullptr, nullptr, &timeout);
#else
        const int ready = select(listener + 1, &readable, nullptr, nullptr, &timeout);
#endif
        if (ready == 1) {
            const Socket client = accept(listener, nullptr, nullptr);
            if (client != invalid) {
                char request[512];
                const int received = static_cast<int>(recv(client, request, sizeof(request), 0));
                if (received > 0) {
                    const std::string body =
                        "{\"api_version\":1,\"status\":0,\"capabilities\":[\"game_info\",\"rescan\",\"uninstall_game\",\"list_manual_sources\",\"remove_manual_source\"]}";
                    const std::string reply = "HTTP/1.1 200 OK\r\nContent-Length: " +
                                              std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                    std::size_t sent = 0;
                    while (sent < reply.size()) {
                        const int used = static_cast<int>(send(client, reply.data() + sent,
                                                               static_cast<int>(reply.size() - sent), 0));
                        if (used <= 0) break;
                        sent += static_cast<std::size_t>(used);
                    }
                    answered = sent == reply.size();
                }
                close_socket(client);
            }
        }
        close_socket(listener);
    });
    auto local = config;
    local.post = nullptr;
    local.post_context = nullptr;
    local.api_port = port;
    const auto diagnostic = shelf::ConsoleTileManager(local).availability();
    server.join();
    check(answered && diagnostic.ready(), "real loopback HTTP framing reaches the version parser");
#ifdef _WIN32
    WSACleanup();
#endif
}
} // namespace

int main() {
    using namespace shelf;
    catalog_read_tests();
    const auto unique = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("isshelf-console-tile-manager-tests-" + std::to_string(unique));
    check(!fs::exists(root), "unique test root");
    fs::create_directories(root);
    const auto homebrew = root / "homebrew";
    const auto bundle = root / "bundle";
    const auto user_app = root / "user-app";
    const auto user_appmeta = root / "user-appmeta";
    const auto retired = root / "retired";
    for (const auto& dir : {homebrew, bundle, user_app, user_appmeta, retired}) fs::create_directories(dir);
    write(bundle / "template/eboot.bin", "fixture eboot");
    write(bundle / "template/sce_module/libc.prx", "fixture runtime");
    write(bundle / "LICENSE-forwarder.txt", "fixture GPL license");
    write(bundle / "catalog.json", "fixture catalog");
    write(bundle / "template/param.json",
          "{\"titleId\":\"PPSA99100\",\"conceptId\":\"99100\",\"contentId\":\"base\","
          "\"localizedParameters\":{\"en-US\":{\"titleName\":\"Forwarder\"}}}");
    write(bundle / "icons/ps5.png", "fixture icon");
    write(bundle / "icons/ps4.png", "fixture icon");
    write(bundle / "icons/ps1.png", "fixture icon");
    FakeApi api;
    const auto hb = homebrew.generic_string(), assets = bundle.generic_string(), app = user_app.generic_string();
    const auto appmeta = user_appmeta.generic_string(), retired_path = retired.generic_string();
    ConsoleTileConfig config;
    config.homebrew_root = hb.c_str(); config.bundle_root = assets.c_str();
    config.bundle_fallback_root = nullptr;
    config.user_app_root = app.c_str(); config.user_appmeta_root = appmeta.c_str();
    config.retired_root = retired_path.c_str(); config.post = post; config.post_context = &api;
    config.test_skip_integrity = true;
    ConsoleTileManager manager(config);
    check(manager.available(), "pinned bundle and API are available");
    check(manager.availability().ready() && manager.availability().reason == ConsoleTileAvailabilityReason::ready,
          "availability reports ready with a bounded diagnostic");
    const auto parameter_file = bundle / "template/param.json";
    fs::rename(parameter_file, bundle / "template/param.saved");
    auto diagnostic = manager.availability();
    check(diagnostic.reason == ConsoleTileAvailabilityReason::bundle_asset_missing_or_unsafe &&
              std::strcmp(diagnostic.detail, "template/param.json") == 0 && !manager.available(),
          "missing bundle asset has a precise failure reason");
    fs::rename(bundle / "template/param.saved", parameter_file);
    config.test_skip_integrity = false;
    diagnostic = ConsoleTileManager(config).availability();
    check(diagnostic.reason == ConsoleTileAvailabilityReason::bundle_asset_integrity &&
              std::strcmp(diagnostic.detail, "template/eboot.bin") == 0,
          "changed bundle bytes fail the pinned digest with a precise asset name");
    config.test_skip_integrity = true;
    api.version_status = -1;
    diagnostic = manager.availability();
    check(diagnostic.reason == ConsoleTileAvailabilityReason::api_transport && !diagnostic.ready(),
          "loopback transport failure is separate from bundle failure");
    api.version_status = 503;
    diagnostic = manager.availability();
    check(diagnostic.reason == ConsoleTileAvailabilityReason::api_http_status && diagnostic.http_status == 503,
          "version HTTP status is retained for diagnostics");
    api.version_status = 200;
    api.version_body = "{";
    check(manager.availability().reason == ConsoleTileAvailabilityReason::api_malformed_response,
          "invalid API JSON has a separate reason");
    api.version_body = "{\"api_version\":1,\"status\":2,\"capabilities\":[]}";
    check(manager.availability().reason == ConsoleTileAvailabilityReason::api_status,
          "nonzero API status has a separate reason");
    api.version_body = "{\"api_version\":2,\"status\":0,\"capabilities\":[]}";
    check(manager.availability().reason == ConsoleTileAvailabilityReason::api_version,
          "unsupported API version has a separate reason");
    api.version_body = "{\"api_version\":1,\"status\":0}";
    check(manager.availability().reason == ConsoleTileAvailabilityReason::api_capabilities,
          "missing capabilities array has a separate reason");
    api.version_body = "{\"api_version\":1,\"status\":0,\"capabilities\":[\"game_info\",\"rescan\"]}";
    diagnostic = manager.availability();
    check(diagnostic.reason == ConsoleTileAvailabilityReason::api_missing_capability &&
              std::strcmp(diagnostic.detail, "uninstall_game") == 0,
          "missing capability is identified without logging server text");
    api.version_body =
        "{\"api_version\":1,\"status\":0,\"capabilities\":[\"game_info\",\"rescan\",\"uninstall_game\"]}";
    diagnostic = manager.availability();
    check(diagnostic.reason == ConsoleTileAvailabilityReason::api_missing_capability &&
              std::strcmp(diagnostic.detail, "list_manual_sources") == 0,
          "manual source list is required before enabling removal");
    api.version_body =
        "{\"api_version\":1,\"status\":0,\"capabilities\":[\"game_info\",\"rescan\",\"uninstall_game\",\"list_manual_sources\"]}";
    diagnostic = manager.availability();
    check(diagnostic.reason == ConsoleTileAvailabilityReason::api_missing_capability &&
              std::strcmp(diagnostic.detail, "remove_manual_source") == 0,
          "manual source removal is required before enabling removal");
    api.version_body =
        "{\"api_version\":1,\"status\":0,\"capabilities\":[\"game_info\",\"rescan\",\"uninstall_game\",\"list_manual_sources\",\"remove_manual_source\"]}";
    check(manager.availability().ready(), "availability recovers after transient API failure");
    loopback_probe(config);
    check(manager.inspect(find_system("ps1")).stage == ConsoleTileStage::absent, "new console has no tile");
    const auto saved_homebrew = root / "homebrew-saved";
    fs::rename(homebrew, saved_homebrew);
    check(manager.inspect(find_system("ps1")).stage == ConsoleTileStage::conflict,
          "missing active root cannot look like an absent owned tile");
    fs::rename(saved_homebrew, homebrew);
    const auto saved_retired = root / "retired-saved";
    fs::rename(retired, saved_retired);
    check(manager.inspect(find_system("ps1")).stage == ConsoleTileStage::absent,
          "recovery root is allowed to be absent before the first removal");
    fs::rename(saved_retired, retired);
    ConsoleTileIntent ps1;
    ps1.system = find_system("ps1");
    api.missing_body = "{\"status\":13}";
    check(manager.upsert(ps1).stage == ConsoleTileStage::unavailable &&
              !fs::exists(homebrew / "PPSA99704"),
          "HTTP 404 with a non-ENOENT status cannot reserve a title ID");
    api.missing_body = "{";
    check(manager.upsert(ps1).stage == ConsoleTileStage::unavailable &&
              !fs::exists(homebrew / "PPSA99704"),
          "malformed HTTP 404 cannot reserve a title ID");
    api.missing_body = "{\"status\":2}";
    auto result = manager.upsert(ps1);
    check(result.stage == ConsoleTileStage::deferred && result.owned &&
              std::strcmp(result.title_id, "PPSA99704") == 0 && api.scans == 1,
          "new PS1 source queues a deferred scan without claiming registration");
    check(fs::exists(homebrew / "PPSA99704/forwarder.json"), "installed source has forwarder metadata");
    check(fs::exists(homebrew / "PPSA99704/LICENSE-forwarder.txt"), "installed source carries forwarder license");
#ifndef _WIN32
    check((fs::status(homebrew / "PPSA99704/eboot.bin").permissions() & fs::perms::all) == fs::perms::all &&
              (fs::status(homebrew / "PPSA99704/sce_module/libc.prx").permissions() & fs::perms::all) == fs::perms::all,
          "forwarder executable and runtime match observed 0777 mode");
    for (const auto& dir : {homebrew / "PPSA99704", homebrew / "PPSA99704/sce_module",
                            homebrew / "PPSA99704/sce_sys"})
        check((fs::status(dir).permissions() & fs::perms::all) == fs::perms::all,
              "installed title directories match observed 0777 mode");
#endif
    check(manager.inspect(ps1.system).stage == ConsoleTileStage::pending, "source remains pending before registration");
    const auto ps1_source = homebrew / "PPSA99704";
    auto legacy_owner = read(ps1_source / "isshelf-console-tile.json");
    const std::string runtime_field =
        ",\"runtime\":\"isshelf-cooperative-forwarder/3\"";
    const std::string previous_runtime_field =
        ",\"runtime\":\"isshelf-cooperative-forwarder/2\"";
    const auto runtime_at = legacy_owner.find(runtime_field);
    check(runtime_at != std::string::npos, "new source records cooperative runtime");
    legacy_owner.replace(runtime_at, runtime_field.size(), previous_runtime_field);
    write(ps1_source / "isshelf-console-tile.json", legacy_owner);
    write(ps1_source / "eboot.bin", "legacy fixture eboot");
    check(manager.inspect(ps1.system).stage == ConsoleTileStage::pending,
          "v2 owned marker remains eligible for an explicit update");
    api.deferred = false;
    result = manager.upsert(ps1);
    check(result.stage == ConsoleTileStage::pending && api.scans == 2 &&
              read(ps1_source / "eboot.bin") == "fixture eboot" &&
              read(ps1_source / "isshelf-console-tile.json").find(runtime_field) != std::string::npos &&
              std::strstr(result.message, "runtime and destination") != nullptr,
          "editing a v2 owned tile atomically upgrades its runtime and retries the scan");
    api.registered["PPSA99704"] = hb + "/PPSA99704";
    result = manager.inspect(ps1.system);
    if (result.stage != ConsoleTileStage::registered)
        std::fprintf(stderr, "registration stage=%d message=%s\n", static_cast<int>(result.stage), result.message);
    check(result.stage == ConsoleTileStage::registered, "registration is read back from API");
    const auto active_ps1 = hb + "/PPSA99704";
    api.info_overrides["PPSA99704"] =
        "{\"status\":0,\"title_id\":\"PPSA99704\",\"path\":\"" + active_ps1 +
        "\",\"installed\":false,\"managed\":false,\"can_uninstall\":false,"
        "\"source_available\":true}";
    check(manager.inspect(ps1.system).stage == ConsoleTileStage::pending,
          "an uninstalled record with an available source remains occupied and pending");
    api.info_overrides["PPSA99704"] =
        "{\"status\":0,\"title_id\":\"PPSA99704\",\"path\":\"" + active_ps1 +
        "\",\"installed\":false,\"source_available\":false}";
    check(manager.inspect(ps1.system).stage == ConsoleTileStage::unavailable,
          "an uninstalled record missing lifecycle booleans fails closed");
    api.info_overrides["PPSA99704"] =
        "{\"status\":0,\"title_id\":\"PPSA99704\",\"path\":\"" + active_ps1 +
        "\",\"installed\":\"false\",\"managed\":false,\"can_uninstall\":false,"
        "\"source_available\":false}";
    check(manager.inspect(ps1.system).stage == ConsoleTileStage::unavailable,
          "a non-boolean installed field fails closed");
    api.info_overrides.clear();
    ps1.destination = ConsoleTileDestination::home;
    write(ps1_source / "sce_sys/icon0.png", "stale icon artwork");
    result = manager.upsert(ps1);
    check(result.stage == ConsoleTileStage::pending && api.scans == 3 &&
              read(ps1_source / "sce_sys/icon0.png") ==
                  read(fs::path(assets) / "icons/ps1.png") &&
              std::strstr(result.message, "artwork updated") != nullptr,
          "editing a stale owned tile atomically refreshes its artwork and queues registration");
    result = manager.upsert(ps1);
    check(result.stage == ConsoleTileStage::registered && api.scans == 3,
          "an already-current artwork and destination update is idempotent");
    check(manager.inspect(ps1.system).intent.destination == ConsoleTileDestination::home,
          "destination edit is read back");
    api.manual_paths = {"/different/source/PPSA99704"};
    result = manager.remove(ps1.system);
    check(result.stage == ConsoleTileStage::conflict && fs::exists(homebrew / "PPSA99704") &&
              api.manual_removes == 0,
          "a conflicting manual source stops removal before source isolation");
    api.manual_paths = {active_ps1, active_ps1};
    result = manager.remove(ps1.system);
    check(result.stage == ConsoleTileStage::unavailable && fs::exists(homebrew / "PPSA99704") &&
              api.manual_removes == 0,
          "duplicate manual tracking fails closed before source isolation");
    api.manual_paths = {active_ps1};
    api.manual_remove_changes = false;
    result = manager.remove(ps1.system);
    check(result.stage == ConsoleTileStage::unavailable && fs::exists(homebrew / "PPSA99704") &&
              api.manual_removes == 1,
          "manual removal requires exact list readback before source isolation");
    api.manual_remove_changes = true;
    api.malformed_uninstall_receipt = true;
    const int uninstalls_before_queue = api.uninstall_requests;
    result = manager.remove(ps1.system);
    check(result.stage == ConsoleTileStage::removed_pending &&
              result.handoff_required &&
              !fs::exists(homebrew / "PPSA99704") && fs::exists(retired / "PPSA99704") &&
              api.manual_paths.empty() && api.manual_removes == 2,
          "remove verifies manual exclusion, then queues the exact isolated source for post-close work");
    check(std::strstr(result.message, "Close isShelf") != nullptr &&
              api.registered.count("PPSA99704") == 1 &&
              api.uninstall_requests == uninstalls_before_queue,
          "foreground removal never asks ShadowMount to uninstall or closes the app");
    api.malformed_uninstall_receipt = false;
    result = manager.reconcile(ps1.system);
    check(result.stage == ConsoleTileStage::removed_pending && result.handoff_required &&
              api.registered.count("PPSA99704") == 1 &&
              api.uninstall_requests == uninstalls_before_queue,
          "reconcile keeps a registered removal queued without a foreground retry");
    api.info_overrides["PPSA99704"] =
        "{\"status\":0,\"title_id\":\"PPSA99704\",\"path\":\"" + active_ps1 +
        "\",\"installed\":false,\"managed\":false,\"can_uninstall\":false,"
        "\"source_available\":false}";
    api.registered.erase("PPSA99704");
    const int accepted_uninstalls = api.uninstall_requests;
    result = manager.reconcile(ps1.system);
    check(result.stage == ConsoleTileStage::removed_pending &&
              api.uninstall_requests == accepted_uninstalls &&
              std::strstr(result.message, "registration is absent") != nullptr,
          "an exact unavailable-source tombstone finishes reconcile without repeating uninstall");
    fs::create_directories(user_app / "PPSA99704");
    fs::create_directories(user_appmeta / "PPSA99704");
    result = manager.confirm_removed(ps1.system);
    check(result.stage == ConsoleTileStage::removed_pending && fs::exists(retired / "PPSA99704") &&
              std::strstr(result.message, "registration files remain") != nullptr,
          "API absence alone cannot clean recovery while registered title roots remain");
    fs::remove(user_app / "PPSA99704");
    fs::remove(user_appmeta / "PPSA99704");
    result = manager.confirm_removed(ps1.system);
    check(result.stage == ConsoleTileStage::absent && !fs::exists(retired / "PPSA99704"),
          "confirmed registration absence cleans only the known owned retired files");
    api.info_overrides.clear();
    ps1.destination = ConsoleTileDestination::library;
    result = manager.upsert(ps1);
    check(result.stage == ConsoleTileStage::pending && result.owned &&
              std::strcmp(result.title_id, "PPSA99704") == 0 &&
              manager.inspect(ps1.system).intent.destination == ConsoleTileDestination::library,
          "confirmed removal frees the exact console title for a new library shortcut");
    api.registered["PPSA99704"] = hb + "/PPSA99704";
    check(manager.inspect(ps1.system).stage == ConsoleTileStage::registered,
          "re-added shortcut becomes independently registered after a fresh scan");

    // A foreign title in the preferred slot must not be overwritten; an
    // alternate is chosen, then found again from its immutable ownership tag.
    write(homebrew / "PPSA99700/foreign.txt", "another app");
    write(homebrew / "PPSA99841/foreign.txt", "unrelated app in the allocation range");
    check(manager.inspect(ps1.system).stage == ConsoleTileStage::registered,
          "unmarked foreign candidate directories do not poison an exact owned tile");
    ConsoleTileIntent ps5;
    ps5.system = find_system("ps5");
    result = manager.upsert(ps5);
    check(result.owned && std::strcmp(result.title_id, "PPSA99752") == 0 &&
              fs::exists(homebrew / "PPSA99700/foreign.txt"),
          "collision uses a checked alternate and preserves foreign title");
    check(std::strcmp(manager.inspect(ps5.system).title_id, "PPSA99752") == 0,
          "owned alternate title ID is rediscovered");
    const auto ps5_marker = homebrew / "PPSA99752/isshelf-console-tile.json";
    const auto saved_ps5_marker = homebrew / "PPSA99752/isshelf-console-tile.saved";
    fs::rename(ps5_marker, saved_ps5_marker);
    write(ps5_marker, "{");
    check(manager.inspect(ps5.system).stage == ConsoleTileStage::conflict &&
              manager.remove(ps5.system).stage == ConsoleTileStage::conflict,
          "a damaged ownership marker cannot look like an absent tile");
    fs::remove(ps5_marker);
    fs::rename(saved_ps5_marker, ps5_marker);
    api.registered["PPSA99701"] = "/user/app/PPSA99701";
    ConsoleTileIntent ps4;
    ps4.system = find_system("ps4");
    result = manager.upsert(ps4);
    check(result.owned && std::strcmp(result.title_id, "PPSA99753") == 0,
          "registered title collision without a source folder uses another alternate");
    const auto ps5_module = homebrew / "PPSA99752/sce_module";
    const auto saved_module = root / "saved-module";
    fs::rename(ps5_module, saved_module);
    write(ps5_module, "not a directory");
    check(manager.inspect(ps5.system).stage == ConsoleTileStage::conflict &&
              manager.remove(ps5.system).stage == ConsoleTileStage::conflict &&
              fs::exists(saved_module / "libc.prx"),
          "replacement module path prevents inspection and removal");
    fs::remove(ps5_module);
    const auto external = root / "external";
    write(external / "sentinel.txt", "belongs elsewhere");
    std::error_code symlink_error;
    fs::create_directory_symlink(external, ps5_module, symlink_error);
    if (!symlink_error) {
        check(manager.inspect(ps5.system).stage == ConsoleTileStage::conflict &&
                  manager.remove(ps5.system).stage == ConsoleTileStage::conflict &&
                  fs::exists(external / "sentinel.txt"),
              "symlinked metadata directory is never followed for removal");
        fs::remove(ps5_module);
    }
    fs::rename(saved_module, ps5_module);
    const auto foreign_link = homebrew / "PPSA99842";
    fs::create_directory_symlink(external, foreign_link, symlink_error);
    if (!symlink_error) {
        check(manager.inspect(ps1.system).stage == ConsoleTileStage::conflict,
              "a no-follow candidate probe rejects a symlink in the allocation range");
        fs::remove(foreign_link);
    }
    write(homebrew / "PPSA99752/extra.txt", "not isShelf data");
    check(manager.inspect(ps5.system).owned &&
              fs::exists(homebrew / "PPSA99752/extra.txt"),
          "an extra file cannot hide an otherwise exact owned source and remains untouched");
    fs::remove(homebrew / "PPSA99752/extra.txt");
    check(manager.inspect(ps5.system).owned, "owned tile is usable after adversarial paths are cleared");

    api.registered["PPSA99753"] = hb + "/PPSA99753";
    write(homebrew / "PPSA99753/foreign.txt", "unknown recovery data");
    result = manager.remove(ps4.system);
    check(result.stage == ConsoleTileStage::removed_pending &&
              fs::exists(retired / "PPSA99753/foreign.txt"),
          "removal isolates an owned tile with an unexpected extra intact");
    api.registered.erase("PPSA99753");
    result = manager.confirm_removed(ps4.system);
    check(result.stage == ConsoleTileStage::failed &&
              fs::exists(retired / "PPSA99753/foreign.txt") &&
              !fs::exists(retired / "PPSA99753/isshelf-console-tile.json"),
          "cleanup removes only known owned files and retains an unexpected extra");
    fs::remove(retired / "PPSA99753/foreign.txt");
    fs::remove(retired / "PPSA99753");

    if (const char* real_bundle = std::getenv("ISSHELF_CONSOLE_TILE_BUNDLE")) {
        const auto virtual_app0 = (root / "unreadable-app0-view").generic_string();
        config.bundle_root = virtual_app0.c_str();
        config.bundle_fallback_root = assets.c_str();
        config.test_skip_integrity = false;
        auto unavailable = ConsoleTileManager(config).availability();
        check(unavailable.reason == ConsoleTileAvailabilityReason::bundle_asset_integrity &&
                  std::strcmp(unavailable.detail, "template/eboot.bin") == 0 &&
                  std::strncmp(unavailable.bundle_source, assets.c_str(),
                               std::strlen(unavailable.bundle_source)) == 0,
              "untrusted fallback bytes fail closed with the direct path identified");
        config.bundle_fallback_root = real_bundle;
        ConsoleTileManager bundled(config);
        const auto source = bundled.availability();
        check(source.ready() && std::strncmp(source.bundle_source, real_bundle,
                                             std::strlen(source.bundle_source)) == 0,
              "inaccessible virtual app0 selects the SHA-pinned direct asset source");
        ConsoleTileIntent ps2;
        ps2.system = find_system("ps2");
        result = bundled.upsert(ps2);
        check(result.owned && std::strcmp(result.title_id, "PPSA99703") == 0,
              "verified fallback bundle creates an owned PS2 tile source");
        check(fs::file_size(homebrew / "PPSA99703/eboot.bin") == fs::file_size(fs::path(real_bundle) / "template/eboot.bin") &&
                  fs::file_size(homebrew / "PPSA99703/sce_module/libc.prx") ==
                      fs::file_size(fs::path(real_bundle) / "template/sce_module/libc.prx"),
              "verified fallback forwarder template bytes are copied completely");

        // Exercise the real pinned bundle through every catalog entry. This
        // proves the common create/remove implementation is not accidentally
        // valid only for the handful of systems used by the focused fixtures.
        const auto all_homebrew = root / "all52-homebrew";
        const auto all_user_app = root / "all52-user-app";
        const auto all_user_appmeta = root / "all52-user-appmeta";
        const auto all_retired = root / "all52-retired";
        for (const auto& directory : {all_homebrew, all_user_app,
                                      all_user_appmeta, all_retired})
            fs::create_directories(directory);
        const auto all_hb = all_homebrew.generic_string();
        const auto all_apps = all_user_app.generic_string();
        const auto all_appmeta = all_user_appmeta.generic_string();
        const auto all_recovery = all_retired.generic_string();
        FakeApi all_api;
        ConsoleTileConfig all_config;
        all_config.homebrew_root = all_hb.c_str();
        all_config.bundle_root = real_bundle;
        all_config.user_app_root = all_apps.c_str();
        all_config.user_appmeta_root = all_appmeta.c_str();
        all_config.retired_root = all_recovery.c_str();
        all_config.post = post;
        all_config.post_context = &all_api;
#ifndef SHELF_PS5
        all_config.test_skip_integrity = true;
#endif
        ConsoleTileManager all_manager(all_config);
        check(all_manager.available(), "real cooperative bundle is available for all 52 systems");
        for (std::size_t system = 0; system < system_count; ++system) {
            ConsoleTileIntent intent;
            intent.system = system;
            const auto added = all_manager.upsert(intent);
            char expected[10]{};
            std::snprintf(expected, sizeof(expected), "PPSA%05zu", 99700 + system);
            const auto directory = all_homebrew / expected;
            check(added.stage == ConsoleTileStage::deferred && added.owned &&
                      std::strcmp(added.title_id, expected) == 0 &&
                      fs::file_size(directory / "eboot.bin") ==
                          fs::file_size(fs::path(real_bundle) / "template/eboot.bin") &&
                      fs::file_size(directory / "sce_module/libc.prx") ==
                          fs::file_size(fs::path(real_bundle) / "template/sce_module/libc.prx") &&
                      read(directory / "isshelf-console-tile.json").find(
                          "\"runtime\":\"isshelf-cooperative-forwarder/3\"") != std::string::npos &&
                      all_manager.inspect(system).owned,
                  "every catalog system creates an exact cooperative shortcut source");
        }
        check(all_api.scans == static_cast<int>(system_count),
              "every newly added shortcut queues exactly one registration scan");

        std::array<ConsoleTileIntent, system_count> expected_intents{};
        for (std::size_t system = 0; system < system_count; ++system) {
            char title_id[10]{};
            std::snprintf(title_id, sizeof(title_id), "PPSA%05zu", 99700 + system);
            all_api.registered[title_id] = all_hb + "/" + title_id;
        }
        std::array<ConsoleTileIntent, system_count> emulator_intents{};
        std::array<bool, system_count> emulator_selected{};
        constexpr const char* emulator_titles[] = {
            "PPSA99303", "PPSA99203", "PPSA99248", "PPSA99008",
            "PPSA99360", "PPSA99764", "PPSA99064", "PPSA99247",
            "PPSA99169",
        };
        std::size_t emulator_count = 0;
        std::size_t direct_emulator_count = 0;
        std::size_t retroarch_library_count = 0;
        for (std::size_t system = 0; system < system_count; ++system)
            for (const char* title : emulator_titles)
                if (tile_job::pinned_emulator_menu(system, title)) {
                    auto& intent = emulator_intents[system];
                    intent.system = system;
                    if (console_tile_emulator_menu_selects_system(title)) {
                        intent.destination = ConsoleTileDestination::emulator;
                        std::snprintf(intent.emulator_title,
                                      sizeof(intent.emulator_title), "%s", title);
                        ++direct_emulator_count;
                    } else {
                        intent.destination = ConsoleTileDestination::library;
                        ++retroarch_library_count;
                    }
                    emulator_selected[system] = true;
                    ++emulator_count;
                    break;
                }
        check(emulator_count == system_count - 3 && direct_emulator_count == 13 &&
                  retroarch_library_count == 36,
              "batch fixture maps direct menus and shared RetroArch systems truthfully");
        const int scans_before_emulator_batch = all_api.scans;
        auto emulator_batch = all_manager.upsert_emulators(
            emulator_intents, emulator_selected);
        check(emulator_batch.complete && !emulator_batch.close_required &&
                  emulator_batch.selected == emulator_count &&
                  emulator_batch.committed == emulator_count &&
                  emulator_batch.created == 0 &&
                  emulator_batch.updated == emulator_count &&
                  all_api.scans == scans_before_emulator_batch,
              "one batch changes all existing supported shortcuts to truthful emulator or library routes without redundant scans");
        for (std::size_t system = 0; system < system_count; ++system)
            if (emulator_selected[system])
                check(emulator_batch.committed_systems[system] &&
                          console_tile_intents_equal(
                              all_manager.inspect(system).intent,
                              emulator_intents[system]),
                      "emulator batch reads back every exact selected route");
        emulator_batch = all_manager.upsert_emulators(
            emulator_intents, emulator_selected);
        check(emulator_batch.complete &&
                  emulator_batch.updated == emulator_count &&
                  all_api.scans == scans_before_emulator_batch,
              "the complete emulator batch is idempotent and safe to rerun");
        const auto repair_system = find_system("threedo");
        char repair_title[10]{};
        std::snprintf(repair_title, sizeof(repair_title),
                      "PPSA%05zu", 99700 + repair_system);
        const auto repair_source = all_hb + "/" + repair_title;
        all_api.manual_paths = {repair_source};
        const auto queued_repair = all_manager.remove(repair_system);
        check(queued_repair.stage == ConsoleTileStage::removed_pending &&
                  queued_repair.handoff_required &&
                  fs::exists(all_retired / repair_title),
              "a supported shortcut can enter the protected post-close removal state");
        all_api.registered.erase(repair_title);
        fs::remove_all(all_user_app / repair_title);
        fs::remove_all(all_user_appmeta / repair_title);
        const int scans_before_recovery = all_api.scans;
        emulator_batch = all_manager.upsert_emulators(
            emulator_intents, emulator_selected);
        check(emulator_batch.complete && emulator_batch.close_required &&
                  emulator_batch.committed == emulator_count &&
                  emulator_batch.created == 1 &&
                  emulator_batch.updated == emulator_count - 1 &&
                  all_api.scans == scans_before_recovery + 1 &&
                  fs::exists(all_homebrew / repair_title) &&
                  !fs::exists(all_retired / repair_title),
              "repair-all finishes a confirmed earlier removal and recreates its emulator tile");
        all_api.registered[repair_title] = repair_source;
        ConsoleTileIntent ps1_home;
        ps1_home.system = find_system("ps1");
        ps1_home.destination = ConsoleTileDestination::home;
        check(all_manager.upsert(ps1_home).stage == ConsoleTileStage::registered,
              "all-tiles fixture can retain a Home destination");
        ConsoleTileIntent ps2_emulator;
        ps2_emulator.system = find_system("ps2");
        ps2_emulator.destination = ConsoleTileDestination::emulator;
        std::snprintf(ps2_emulator.emulator_title,
                      sizeof(ps2_emulator.emulator_title), "PPSA99203");
        check(all_manager.upsert(ps2_emulator).stage == ConsoleTileStage::registered,
              "all-tiles fixture can retain an emulator destination");
        write(all_homebrew / repair_title / "forwarder.json",
              "{\"format\":\"ps5-forwarder/1\",\"target\":\"PPSA99541\","
              "\"args\":[\"--isshelf-console-tile=threedo,emulator,PPSA99169\"],"
              "\"creator\":\"isShelf\"}\n");
        check(all_manager.inspect(repair_system).intent.destination ==
                  ConsoleTileDestination::emulator,
              "legacy 3DO RetroArch route is recognized before migration");
        for (std::size_t system = 0; system < system_count; ++system)
            check(normalize_console_tile_intent(all_manager.inspect(system).intent,
                                                expected_intents[system]),
                  "every installed route has a supported refresh destination");

        const auto make_legacy = [&](std::size_t system) {
            char title_id[10]{};
            std::snprintf(title_id, sizeof(title_id), "PPSA%05zu", 99700 + system);
            const auto directory = all_homebrew / title_id;
            auto owner = read(directory / "isshelf-console-tile.json");
            const auto at = owner.find(runtime_field);
            check(at != std::string::npos, "cooperative owner can become a legacy fixture");
            owner.replace(at, runtime_field.size(), previous_runtime_field);
            write(directory / "isshelf-console-tile.json", owner);
            write(directory / "eboot.bin", "legacy fixture eboot");
        };
        const auto cooperative = find_system("threedo");
        for (std::size_t system = 0; system < system_count; ++system)
            if (system != cooperative) make_legacy(system);

        auto batch = all_manager.refresh_installed();
        check(batch.complete && batch.handoff_required &&
                  batch.installed == system_count &&
                  batch.refreshed == system_count &&
                  batch.registration_refreshes == system_count &&
                  all_api.uninstall_requests == 0 &&
                  !batch.removing &&
                  all_api.scans == static_cast<int>(system_count) + 1,
              "one explicit batch action refreshes sources and defers stale registrations to the paced post-close helper");
        for (std::size_t system = 0; system < system_count; ++system) {
            char title_id[10]{};
            std::snprintf(title_id, sizeof(title_id), "PPSA%05zu", 99700 + system);
            const auto directory = all_homebrew / title_id;
            check(read(directory / "isshelf-console-tile.json").find(runtime_field) != std::string::npos &&
                      fs::file_size(directory / "eboot.bin") ==
                          fs::file_size(fs::path(real_bundle) / "template/eboot.bin") &&
                      console_tile_intents_equal(all_manager.inspect(system).intent,
                                                 expected_intents[system]),
                  "batch refresh pins the cooperative runtime and preserves every exact route");
            all_api.registered[title_id] = all_hb + "/" + title_id;
            write(all_user_app / title_id / "sce_sys/icon0.png",
                  read(directory / "sce_sys/icon0.png"));
            write(all_user_appmeta / title_id / "icon0.png",
                  read(directory / "sce_sys/icon0.png"));
        }
        check(all_manager.inspect(repair_system).intent.destination ==
                  ConsoleTileDestination::library,
              "batch refresh migrates the legacy 3DO RetroArch menu to its isShelf library");
        batch = all_manager.refresh_installed();
        check(batch.complete && batch.refreshed == system_count &&
                  batch.registration_refreshes == 0 &&
                  all_api.uninstall_requests == 0 &&
                  all_api.scans == static_cast<int>(system_count) + 2,
              "current registered artwork is not uninstalled and each action queues one scan");

        // Stale registered artwork is always a bounded post-close handoff.
        // The foreground app sends no uninstall burst, independent of whether
        // live ShadowMount would currently return game_active or scanner_busy.
        std::string expected_handoff;
        for (std::size_t system = 0; system < system_count; ++system) {
            char title_id[10]{};
            std::snprintf(title_id, sizeof(title_id), "PPSA%05zu", 99700 + system);
            fs::remove(all_user_app / title_id / "sce_sys/icon0.png");
            if (!expected_handoff.empty()) expected_handoff += ',';
            expected_handoff += title_id;
        }
        all_api.game_active_uninstall = true;
        const int scans_before_handoff = all_api.scans;
        const int uninstalls_before_handoff = all_api.uninstall_requests;
        batch = all_manager.refresh_installed();
        check(batch.complete && batch.handoff_required &&
                  batch.registration_refreshes == system_count &&
                  expected_handoff == batch.handoff_titles &&
                  all_api.uninstall_requests == uninstalls_before_handoff &&
                  all_api.scans == scans_before_handoff,
              "stale artwork prepares one exact full cooperative post-close refresh without a foreground uninstall");
        all_api.game_active_uninstall = false;
        for (std::size_t system = 0; system < system_count; ++system) {
            char title_id[10]{};
            std::snprintf(title_id, sizeof(title_id), "PPSA%05zu", 99700 + system);
            write(all_user_app / title_id / "sce_sys/icon0.png",
                  read(all_homebrew / title_id / "sce_sys/icon0.png"));
        }

        // Even a malformed live uninstall response is irrelevant to the
        // foreground refresh: it sends no request and prepares one exact title
        // for the paced post-close helper.
        fs::remove(all_user_app / "PPSA99700/sce_sys/icon0.png");
        all_api.malformed_uninstall_receipt = true;
        const int scans_before_rejection = all_api.scans;
        const int uninstalls_before_rejection = all_api.uninstall_requests;
        batch = all_manager.refresh_installed();
        check(batch.complete && batch.handoff_required &&
                  batch.refreshed == system_count &&
                  batch.registration_refreshes == 1 &&
                  std::strcmp(batch.handoff_titles, "PPSA99700") == 0 &&
                  all_api.uninstall_requests == uninstalls_before_rejection &&
                  all_api.scans == scans_before_rejection,
              "a stale registration is deferred without consulting the foreground uninstall route");
        all_api.malformed_uninstall_receipt = false;
        write(all_user_app / "PPSA99700/sce_sys/icon0.png",
              read(all_homebrew / "PPSA99700/sce_sys/icon0.png"));

        // An ambiguous source found during the all-or-nothing preflight must
        // leave an earlier legacy runtime untouched.
        constexpr std::size_t first = 0, second = 1;
        make_legacy(first);
        char second_title[10]{};
        std::snprintf(second_title, sizeof(second_title), "PPSA%05zu", 99700 + second);
        const auto second_owner_path = all_homebrew / second_title /
                                       "isshelf-console-tile.json";
        const auto second_owner = read(second_owner_path);
        write(second_owner_path, "{}");
        batch = all_manager.refresh_installed();
        char first_title[10]{};
        std::snprintf(first_title, sizeof(first_title), "PPSA%05zu", 99700 + first);
        check(!batch.complete && batch.refreshed == 0 &&
                  read(all_homebrew / first_title / "eboot.bin") ==
                      "legacy fixture eboot",
              "batch preflight stops before writes when any owned source is ambiguous");
        write(second_owner_path, second_owner);
        batch = all_manager.refresh_installed();
        check(batch.complete && batch.refreshed == system_count,
              "a corrected preflight failure is fully rerunnable");

        // A filesystem race after preflight may produce a durable prefix, so
        // report it exactly and let the same action safely finish the rest.
        make_legacy(first);
        make_legacy(second);
        const auto blocked_temporary = all_homebrew / second_title /
                                       "eboot.bin.isshelf-runtime-next";
        write(blocked_temporary, "occupied");
        batch = all_manager.refresh_installed();
        check(!batch.complete && batch.refreshed == 1 &&
                  batch.failed_system == second &&
                  read(all_homebrew / first_title /
                       "isshelf-console-tile.json").find(runtime_field) != std::string::npos,
              "a mid-run failure reports the exact durable prefix");
        fs::remove(blocked_temporary);
        batch = all_manager.refresh_installed();
        check(batch.complete && batch.refreshed == system_count &&
                  read(second_owner_path).find(runtime_field) != std::string::npos,
              "the same batch action completes after a transient mid-run failure");

        const int uninstalls_before_removal = all_api.uninstall_requests;
        std::string expected_removal_handoff;
        all_api.manual_paths.clear();
        for (std::size_t system = 0; system < system_count; ++system) {
            char title_id[10]{};
            std::snprintf(title_id, sizeof(title_id), "PPSA%05zu", 99700 + system);
            const auto source_path = all_hb + "/" + title_id;
            all_api.registered[title_id] = source_path;
            all_api.manual_paths.push_back(source_path);
            if (!expected_removal_handoff.empty()) expected_removal_handoff += ',';
            expected_removal_handoff += title_id;
        }
        const auto removal_batch = all_manager.remove_installed();
        check(removal_batch.complete && removal_batch.handoff_required &&
                  removal_batch.found == system_count &&
                  removal_batch.queued == system_count &&
                  expected_removal_handoff == removal_batch.handoff_titles &&
                  all_api.uninstall_requests == uninstalls_before_removal,
              "one in-app action queues every exact owned title without a foreground uninstall");
        const auto repeated_removal_batch = all_manager.remove_installed();
        check(repeated_removal_batch.complete &&
                  repeated_removal_batch.handoff_required &&
                  repeated_removal_batch.already_queued == system_count &&
                  repeated_removal_batch.queued == system_count &&
                  expected_removal_handoff == repeated_removal_batch.handoff_titles &&
                  all_api.manual_removes == static_cast<int>(system_count) + 1 &&
                  all_api.uninstall_requests == uninstalls_before_removal,
              "remove-all safely recovers the same bounded queue after reopening isShelf");
        for (std::size_t system = 0; system < system_count; ++system) {
            char title_id[10]{};
            std::snprintf(title_id, sizeof(title_id), "PPSA%05zu", 99700 + system);
            check(removal_batch.queued_systems[system] &&
                      !fs::exists(all_homebrew / title_id) &&
                      fs::exists(all_retired / title_id),
                  "bulk removal isolates every catalog source into its recovery folder");
            // Simulate the asynchronous PS5 registration cleanup accepted by
            // the one post-close helper before asking the app to clean its
            // retained recovery source on a later opening.
            all_api.registered.erase(title_id);
            fs::remove_all(all_user_app / title_id);
            fs::remove_all(all_user_appmeta / title_id);
            const auto confirmed = all_manager.confirm_removed(system);
            check(confirmed.stage == ConsoleTileStage::absent &&
                      !fs::exists(all_retired / title_id),
                  "every catalog system cleans its exact recovery source after confirmed removal");
        }
        check(all_api.uninstall_requests == uninstalls_before_removal &&
                   all_api.manual_removes == static_cast<int>(system_count) + 1,
              "all 52 removals remove exact manual sources and defer every uninstall until close");

        // If a delayed PS5 shell update recreates only the exact registered
        // icon/metadata pair after recovery cleanup, Remove all must still be
        // able to finish the platform uninstall. No generic /user/app entry
        // is sufficient: both identities and both reviewed icons must match.
        for (std::size_t system = 0; system < system_count; ++system) {
            char title_id[10]{};
            std::snprintf(title_id, sizeof(title_id), "PPSA%05zu", 99700 + system);
            const std::string metadata =
                "{\"titleId\":\"" + std::string(title_id) +
                "\",\"contentId\":\"UP9000-" + std::string(title_id) +
                "_00-PS5FORWARDER0000\"}";
            const auto icon = read(fs::path(real_bundle) / "icons" /
                                   (std::string(catalog[system].id) + ".png"));
            write(all_user_app / title_id / "sce_sys/param.json", metadata);
            write(all_user_app / title_id / "sce_sys/icon0.png", icon);
            write(all_user_app / title_id / "icon0.png", icon);
            write(all_user_appmeta / title_id / "param.json", metadata);
            write(all_user_appmeta / title_id / "icon0.png", icon);
        }
        const auto residual_batch = all_manager.remove_installed();
        check(residual_batch.complete && residual_batch.handoff_required &&
                  residual_batch.found == system_count &&
                  residual_batch.queued == system_count &&
                  residual_batch.already_queued == 0 &&
                  expected_removal_handoff == residual_batch.handoff_titles &&
                  all_api.uninstall_requests == uninstalls_before_removal,
              "remove-all recovers all 52 exact residual PS5 registrations without foreground uninstalls");
        write(all_user_appmeta / "PPSA99700/param.json", "{}");
        const auto guarded_residual_batch = all_manager.remove_installed();
        check(guarded_residual_batch.complete &&
                  guarded_residual_batch.found == system_count - 1 &&
                  guarded_residual_batch.queued == system_count - 1 &&
                  !guarded_residual_batch.queued_systems[0] &&
                  std::strstr(guarded_residual_batch.handoff_titles,
                              "PPSA99700") == nullptr,
              "a mismatched residual registration is never admitted as an isShelf tile");
        for (std::size_t system = 0; system < system_count; ++system) {
            char title_id[10]{};
            std::snprintf(title_id, sizeof(title_id), "PPSA%05zu", 99700 + system);
            fs::remove_all(all_user_app / title_id);
            fs::remove_all(all_user_appmeta / title_id);
        }
        const auto no_residual_batch = all_manager.remove_installed();
        check(no_residual_batch.complete && !no_residual_batch.handoff_required &&
                  no_residual_batch.found == 0 && no_residual_batch.queued == 0,
              "remove-all is idempotent after exact residual registration cleanup");
    }
    // Keep every title occupied without making a shortcut. The failing
    // outcome records exact filesystem kinds/errors and the API disposition
    // for the preferred slot and first alternate, while preserving the same
    // user-facing message and collision protection.
    fs::create_directories(homebrew / shelf_title_id);
    ConsoleTileIntent ps3;
    ps3.system = find_system("ps3");
    api.registered["PPSA99702"] = "/user/app/PPSA99702";
    for (unsigned number = 99753; number <= 99899; ++number) {
        if (number == 99764) continue;
        char title_id[10];
        check(console_tile_title_id(number, title_id), "diagnostic title ID is valid");
        api.registered[title_id] = std::string("/user/app/") + title_id;
    }
    result = manager.upsert(ps3);
    const bool diagnostic_ok = result.stage == ConsoleTileStage::conflict &&
              std::strcmp(result.message, "No free shortcut title ID remains.") == 0 &&
              std::strstr(result.diagnostic, "pref=PPSA99702 hb=missing/") &&
              std::strstr(result.diagnostic, "api=conflict(http=200,status=0,diag=0)") &&
              std::strstr(result.diagnostic, "alt=PPSA99752 hb=directory/0") &&
              std::strstr(result.diagnostic, "api=missing(http=404,status=2,diag=1)") &&
              std::strstr(result.diagnostic, "roots hb=") &&
              std::strstr(result.diagnostic, "pref.hb stat=error/") &&
              std::strstr(result.diagnostic, "list=absent/0") &&
              std::strstr(result.diagnostic, "alt.hb stat=directory/0") &&
              std::strstr(result.diagnostic, "known.hb-app stat=directory/0") &&
              !std::strstr(result.diagnostic, "CAPABILITY-TRUNCATED") &&
              !fs::exists(homebrew / "PPSA99702");
    if (!diagnostic_ok)
        std::fprintf(stderr, "allocator stage=%d message=%s diagnostic=%s\n",
                     static_cast<int>(result.stage), result.message, result.diagnostic);
    check(diagnostic_ok,
          "exhausted ID allocation records bounded preferred/alternate evidence without creating a tile");
    std::error_code probe_link_error;
    fs::create_directory_symlink(homebrew / shelf_title_id, homebrew / "PPSA99702", probe_link_error);
    if (!probe_link_error) {
        result = manager.upsert(ps3);
        check(result.stage == ConsoleTileStage::conflict &&
                  std::strstr(result.diagnostic, "active count=0 issue=candidate-path title=PPSA99702") &&
                  fs::is_symlink(homebrew / "PPSA99702"),
              "no-follow ownership probe identifies a symlink without allowing its title ID");
        fs::remove(homebrew / "PPSA99702");
    }
    fs::remove_all(root);
    std::puts("console tile manager tests passed");
    return 0;
}
