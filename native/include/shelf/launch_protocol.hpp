// SPDX-License-Identifier: GPL-3.0-only
// Owned launch protocol and cooperative handoff policy. No console API or
// filesystem operations live here; the payload/app integration is separate.
#pragma once
#include "tile_residual_guard.hpp"
#include "update_protocol.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string_view>

namespace shelf {
inline constexpr char shelf_title_id[] = "PPSA99541";
inline constexpr std::size_t launch_message_capacity = 2048;
enum class LaunchMode {
    emulator_menu,
    retroarch_game,
    native_title,
    psswan_game,
    tile_uninstall,
    tile_refresh,
    tile_remove,
    tile_residual,
    self_update,
    update_recover,
};
struct LaunchRequest {
    char id[33]{}, title[10]{}, rom[768]{}, core[65]{};
    std::uint64_t issued = 0;
    LaunchMode mode = LaunchMode::emulator_menu;
};
inline bool launch_title_valid(std::string_view title) {
    if (title.size() != 9 || (std::memcmp(title.data(), "PPSA", 4) != 0 &&
                              std::memcmp(title.data(), "CUSA", 4) != 0))
        return false;
    for (std::size_t i = 4; i < title.size(); ++i) {
        const char c = title[i];
        if (c < '0' || c > '9')
            return false;
    }
    return title != shelf_title_id;
}
inline bool launch_menu_known(std::string_view title) {
    for (auto known : {"PPSA99169", "PPSA99203", "PPSA97510", "PPSA99303", "PPSA99008", "PPSA99764", "PPSA99360",
                       "PPSA99064", "PPSA99247", "PPSA98510", "PPSA99248"})
        if (title == known)
            return true;
    return false;
}
inline bool launch_tile_title_valid(std::string_view title) {
    if (title.size() != 9 || std::memcmp(title.data(), "PPSA", 4) != 0)
        return false;
    unsigned number = 0;
    for (std::size_t i = 4; i < title.size(); ++i) {
        const char c = title[i];
        if (c < '0' || c > '9')
            return false;
        number = number * 10 + static_cast<unsigned>(c - '0');
    }
    return number != 99764 &&
           ((number >= 99700 && number < 99752) ||
            (number >= 99752 && number < 99804));
}
inline bool launch_tile_refresh_titles_valid(std::string_view first,
                                             std::string_view remaining,
                                             std::size_t* count = nullptr) {
    std::array<bool, 104> seen{};
    std::size_t total = 0;
    const auto accept = [&](std::string_view title) {
        if (!launch_tile_title_valid(title) || total >= 52)
            return false;
        unsigned number = 0;
        for (std::size_t i = 4; i < title.size(); ++i)
            number = number * 10 + static_cast<unsigned>(title[i] - '0');
        const auto slot = static_cast<std::size_t>(number - 99700);
        if (slot >= seen.size() || seen[slot])
            return false;
        seen[slot] = true;
        ++total;
        return true;
    };
    if (!accept(first))
        return false;
    while (!remaining.empty()) {
        const auto comma = remaining.find(',');
        const auto length = comma == remaining.npos ? remaining.size() : comma;
        const std::string_view title(remaining.data(), length);
        if (!accept(title))
            return false;
        if (comma == remaining.npos)
            remaining = {};
        else {
            remaining.remove_prefix(comma + 1);
            if (remaining.empty())
                return false;
        }
    }
    if (count)
        *count = total;
    return true;
}
inline bool launch_tile_residual_titles_valid(std::string_view first,
                                              std::string_view remaining,
                                              std::size_t* count = nullptr) {
    std::size_t expected = 0;
    const auto accept = [&](std::string_view title) {
        std::size_t system = 0;
        if (!tile_residual_title_index(title, system) || system != expected)
            return false;
        ++expected;
        return true;
    };
    if (!accept(first))
        return false;
    while (!remaining.empty()) {
        const auto comma = remaining.find(',');
        const auto length = comma == remaining.npos ? remaining.size() : comma;
        if (!accept(std::string_view(remaining.data(), length)))
            return false;
        if (comma == remaining.npos)
            remaining = {};
        else {
            remaining.remove_prefix(comma + 1);
            if (remaining.empty())
                return false;
        }
    }
    if (count)
        *count = expected;
    return expected == 52;
}
template <std::size_t N> inline bool launch_bounded(const char (&text)[N]) {
    return std::memchr(text, 0, N) != nullptr;
}
inline bool launch_media_path(std::string_view path) {
    bool permitted = false;
    for (std::string_view root : {"/data/homebrew/PPSA99169/content/", "/data/homebrew/PPSA99169/roms/",
                                  "/data/PSXS5/games/", "/data/SwanStationPS5/games/", "/data/PCSX2/games/",
                                  "/data/homebrew/PPSA99248/games/", "/data/psswanstation/games/"})
        if (path.size() > root.size() && path.substr(0, root.size()) == root)
            permitted = true;
    if (!permitted || path.back() == '/' || path.find('\\') != path.npos)
        return false;
    for (unsigned char c : path)
        if (c < 32 || c == 127)
            return false;
    std::size_t start = 1;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto part = path.substr(start, end == path.npos ? path.size() - start : end - start);
        if (part.empty() || part == "." || part == "..")
            return false;
        start = end == path.npos ? path.size() : end + 1;
    }
    return true;
}
inline constexpr std::string_view psswan_ftp_origin = "ftp://127.0.0.1:2121";
inline bool launch_ps1_path(std::string_view path) {
    if (!launch_media_path(path))
        return false;
    for (std::string_view root : {"/data/PSXS5/games/", "/data/SwanStationPS5/games/",
                                  "/data/homebrew/PPSA99248/games/", "/data/psswanstation/games/",
                                  "/data/homebrew/PPSA99169/content/psx/"})
        if (path.size() > root.size() && path.substr(0, root.size()) == root)
            return true;
    return false;
}
inline bool launch_psswan_path(std::string_view path) {
    return path.substr(0, psswan_ftp_origin.size()) == psswan_ftp_origin &&
           launch_ps1_path(path.substr(psswan_ftp_origin.size()));
}
inline bool launch_request_valid(const LaunchRequest& r, std::uint64_t now) {
    if (!launch_bounded(r.id) || !launch_bounded(r.title) || !launch_bounded(r.rom) || !launch_bounded(r.core) ||
        std::strlen(r.id) != 32 || !r.issued || !now)
        return false;
    for (char c : std::string_view(r.id))
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    if ((now >= r.issued && now - r.issued > 30) || (r.issued > now && r.issued - now > 2))
        return false;
    if (r.mode == LaunchMode::self_update || r.mode == LaunchMode::update_recover) {
        unsigned version[3]{};
        return std::strcmp(r.title, shelf_title_id) == 0 && update_semver(r.rom, version) &&
               update_hex_sha256(r.core);
    }
    if (!launch_title_valid(r.title))
        return false;
    if (r.mode == LaunchMode::tile_uninstall)
        return launch_tile_title_valid(r.title) && !r.rom[0] && !r.core[0];
    if (r.mode == LaunchMode::tile_remove || r.mode == LaunchMode::tile_refresh)
        return launch_tile_refresh_titles_valid(r.title, r.rom) && !r.core[0];
    if (r.mode == LaunchMode::tile_residual)
        return launch_tile_residual_titles_valid(r.title, r.rom) && !r.core[0];
    if (r.mode == LaunchMode::emulator_menu)
        return launch_menu_known(r.title) && !r.rom[0] && !r.core[0];
    if (r.mode == LaunchMode::native_title)
        return !r.rom[0] && !r.core[0];
    if (r.mode == LaunchMode::psswan_game)
        return std::strcmp(r.title, "PPSA99248") == 0 && !r.core[0] && launch_psswan_path(r.rom);
    if (r.mode != LaunchMode::retroarch_game || std::strcmp(r.title, "PPSA99169") != 0 || !r.core[0] ||
        !launch_media_path(r.rom))
        return false;
    for (char c : std::string_view(r.core))
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}
inline const char* launch_mode_name(LaunchMode mode) {
    if (mode == LaunchMode::emulator_menu)
        return "menu";
    if (mode == LaunchMode::retroarch_game)
        return "retroarch";
    if (mode == LaunchMode::psswan_game)
        return "psswan";
    if (mode == LaunchMode::tile_uninstall)
        return "tile-uninstall";
    if (mode == LaunchMode::tile_remove)
        return "tile-remove";
    if (mode == LaunchMode::tile_refresh)
        return "tile-refresh";
    if (mode == LaunchMode::tile_residual)
        return "tile-residual";
    if (mode == LaunchMode::self_update)
        return "self-update";
    if (mode == LaunchMode::update_recover)
        return "update-recover";
    return mode == LaunchMode::native_title ? "native" : "invalid";
}
inline bool encode_launch_request(const LaunchRequest& request, std::uint64_t now, char* output, std::size_t capacity) {
    if (!output || !capacity || !launch_request_valid(request, now))
        return false;
    const int used = std::snprintf(output, capacity,
                                   "console-shelf-launch 1\nid=%s\nissued=%llu\nmode=%s\ntitle=%s\nrom=%s\ncore=%s\n",
                                   request.id, static_cast<unsigned long long>(request.issued),
                                   launch_mode_name(request.mode), request.title, request.rom, request.core);
    return used > 0 && static_cast<std::size_t>(used) < capacity &&
           static_cast<std::size_t>(used) < launch_message_capacity;
}
inline bool parse_launch_request(std::string_view message, std::uint64_t now, LaunchRequest& output) {
    if (message.empty() || message.size() >= launch_message_capacity || message.back() != '\n')
        return false;
    for (unsigned char c : message)
        if ((c < 32 && c != '\n') || c == 127)
            return false;
    constexpr std::string_view header = "console-shelf-launch 1\n";
    if (message.substr(0, header.size()) != header)
        return false;
    message.remove_prefix(header.size());
    auto field = [&message](std::string_view tag, char* value, std::size_t capacity) {
        const auto newline = message.find('\n');
        if (newline == message.npos || message.substr(0, tag.size()) != tag || newline < tag.size() ||
            newline - tag.size() >= capacity)
            return false;
        const auto length = newline - tag.size();
        std::memcpy(value, message.data() + tag.size(), length);
        value[length] = 0;
        message.remove_prefix(newline + 1);
        return true;
    };
    LaunchRequest candidate;
    char issued[21]{}, mode[16]{};
    if (!field("id=", candidate.id, sizeof(candidate.id)) || !field("issued=", issued, sizeof(issued)) ||
        !field("mode=", mode, sizeof(mode)) || !field("title=", candidate.title, sizeof(candidate.title)) ||
        !field("rom=", candidate.rom, sizeof(candidate.rom)) ||
        !field("core=", candidate.core, sizeof(candidate.core)) || !message.empty() || !issued[0])
        return false;
    for (char c : std::string_view(issued)) {
        if (c < '0' || c > '9' || candidate.issued > (UINT64_MAX - static_cast<unsigned>(c - '0')) / 10)
            return false;
        candidate.issued = candidate.issued * 10 + static_cast<unsigned>(c - '0');
    }
    if (std::strcmp(mode, "menu") == 0)
        candidate.mode = LaunchMode::emulator_menu;
    else if (std::strcmp(mode, "retroarch") == 0)
        candidate.mode = LaunchMode::retroarch_game;
    else if (std::strcmp(mode, "native") == 0)
        candidate.mode = LaunchMode::native_title;
    else if (std::strcmp(mode, "psswan") == 0)
        candidate.mode = LaunchMode::psswan_game;
    else if (std::strcmp(mode, "tile-uninstall") == 0)
        candidate.mode = LaunchMode::tile_uninstall;
    else if (std::strcmp(mode, "tile-remove") == 0)
        candidate.mode = LaunchMode::tile_remove;
    else if (std::strcmp(mode, "tile-refresh") == 0)
        candidate.mode = LaunchMode::tile_refresh;
    else if (std::strcmp(mode, "tile-residual") == 0)
        candidate.mode = LaunchMode::tile_residual;
    else if (std::strcmp(mode, "self-update") == 0)
        candidate.mode = LaunchMode::self_update;
    else if (std::strcmp(mode, "update-recover") == 0)
        candidate.mode = LaunchMode::update_recover;
    else
        return false;
    if (!launch_request_valid(candidate, now))
        return false;
    output = candidate;
    return true;
}
inline bool encode_tile_uninstall_http_request(const LaunchRequest& request,
                                               char* output, std::size_t capacity) {
    if (!output || !capacity || request.mode != LaunchMode::tile_uninstall ||
        !launch_tile_title_valid(request.title) || request.rom[0] || request.core[0])
        return false;
    char body[48];
    const int body_size = std::snprintf(body, sizeof(body),
                                        "{\"title_id\":\"%s\"}", request.title);
    if (body_size <= 0 || static_cast<std::size_t>(body_size) >= sizeof(body))
        return false;
    const int used = std::snprintf(
        output, capacity,
        "POST /api/v1/games/uninstall HTTP/1.1\r\n"
        "Host: 127.0.0.1:10101\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n%s",
        body_size, body);
    return used > 0 && static_cast<std::size_t>(used) < capacity;
}
enum class TileUninstallHttpResult {
    accepted,
    game_active,
    scanner_busy,
    invalid,
};
inline TileUninstallHttpResult tile_uninstall_http_response(
        std::string_view response, std::string_view title) {
    if (!launch_tile_title_valid(title) || response.empty() || response.size() > 4096)
        return TileUninstallHttpResult::invalid;
    for (unsigned char c : response)
        if ((c < 32 && c != '\r' && c != '\n' && c != '\t') || c == 127)
            return TileUninstallHttpResult::invalid;
    const auto line_end = response.find("\r\n");
    const auto header_end = response.find("\r\n\r\n");
    if (line_end == response.npos || header_end == response.npos || line_end > header_end)
        return TileUninstallHttpResult::invalid;
    const std::string_view status(response.data(), line_end);
    const std::string_view body(response.data() + header_end + 4,
                                response.size() - header_end - 4);
    const auto once = [&](std::string_view field) {
        const auto at = body.find(field);
        return at != body.npos && body.find(field, at + 1) == body.npos;
    };
    char title_field[32];
    const int used = std::snprintf(title_field, sizeof(title_field),
                                   "\"title_id\":\"%.*s\"",
                                   static_cast<int>(title.size()), title.data());
    if (used <= 0 || static_cast<std::size_t>(used) >= sizeof(title_field))
        return TileUninstallHttpResult::invalid;
    if ((status == "HTTP/1.1 200 OK" || status == "HTTP/1.0 200 OK") &&
        once("\"status\":0") && once(title_field) &&
        once("\"uninstall_requested\":true"))
        return TileUninstallHttpResult::accepted;
    if (status != "HTTP/1.1 409 Conflict" && status != "HTTP/1.0 409 Conflict")
        return TileUninstallHttpResult::invalid;
    if (!once("\"status\":16"))
        return TileUninstallHttpResult::invalid;
    if (once("\"error_reason\":\"game_active\"") &&
        once("\"error\":\"a game is active or awaiting exit; close the game and try again\""))
        return TileUninstallHttpResult::game_active;
    if (once("\"error_reason\":\"scanner_busy\"") &&
        once("\"error\":\"scanner or another storage operation is busy; try again when it finishes\""))
        return TileUninstallHttpResult::scanner_busy;
    return TileUninstallHttpResult::invalid;
}
inline bool tile_uninstall_http_response_valid(std::string_view response,
                                               std::string_view title) {
    return tile_uninstall_http_response(response, title) ==
           TileUninstallHttpResult::accepted;
}
inline bool encode_tile_scan_http_request(char* output, std::size_t capacity) {
    if (!output || !capacity)
        return false;
    constexpr char request[] =
        "POST /api/v1/scan HTTP/1.1\r\n"
        "Host: 127.0.0.1:10101\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n"
        "Connection: close\r\n\r\n{}";
    if (sizeof(request) > capacity)
        return false;
    std::memcpy(output, request, sizeof(request));
    return true;
}
inline bool tile_scan_http_response_valid(std::string_view response) {
    if (response.empty() || response.size() > 4096)
        return false;
    for (unsigned char c : response)
        if ((c < 32 && c != '\r' && c != '\n' && c != '\t') || c == 127)
            return false;
    const auto line_end = response.find("\r\n");
    const auto header_end = response.find("\r\n\r\n");
    if (line_end == response.npos || header_end == response.npos || line_end > header_end)
        return false;
    const std::string_view status(response.data(), line_end);
    if (status != "HTTP/1.1 200 OK" && status != "HTTP/1.0 200 OK")
        return false;
    const std::string_view body(response.data() + header_end + 4,
                                response.size() - header_end - 4);
    return body.find("\"status\":0") != body.npos &&
           body.find("\"scan_queued\":true") != body.npos;
}
inline bool encode_tile_manual_list_http_request(char* output,
                                                 std::size_t capacity) {
    if (!output || !capacity)
        return false;
    constexpr char request[] =
        "POST /api/v1/manual/list HTTP/1.1\r\n"
        "Host: 127.0.0.1:10101\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n"
        "Connection: close\r\n\r\n{}";
    if (sizeof(request) > capacity)
        return false;
    std::memcpy(output, request, sizeof(request));
    return true;
}
inline bool tile_residual_manual_list_valid(std::string_view response) {
    if (response.empty() || response.size() > 8192)
        return false;
    for (unsigned char value : response)
        if ((value < 32 && value != '\r' && value != '\n' && value != '\t') ||
            value == 127)
            return false;
    const auto line_end = response.find("\r\n");
    const auto header_end = response.find("\r\n\r\n");
    if (line_end == response.npos || header_end == response.npos ||
        line_end > header_end)
        return false;
    const std::string_view status(response.data(), line_end);
    if (status != "HTTP/1.1 200 OK" && status != "HTTP/1.0 200 OK")
        return false;
    const std::string_view body(response.data() + header_end + 4,
                                response.size() - header_end - 4);
    const auto once = [&](std::string_view token) {
        const auto at = body.find(token);
        return at != body.npos && body.find(token, at + 1) == body.npos;
    };
    if (body.size() < 35 || body.front() != '{' || body.back() != '}' ||
        !once("\"status\":0") || !once("\"count\":") ||
        !once("\"paths\":["))
        return false;
    for (unsigned number = 99700; number < 99752; ++number) {
        char title[10];
        const int used = std::snprintf(title, sizeof(title), "PPSA%05u", number);
        if (used != 9 || body.find(title) != body.npos)
            return false;
    }
    return true;
}
// RetroArch v4's public external-forwarder contract. These are argument values,
// never shell commands; filenames with spaces remain a single argv element.
inline std::array<const char*, 6> launch_arguments(const LaunchRequest& request) {
    if (request.mode == LaunchMode::retroarch_game)
        return {"--rom", request.rom, "--core", request.core, "--exit-after-game", nullptr};
    // PSSwanStation build 25's documented forwarder contract. Its own read-only
    // FTP source reaches the same console-local games without moving them.
    if (request.mode == LaunchMode::psswan_game)
        return {"--rom", request.rom, "--exit-after-game", nullptr, nullptr, nullptr};
    return {};
}
enum class LaunchStage {
    sending,
    ready,
    committed,
    cancelled,
    started,
    unconfirmed,
    refused,
    invalid,
    foreground_changed,
    reply_failed,
    close_timeout,
    service_unavailable,
    foreground_requested,
    foreground_observed
};
struct LaunchStatus {
    char id[33]{};
    std::uint64_t issued = 0;
    LaunchStage stage = LaunchStage::invalid;
    std::uint32_t code = 0;
};
inline constexpr std::array<std::string_view, 14> launch_stage_names = {
    "sending", "ready",   "committed",          "cancelled",    "started",       "unconfirmed",
    "refused", "invalid", "foreground-changed", "reply-failed", "close-timeout", "service-unavailable",
    "foreground-requested", "foreground-observed"};
