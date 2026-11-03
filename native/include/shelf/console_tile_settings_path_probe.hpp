// SPDX-License-Identifier: GPL-3.0-only
// Fixed, read-only app-process diagnostic for the two existing settings files.
// There is no caller-supplied path, content read, directory traversal, or
// write. O_NONBLOCK keeps a replaced FIFO from hanging the app process.
#pragma once
#include "console_tile_route_path_probe.hpp"

namespace shelf::tile_route {
struct SettingsPathFile {
    RoutePathStat before{}, after{};
    RoutePathOpen direct{};
    bool named_match = false;
};
struct SettingsPathReport {
    std::uint64_t uid = 0, euid = 0;
    RoutePathOpen parent_open{};
    SettingsPathFile files[2]{};
};

#if !defined(_WIN32)
namespace detail {
inline SettingsPathReport probe_settings_at(const char* parent, const char* first,
                                            const char* second) {
    const char* paths[2] = {first, second};
    constexpr unsigned flags = O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC;
    SettingsPathReport report;
    report.uid = static_cast<std::uint64_t>(::getuid());
    report.euid = static_cast<std::uint64_t>(::geteuid());
    report.parent_open = open_stat_close(
        parent, flags | O_DIRECTORY);
    for (unsigned i = 0; i < 2; ++i) {
        auto& file = report.files[i];
        file.before = named_stat(paths[i]);
        // UI54 found that lstat can return EPERM in the app sandbox. Always
        // attempt the safe read-only open so its independent result is known.
        file.direct = open_stat_close(paths[i], flags);
        file.after = named_stat(paths[i]);
        file.named_match = file.direct.opened && file.direct.close_error == 0 &&
                           same_identity(file.before, file.after) &&
                           same_identity(file.before, file.direct.fd_stat) &&
                           S_ISREG(file.before.mode) &&
                           file.before.mode == file.after.mode &&
                           file.after.mode == file.direct.fd_stat.mode;
    }
    return report;
}
} // namespace detail

inline SettingsPathReport probe_settings_path() {
    return detail::probe_settings_at("/data/console-shelf",
                                     "/data/console-shelf/settings.ini",
                                     "/data/console-shelf/settings.ini.bak");
}
#endif
} // namespace shelf::tile_route
