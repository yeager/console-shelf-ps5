// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/console_tile_settings_path_probe.hpp"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>

int main() {
    char root[] = "/tmp/isshelf-settings-path-XXXXXX";
    char* made = ::mkdtemp(root);
    assert(made);
    const std::string parent(made);
    const std::string main = parent + "/settings.ini";
    const std::string backup = parent + "/settings.ini.bak";

    FILE* file = ::fopen(main.c_str(), "wb");
    assert(file && ::fclose(file) == 0);
    assert(::chmod(main.c_str(), 0600) == 0);
    auto present = shelf::tile_route::detail::probe_settings_at(
        parent.c_str(), main.c_str(), backup.c_str());
    assert(present.parent_open.attempted && present.parent_open.opened &&
           present.parent_open.fd_stat.ok && present.parent_open.close_error == 0);
    assert(S_ISDIR(present.parent_open.fd_stat.mode));
    assert(present.files[0].before.attempted && present.files[0].before.ok &&
           present.files[0].direct.attempted && present.files[0].direct.opened &&
           present.files[0].direct.fd_stat.ok && present.files[0].after.ok &&
           present.files[0].named_match);
    assert(present.files[1].before.attempted && !present.files[1].before.ok &&
           present.files[1].before.error == ENOENT &&
           present.files[1].direct.attempted && !present.files[1].direct.opened &&
           present.files[1].direct.open_error == ENOENT &&
           present.files[1].after.attempted && !present.files[1].named_match);

    // Final-component symlink must fail at the safe open and never match.
    assert(::unlink(main.c_str()) == 0);
    assert(::symlink(backup.c_str(), main.c_str()) == 0);
    auto linked = shelf::tile_route::detail::probe_settings_at(
        parent.c_str(), main.c_str(), backup.c_str());
    assert(linked.files[0].before.ok && S_ISLNK(linked.files[0].before.mode));
    assert(linked.files[0].direct.attempted && !linked.files[0].direct.opened &&
           linked.files[0].direct.open_error == ELOOP && !linked.files[0].named_match);
    assert(::unlink(main.c_str()) == 0);

    // A substituted FIFO cannot block a read-only O_NONBLOCK diagnostic.
    assert(::mkfifo(main.c_str(), 0600) == 0);
    auto fifo = shelf::tile_route::detail::probe_settings_at(
        parent.c_str(), main.c_str(), backup.c_str());
    assert(fifo.files[0].direct.attempted && fifo.files[0].direct.opened &&
           S_ISFIFO(fifo.files[0].direct.fd_stat.mode) && !fifo.files[0].named_match);
    assert(::unlink(main.c_str()) == 0);

    assert(::mkdir(main.c_str(), 0700) == 0);
    auto directory = shelf::tile_route::detail::probe_settings_at(
        parent.c_str(), main.c_str(), backup.c_str());
    assert(directory.files[0].direct.attempted && directory.files[0].direct.opened &&
           S_ISDIR(directory.files[0].direct.fd_stat.mode) && !directory.files[0].named_match);
    assert(::rmdir(main.c_str()) == 0);
    assert(::rmdir(parent.c_str()) == 0);
    std::puts("settings path probe host tests passed");
}
