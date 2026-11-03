// SPDX-License-Identifier: GPL-3.0-only
// Private UI49 scratch observation. It creates no title folder or Home tile.
#pragma once
#if defined(_WIN32)
#error "The app source probe requires POSIX descriptor semantics"
#endif

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace shelf::app_tile_source_probe {

constexpr char fixed_root[] = "/data/console-shelf";
constexpr char folder_prefix[] = ".app-tile-source-v1-";
constexpr char receipt_prefix[] = "app-tile-source-v1-";
constexpr char nested_name[] = "nested";
constexpr char sample_name[] = "sample.bin";
constexpr std::uint64_t fnv_offset = 14695981039346656037ull;
constexpr std::uint64_t fnv_prime = 1099511628211ull;

struct Result {
    bool attempted = false;
    bool complete = false;
    std::size_t receipt_bytes = 0;
    std::uint64_t receipt_fnv = 0;
    const char* failure = "none";
};

inline std::uint64_t fnv(const unsigned char* bytes, std::size_t count) {
    std::uint64_t value = fnv_offset;
    for (std::size_t i = 0; i < count; ++i) {
        value ^= bytes[i];
        value *= fnv_prime;
    }
    return value;
}

inline bool scratch_link_count_ok(nlink_t count) {
    // The PS5 /data mount has reported zero for an otherwise readable,
    // descriptor-verified file. Two or more links remain disallowed.
    return count == 0 || count == 1;
}

inline bool nonce_valid(const char* nonce) {
    if (!nonce || std::strlen(nonce) != 32) return false;
    for (unsigned i = 0; i < 32; ++i)
        if (!((nonce[i] >= '0' && nonce[i] <= '9') ||
              (nonce[i] >= 'a' && nonce[i] <= 'f'))) return false;
    return true;
}

inline bool same_directory(const struct stat& named, const struct stat& held) {
    return S_ISDIR(named.st_mode) && S_ISDIR(held.st_mode) &&
           named.st_dev == held.st_dev && named.st_ino == held.st_ino &&
           named.st_uid == held.st_uid;
}

// The PS5 permits open/fstat on this app-data tree but has returned EPERM
// from lstat on its names. Reopen without following a final symlink and
// compare the held directory identity. Absolute resolution can still race
// a concurrent namespace change, so this is a static-name guard only.
inline bool same_directory_name(const char* path, const struct stat& held) {
    const int reopened_fd = ::open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (reopened_fd < 0) return false;
    struct stat reopened{};
    const bool matched = ::fstat(reopened_fd, &reopened) == 0 &&
                         same_directory(reopened, held);
    const bool closed = ::close(reopened_fd) == 0;
    return matched && closed;
}

inline bool write_exact(int fd, const char* bytes, std::size_t count) {
    std::size_t used = 0;
    while (used < count) {
        const ssize_t wrote = ::write(fd, bytes + used, count - used);
        if (wrote <= 0) return false;
        used += static_cast<std::size_t>(wrote);
    }
    return true;
}

inline bool read_exact(const char* path, const char* expected, std::size_t count,
                       mode_t mode, uid_t owner) {
    const int fd = ::open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat info{};
    char bytes[512]{};
    const bool regular = count < sizeof(bytes) && ::fstat(fd, &info) == 0 &&
        S_ISREG(info.st_mode) && scratch_link_count_ok(info.st_nlink) &&
        info.st_uid == owner &&
        (info.st_mode & 07777) == mode && info.st_size == static_cast<off_t>(count);
    const ssize_t got = regular ? ::read(fd, bytes, count + 1) : -1;
    const bool closed = ::close(fd) == 0;
    return regular && got == static_cast<ssize_t>(count) && closed &&
           std::memcmp(bytes, expected, count) == 0;
}

// Only the fixed app data directory is allowed in the PS5 build. Host tests
// provide a temporary root. A created scratch folder is intentionally kept
// for independent FTP readback; a partial folder is also kept for diagnosis.
inline Result run_once(const char* root, const char* nonce) {
    Result result;
    if (!root || !nonce_valid(nonce) || std::strlen(root) > 2000) {
        result.failure = "input";
        return result;
    }
#ifdef SHELF_PS5
    if (std::strcmp(root, fixed_root) != 0) {
        result.failure = "root";
        return result;
    }
#endif
    char folder[2304], nested[2304], sample[2304], receipt[2304], body[512], report[512];
    const int folder_size = std::snprintf(folder, sizeof(folder), "%s/%s%s",
                                          root, folder_prefix, nonce);
    const int nested_size = std::snprintf(nested, sizeof(nested), "%s/%s",
                                          folder, nested_name);
    const int sample_size = std::snprintf(sample, sizeof(sample), "%s/%s",
                                          nested, sample_name);
    const int receipt_size = std::snprintf(receipt, sizeof(receipt), "%s/%s%s.txt",
                                           root, receipt_prefix, nonce);
    const int body_size = std::snprintf(body, sizeof(body),
                                        "ISSHELF_APP_TILE_SOURCE_V1 %s\n", nonce);
    if (folder_size <= 0 || folder_size >= static_cast<int>(sizeof(folder)) ||
        nested_size <= 0 || nested_size >= static_cast<int>(sizeof(nested)) ||
        sample_size <= 0 || sample_size >= static_cast<int>(sizeof(sample)) ||
        receipt_size <= 0 || receipt_size >= static_cast<int>(sizeof(receipt)) ||
        body_size <= 0 || body_size >= static_cast<int>(sizeof(body))) {
        result.failure = "path-format";
        return result;
    }
    const int parent_fd = ::open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent_fd < 0) { result.failure = "parent-open"; return result; }
    struct stat parent_held{}, child_held{}, nested_held{};
    const bool parent_ready = ::fstat(parent_fd, &parent_held) == 0 &&
        S_ISDIR(parent_held.st_mode) && same_directory_name(root, parent_held) &&
        ::fsync(parent_fd) == 0;
    if (!parent_ready) {
        ::close(parent_fd);
        result.failure = "parent-check";
        return result;
    }
    if (::mkdir(folder, 0700) != 0) {
        const int mkdir_errno = errno;
        ::close(parent_fd);
        result.failure = mkdir_errno == EEXIST ? "collision" : "mkdir";
        return result;
    }
    result.attempted = true;
    const int folder_fd = ::open(folder, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (folder_fd < 0) {
        ::close(parent_fd);
        result.failure = "folder-open";
        return result;
    }
    if (::mkdir(nested, 0700) != 0) {
        ::close(folder_fd);
        ::close(parent_fd);
        result.failure = "nested-mkdir";
        return result;
    }
    const int nested_fd = ::open(nested, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (nested_fd < 0) {
        ::close(folder_fd);
        ::close(parent_fd);
        result.failure = "nested-open";
        return result;
    }
    const int sample_fd = ::open(sample, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    bool okay = sample_fd >= 0;
    if (okay) {
        okay = write_exact(sample_fd, body, static_cast<std::size_t>(body_size)) &&
               ::fchmod(sample_fd, 0644) == 0 && ::fsync(sample_fd) == 0;
        okay = ::close(sample_fd) == 0 && okay;
    }
    if (!okay || ::fchmod(nested_fd, 0755) != 0 || ::fsync(nested_fd) != 0 ||
        ::fstat(nested_fd, &nested_held) != 0 ||
        !same_directory_name(nested, nested_held) || (nested_held.st_mode & 07777) != 0755 ||
        ::fchmod(folder_fd, 0755) != 0 || ::fsync(folder_fd) != 0 ||
        ::fstat(folder_fd, &child_held) != 0 ||
        !same_directory_name(folder, child_held) || child_held.st_uid != nested_held.st_uid ||
        (child_held.st_mode & 07777) != 0755 ||
        !read_exact(sample, body, static_cast<std::size_t>(body_size), 0644, child_held.st_uid) ||
        !same_directory_name(root, parent_held) || ::fsync(parent_fd) != 0) {
        ::close(nested_fd);
        ::close(folder_fd);
        ::close(parent_fd);
        result.failure = "source-check";
        return result;
    }
    const int report_size = std::snprintf(report, sizeof(report),
        "ISSHELF_APP_TILE_SOURCE_PROBE_V1\nnonce=%s\nstate=complete\n"
        "parent_owner=%llu\nfolder_owner=%llu\nnested_owner=%llu\n"
        "parent_device=%llu\nparent_inode=%llu\nfolder_device=%llu\nfolder_inode=%llu\n"
        "nested_device=%llu\nnested_inode=%llu\nfolder_mode=493\nnested_mode=493\n"
        "sample_mode=420\nsample_bytes=%d\nsample_fnv=%016llx\n",
        nonce, static_cast<unsigned long long>(parent_held.st_uid),
        static_cast<unsigned long long>(child_held.st_uid),
        static_cast<unsigned long long>(nested_held.st_uid),
        static_cast<unsigned long long>(parent_held.st_dev),
        static_cast<unsigned long long>(parent_held.st_ino),
        static_cast<unsigned long long>(child_held.st_dev),
        static_cast<unsigned long long>(child_held.st_ino),
        static_cast<unsigned long long>(nested_held.st_dev),
        static_cast<unsigned long long>(nested_held.st_ino), body_size,
        static_cast<unsigned long long>(fnv(
            reinterpret_cast<const unsigned char*>(body), static_cast<std::size_t>(body_size))));
    if (report_size <= 0 || report_size >= static_cast<int>(sizeof(report))) {
        ::close(nested_fd); ::close(folder_fd); ::close(parent_fd);
        result.failure = "report-format";
        return result;
    }
    const int report_fd = ::open(receipt, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    okay = report_fd >= 0;
    if (okay) {
        okay = write_exact(report_fd, report, static_cast<std::size_t>(report_size)) &&
               ::fchmod(report_fd, 0644) == 0 && ::fsync(report_fd) == 0;
        okay = ::close(report_fd) == 0 && okay;
    }
    okay = okay && ::fsync(parent_fd) == 0 &&
           read_exact(receipt, report, static_cast<std::size_t>(report_size), 0644, child_held.st_uid);
    const bool nested_closed = ::close(nested_fd) == 0;
    const bool folder_closed = ::close(folder_fd) == 0;
    const bool parent_closed = ::close(parent_fd) == 0;
    if (!okay || !nested_closed || !folder_closed || !parent_closed) {
        result.failure = "report-check";
        return result;
    }
    result.complete = true;
    result.receipt_bytes = static_cast<std::size_t>(report_size);
    result.receipt_fnv = fnv(reinterpret_cast<const unsigned char*>(report), result.receipt_bytes);
    return result;
}
} // namespace shelf::app_tile_source_probe
