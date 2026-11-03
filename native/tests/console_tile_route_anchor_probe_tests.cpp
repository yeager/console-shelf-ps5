// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/console_tile_route_anchor_probe.hpp"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>

int main() {
    using shelf::tile_route::RouteAnchorId;
    constexpr std::size_t count = static_cast<unsigned>(RouteAnchorId::count);
    char root[] = "/tmp/isshelf-route-anchor-XXXXXX";
    assert(::mkdtemp(root));
    const std::string base(root);
    const std::string data = base + "/data";
    const std::string shelf = data + "/console-shelf";
    const std::string user = base + "/user";
    const std::string home = user + "/home";
    const std::string system = base + "/system_data";
    const std::string priv = system + "/priv";
    assert(::mkdir(data.c_str(), 0700) == 0);
    assert(::mkdir(shelf.c_str(), 0700) == 0);
    assert(::mkdir(user.c_str(), 0700) == 0);
    assert(::mkdir(home.c_str(), 0700) == 0);
    assert(::mkdir(system.c_str(), 0700) == 0);
    assert(::mkdir(priv.c_str(), 0700) == 0);
    const char* paths[count] = {base.c_str(), data.c_str(), shelf.c_str(),
                                user.c_str(), home.c_str(), system.c_str(), priv.c_str()};

    const auto present = shelf::tile_route::detail::probe_anchor_paths(paths, count);
    assert(present.uid == static_cast<std::uint64_t>(::getuid()));
    assert(present.euid == static_cast<std::uint64_t>(::geteuid()));
    constexpr unsigned flags = O_RDONLY | O_DIRECTORY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC;
    for (const auto& node : present.nodes) {
        assert(node.before.ok && node.after.ok && node.direct.attempted &&
               node.direct.opened && node.direct.fd_stat.ok &&
               node.direct.flags == flags && node.direct.close_error == 0 &&
               node.named_match && S_ISDIR(node.direct.fd_stat.mode));
    }

    assert(::rmdir(home.c_str()) == 0);
    const auto missing = shelf::tile_route::detail::probe_anchor_paths(paths, count);
    const auto& missing_home = missing.nodes[static_cast<unsigned>(RouteAnchorId::user_home)];
    assert(missing_home.before.attempted && !missing_home.before.ok &&
           missing_home.before.error == ENOENT && missing_home.direct.attempted &&
           !missing_home.direct.opened && missing_home.direct.open_error == ENOENT &&
           missing_home.after.attempted && !missing_home.named_match);
    assert(missing.nodes[static_cast<unsigned>(RouteAnchorId::user)].named_match);

    assert(::symlink(data.c_str(), home.c_str()) == 0);
    const auto linked = shelf::tile_route::detail::probe_anchor_paths(paths, count);
    const auto& symlink = linked.nodes[static_cast<unsigned>(RouteAnchorId::user_home)];
    assert(symlink.before.ok && S_ISLNK(symlink.before.mode) &&
           !symlink.direct.attempted && !symlink.after.attempted &&
           !symlink.named_match);
    assert(::unlink(home.c_str()) == 0);

    assert(::mkfifo(home.c_str(), 0600) == 0);
    const auto fifo = shelf::tile_route::detail::probe_anchor_paths(paths, count);
    const auto& special = fifo.nodes[static_cast<unsigned>(RouteAnchorId::user_home)];
    assert(special.before.ok && S_ISFIFO(special.before.mode) &&
           !special.direct.attempted && !special.named_match);
    assert(::unlink(home.c_str()) == 0);

    assert(::rmdir(priv.c_str()) == 0);
    assert(::rmdir(system.c_str()) == 0);
    assert(::rmdir(user.c_str()) == 0);
    assert(::rmdir(shelf.c_str()) == 0);
    assert(::rmdir(data.c_str()) == 0);
    assert(::rmdir(base.c_str()) == 0);
    std::puts("route anchor probe host tests passed");
}
