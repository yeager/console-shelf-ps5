// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/console_tile_job_directory.hpp"
#include "shelf/console_tile_job_dispatch.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
void check(bool okay, const char* message) {
    if (!okay) {
        std::fprintf(stderr, "FAIL tile directory: %s\n", message);
        std::exit(1);
    }
}
}

int main() {
    using namespace shelf::tile_job;
    DirectoryIdentity parent{2, 100, 0, 0777, true, false};
    DirectoryIdentity child{2, 200, 1001, 0700, true, false};
    check(parent_identity_valid(parent, parent), "actual root-owned 0777 parent is accepted");
    check(private_child_identity_valid(child, child, 1001), "owned 0700 child is accepted");

    auto wrong = parent;
    wrong.owner = 1001;
    check(!parent_identity_valid(wrong, wrong), "unowned parent is rejected");
    wrong = parent;
    wrong.symlink = true;
    wrong.directory = false;
    check(!parent_identity_valid(wrong, parent), "symlink parent is rejected");
    wrong = parent;
    wrong.inode = 101;
    check(!parent_identity_valid(wrong, parent), "renamed parent is rejected");

    wrong = child;
    wrong.owner = 1002;
    check(!private_child_identity_valid(wrong, wrong, 1001),
          "preexisting child with wrong owner is rejected");
    wrong = child;
    wrong.mode = 0777;
    check(!private_child_identity_valid(wrong, wrong, 1001),
          "preexisting world-writable child is rejected");
    wrong.mode = 0755;
    check(!private_child_identity_valid(wrong, wrong, 1001),
          "preexisting world-readable child is rejected");
    wrong = child;
    wrong.symlink = true;
    wrong.directory = false;
    check(!private_child_identity_valid(wrong, child, 1001),
          "symlink child is rejected");
    wrong = child;
    wrong.inode = 201;
    check(!private_child_identity_valid(wrong, child, 1001),
          "name replacement after opening held child is rejected");
    wrong = child;
    wrong.device = 3;
    check(!private_child_identity_valid(wrong, child, 1001),
          "child replacement across devices is rejected");
    wrong = child;
    wrong.inode = 0;
    check(!private_child_identity_valid(wrong, child, 1001),
          "incomplete identity record is rejected");
    check(!private_child_identity_valid(child, child, 0),
          "current process owner must match held child");

    char nonce[33] = "0123456789abcdef0123456789abcdef";
    char path[maximum_job_path]{};
    check(job_path(JobFile::request, nonce, path) &&
          std::strcmp(path, "/data/console-shelf/tile-jobs-v1/"
                            "console-tile-job-0123456789abcdef0123456789abcdef.txt") == 0,
          "request path uses the private child directory");
    check(job_path(JobFile::result, nonce, path) &&
          std::strncmp(path, job_root, std::strlen(job_root)) == 0,
          "receipt uses the same private child directory");
    std::puts("PASS: private tile directory owner/mode/symlink/race contract");
}
