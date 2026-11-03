// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/console_tile_route_path_probe.hpp"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main() {
    char root[] = "/tmp/isshelf-route-path-XXXXXX";
    char* made = ::mkdtemp(root);
    assert(made != nullptr);
    const std::string parent(made);
    const std::string child = parent + "/tile-routes-v1";
    const auto owner = static_cast<std::uint64_t>(::geteuid());

    // A missing child still reaches its exact safe open, with ENOENT.
    auto missing = shelf::tile_route::detail::probe_at(parent.c_str(), child.c_str(), owner, 0700);
    assert(missing.parent_expected && missing.parent_match);
    assert(missing.parent_before.ok && missing.parent_after.ok);
    for (const auto& trial : missing.parent_open)
        assert(trial.attempted && trial.opened && trial.fd_stat.ok && trial.close_error == 0);
    assert(missing.child_before.attempted && !missing.child_before.ok && missing.child_before.error == ENOENT);
    assert(missing.child_open.attempted && !missing.child_open.opened && missing.child_open.open_error == ENOENT);
    assert(!missing.child_after.attempted && !missing.child_match);

    assert(::mkdir(child.c_str(), 0700) == 0);
    auto present = shelf::tile_route::detail::probe_at(parent.c_str(), child.c_str(), owner, 0700);
    assert(present.parent_match && present.child_match);
    assert(present.child_before.ok && present.child_open.opened && present.child_open.fd_stat.ok);
    assert(present.child_after.ok && present.child_open.close_error == 0);
    assert(::rmdir(child.c_str()) == 0);

    // A symlink is reported as metadata and is never opened.
    assert(::symlink("/tmp", child.c_str()) == 0);
    auto symlinked = shelf::tile_route::detail::probe_at(parent.c_str(), child.c_str(), owner, 0700);
    assert(symlinked.parent_match && symlinked.child_before.ok && !symlinked.child_open.attempted);
    assert(!symlinked.child_match);
    assert(::unlink(child.c_str()) == 0);

    auto wrong_mode = shelf::tile_route::detail::probe_at(parent.c_str(), child.c_str(), owner, 0777);
    assert(!wrong_mode.parent_expected && !wrong_mode.parent_open[0].attempted &&
           !wrong_mode.parent_after.attempted && !wrong_mode.child_before.attempted);
    assert(::rmdir(parent.c_str()) == 0);

    auto absent = shelf::tile_route::detail::probe_at(parent.c_str(), child.c_str(), owner, 0700);
    assert(absent.parent_before.attempted && !absent.parent_before.ok && absent.parent_before.error == ENOENT);
    assert(!absent.parent_open[3].attempted && !absent.child_before.attempted);
    std::puts("route path probe host tests passed");
}
