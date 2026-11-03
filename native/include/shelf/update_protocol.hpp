// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string_view>

namespace shelf {
inline constexpr char update_api_url[] =
    "https://api.github.com/repos/Iswebro/console-shelf-ps5/releases/latest";
inline constexpr char update_download_prefix[] =
    "https://github.com/Iswebro/console-shelf-ps5/releases/download/";
inline constexpr char update_asset_name[] = "PPSA99541.zip";
inline constexpr char update_marker[] = "ISSHELF_IN_APP_UPDATER_V1";
inline constexpr std::uint64_t update_min_archive_bytes = 1024 * 1024;
inline constexpr std::uint64_t update_max_archive_bytes = 512ull * 1024 * 1024;
inline constexpr std::size_t update_max_files = 512;

struct UpdateRelease {
    char tag[32]{};
    char title[96]{};
    char asset_url[512]{};
    char sha256[65]{};
    std::uint64_t bytes = 0;
};
enum class UpdateDecision { invalid, current, newer, available };

inline bool update_semver(std::string_view value, unsigned (&parts)[3]) {
    parts[0] = parts[1] = parts[2] = 0;
    if (value.size() < 6 || value.front() != 'v') return false;
    value.remove_prefix(1);
    for (std::size_t index = 0; index < 3; ++index) {
        const auto dot = value.find('.');
        const auto number = std::string_view(value.data(), dot == value.npos ? value.size() : dot);
        if (number.empty() || (number.size() > 1 && number.front() == '0') || number.size() > 9)
            return false;
        unsigned parsed = 0;
        for (const char c : number) {
            if (c < '0' || c > '9' ||
                parsed > (UINT32_MAX - static_cast<unsigned>(c - '0')) / 10)
                return false;
            parsed = parsed * 10 + static_cast<unsigned>(c - '0');
        }
        parts[index] = parsed;
        if (index < 2) {
            if (dot == value.npos) return false;
            value.remove_prefix(dot + 1);
        } else if (dot != value.npos) {
            return false;
        }
    }
    return true;
}
inline int compare_update_versions(std::string_view left, std::string_view right) {
    unsigned a[3]{}, b[3]{};
    if (!update_semver(left, a) || !update_semver(right, b)) return 2;
    for (std::size_t i = 0; i < 3; ++i)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
UpdateDecision parse_update_release(std::string_view json, std::string_view current,
                                    UpdateRelease& release, char* error, std::size_t error_capacity);

inline bool update_hex_sha256(std::string_view value) {
    if (value.size() != 64) return false;
    for (const char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}
inline bool update_path_safe(std::string_view path) {
    if (path.empty() || path.size() >= 256 || path.front() == '/' || path.back() == '/' ||
        path.find('\\') != path.npos || path.find(':') != path.npos)
        return false;
    std::size_t start = 0;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto part = std::string_view(path.data() + start,
            end == path.npos ? path.size() - start : end - start);
        if (part.empty() || part == "." || part == "..") return false;
        for (const unsigned char c : part)
            if (c < 32 || c == 127 || c == '\t') return false;
        start = end == path.npos ? path.size() : end + 1;
    }
    return true;
}

// The updater may replace only packaged isShelf program material. Preferences,
// routes, personal artwork and update state live under /data/console-shelf;
// emulators, BIOS, games, ROMs and saves live outside this program tree. An
// unknown top-level member is rejected rather than risking user-created data
// that may have been placed beside the application manually.
inline bool update_program_path(std::string_view path) {
    if (!update_path_safe(path)) return false;
    for (const std::string_view exact : {
             std::string_view("eboot.bin"), std::string_view("LICENSE"),
             std::string_view("THIRD_PARTY.md"), std::string_view("START-HERE.md"),
             std::string_view("build-source.json")})
        if (path == exact) return true;
    for (const std::string_view prefix : {
             std::string_view("assets/"), std::string_view("licenses/"),
             std::string_view("native/assets/fonts/"), std::string_view("sce_module/"),
             std::string_view("sce_sys/")})
        if (path.size() > prefix.size() && path.substr(0, prefix.size()) == prefix) return true;
    return false;
}

struct UpdateManifestEntry {
    char path[256]{};
    char sha256[65]{};
    std::uint64_t bytes = 0;
};
struct UpdateManifest {
    char tag[32]{};
    std::size_t count = 0;
    std::uint64_t total_bytes = 0;
};

inline bool parse_update_unsigned(std::string_view text, std::uint64_t& value) {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) return false;
    value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9' || value > (UINT64_MAX - static_cast<unsigned>(c - '0')) / 10)
            return false;
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    return true;
}

inline bool parse_update_manifest(std::string_view text, UpdateManifest& output,
                                  UpdateManifestEntry* entries, std::size_t capacity) {
    constexpr std::string_view header = "isshelf-update 1\n";
    if (!entries || text.size() < header.size() ||
        std::memcmp(text.data(), header.data(), header.size()) != 0 ||
        text.size() > 256 * 1024 || text.back() != '\n')
        return false;
    text.remove_prefix(header.size());
    const auto line = [&text](std::string_view prefix, std::string_view& value) {
        const auto end = text.find('\n');
        if (end == text.npos || end < prefix.size() || text.size() < prefix.size() ||
            std::memcmp(text.data(), prefix.data(), prefix.size()) != 0) return false;
        value = std::string_view(text.data() + prefix.size(), end - prefix.size());
        text.remove_prefix(end + 1);
        return true;
    };
    std::string_view tag, count_text;
    unsigned version[3]{};
    std::uint64_t count = 0;
    if (!line("tag=", tag) || tag.size() >= sizeof(output.tag) || !update_semver(tag, version) ||
        !line("count=", count_text) || !parse_update_unsigned(count_text, count) ||
        !count || count > update_max_files || count > capacity)
        return false;
    UpdateManifest candidate;
    std::memcpy(candidate.tag, tag.data(), tag.size());
    candidate.tag[tag.size()] = 0;
    candidate.count = static_cast<std::size_t>(count);
    bool have_eboot = false, have_param = false;
    for (std::size_t i = 0; i < candidate.count; ++i) {
        std::string_view record;
        if (!line("file=", record)) return false;
        const auto bytes_at = record.find("\tbytes=");
        const auto sha_at = record.find("\tsha256=", bytes_at == record.npos ? 0 : bytes_at + 7);
        if (bytes_at == record.npos || sha_at == record.npos ||
            record.find('\t', sha_at + 8) != record.npos)
            return false;
        const auto path = std::string_view(record.data(), bytes_at);
        const auto size_text = std::string_view(record.data() + bytes_at + 7,
                                                sha_at - (bytes_at + 7));
        const auto sha = std::string_view(record.data() + sha_at + 8,
                                          record.size() - (sha_at + 8));
        std::uint64_t bytes = 0;
        if (!update_program_path(path) || !parse_update_unsigned(size_text, bytes) || !update_hex_sha256(sha) ||
            bytes > update_max_archive_bytes || candidate.total_bytes > update_max_archive_bytes - bytes)
            return false;
        for (std::size_t prior = 0; prior < i; ++prior)
            if (path == entries[prior].path) return false;
        auto& entry = entries[i];
        std::memcpy(entry.path, path.data(), path.size());
        entry.path[path.size()] = 0;
        std::memcpy(entry.sha256, sha.data(), sha.size());
        entry.sha256[sha.size()] = 0;
        entry.bytes = bytes;
        candidate.total_bytes += bytes;
        have_eboot = have_eboot || path == "eboot.bin";
        have_param = have_param || path == "sce_sys/param.json";
    }
    if (!text.empty() || !have_eboot || !have_param) return false;
    output = candidate;
    return true;
}
} // namespace shelf
