// SPDX-License-Identifier: GPL-3.0-only
// Offline candidate for a read-only probe from the isShelf app process.
// It is not wired to UI control and never creates, changes, or registers a tile.
#pragma once
#if defined(_WIN32)
#error "The protected PS2 source probe requires POSIX descriptor semantics"
#endif

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace shelf::protected_ps2_source_read_probe {

inline constexpr const char* fixed_root = "/";
inline constexpr const char* pilot_folder = ".ps2-pilot-745991606ec7";
inline constexpr const char* pilot_title = "PPSA99703";
inline constexpr std::size_t node_count = 8;
inline constexpr std::size_t path_capacity = 512;

struct Identity {
    dev_t device{};
    ino_t inode{};
    uid_t owner{};
    gid_t group{};
    mode_t mode{};
    off_t size{};
};

struct Result {
    bool complete = false;
    const char* failure = "none";
    uid_t uid{};
    uid_t euid{};
    Identity nodes[node_count]{};
    std::size_t observed_nodes = 0;
    std::size_t failed_node = node_count;
    unsigned failed_pass = 0;
    int system_error = 0;
    // Only one byte per file is read; the bytes are never included in a report.
    unsigned files_read = 0;
};

inline bool same_identity(const Identity& a, const Identity& b) {
    return a.device == b.device && a.inode == b.inode &&
           a.owner == b.owner && a.group == b.group &&
           a.mode == b.mode &&
           (!S_ISREG(a.mode) || a.size == b.size);
}

inline Identity identity(const struct stat& info) {
    return {info.st_dev, info.st_ino, info.st_uid, info.st_gid,
            info.st_mode, info.st_size};
}

inline bool join(char* out, std::size_t capacity, const char* parent,
                 const char* child) {
    const int used = std::snprintf(out, capacity, std::strcmp(parent, "/") == 0
        ? "/%s" : "%s/%s", parent, child);
    return used > 0 && static_cast<std::size_t>(used) < capacity;
}

inline bool make_paths(const char* root, char paths[node_count][path_capacity]) {
    if (!root || root[0] != '/' || std::strlen(root) >= path_capacity ||
        (std::strcmp(root, "/") != 0 && root[std::strlen(root) - 1] == '/'))
        return false;
#ifdef SHELF_PS5
    if (std::strcmp(root, fixed_root) != 0) return false;
#endif
    if (std::snprintf(paths[0], path_capacity, "%s", root) <= 0) return false;
    return join(paths[1], path_capacity, paths[0], "system_data") &&
           join(paths[2], path_capacity, paths[1], "isshelf-tiles") &&
           join(paths[3], path_capacity, paths[2], pilot_folder) &&
           join(paths[4], path_capacity, paths[3], pilot_title) &&
           join(paths[5], path_capacity, paths[4], "sce_sys") &&
           join(paths[6], path_capacity, paths[4], "eboot.bin") &&
           join(paths[7], path_capacity, paths[5], "param.json");
}

inline bool close_all(int fds[node_count]) {
    bool good = true;
    for (std::size_t i = 0; i < node_count; ++i) {
        if (fds[i] >= 0) {
            if (::close(fds[i]) != 0) good = false;
            fds[i] = -1;
        }
    }
    return good;
}

inline bool capture(const char paths[node_count][path_capacity],
                    uid_t expected_owner, Identity out[node_count],
                    int fds[node_count], unsigned& files_read,
                    std::size_t& observed, std::size_t& failed_node,
                    const char*& failure, int& system_error) {
    files_read = 0;
    observed = 0;
    for (std::size_t i = 0; i < node_count; ++i) {
        failed_node = i;
        const bool directory = i < 6;
        const int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC |
                          (directory ? O_DIRECTORY : 0);
        fds[i] = ::open(paths[i], flags);
        if (fds[i] < 0) { failure = "open"; system_error = errno; return false; }
        struct stat before{}, after{};
        if (::fstat(fds[i], &before) != 0) {
            failure = "stat"; system_error = errno; return false;
        }
        out[i] = identity(before);
        observed = i + 1;
        const mode_t expected_mode = directory ? 0755 : (i == 6 ? 0755 : 0644);
        const bool type_ok = directory ? S_ISDIR(before.st_mode) : S_ISREG(before.st_mode);
        if (!type_ok || before.st_uid != expected_owner ||
            (i != 0 && (before.st_mode & 07777) != expected_mode) ||
            (i > 1 && before.st_dev != out[1].device)) {
            failure = "identity-policy"; return false;
        }
        if (!directory) {
            const off_t max_size = i == 6 ? 16 * 1024 * 1024 : 128 * 1024;
            if (before.st_size <= 0 || before.st_size > max_size ||
                (before.st_nlink != 0 && before.st_nlink != 1)) {
                failure = "file-bound"; return false;
            }
            unsigned char byte = 0;
            const ssize_t got = ::read(fds[i], &byte, 1);
            if (got != 1) {
                failure = "read"; system_error = got < 0 ? errno : 0; return false;
            }
            ++files_read;
        }
        if (::fstat(fds[i], &after) != 0) {
            failure = "re-stat"; system_error = errno; return false;
        }
        if (!same_identity(out[i], identity(after))) {
            failure = "changed-during-read"; return false;
        }
    }
    failed_node = node_count;
    return true;
}

inline Result inspect_impl(const char* root, uid_t expected_owner,
                           uid_t expected_euid,
                           void (*between_passes)(void*), void* context) {
    Result result;
    result.uid = ::getuid();
    result.euid = ::geteuid();
    char paths[node_count][path_capacity]{};
    if (!make_paths(root, paths)) { result.failure = "path"; return result; }
    if (result.euid != expected_euid) {
        result.failure = "process-identity";
        return result;
    }
    Identity first[node_count]{}, second[node_count]{};
    int held[node_count], reopened[node_count];
    for (std::size_t i = 0; i < node_count; ++i) held[i] = reopened[i] = -1;
    unsigned first_reads = 0, second_reads = 0;
    const char* capture_failure = "none";
    std::size_t observed = 0, failed_node = node_count;
    int system_error = 0;
    if (!capture(paths, expected_owner, first, held, first_reads,
                 observed, failed_node, capture_failure, system_error)) {
        for (std::size_t i = 0; i < observed; ++i) result.nodes[i] = first[i];
        result.observed_nodes = observed;
        result.failed_node = failed_node;
        result.failed_pass = 1;
        result.system_error = system_error;
        close_all(held);
        result.failure = capture_failure;
        return result;
    }
#ifdef SHELF_PS5
    if (first[0].device == first[1].device) {
        for (std::size_t i = 0; i < node_count; ++i) result.nodes[i] = first[i];
        result.observed_nodes = node_count;
        result.failed_node = 1;
        result.failed_pass = 1;
        close_all(held);
        result.failure = "mount-identity";
        return result;
    }
#endif
    if (between_passes) between_passes(context);
    if (!capture(paths, expected_owner, second, reopened, second_reads,
                 observed, failed_node, capture_failure, system_error)) {
        for (std::size_t i = 0; i < node_count; ++i) result.nodes[i] = first[i];
        result.observed_nodes = node_count;
        result.failed_node = failed_node;
        result.failed_pass = 2;
        result.system_error = system_error;
        close_all(reopened);
        close_all(held);
        result.failure = capture_failure;
        return result;
    }
    bool stable = first_reads == 2 && second_reads == 2;
    for (std::size_t i = 0; i < node_count; ++i) {
        struct stat held_now{};
        const bool node_stable = same_identity(first[i], second[i]) &&
            ::fstat(held[i], &held_now) == 0 &&
            same_identity(first[i], identity(held_now));
        if (!node_stable && result.failed_node == node_count) {
            result.failed_node = i;
            result.failed_pass = 2;
        }
        stable = stable && node_stable;
        result.nodes[i] = first[i];
    }
    const bool closed_reopened = close_all(reopened);
    const bool closed_held = close_all(held);
    result.observed_nodes = node_count;
    if (!stable) { result.failure = "identity-changed"; return result; }
    if (!closed_reopened || !closed_held) {
        result.failure = "close";
        return result;
    }
    result.files_read = first_reads + second_reads;
    result.complete = true;
    return result;
}

inline Result inspect(const char* root, uid_t expected_owner,
                      uid_t expected_euid) {
    return inspect_impl(root, expected_owner, expected_euid, nullptr, nullptr);
}

#ifndef SHELF_PS5
// Deterministic host race fixture. It is unavailable in the PS5 build.
inline Result inspect_host_with_hook(const char* root, uid_t expected_owner,
                                     uid_t expected_euid,
                                     void (*between_passes)(void*), void* context) {
    return inspect_impl(root, expected_owner, expected_euid,
                        between_passes, context);
}
#endif

} // namespace shelf::protected_ps2_source_read_probe