inline bool encode_launch_status(const LaunchStatus& status, char* output, std::size_t capacity) {
    const auto stage = static_cast<std::size_t>(status.stage);
    if (!output || !capacity || !launch_bounded(status.id) || std::strlen(status.id) != 32 || !status.issued ||
        stage >= launch_stage_names.size())
        return false;
    for (char c : std::string_view(status.id))
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    const int used = std::snprintf(
        output, capacity, "console-shelf-status 1\nid=%s\nissued=%llu\nstate=%.*s\ncode=%08x\n", status.id,
        static_cast<unsigned long long>(status.issued), static_cast<int>(launch_stage_names[stage].size()),
        launch_stage_names[stage].data(), status.code);
    return used > 0 && static_cast<std::size_t>(used) < capacity;
}
inline bool parse_launch_status(std::string_view message, std::uint64_t now, LaunchStatus& output) {
    if (message.empty() || message.size() >= 256 || message.back() != '\n')
        return false;
    for (unsigned char c : message)
        if ((c < 32 && c != '\n') || c == 127)
            return false;
    constexpr std::string_view header = "console-shelf-status 1\n";
    if (message.substr(0, header.size()) != header)
        return false;
    message.remove_prefix(header.size());
    auto field = [&message](std::string_view tag, char* value, std::size_t capacity) {
        const auto end = message.find('\n');
        if (end == message.npos || end < tag.size() || message.substr(0, tag.size()) != tag ||
            end - tag.size() >= capacity)
            return false;
        const auto length = end - tag.size();
        std::memcpy(value, message.data() + tag.size(), length);
        value[length] = 0;
        message.remove_prefix(end + 1);
        return true;
    };
    LaunchStatus candidate;
    char issued[21]{}, state[24]{}, code[9]{};
    if (!field("id=", candidate.id, sizeof(candidate.id)) || !field("issued=", issued, sizeof(issued)) ||
        !field("state=", state, sizeof(state)) || !field("code=", code, sizeof(code)) || !message.empty() ||
        std::strlen(candidate.id) != 32 || !issued[0] || std::strlen(code) != 8)
        return false;
    for (char c : std::string_view(candidate.id))
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    for (char c : std::string_view(issued)) {
        if (c < '0' || c > '9' || candidate.issued > (UINT64_MAX - static_cast<unsigned>(c - '0')) / 10)
            return false;
        candidate.issued = candidate.issued * 10 + static_cast<unsigned>(c - '0');
    }
    if (!now || !candidate.issued || (now >= candidate.issued && now - candidate.issued > 30) ||
        (candidate.issued > now && candidate.issued - now > 2))
        return false;
    for (char c : std::string_view(code)) {
        const int value = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (value < 0)
            return false;
        candidate.code = candidate.code * 16 + static_cast<unsigned>(value);
    }
    bool found = false;
    for (std::size_t i = 0; i < launch_stage_names.size(); ++i)
        if (state == launch_stage_names[i]) {
            candidate.stage = static_cast<LaunchStage>(i);
            found = true;
        }
    if (!found)
        return false;
    output = candidate;
    return true;
}
inline bool launch_status_owned_by_request(std::string_view message,
                                           const LaunchRequest& request,
                                           LaunchStatus& output) {
    if (!launch_bounded(request.id) || std::strlen(request.id) != 32 || !request.issued)
        return false;
    LaunchStatus candidate;
    // The helper already consumed and validated this nonce-bound request. Use
    // its own issuance time only to parse the existing status shape, then bind
    // both exact identity fields. This lets a long bounded maintenance action
    // publish its final state without making stale status acceptable to clients.
    if (!parse_launch_status(message, request.issued, candidate) ||
        candidate.issued != request.issued ||
        std::strcmp(candidate.id, request.id) != 0)
        return false;
    output = candidate;
    return true;
}
enum class MaintenanceEndpointProbe { ready, unavailable, failed };
enum class MaintenanceEndpointResult {
    ready,
    invalid,
    foreground_changed,
    timeout,
    failed,
};
struct MaintenanceEndpointOps {
    void* context = nullptr;
    // The readiness probe is read-only. It may open and close the fixed
    // ShadowMount loopback port, but it must not send an HTTP request.
    MaintenanceEndpointProbe (*probe)(void*) = nullptr;
    // 0: no running big app; any other value stops readiness polling.
    int (*current)(void*, char (&title)[10]) = nullptr;
    void (*wait)(void*, unsigned milliseconds) = nullptr;
};
inline MaintenanceEndpointResult wait_for_maintenance_endpoint(
        const MaintenanceEndpointOps& ops) {
    if (!ops.probe || !ops.current || !ops.wait)
        return MaintenanceEndpointResult::invalid;
    // One immediate probe plus 32 bounded 250 ms waits: at most eight
    // seconds, and still no uninstall request has been sent.
    for (unsigned attempt = 0; attempt <= 32; ++attempt) {
        char title[10]{};
        if (ops.current(ops.context, title) != 0)
            return MaintenanceEndpointResult::foreground_changed;
        const auto result = ops.probe(ops.context);
        if (result == MaintenanceEndpointProbe::ready)
            return MaintenanceEndpointResult::ready;
        if (result == MaintenanceEndpointProbe::failed)
            return MaintenanceEndpointResult::failed;
        if (attempt == 32)
            return MaintenanceEndpointResult::timeout;
        ops.wait(ops.context, 250);
    }
    return MaintenanceEndpointResult::timeout;
}
enum class HandoffResult {
    invalid,
    foreground_changed,
    reply_failed,
    close_timeout,
    started,
    unconfirmed,
    refused,
    cancelled,
    foreground_observed
};
enum class HandoffConsent { pending, committed, cancelled };
// A current-title probe may briefly lose the title while the exact, previously
// identified isShelf app ID is being released by the shell. This state never
// counts as idle and never authorizes a launch; it only permits the bounded
// close loop to keep observing until a real no-app result is stable.
inline constexpr int handoff_origin_releasing = 2;
struct HandoffOps {
    void* context = nullptr;
    // 0: no running big app; 1: title filled; -1: current title is unknown;
    // handoff_origin_releasing: the previously proven origin ID is releasing.
    int (*current)(void*, char (&title)[10]) = nullptr;
    bool (*ready)(void*, const char* request_id) = nullptr;
    void (*wait)(void*, unsigned milliseconds) = nullptr;
    // After five seconds of committed, continuously observed isShelf foreground,
    // one exact-origin close may be requested. No alternate title or retry.
    int (*close_origin)(void*) = nullptr;
    // One committed launch or bounded tile-maintenance operation only. No
    // alternate or blind retry.
    int (*launch)(void*, const LaunchRequest&) = nullptr;
    std::uint64_t (*now)(void*) = nullptr;
    HandoffConsent (*consent)(void*) = nullptr;
    // Read-only focus observation: 1 target focused, 0 not focused, -1 unknown.
    int (*focused)(void*, const LaunchRequest&) = nullptr;
};
inline HandoffResult cooperative_handoff(const LaunchRequest& request, const HandoffOps& ops) {
    if (!ops.current || !ops.ready || !ops.wait || !ops.close_origin || !ops.launch ||
        !ops.now || !ops.consent || !ops.focused ||
        !launch_request_valid(request, ops.now(ops.context)))
        return HandoffResult::invalid;
    char title[10]{};
    if (ops.current(ops.context, title) != 1 || !launch_bounded(title) || std::strcmp(title, shelf_title_id) != 0)
        return HandoffResult::foreground_changed;
    if (!ops.ready(ops.context, request.id))
        return HandoffResult::reply_failed;
    unsigned idle = 0;
    bool origin_close_requested = false;
    for (unsigned i = 0; i < 60; ++i) {
        const auto consent = ops.consent(ops.context);
        if (consent == HandoffConsent::cancelled)
            return HandoffResult::cancelled;
        title[0] = 0;
        const int current = ops.current(ops.context, title);
        if (current == handoff_origin_releasing) {
            // Do not mistake the shell's title-release gap for Home. Only a
            // committed handoff may continue through it, and a full stable
            // no-app interval is still required afterward.
            if (consent != HandoffConsent::committed)
                return HandoffResult::cancelled;
            idle = 0;
            if (i == 59)
                return HandoffResult::close_timeout;
            ops.wait(ops.context, 250);
            continue;
        }
        if (current < 0 || (current && (!launch_bounded(title) || std::strcmp(title, shelf_title_id) != 0)))
            return HandoffResult::foreground_changed;
        if (current == 0 && consent != HandoffConsent::committed)
            return HandoffResult::cancelled;
        if (current == 1 && consent == HandoffConsent::committed &&
            i >= 20 && !origin_close_requested) {
            origin_close_requested = true;
            (void)ops.close_origin(ops.context);
        }
        idle = current == 0 ? idle + 1 : 0;
        // Five observations 250 ms apart give the shell a full second after
        // Shelf disappears. Launching during its asynchronous return-home
        // transition can leave the new app running behind the home screen.
        if (idle >= 5)
            break;
        if (i == 59)
            return HandoffResult::close_timeout;
        ops.wait(ops.context, 250);
    }
    if (!launch_request_valid(request, ops.now(ops.context)))
        return HandoffResult::invalid;
    if (ops.consent(ops.context) != HandoffConsent::committed)
        return HandoffResult::cancelled;
    const int result = ops.launch(ops.context, request);
    if (request.mode == LaunchMode::tile_uninstall ||
        request.mode == LaunchMode::tile_remove ||
        request.mode == LaunchMode::tile_refresh ||
        request.mode == LaunchMode::tile_residual ||
        request.mode == LaunchMode::self_update ||
        request.mode == LaunchMode::update_recover)
        return result == 0 ? HandoffResult::started : HandoffResult::refused;
    bool observed = false;
    for (unsigned i = 0; i < (result >= 0 ? 32u : 16u); ++i) {
        title[0] = 0;
        const int current = ops.current(ops.context, title);
        if (current == 1 && launch_bounded(title) && std::strcmp(title, request.title) == 0) {
            observed = true;
            break;
        }
        // An unknown or different app can never authorize a second launch.
        if (current != 0)
            break;
        ops.wait(ops.context, 250);
    }
    if (!observed)
        return result >= 0 ? HandoffResult::unconfirmed : HandoffResult::refused;
    // A running title is not proof of focus. Observe the shell after its one
    // launch flow; never issue a second launch or a focus-changing API call.
    for (unsigned i = 0; i < 32; ++i) {
        if (!launch_request_valid(request, ops.now(ops.context)))
            return HandoffResult::invalid;
        if (ops.consent(ops.context) != HandoffConsent::committed)
            return HandoffResult::cancelled;
        title[0] = 0;
        if (ops.current(ops.context, title) != 1 || !launch_bounded(title) || std::strcmp(title, request.title) != 0)
            return HandoffResult::foreground_changed;
        const int focus = ops.focused(ops.context, request);
        if (focus == 1)
            return HandoffResult::foreground_observed;
        if (focus < 0)
            return HandoffResult::unconfirmed;
        if (i < 31)
            ops.wait(ops.context, 250);
    }
    return HandoffResult::unconfirmed;
}
} // namespace shelf
