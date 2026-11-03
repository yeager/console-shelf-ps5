// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/protected_ps2_source_read_probe.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace shelf::protected_ps2_source_read_probe;

static void make_dir(const std::string& path) {
    assert(::mkdir(path.c_str(), 0755) == 0);
    assert(::chmod(path.c_str(), 0755) == 0);
}

static void make_file(const std::string& path, mode_t mode) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, mode);
    assert(fd >= 0);
    const char body[] = "read-only-probe-fixture\n";
    assert(::write(fd, body, sizeof(body) - 1) ==
           static_cast<ssize_t>(sizeof(body) - 1));
    assert(::fchmod(fd, mode) == 0);
    assert(::close(fd) == 0);
}

struct Paths {
    std::string system, tiles, stage, title, sys, eboot, param;
};

static Paths paths(const std::string& root, const std::string& stage_name) {
    Paths p;
    p.system = root + "/system_data";
    p.tiles = p.system + "/isshelf-tiles";
    p.stage = p.tiles + "/" + stage_name;
    p.title = p.stage + "/PPSA99703";
    p.sys = p.title + "/sce_sys";
    p.eboot = p.title + "/eboot.bin";
    p.param = p.sys + "/param.json";
    return p;
}

static void make_stage(const Paths& p) {
    make_dir(p.stage);
    make_dir(p.title);
    make_dir(p.sys);
    make_file(p.eboot, 0755);
    make_file(p.param, 0644);
}

static void remove_stage(const Paths& p) {
    assert(::unlink(p.param.c_str()) == 0);
    assert(::unlink(p.eboot.c_str()) == 0);
    assert(::rmdir(p.sys.c_str()) == 0);
    assert(::rmdir(p.title.c_str()) == 0);
    assert(::rmdir(p.stage.c_str()) == 0);
}

struct Rebind {
    Paths original, replacement;
};

static void replace_stage(void* context) {
    auto* change = static_cast<Rebind*>(context);
    assert(::rename(change->original.stage.c_str(),
                    change->replacement.stage.c_str()) == 0);
    make_stage(change->original);
}

int main() {
    char root_chars[] = "/tmp/isshelf-protected-ps2-read-XXXXXX";
    assert(::mkdtemp(root_chars) != nullptr);
    const std::string root(root_chars);
    const Paths p = paths(root, pilot_folder);
    make_dir(p.system);
    make_dir(p.tiles);
    make_stage(p);

    const auto good = inspect(root.c_str(), ::geteuid(), ::geteuid());
    assert(good.complete && std::strcmp(good.failure, "none") == 0 &&
           good.files_read == 4 && S_ISDIR(good.nodes[4].mode) &&
           S_ISREG(good.nodes[6].mode) &&
           good.nodes[1].device == good.nodes[7].device);

    const auto wrong_process = inspect(root.c_str(), ::geteuid(),
                                       static_cast<uid_t>(::geteuid() + 1));
    assert(!wrong_process.complete &&
           std::strcmp(wrong_process.failure, "process-identity") == 0);

    // A symlink at an individual component is never followed by that open.
    const std::string alias = root + "-alias";
    assert(::symlink(root.c_str(), alias.c_str()) == 0);
    const auto linked_root = inspect(alias.c_str(), ::geteuid(), ::geteuid());
    assert(!linked_root.complete &&
           std::strcmp(linked_root.failure, "open") == 0 &&
           linked_root.failed_pass == 1 && linked_root.failed_node == 0);

    const std::string saved_param = p.param + ".saved";
    assert(::rename(p.param.c_str(), saved_param.c_str()) == 0);
    assert(::symlink(saved_param.c_str(), p.param.c_str()) == 0);
    const auto linked_file = inspect(root.c_str(), ::geteuid(), ::geteuid());
    assert(!linked_file.complete &&
           std::strcmp(linked_file.failure, "open") == 0 &&
           linked_file.failed_pass == 1 && linked_file.failed_node == 7 &&
           linked_file.observed_nodes == 7);
    assert(::unlink(p.param.c_str()) == 0);
    assert(::rename(saved_param.c_str(), p.param.c_str()) == 0);

    const std::string saved_stage = p.stage + ".saved";
    assert(::rename(p.stage.c_str(), saved_stage.c_str()) == 0);
    assert(::symlink(saved_stage.c_str(), p.stage.c_str()) == 0);
    const auto linked_stage = inspect(root.c_str(), ::geteuid(), ::geteuid());
    assert(!linked_stage.complete &&
           std::strcmp(linked_stage.failure, "open") == 0 &&
           linked_stage.failed_pass == 1 && linked_stage.failed_node == 3);
    assert(::unlink(p.stage.c_str()) == 0);
    assert(::rename(saved_stage.c_str(), p.stage.c_str()) == 0);

    // A writable protected parent is refused even for a privileged tester.
    assert(::chmod(p.tiles.c_str(), 0777) == 0);
    const auto writable_parent = inspect(root.c_str(), ::geteuid(), ::geteuid());
    assert(!writable_parent.complete &&
           std::strcmp(writable_parent.failure, "identity-policy") == 0 &&
           writable_parent.failed_pass == 1 && writable_parent.failed_node == 2 &&
           (writable_parent.nodes[2].mode & 07777) == 0777);
    assert(::chmod(p.tiles.c_str(), 0755) == 0);

    // Replace the exact stage name between two read passes with an equally
    // well-formed directory. The held old inode and reopened new inode differ.
    const Paths moved = paths(root, ".ps2-pilot-moved-for-test");
    Rebind change{p, moved};
    const auto changed = inspect_host_with_hook(root.c_str(), ::geteuid(),
                                                ::geteuid(), replace_stage, &change);
    assert(!changed.complete &&
           std::strcmp(changed.failure, "identity-changed") == 0 &&
           changed.failed_pass == 2 && changed.failed_node == 3);

    remove_stage(p);
    remove_stage(moved);
    assert(::rmdir(p.tiles.c_str()) == 0);
    assert(::rmdir(p.system.c_str()) == 0);
    assert(::unlink(alias.c_str()) == 0);
    assert(::rmdir(root.c_str()) == 0);
    std::puts("PASS: protected PS2 source probe rejects symlinks, writable parent, and changed name");
}
