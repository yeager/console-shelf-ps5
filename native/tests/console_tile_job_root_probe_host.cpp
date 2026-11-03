// SPDX-License-Identifier: GPL-3.0-only
// Host fixture entry point for the private app-side job-root diagnostic.
#include "shelf/console_tile_job_root_probe.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    if (argc != 4) return 90;
    const auto owner = static_cast<std::uint64_t>(std::strtoull(argv[2], nullptr, 10));
    auto observed = shelf::tile_job::inspect_job_root(argv[1], owner);
    const bool create = std::strcmp(argv[3], "create") == 0 ||
                        std::strcmp(argv[3], "stale-parent") == 0 ||
                        std::strcmp(argv[3], "stale-euid") == 0;
    if (create) {
        shelf::tile_job::JobRootCreatePin pin;
        pin.euid = observed.euid;
        pin.parent = observed.parent;
        if (std::strcmp(argv[3], "stale-parent") == 0) ++pin.parent.inode;
        if (std::strcmp(argv[3], "stale-euid") == 0) ++pin.euid;
        observed = shelf::tile_job::create_job_root_once(argv[1], owner, pin);
    } else if (std::strcmp(argv[3], "inspect") != 0) return 91;
    std::printf("uid=%llu euid=%llu ran=%u parent=%u parent_dev=%llu parent_ino=%llu "
                "child_seen=%u child_valid=%u child_dev=%llu child_ino=%llu child_owner=%llu "
                "child_mode=%u mkdir=%u mkdir_errno=%d child_lookup_errno=%d path_errno=%d "
                "sync_errno=%d close_errno=%d created=%u complete=%u\n",
                static_cast<unsigned long long>(observed.uid),
                static_cast<unsigned long long>(observed.euid),
                observed.ran ? 1u : 0u, observed.parent_valid ? 1u : 0u,
                static_cast<unsigned long long>(observed.parent.device),
                static_cast<unsigned long long>(observed.parent.inode),
                observed.child_seen ? 1u : 0u, observed.child_valid ? 1u : 0u,
                static_cast<unsigned long long>(observed.child.device),
                static_cast<unsigned long long>(observed.child.inode),
                static_cast<unsigned long long>(observed.child.owner), observed.child.mode,
                observed.mkdir_attempted ? 1u : 0u, observed.mkdir_errno,
                observed.child_lookup_errno, observed.path_errno, observed.sync_errno,
                observed.close_errno, observed.created ? 1u : 0u, observed.complete ? 1u : 0u);
    return 0;
}
