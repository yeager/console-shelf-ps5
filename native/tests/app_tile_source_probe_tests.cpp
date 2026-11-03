// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/app_tile_source_probe.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

int main() {
    char root[] = "/tmp/isshelf-app-source-XXXXXX";
    assert(::mkdtemp(root) != nullptr);
    constexpr char nonce[] = "0123456789abcdef0123456789abcdef";
    using namespace shelf::app_tile_source_probe;
    assert(scratch_link_count_ok(0) && scratch_link_count_ok(1) &&
           !scratch_link_count_ok(2));
    const auto invalid = run_once(root, "0123456789abcdef0123456789abcdeg");
    assert(!invalid.attempted && !invalid.complete &&
           std::strcmp(invalid.failure, "input") == 0);
    const std::string alias = std::string(root) + "-alias";
    assert(::symlink(root, alias.c_str()) == 0);
    const auto redirected = run_once(alias.c_str(), "fedcba9876543210fedcba9876543210");
    assert(!redirected.attempted && !redirected.complete &&
           std::strcmp(redirected.failure, "parent-open") == 0);

    const auto first = run_once(root, nonce);
    assert(first.attempted && first.complete && first.receipt_bytes > 100 &&
           first.receipt_fnv != 0 && std::strcmp(first.failure, "none") == 0);
    const auto again = run_once(root, nonce);
    assert(!again.attempted && !again.complete &&
           std::strcmp(again.failure, "collision") == 0);

    // A preexisting path is rejected by mkdir itself. Never follow it or
    // replace its contents, including when it is a symlink.
    constexpr char occupied_nonce[] = "11111111111111111111111111111111";
    const std::string occupied = std::string(root) + "/" + folder_prefix + occupied_nonce;
    assert(::symlink(root, occupied.c_str()) == 0);
    const auto occupied_result = run_once(root, occupied_nonce);
    assert(!occupied_result.attempted && !occupied_result.complete &&
           std::strcmp(occupied_result.failure, "collision") == 0);
    struct stat occupied_stat{};
    assert(::lstat(occupied.c_str(), &occupied_stat) == 0 && S_ISLNK(occupied_stat.st_mode));
    assert(::unlink(occupied.c_str()) == 0);

    // The receipt uses O_EXCL. If that name is occupied, preserve its bytes
    // and leave the partial source for inspection rather than overwrite it.
    constexpr char receipt_nonce[] = "22222222222222222222222222222222";
    const std::string occupied_receipt =
        std::string(root) + "/" + receipt_prefix + receipt_nonce + ".txt";
    FILE* preexisting = std::fopen(occupied_receipt.c_str(), "wb");
    assert(preexisting != nullptr && std::fwrite("keep", 1, 4, preexisting) == 4 &&
           std::fclose(preexisting) == 0);
    const auto receipt_result = run_once(root, receipt_nonce);
    assert(receipt_result.attempted && !receipt_result.complete &&
           std::strcmp(receipt_result.failure, "report-check") == 0);
    char preserved[5]{};
    preexisting = std::fopen(occupied_receipt.c_str(), "rb");
    assert(preexisting != nullptr && std::fread(preserved, 1, 4, preexisting) == 4 &&
           std::fclose(preexisting) == 0 && std::strcmp(preserved, "keep") == 0);
    const std::string partial_folder = std::string(root) + "/" + folder_prefix + receipt_nonce;
    assert(::unlink((partial_folder + "/nested/sample.bin").c_str()) == 0 &&
           ::rmdir((partial_folder + "/nested").c_str()) == 0 &&
           ::rmdir(partial_folder.c_str()) == 0 && ::unlink(occupied_receipt.c_str()) == 0);

    const std::string folder = std::string(root) + "/" + folder_prefix + nonce;
    const std::string nested = folder + "/" + nested_name;
    const std::string sample = nested + "/" + sample_name;
    const std::string receipt = std::string(root) + "/" + receipt_prefix + nonce + ".txt";
    struct stat folder_stat{}, nested_stat{}, sample_stat{}, receipt_stat{};
    assert(::lstat(folder.c_str(), &folder_stat) == 0 && S_ISDIR(folder_stat.st_mode) &&
           (folder_stat.st_mode & 07777) == 0755);
    assert(::lstat(nested.c_str(), &nested_stat) == 0 && S_ISDIR(nested_stat.st_mode) &&
           (nested_stat.st_mode & 07777) == 0755 && nested_stat.st_uid == folder_stat.st_uid);
    assert(::lstat(sample.c_str(), &sample_stat) == 0 && S_ISREG(sample_stat.st_mode) &&
           (sample_stat.st_mode & 07777) == 0644);
    assert(::lstat(receipt.c_str(), &receipt_stat) == 0 && S_ISREG(receipt_stat.st_mode) &&
           (receipt_stat.st_mode & 07777) == 0644 &&
           receipt_stat.st_size == static_cast<off_t>(first.receipt_bytes));

    char report[512]{};
    FILE* file = std::fopen(receipt.c_str(), "rb");
    assert(file != nullptr && std::fread(report, 1, first.receipt_bytes, file) == first.receipt_bytes &&
           std::fclose(file) == 0);
    assert(fnv(reinterpret_cast<const unsigned char*>(report), first.receipt_bytes) ==
           first.receipt_fnv);
    assert(std::strstr(report, "state=complete\n") != nullptr);
    assert(std::strstr(report, "folder_mode=493\nnested_mode=493\nsample_mode=420\n") != nullptr);

    assert(::unlink(sample.c_str()) == 0 && ::unlink(receipt.c_str()) == 0 &&
           ::rmdir(nested.c_str()) == 0 && ::rmdir(folder.c_str()) == 0 &&
           ::unlink(alias.c_str()) == 0 && ::rmdir(root) == 0);
    std::puts("PASS: app source scratch is create-only, readable, mode-checked, and nonce-bound");
}
