// SPDX-License-Identifier: GPL-3.0-only
// Private app-side qualification of the fixed tile job directory. No title writes.
#pragma once
#include "console_tile_job_directory.hpp"
#include <cstdint>
#if defined(SHELF_PS5) || defined(SHELF_TILE_JOB_ROOT_HOST_TEST)
#include "../../../tools/console_badges/checked_at_ps5.hpp"
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace shelf::tile_job {

struct JobRootProbeResult {
    std::uint64_t uid = 0, euid = 0;
    DirectoryIdentity parent{}, child{};
    bool ran = false, parent_valid = false, child_seen = false, child_valid = false;
    bool mkdir_attempted = false, created = false, complete = false;
    int path_errno = 0, child_lookup_errno = 0, mkdir_errno = 0;
    int sync_errno = 0, close_errno = 0;
};

// The creation request is a separately selected private bridge action. Its
// parent identity and EUID must match a prior read-only observation in the same
// armed bridge session. Every returned field is bounded and contains no path.
struct JobRootCreatePin {
    std::uint64_t euid = 0;
    DirectoryIdentity parent{};
};

#if defined(SHELF_PS5) || defined(SHELF_TILE_JOB_ROOT_HOST_TEST)
namespace detail {
inline DirectoryIdentity directory_identity(const struct stat& info) {
    return {static_cast<std::uint64_t>(info.st_dev),
            static_cast<std::uint64_t>(info.st_ino),
            static_cast<std::uint64_t>(info.st_uid),
            static_cast<unsigned>(info.st_mode & 07777u),
            S_ISDIR(info.st_mode) != 0, S_ISLNK(info.st_mode) != 0};
}
inline bool same_pin(const DirectoryIdentity& actual, const DirectoryIdentity& pinned) {
    return actual.directory && !actual.symlink &&
           actual.device == pinned.device && actual.inode == pinned.inode &&
           actual.owner == pinned.owner && actual.mode == pinned.mode;
}
inline JobRootProbeResult job_root_probe(const char* data_path,
                                         std::uint64_t expected_parent_owner,
                                         const JobRootCreatePin* create_pin) {
    JobRootProbeResult out;
    out.ran = true;
    out.uid = static_cast<std::uint64_t>(getuid());
    out.euid = static_cast<std::uint64_t>(geteuid());
    int data_fd = -1, parent_fd = -1, child_fd = -1;
    auto parent_still_named = [&]() -> bool {
        struct stat named{}, held{};
        if (isshelf_badge::checked_at::fstatat(data_fd, "console-shelf", &named,
                                              AT_SYMLINK_NOFOLLOW) != 0 ||
            fstat(parent_fd, &held) != 0) {
            out.path_errno = errno;
            out.parent_valid = false;
            return false;
        }
        out.parent_valid = parent_identity_valid(directory_identity(named),
                                                 directory_identity(held),
                                                 expected_parent_owner) &&
                           same_pin(directory_identity(named), out.parent);
        return out.parent_valid;
    };
    auto inspect_child = [&]() -> bool {
        struct stat named{}, held{};
        if (isshelf_badge::checked_at::fstatat(parent_fd, "tile-jobs-v1", &named,
                                              AT_SYMLINK_NOFOLLOW) != 0) {
            out.child_lookup_errno = errno;
            return out.child_lookup_errno == ENOENT;
        }
        out.child_seen = true;
        out.child = directory_identity(named);
        child_fd = isshelf_badge::checked_at::openat(
            parent_fd, "tile-jobs-v1", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child_fd < 0) { out.path_errno = errno; return false; }
        if (fstat(child_fd, &held) != 0) { out.path_errno = errno; return false; }
        out.child_valid = private_child_identity_valid(out.child, directory_identity(held), out.euid);
        return out.child_valid;
    };
    auto work = [&]() {
        data_fd = open(data_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (data_fd < 0) { out.path_errno = errno; return; }
        parent_fd = isshelf_badge::checked_at::openat(
            data_fd, "console-shelf", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (parent_fd < 0) { out.path_errno = errno; return; }
        struct stat named{}, held{};
        if (isshelf_badge::checked_at::fstatat(data_fd, "console-shelf", &named,
                                              AT_SYMLINK_NOFOLLOW) != 0 ||
            fstat(parent_fd, &held) != 0) {
            out.path_errno = errno;
            return;
        }
        out.parent = directory_identity(named);
        out.parent_valid = parent_identity_valid(out.parent, directory_identity(held),
                                                 expected_parent_owner);
        if (!out.parent_valid) return;
        if (create_pin && (out.euid != create_pin->euid ||
                           !same_pin(out.parent, create_pin->parent))) return;
        if (!inspect_child()) return;
        if (!create_pin || out.child_seen) {
            out.complete = parent_still_named(); // missing or valid existing child
            return;
        }
        // The only create call in this diagnostic: exact child leaf, exactly
        // once, never a parent, title, job file, receipt or retry.
        out.mkdir_attempted = true;
        if (isshelf_badge::checked_at::mkdirat(parent_fd, "tile-jobs-v1", 0700) != 0) {
            out.mkdir_errno = errno;
        } else {
            out.created = true;
            if (fsync(parent_fd) != 0) out.sync_errno = errno;
        }
        out.child_seen = out.child_valid = false;
        out.child = {};
        out.child_lookup_errno = 0;
        if (child_fd >= 0) { // no descriptor is expected after ENOENT
            if (close(child_fd) != 0) out.close_errno = errno;
            child_fd = -1;
        }
        const bool post_child = inspect_child();
        // A changed parent name cannot validate an operation through a held
        // descriptor. The post-check is read-only and cannot repair a race.
        const bool post_parent = parent_still_named();
        out.complete = out.mkdir_errno == 0 && out.sync_errno == 0 &&
                       post_child && post_parent && out.created;
    };
    work();
    if (child_fd >= 0 && close(child_fd) != 0 && !out.close_errno) out.close_errno = errno;
    if (parent_fd >= 0 && close(parent_fd) != 0 && !out.close_errno) out.close_errno = errno;
    if (data_fd >= 0 && close(data_fd) != 0 && !out.close_errno) out.close_errno = errno;
    if (out.close_errno) out.complete = false;
    return out;
}
} // namespace detail

#if defined(SHELF_TILE_JOB_ROOT_HOST_TEST)
inline JobRootProbeResult inspect_job_root(const char* data_path,
                                          std::uint64_t parent_owner) {
    return detail::job_root_probe(data_path, parent_owner, nullptr);
}
inline JobRootProbeResult create_job_root_once(const char* data_path,
                                                std::uint64_t parent_owner,
                                                const JobRootCreatePin& pin) {
    return detail::job_root_probe(data_path, parent_owner, &pin);
}
#else
inline JobRootProbeResult inspect_job_root() {
    return detail::job_root_probe("/data", 0, nullptr);
}
inline JobRootProbeResult create_job_root_once(const JobRootCreatePin& pin) {
    return detail::job_root_probe("/data", 0, &pin);
}
#endif
#endif
} // namespace shelf::tile_job
