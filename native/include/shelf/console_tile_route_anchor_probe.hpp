// SPDX-License-Identifier: GPL-3.0-only
// Offline candidate for a fixed, read-only app-process directory observation.
// A report is metadata only: it cannot qualify a saved-route write. In
// particular, an absolute open can cross an unverified intermediate name.
#pragma once
#include "console_tile_route_path_probe.hpp"
#include <cstddef>

namespace shelf::tile_route {

enum class RouteAnchorId : unsigned {
    root, data, console_shelf, user, user_home, system_data, system_data_priv,
    count
};

struct RouteAnchorNode {
    RoutePathStat before{}, after{};
    RoutePathOpen direct{};
    bool named_match = false;
};

struct RouteAnchorReport {
    std::uint64_t uid = 0, euid = 0;
    RouteAnchorNode nodes[static_cast<unsigned>(RouteAnchorId::count)]{};
};

#if !defined(_WIN32)
namespace detail {

// Test-only paths can be injected here; the production entry point below has
// no path argument. Never follow a final symlink or open a known non-directory.
inline RouteAnchorReport probe_anchor_paths(const char* const* paths, std::size_t count) {
    RouteAnchorReport report;
    report.uid = static_cast<std::uint64_t>(::getuid());
    report.euid = static_cast<std::uint64_t>(::geteuid());
    constexpr unsigned flags = O_RDONLY | O_DIRECTORY | O_NONBLOCK |
                               O_NOFOLLOW | O_CLOEXEC;
    static_assert((flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) == 0,
                  "anchor observation must not open for writing");
    if (!paths || count != static_cast<unsigned>(RouteAnchorId::count)) return report;
    for (std::size_t i = 0; i < count; ++i) {
        if (!paths[i] || paths[i][0] != '/') continue;
        auto& node = report.nodes[i];
        node.before = named_stat(paths[i]);
        if (node.before.ok && !S_ISDIR(node.before.mode)) continue;
        node.direct = open_stat_close(paths[i], flags);
        node.after = named_stat(paths[i]);
        node.named_match = node.before.ok && node.after.ok &&
                           node.direct.opened && node.direct.fd_stat.ok &&
                           node.direct.close_error == 0 &&
                           S_ISDIR(node.before.mode) &&
                           same_identity(node.before, node.after) &&
                           same_identity(node.before, node.direct.fd_stat) &&
                           node.before.mode == node.after.mode &&
                           node.after.mode == node.direct.fd_stat.mode;
    }
    return report;
}
} // namespace detail

// These seven literal paths cover the already observed data and system roots
// plus the user-home directory name. No account ID is guessed or enumerated.
inline RouteAnchorReport probe_route_anchors() {
    constexpr const char* paths[] = {
        "/", "/data", "/data/console-shelf", "/user", "/user/home",
        "/system_data", "/system_data/priv"};
    return detail::probe_anchor_paths(paths, sizeof(paths) / sizeof(paths[0]));
}
#endif
} // namespace shelf::tile_route
