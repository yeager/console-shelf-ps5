// SPDX-License-Identifier: GPL-3.0-only
// Private UI50 source-only N64 trial. No ShadowMount or app.db operation.
#pragma once
#if defined(_WIN32)
#error "The N64 source trial requires POSIX descriptor semantics"
#endif

#include "app_tile_source_probe.hpp"
#include "../../../tools/console_badges/generic_tile_pack_native.hpp"
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace shelf::n64_source_trial {

constexpr char fixed_root[] = "/data/console-shelf";
constexpr char fixed_pack[] = "/data/homebrew/PPSA99541/assets/console-tiles/pack-v1/pack.bin";
constexpr char folder_prefix[] = ".isshelf-tile-v1-";
constexpr char receipt_prefix[] = "isshelf-tile-v1-";
constexpr char title_id[] = "PPSA99711";
constexpr char route[] = "n64,library,-";
constexpr char forwarder[] =
    "{\"format\":\"ps5-forwarder/1\",\"target\":\"PPSA99541\","
    "\"args\":[\"--isshelf-console-tile=n64,library,-\"],\"creator\":\"isShelf\"}\n";
constexpr char forwarder_sha256[] = "83499860accf804fb5de7468025a34c79ed00a7df92921c4358c1aa557e8a7d8";
constexpr std::size_t n64_index = 11;
static_assert(std::string_view(isshelf_badge::generic_tile::tiles[n64_index].system) == "n64");
static_assert(std::string_view(isshelf_badge::generic_tile::tiles[n64_index].title_id) == title_id);

struct Result {
    bool attempted = false;
    bool complete = false;
    std::size_t receipt_bytes = 0;
    std::uint64_t receipt_fnv = 0;
    const char* failure = "none";
};

inline bool same_file(const struct stat& left, const struct stat& right) {
    return S_ISREG(left.st_mode) && S_ISREG(right.st_mode) &&
           left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
           left.st_uid == right.st_uid &&
           app_tile_source_probe::scratch_link_count_ok(left.st_nlink) &&
           app_tile_source_probe::scratch_link_count_ok(right.st_nlink);
}

inline bool same_file_name(const char* path, const struct stat& held) {
    const int fd = ::open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat named{};
    const bool same = ::fstat(fd, &named) == 0 && same_file(named, held);
    const bool closed = ::close(fd) == 0;
    return same && closed;
}

inline bool hex_digest(const unsigned char* bytes, std::size_t count, const char* expected) {
    return isshelf_badge::generic_tile::pack_hash_equals(bytes, count, expected);
}

inline bool read_pack(const char* path, unsigned char*& bytes) {
    using namespace isshelf_badge::generic_tile;
    bytes = nullptr;
    const int fd = ::open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat first{}, last{};
    bool okay = ::fstat(fd, &first) == 0 && S_ISREG(first.st_mode) &&
                app_tile_source_probe::scratch_link_count_ok(first.st_nlink) &&
                first.st_size == static_cast<off_t>(pack_bytes);
    if (okay) bytes = static_cast<unsigned char*>(std::malloc(pack_bytes));
    okay = okay && bytes;
    std::size_t used = 0;
    while (okay && used < pack_bytes) {
        const ssize_t got = ::read(fd, bytes + used, pack_bytes - used);
        if (got <= 0) okay = false;
        else used += static_cast<std::size_t>(got);
    }
    okay = okay && ::fstat(fd, &last) == 0 && same_file(first, last) &&
           last.st_size == static_cast<off_t>(pack_bytes) &&
           same_file_name(path, first);
    const bool closed = ::close(fd) == 0;
    okay = okay && closed;
    if (!okay || !hex_digest(bytes, pack_bytes, pack_sha256)) {
        std::free(bytes);
        bytes = nullptr;
        return false;
    }
    return true;
}

inline bool readback_file(const char* path, const unsigned char* expected,
                          std::size_t count, const char* digest,
                          mode_t mode, uid_t owner) {
    const int fd = ::open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat first{}, last{};
    bool okay = ::fstat(fd, &first) == 0 && S_ISREG(first.st_mode) &&
                app_tile_source_probe::scratch_link_count_ok(first.st_nlink) &&
                first.st_uid == owner && (first.st_mode & 07777) == mode &&
                first.st_size == static_cast<off_t>(count);
    isshelf_badge::Sha256 hash;
    unsigned char buffer[65536];
    std::size_t used = 0;
    while (okay && used < count) {
        const std::size_t want = count - used < sizeof(buffer) ? count - used : sizeof(buffer);
        const ssize_t got = ::read(fd, buffer, want);
        if (got <= 0 || std::memcmp(buffer, expected + used, static_cast<std::size_t>(got)) != 0)
            okay = false;
        else {
            hash.update(buffer, static_cast<std::size_t>(got));
            used += static_cast<std::size_t>(got);
        }
    }
    unsigned char trailing = 0;
    okay = okay && ::read(fd, &trailing, 1) == 0;
    const auto actual = hash.finish();
    constexpr char alphabet[] = "0123456789abcdef";
    if (digest) {
        if (std::strlen(digest) != 64) okay = false;
        for (std::size_t i = 0; okay && i < actual.size(); ++i)
            if (digest[i * 2] != alphabet[actual[i] >> 4] ||
                digest[i * 2 + 1] != alphabet[actual[i] & 15]) okay = false;
    }
    okay = okay && ::fstat(fd, &last) == 0 && same_file(first, last) &&
           (last.st_mode & 07777) == mode && last.st_size == static_cast<off_t>(count) &&
           same_file_name(path, first);
    const bool closed = ::close(fd) == 0;
    return okay && closed;
}

inline Result run_once(const char* root, const char* pack_path, const char* nonce) {
    using namespace isshelf_badge::generic_tile;
    Result result;
    if (!root || !pack_path || !app_tile_source_probe::nonce_valid(nonce) ||
        std::strlen(root) > 2000 || std::strlen(pack_path) > 2000) {
        result.failure = "input";
        return result;
    }
#ifdef SHELF_PS5
    if (std::strcmp(root, fixed_root) != 0 || std::strcmp(pack_path, fixed_pack) != 0) {
        result.failure = "fixed-path";
        return result;
    }
#endif
    char folder[2304], title[2304], module[2304], sys[2304], receipt[2304];
    if (std::snprintf(folder, sizeof(folder), "%s/%s%s", root, folder_prefix, nonce) >=
            static_cast<int>(sizeof(folder)) ||
        std::snprintf(title, sizeof(title), "%s/%s", folder, title_id) >=
            static_cast<int>(sizeof(title)) ||
        std::snprintf(module, sizeof(module), "%s/sce_module", title) >=
            static_cast<int>(sizeof(module)) ||
        std::snprintf(sys, sizeof(sys), "%s/sce_sys", title) >=
            static_cast<int>(sizeof(sys)) ||
        std::snprintf(receipt, sizeof(receipt), "%s/%s%s.txt", root, receipt_prefix, nonce) >=
            static_cast<int>(sizeof(receipt))) {
        result.failure = "path-format";
        return result;
    }
    const int root_fd = ::open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root_fd < 0) { result.failure = "root-open"; return result; }
    struct stat root_stat{};
    if (::fstat(root_fd, &root_stat) != 0 || !S_ISDIR(root_stat.st_mode) ||
        !app_tile_source_probe::same_directory_name(root, root_stat)) {
        ::close(root_fd);
        result.failure = "root-check";
        return result;
    }
    unsigned char* pack_bytes_owned = nullptr;
    if (!read_pack(pack_path, pack_bytes_owned)) {
        ::close(root_fd);
        result.failure = "pack-check";
        return result;
    }
    VerifiedNativePack pack(pack_bytes_owned, pack_bytes);
    if (!pack.valid() || !hex_digest(reinterpret_cast<const unsigned char*>(forwarder),
                                    sizeof(forwarder) - 1, forwarder_sha256)) {
        std::free(pack_bytes_owned); ::close(root_fd);
        result.failure = "pin-check";
        return result;
    }
    const char* paths[] = {folder, title, module, sys};
    int fds[4] = {-1, -1, -1, -1};
    struct stat dir_stats[4]{};
    auto finish = [&](const char* failure) {
        bool closed = true;
        for (int& fd : fds) if (fd >= 0) { closed = ::close(fd) == 0 && closed; fd = -1; }
        closed = ::close(root_fd) == 0 && closed;
        std::free(pack_bytes_owned);
        if (failure) result.failure = failure;
        else if (!closed) { result.failure = "close"; result.complete = false; }
        return result;
    };
    for (unsigned i = 0; i < 4; ++i) {
        if (!app_tile_source_probe::same_directory_name(root, root_stat) ||
            (i && !app_tile_source_probe::same_directory_name(paths[i-1], dir_stats[i-1])))
            return finish("parent-changed");
        if (::mkdir(paths[i], 0700) != 0) {
            const int mkdir_errno = errno;
            return finish(mkdir_errno == EEXIST ? "collision" : "mkdir");
        }
        result.attempted = true;
        fds[i] = ::open(paths[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fds[i] < 0 || ::fstat(fds[i], &dir_stats[i]) != 0 ||
            !S_ISDIR(dir_stats[i].st_mode) || (dir_stats[i].st_mode & 07777) != 0700 ||
            (i && dir_stats[i].st_uid != dir_stats[0].st_uid) ||
            !app_tile_source_probe::same_directory_name(paths[i], dir_stats[i]))
            return finish("directory-check");
    }
    const auto& pin = tiles[n64_index];
    for (std::size_t index = 0; index < 7; ++index) {
        PackView view{};
        const char* digest = pin.files[index].sha256;
        if (index == 4) {
            view = {reinterpret_cast<const unsigned char*>(forwarder), sizeof(forwarder) - 1};
            digest = forwarder_sha256;
        } else if (!pack.file(n64_index, index, view) ||
                   view.bytes != pin.files[index].bytes ||
                   !hex_digest(view.data, view.bytes, digest))
            return finish("source-pin");
        char target[2304];
        if (std::snprintf(target, sizeof(target), "%s/%s", title, pin.files[index].path) >=
            static_cast<int>(sizeof(target))) return finish("file-path");
        const char* parent = index == 1 ? module : (index == 3 || index == 6 ? sys : title);
        const unsigned parent_index = index == 1 ? 2 : (index == 3 || index == 6 ? 3 : 1);
        if (!app_tile_source_probe::same_directory_name(root, root_stat) ||
            !app_tile_source_probe::same_directory_name(folder, dir_stats[0]) ||
            !app_tile_source_probe::same_directory_name(title, dir_stats[1]) ||
            !app_tile_source_probe::same_directory_name(parent, dir_stats[parent_index]))
            return finish("parent-changed");
        const int file_fd = ::open(target, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (file_fd < 0) return finish("file-create");
        const mode_t mode = index < 2 ? 0755 : 0644;
        const bool wrote = app_tile_source_probe::write_exact(file_fd,
              reinterpret_cast<const char*>(view.data), view.bytes) &&
              ::fchmod(file_fd, mode) == 0 && ::fsync(file_fd) == 0;
        struct stat created{};
        const bool checked = wrote && ::fstat(file_fd, &created) == 0 &&
              S_ISREG(created.st_mode) && (created.st_mode & 07777) == mode &&
              created.st_size == static_cast<off_t>(view.bytes) &&
              same_file_name(target, created);
        const bool closed = ::close(file_fd) == 0;
        if (!checked || !closed ||
            !readback_file(target, view.data, view.bytes, digest, mode, created.st_uid))
            return finish("file-check");
    }
    for (unsigned i = 4; i-- > 0;) {
        if (::fchmod(fds[i], 0755) != 0 || ::fsync(fds[i]) != 0 ||
            ::fstat(fds[i], &dir_stats[i]) != 0 ||
            (dir_stats[i].st_mode & 07777) != 0755 ||
            !app_tile_source_probe::same_directory_name(paths[i], dir_stats[i]))
            return finish("finalize");
    }
    if (!app_tile_source_probe::same_directory_name(root, root_stat) || ::fsync(root_fd) != 0)
        return finish("root-finalize");
    char body[512];
    const int length = std::snprintf(body, sizeof(body),
        "ISSHELF_N64_SOURCE_V1\nnonce=%s\nsource=%s\ntitle=PPSA99711\n"
        "route=n64,library,-\npack_sha256=%s\nfiles=7\nstate=source-only\n",
        nonce, title, pack_sha256);
    if (length <= 0 || length >= static_cast<int>(sizeof(body)))
        return finish("receipt-format");
    const int report_fd = ::open(receipt, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (report_fd < 0) return finish("receipt-create");
    const bool report_written = app_tile_source_probe::write_exact(report_fd, body,
          static_cast<std::size_t>(length)) && ::fchmod(report_fd, 0644) == 0 &&
          ::fsync(report_fd) == 0;
    struct stat report_stat{};
    const bool report_valid = report_written && ::fstat(report_fd, &report_stat) == 0 &&
          S_ISREG(report_stat.st_mode) && (report_stat.st_mode & 07777) == 0644 &&
          report_stat.st_size == length && same_file_name(receipt, report_stat);
    const bool report_closed = ::close(report_fd) == 0;
    if (!report_valid || !report_closed ||
        !readback_file(receipt, reinterpret_cast<const unsigned char*>(body),
                       static_cast<std::size_t>(length), nullptr, 0644, report_stat.st_uid)) {
        // The receipt readback below uses byte comparison; its FNV is bound to
        // the bridge response. A null digest skips only the SHA comparison.
        return finish("receipt-check");
    }
    if (::fsync(root_fd) != 0 || !app_tile_source_probe::same_directory_name(root, root_stat))
        return finish("receipt-sync");
    result.complete = true;
    result.receipt_bytes = static_cast<std::size_t>(length);
    result.receipt_fnv = app_tile_source_probe::fnv(
        reinterpret_cast<const unsigned char*>(body), result.receipt_bytes);
    return finish(nullptr);
}
} // namespace shelf::n64_source_trial
