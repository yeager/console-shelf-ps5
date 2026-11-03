// SPDX-License-Identifier: GPL-3.0-only
// Fixed, read-only app-process diagnostic for the route preference root.
// The production entry point accepts no path. No file contents are read and
// no directory is created or enumerated.
#pragma once
#include <cstdint>
#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace shelf::tile_route {
struct RoutePathStat {
    bool attempted = false, ok = false;
    int error = 0;
    std::uint64_t device = 0, inode = 0, owner = 0, mode = 0;
};
struct RoutePathOpen {
    unsigned flags = 0;
    bool attempted = false, opened = false;
    int open_error = 0, close_error = 0;
    RoutePathStat fd_stat{};
};
struct RoutePathReport {
    std::uint64_t uid = 0, euid = 0;
    RoutePathStat parent_before{}, parent_after{}, child_before{}, child_after{};
    RoutePathOpen parent_open[4]{}, child_open{};
    bool parent_expected = false, parent_match = false, child_match = false;
};

#if !defined(_WIN32)
namespace detail {
inline RoutePathStat snapshot(const struct stat& value) {
    RoutePathStat out;
    out.attempted = out.ok = true;
    out.device = static_cast<std::uint64_t>(value.st_dev);
    out.inode = static_cast<std::uint64_t>(value.st_ino);
    out.owner = static_cast<std::uint64_t>(value.st_uid);
    out.mode = static_cast<std::uint64_t>(value.st_mode);
    return out;
}
inline RoutePathStat named_stat(const char* path) {
    RoutePathStat out;
    out.attempted = true;
    struct stat value{};
    errno = 0;
    if (::lstat(path, &value) == 0) return snapshot(value);
    out.error = errno;
    return out;
}
inline RoutePathOpen open_stat_close(const char* path, unsigned flags) {
    RoutePathOpen out;
    out.flags = flags;
    out.attempted = true;
    errno = 0;
    const int fd = ::open(path, static_cast<int>(flags));
    if (fd < 0) {
        out.open_error = errno;
        return out;
    }
    out.opened = true;
    out.fd_stat.attempted = true;
    struct stat value{};
    errno = 0;
    if (::fstat(fd, &value) == 0)
        out.fd_stat = snapshot(value);
    else
        out.fd_stat.error = errno;
    errno = 0;
    if (::close(fd) != 0) out.close_error = errno;
    return out;
}
inline bool same_identity(const RoutePathStat& a, const RoutePathStat& b) {
    return a.ok && b.ok && a.device == b.device && a.inode != 0 && a.inode == b.inode &&
           a.owner == b.owner && (a.mode & S_IFMT) == (b.mode & S_IFMT);
}
inline RoutePathReport probe_at(const char* parent, const char* child,
                                std::uint64_t expected_owner, std::uint64_t expected_mode) {
    RoutePathReport out;
    out.uid = static_cast<std::uint64_t>(::getuid());
    out.euid = static_cast<std::uint64_t>(::geteuid());
    // lstat first: a non-directory or symlink is never opened by this probe.
    out.parent_before = named_stat(parent);
    out.parent_expected = out.parent_before.ok && S_ISDIR(out.parent_before.mode) &&
                          out.parent_before.owner == expected_owner &&
                          (out.parent_before.mode & 07777u) == expected_mode;
    if (!out.parent_expected) return out;
    // O_NONBLOCK protects the two variants without O_DIRECTORY from a racing
    // replacement with a FIFO. Their exact numeric flags are reported.
    constexpr unsigned flags[4] = {
        O_RDONLY | O_CLOEXEC | O_NONBLOCK,
        O_RDONLY | O_DIRECTORY | O_CLOEXEC,
        O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK,
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC
    };
    for (unsigned i = 0; i < 4; ++i)
        out.parent_open[i] = open_stat_close(parent, flags[i]);
    out.parent_after = named_stat(parent);
    const RoutePathOpen& full = out.parent_open[3];
    out.parent_match = out.parent_after.ok && out.parent_expected && full.opened &&
                       full.fd_stat.ok && full.close_error == 0 &&
                       same_identity(out.parent_before, out.parent_after) &&
                       same_identity(out.parent_after, full.fd_stat) &&
                       (full.fd_stat.mode & 07777u) == expected_mode;
    if (!out.parent_match) return out;
    out.child_before = named_stat(child);
    // A present non-directory, including a symlink, is never opened. Missing
    // or inaccessible children are tried with the store's exact safe flags.
    if (out.child_before.ok && !S_ISDIR(out.child_before.mode)) return out;
    out.child_open = open_stat_close(child, flags[3]);
    if (out.child_open.opened) out.child_after = named_stat(child);
    out.child_match = out.child_before.ok && out.child_after.ok && out.child_open.opened &&
                      out.child_open.fd_stat.ok && out.child_open.close_error == 0 &&
                      same_identity(out.child_before, out.child_after) &&
                      same_identity(out.child_after, out.child_open.fd_stat) &&
                      out.child_after.device == out.parent_after.device &&
                      out.child_after.owner == out.euid &&
                      (out.child_after.mode & 07777u) == 0700u &&
                      (out.child_open.fd_stat.mode & 07777u) == 0700u;
    return out;
}
} // namespace detail

inline RoutePathReport probe_route_path() {
    return detail::probe_at("/data/console-shelf",
                            "/data/console-shelf/tile-routes-v1", 0, 0777);
}
#endif
} // namespace shelf::tile_route
