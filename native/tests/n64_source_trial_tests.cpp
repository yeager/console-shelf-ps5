// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/n64_source_trial.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static std::string read_file(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    assert(file);
    std::string bytes;
    char block[65536];
    for (std::size_t count; (count = std::fread(block, 1, sizeof(block), file)) != 0;)
        bytes.append(block, count);
    assert(!std::ferror(file) && std::fclose(file) == 0);
    return bytes;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    using namespace shelf::n64_source_trial;
    const char* pack_path = argv[1];
    char root[] = "/tmp/isshelf-n64-source-XXXXXX";
    assert(::mkdtemp(root));
    constexpr char nonce[] = "0123456789abcdef0123456789abcdef";
    constexpr char other[] = "11111111111111111111111111111111";
    const auto invalid = run_once(root, pack_path, "not-a-nonce");
    assert(!invalid.attempted && !invalid.complete && std::strcmp(invalid.failure, "input") == 0);
    const auto first = run_once(root, pack_path, nonce);
    assert(first.attempted && first.complete && first.receipt_bytes > 200 &&
           first.receipt_fnv && std::strcmp(first.failure, "none") == 0);
    const auto repeat = run_once(root, pack_path, nonce);
    assert(!repeat.attempted && !repeat.complete &&
           std::strcmp(repeat.failure, "collision") == 0);
    const std::string container = std::string(root) + "/" + folder_prefix + nonce;
    const std::string title = container + "/" + title_id;
    const std::string report = std::string(root) + "/" + receipt_prefix + nonce + ".txt";
    const auto receipt = read_file(report);
    assert(receipt.size() == first.receipt_bytes &&
           shelf::app_tile_source_probe::fnv(
               reinterpret_cast<const unsigned char*>(receipt.data()), receipt.size()) == first.receipt_fnv);
    assert(receipt.find(std::string("nonce=") + nonce + "\n") != std::string::npos);
    assert(receipt.find("route=n64,library,-\n") != std::string::npos);
    assert(receipt.find("state=source-only\n") != std::string::npos);
    const auto& pin = isshelf_badge::generic_tile::tiles[n64_index];
    for (std::size_t index = 0; index < 7; ++index) {
        const std::string path = title + "/" + pin.files[index].path;
        const auto file = read_file(path);
        const char* digest = index == 4 ? forwarder_sha256 : pin.files[index].sha256;
        const auto count = index == 4 ? sizeof(forwarder) - 1 : pin.files[index].bytes;
        assert(file.size() == count && hex_digest(
            reinterpret_cast<const unsigned char*>(file.data()), file.size(), digest));
        assert(index != 4 || file == forwarder);
        struct stat info{};
        assert(::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
               (info.st_mode & 07777) == (index < 2 ? 0755 : 0644));
    }
    for (const auto& directory : {container, title, title + "/sce_module", title + "/sce_sys"}) {
        struct stat info{};
        assert(::lstat(directory.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
               (info.st_mode & 07777) == 0755);
    }
    const std::string alias = std::string(root) + "-alias";
    assert(::symlink(root, alias.c_str()) == 0);
    const auto redirected = run_once(alias.c_str(), pack_path, other);
    assert(!redirected.attempted && !redirected.complete &&
           std::strcmp(redirected.failure, "root-open") == 0);
    const std::string bad_pack = std::string(root) + "/changed-pack.bin";
    {
        FILE* file = std::fopen(bad_pack.c_str(), "wb");
        assert(file && std::fwrite("changed", 1, 7, file) == 7 && std::fclose(file) == 0);
    }
    const auto rejected = run_once(root, bad_pack.c_str(), other);
    assert(!rejected.attempted && !rejected.complete &&
           std::strcmp(rejected.failure, "pack-check") == 0);
    const std::string second = std::string(root) + "/" + folder_prefix + other;
    struct stat info{};
    assert(::lstat(second.c_str(), &info) != 0);

    for (std::size_t index = 0; index < 7; ++index)
        assert(::unlink((title + "/" + pin.files[index].path).c_str()) == 0);
    assert(::rmdir((title + "/sce_module").c_str()) == 0);
    assert(::rmdir((title + "/sce_sys").c_str()) == 0);
    assert(::rmdir(title.c_str()) == 0 && ::rmdir(container.c_str()) == 0);
    assert(::unlink(report.c_str()) == 0 && ::unlink(bad_pack.c_str()) == 0 &&
           ::unlink(alias.c_str()) == 0 && ::rmdir(root) == 0);
    std::puts("PASS: N64 source-only trial exact seven files, library route, replay and changed-pack refusal");
}
