// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace shelf {
inline constexpr char console_tile_probe_request_name[] = "console-tile-path-request-v2.txt";
inline constexpr char console_tile_probe_receipt_prefix[] = "console-tile-path-v2-";
inline constexpr char console_tile_probe_root[] = "/data/console-shelf";

inline bool console_tile_probe_nonce(const char* nonce) {
    if (!nonce || std::strlen(nonce) != 32) return false;
    for (unsigned i = 0; i < 32; ++i)
        if (!((nonce[i] >= '0' && nonce[i] <= '9') || (nonce[i] >= 'a' && nonce[i] <= 'f')))
            return false;
    return true;
}

// The nonce is public, but only the owner may change a request after it has
// been created. Check the real mode after umask instead of trusting open's mode.
inline bool console_tile_probe_request_mode_allowed(unsigned mode) {
    return (mode & 0004u) != 0 && (mode & 0022u) == 0;
}

inline bool console_tile_probe_request(const char* nonce, char (&out)[96], std::size_t& used) {
    if (!console_tile_probe_nonce(nonce)) return false;
    const int count = std::snprintf(out, sizeof(out),
        "format=isshelf-console-tile-path-probe/2\nnonce=%s\n", nonce);
    if (count <= 0 || static_cast<std::size_t>(count) >= sizeof(out)) return false;
    used = static_cast<std::size_t>(count);
    return true;
}

inline bool console_tile_probe_receipt_name(const char* nonce, char (&out)[96]) {
    if (!console_tile_probe_nonce(nonce)) return false;
    const int count = std::snprintf(out, sizeof(out), "%s%s.txt", console_tile_probe_receipt_prefix, nonce);
    return count > 0 && static_cast<std::size_t>(count) < sizeof(out);
}

// The app's native runtime does not provide a usable openat import. Keep its
// one-shot request and read-only receipt at these exact absolute paths. The
// v2 payload also uses fixed absolute open paths under its separate SDK runtime.
template<std::size_t N>
inline bool console_tile_probe_request_path(char (&out)[N]) {
    const int count = std::snprintf(out, N, "%s/%s", console_tile_probe_root,
                                    console_tile_probe_request_name);
    return count > 0 && static_cast<std::size_t>(count) < N;
}

template<std::size_t N>
inline bool console_tile_probe_receipt_path(const char* nonce, char (&out)[N]) {
    char name[96];
    if (!console_tile_probe_receipt_name(nonce, name)) return false;
    const int count = std::snprintf(out, N, "%s/%s", console_tile_probe_root, name);
    return count > 0 && static_cast<std::size_t>(count) < N;
}

// The ELF's output is a fixed ordered record. Exact shape and paths are checked
// before the bridge calls it complete; numeric observations remain evidence,
// not an assertion that any title is installed or writable.
inline bool console_tile_probe_receipt_valid(std::string_view bytes, const char* nonce) {
    if (!console_tile_probe_nonce(nonce) || bytes.empty() || bytes.size() > 8192) return false;
    std::size_t offset = 0;
    const auto line = [&](std::string_view key, std::string_view value, bool numeric = false,
                          bool octal = false) -> bool {
        if (offset >= bytes.size() || bytes.find('\n', offset) == std::string_view::npos) return false;
        const auto end = bytes.find('\n', offset);
        const auto item = bytes.substr(offset, end - offset);
        offset = end + 1;
        if (item.size() < key.size() + 1 || item.substr(0, key.size()) != key || item[key.size()] != '=')
            return false;
        const auto found = item.substr(key.size() + 1);
        if (!numeric) return found == value;
        if (found.empty() || found.size() > 10) return false;
        for (char digit : found)
            if (digit < '0' || digit > (octal ? '7' : '9')) return false;
        return true;
    };
    if (!line("format", "isshelf-console-tile-path-probe/2") || !line("nonce", nonce) ||
        !line("runtime", "standard-sdk-v0.42") || !line("request_unlinked", "0")) return false;
    constexpr const char* roots[] = {"/data/homebrew", "/user/app", "/user/appmeta"};
    constexpr const char* paths[] = {
        "/data/homebrew/PPSA99541", "/data/homebrew/PPSA99704", "/user/app/PPSA99704",
        "/user/appmeta/PPSA99704", "/data/homebrew/PPSA99752", "/user/app/PPSA99752",
        "/user/appmeta/PPSA99752"};
    constexpr const char* path_fields[] = {
        "lstat_errno", "lstat_type", "lstat_mode", "stat_errno", "stat_type", "stat_mode",
        "open_errno", "fstat_errno", "fstat_type", "fstat_mode", "fstat_same_lstat"};
    constexpr const char* dir_fields[] = {
        "dir_open_errno", "dir_read_errno", "dir_close_errno", "entries_seen", "complete",
        "seen_PPSA99541", "seen_PPSA99704", "seen_PPSA99752"};
    char key[64];
    for (unsigned group = 0; group < 10; ++group) {
        const bool root = group < 3;
        const unsigned index = root ? group : group - 3;
        const char* prefix = root ? "root" : "path";
        if (std::snprintf(key, sizeof(key), "%s_%u", prefix, index) <= 0 ||
            !line(key, root ? roots[index] : paths[index])) return false;
        for (const char* field : path_fields) {
            if (std::snprintf(key, sizeof(key), "%s_%u_%s", prefix, index, field) <= 0 ||
                !line(key, {}, true, std::strstr(field, "mode") != nullptr)) return false;
        }
        if (root)
            for (const char* field : dir_fields) {
                if (std::snprintf(key, sizeof(key), "root_%u_%s", index, field) <= 0 ||
                    !line(key, {}, true)) return false;
            }
    }
    return line("title_root_write_attempted", "0") && line("database_open_attempted", "0") &&
           line("database_write_attempted", "0") && line("network_attempted", "0") &&
           line("record_complete", "1") && offset == bytes.size();
}

inline std::uint64_t console_tile_probe_fnv(std::string_view bytes) {
    std::uint64_t value = 14695981039346656037ull;
    for (unsigned char byte : bytes) value = (value ^ byte) * 1099511628211ull;
    return value;
}
} // namespace shelf
