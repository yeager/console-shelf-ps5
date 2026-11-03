// SPDX-License-Identifier: GPL-3.0-only
// One fixed, bounded, read-only app-process observation of the PS2 pilot file.
#pragma once
#if defined(_WIN32)
#error "The direct PS2 file probe requires POSIX descriptor semantics"
#endif

#include "pinned_helper_sha256.hpp"
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace shelf::protected_ps2_direct_file_probe {

inline constexpr char fixed_path[] =
    "/system_data/isshelf-tiles/.ps2-pilot-745991606ec7/PPSA99703/sce_sys/param.json";
inline constexpr std::size_t expected_bytes = 676;
inline constexpr char expected_sha256[] =
    "9d26a475126b9272601879cb2b4cadbadcc0e68a1644fbf02e5d0c68a848f11d";

struct Result {
    bool complete = false;
    bool target_opened = false;
    bool content_matched = false;
    bool mount_distinct = false;
    const char* failure = "none";
    int system_error = 0;
    uid_t uid{};
    uid_t euid{};
    std::size_t bytes_read = 0;
    std::uint64_t root_device = 0, system_device = 0;
    std::uint64_t target_device = 0, target_inode = 0, target_owner = 0, target_mode = 0;
    std::int64_t target_size = 0;
};

// Host tests may prefix the immutable console path with an absolute fixture
// root. The console build accepts only "/".
inline Result inspect(const char* root = "/") {
    Result result;
    result.uid = ::getuid();
    result.euid = ::geteuid();
    if (!root || root[0] != '/' || std::strlen(root) > 350 ||
        (std::strcmp(root, "/") != 0 && root[std::strlen(root) - 1] == '/')) {
        result.failure = "path";
        return result;
    }
#ifdef SHELF_PS5
    if (std::strcmp(root, "/") != 0) { result.failure = "path"; return result; }
#endif
    char target[512]{};
    char system[512]{};
    const int target_length = std::snprintf(target, sizeof(target), "%s%s",
        std::strcmp(root, "/") == 0 ? "" : root, fixed_path);
    const int system_length = std::snprintf(system, sizeof(system), "%s/system_data",
        std::strcmp(root, "/") == 0 ? "" : root);
    if (target_length <= 0 || static_cast<std::size_t>(target_length) >= sizeof(target) ||
        system_length <= 0 || static_cast<std::size_t>(system_length) >= sizeof(system)) {
        result.failure = "path";
        return result;
    }
    const int fd = ::open(target, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { result.failure = "open-target"; result.system_error = errno; return result; }
    result.target_opened = true;
    struct stat before{}, after{};
    if (::fstat(fd, &before) != 0) {
        result.failure = "stat-target"; result.system_error = errno;
    } else {
        result.target_device = static_cast<std::uint64_t>(before.st_dev);
        result.target_inode = static_cast<std::uint64_t>(before.st_ino);
        result.target_owner = static_cast<std::uint64_t>(before.st_uid);
        result.target_mode = static_cast<std::uint64_t>(before.st_mode);
        result.target_size = static_cast<std::int64_t>(before.st_size);
        if (!S_ISREG(before.st_mode) || before.st_size != static_cast<off_t>(expected_bytes) ||
            (before.st_nlink != 0 && before.st_nlink != 1)) {
            result.failure = "file-bound";
        } else {
            unsigned char bytes[expected_bytes]{};
            unsigned interrupted_reads = 0;
            while (result.bytes_read < expected_bytes) {
                const ssize_t got = ::read(fd, bytes + result.bytes_read, expected_bytes - result.bytes_read);
                if (got < 0 && errno == EINTR && interrupted_reads++ < 2) continue;
                if (got <= 0) {
                    result.failure = "read-target";
                    result.system_error = got < 0 ? errno : 0;
                    break;
                }
                result.bytes_read += static_cast<std::size_t>(got);
            }
            if (std::strcmp(result.failure, "none") == 0) {
                unsigned char extra = 0;
                const ssize_t got = ::read(fd, &extra, 1);
                if (got != 0) {
                    result.failure = "read-bound";
                    result.system_error = got < 0 ? errno : 0;
                } else if (::fstat(fd, &after) != 0) {
                    result.failure = "restat-target"; result.system_error = errno;
                } else if (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
                           before.st_size != after.st_size || before.st_mode != after.st_mode) {
                    result.failure = "target-changed";
                } else {
                    result.content_matched = tile_job::detail::exact_helper_sha256(
                        bytes, expected_bytes, expected_bytes, expected_sha256);
                    if (!result.content_matched) result.failure = "content-mismatch";
                }
            }
        }
    }
    if (::close(fd) != 0 && std::strcmp(result.failure, "none") == 0) {
        result.failure = "close-target"; result.system_error = errno;
    }
    // Reach the exact target first. This mount observation follows even when
    // the old prefix policy would have stopped at /system_data mode 0775.
    if (!result.content_matched || std::strcmp(result.failure, "none") != 0) return result;
    const int root_fd = ::open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root_fd < 0) { result.failure = "open-root"; result.system_error = errno; return result; }
    struct stat root_stat{};
    if (::fstat(root_fd, &root_stat) != 0) {
        result.failure = "stat-root"; result.system_error = errno;
    } else {
        result.root_device = static_cast<std::uint64_t>(root_stat.st_dev);
    }
    if (::close(root_fd) != 0 && std::strcmp(result.failure, "none") == 0) {
        result.failure = "close-root"; result.system_error = errno;
    }
    if (std::strcmp(result.failure, "none") != 0) return result;
    const int system_fd = ::open(system, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (system_fd < 0) { result.failure = "open-system"; result.system_error = errno; return result; }
    struct stat system_stat{};
    if (::fstat(system_fd, &system_stat) != 0) {
        result.failure = "stat-system"; result.system_error = errno;
    } else {
        result.system_device = static_cast<std::uint64_t>(system_stat.st_dev);
    }
    if (::close(system_fd) != 0 && std::strcmp(result.failure, "none") == 0) {
        result.failure = "close-system"; result.system_error = errno;
    }
    if (std::strcmp(result.failure, "none") != 0) return result;
    result.mount_distinct = root_stat.st_dev != system_stat.st_dev &&
                            system_stat.st_dev == before.st_dev;
    if (!result.mount_distinct) { result.failure = "mount-alias"; return result; }
    result.complete = true;
    return result;
}

} // namespace shelf::protected_ps2_direct_file_probe
