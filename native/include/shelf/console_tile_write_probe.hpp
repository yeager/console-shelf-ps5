// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "console_tile_path_probe.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace shelf {
inline constexpr char console_tile_write_request_name[] = "console-tile-write-request-v3.txt";
inline constexpr char console_tile_write_receipt_prefix[] = "console-tile-write-v3-";
inline constexpr char console_tile_write_scratch_prefix[] = ".isshelf-write-probe-v3-";
inline constexpr const char* console_tile_write_stages[] = {
    "receipt_root_open", "request_read", "receipt_reserve", "receipt_mode",
    "receipt_header_write", "receipt_header_sync", "homebrew_open", "homebrew_stat",
    "scratch_absence", "scratch_mkdirat", "scratch_openat", "scratch_identity",
    "scratch_private_mode", "file_openat", "file_write", "file_fsync",
    "file_seek", "file_readback", "file_close", "renameat",
    "directory_fsync", "final_openat", "final_readback", "final_close",
    "file_unlinkat", "directory_resync", "scratch_close", "scratch_cleanup_identity",
    "scratch_unlinkat", "homebrew_fsync", "homebrew_close", "remaining_lstat"
};

inline bool console_tile_write_request(const char* nonce, char (&out)[96], std::size_t& used) {
    if (!console_tile_probe_nonce(nonce)) return false;
    const int count = std::snprintf(out, sizeof(out),
        "format=isshelf-console-tile-write-probe/3\nnonce=%s\n", nonce);
    if (count <= 0 || static_cast<std::size_t>(count) >= sizeof(out)) return false;
    used = static_cast<std::size_t>(count);
    return true;
}

inline bool console_tile_write_receipt_name(const char* nonce, char (&out)[96]) {
    if (!console_tile_probe_nonce(nonce)) return false;
    const int count = std::snprintf(out, sizeof(out), "%s%s.txt", console_tile_write_receipt_prefix, nonce);
    return count > 0 && static_cast<std::size_t>(count) < sizeof(out);
}

template<std::size_t N>
inline bool console_tile_write_request_path(char (&out)[N]) {
    const int count = std::snprintf(out, N, "%s/%s", console_tile_probe_root,
                                    console_tile_write_request_name);
    return count > 0 && static_cast<std::size_t>(count) < N;
}

template<std::size_t N>
inline bool console_tile_write_receipt_path(const char* nonce, char (&out)[N]) {
    char name[96];
    if (!console_tile_write_receipt_name(nonce, name)) return false;
    const int count = std::snprintf(out, N, "%s/%s", console_tile_probe_root, name);
    return count > 0 && static_cast<std::size_t>(count) < N;
}

inline bool console_tile_write_receipt_valid(std::string_view bytes, const char* nonce) {
    if (!console_tile_probe_nonce(nonce) || bytes.empty() || bytes.size() > 8192) return false;
    std::size_t offset = 0;
    const auto line = [&](std::string_view key, std::string_view& value) -> bool {
        const auto end = bytes.find('\n', offset);
        if (end == std::string_view::npos) return false;
        const auto item = bytes.substr(offset, end - offset);
        offset = end + 1;
        if (item.size() <= key.size() || item.substr(0, key.size()) != key || item[key.size()] != '=')
            return false;
        value = item.substr(key.size() + 1);
        return true;
    };
    const auto exact = [&](std::string_view key, std::string_view expected) -> bool {
        std::string_view value;
        return line(key, value) && value == expected;
    };
    const auto number = [&](std::string_view key, bool signed_value, int& parsed) -> bool {
        std::string_view value;
        if (!line(key, value) || value.empty() || value.size() > 5) return false;
        std::size_t start = 0;
        if (signed_value && value[0] == '-') {
            if (value != "-1") return false;
            parsed = -1;
            return true;
        }
        parsed = 0;
        for (; start < value.size(); ++start) {
            if (value[start] < '0' || value[start] > '9') return false;
            parsed = parsed * 10 + (value[start] - '0');
        }
        return parsed <= 4095;
    };
    if (!exact("format", "isshelf-console-tile-write-probe/3") ||
        !exact("nonce", nonce) || !exact("runtime", "standard-sdk-v0.42")) return false;
    char expected_scratch[128];
    const int scratch_size = std::snprintf(expected_scratch, sizeof(expected_scratch),
        "/data/homebrew/%s%s", console_tile_write_scratch_prefix, nonce);
    if (scratch_size <= 0 || static_cast<std::size_t>(scratch_size) >= sizeof(expected_scratch) ||
        !exact("scratch", expected_scratch)) return false;
    std::string_view remaining;
    if (!line("scratch_remaining", remaining) ||
        (remaining != "absent" && remaining != "present" && remaining != "unknown")) return false;
    int remaining_errno = 0;
    std::string_view scratch_write;
    std::string_view scratch_created;
    if (!number("scratch_remaining_errno", false, remaining_errno) ||
        !exact("valid_title_id_write_attempted", "0") ||
        !line("homebrew_scratch_write_attempted", scratch_write) ||
        (scratch_write != "0" && scratch_write != "1") ||
        !line("homebrew_scratch_created", scratch_created) ||
        (scratch_created != "0" && scratch_created != "1") ||
        !exact("database_open_attempted", "0") ||
        !exact("network_attempted", "0")) return false;
    bool all_zero = true;
    for (const char* stage : console_tile_write_stages) {
        char key[80];
        const int used = std::snprintf(key, sizeof(key), "stage_%s_errno", stage);
        int error = 0;
        if (used <= 0 || static_cast<std::size_t>(used) >= sizeof(key) ||
            !number(key, true, error)) return false;
        all_zero &= error == 0;
    }
    std::string_view first;
    if (!line("first_failure", first)) return false;
    bool known_failure = first == "none";
    for (const char* stage : console_tile_write_stages) known_failure |= first == stage;
    if (!known_failure) return false;
    int first_errno = 0;
    if (!number("first_errno", false, first_errno)) return false;
    std::string_view complete;
    if (!line("transaction_complete", complete) ||
        (complete != "0" && complete != "1") ||
        !exact("record_complete", "1") || offset != bytes.size()) return false;
    if (complete == "1")
        return scratch_write == "1" && scratch_created == "1" && remaining == "absent" &&
               remaining_errno == ENOENT && first == "none" &&
               first_errno == 0 && all_zero;
    return first != "none" && first_errno > 0;
}
} // namespace shelf
