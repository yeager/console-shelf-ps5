// SPDX-License-Identifier: GPL-3.0-only
// Offline POSIX prototype for a per-console launch preference. Records remain
// untrusted UI input under the observed 0777 parent. The checked_at raw syscall
// wrappers have not been qualified inside the PS5 app process.
#pragma once
#include "console_tile_route_preference.hpp"
#if !defined(_WIN32)
#include "../../../tools/console_badges/checked_at_ps5.hpp"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace shelf::tile_route {
inline constexpr char route_directory[] = "tile-routes-v1";
enum class Save { saved, refused, unknown };
enum class Root { present, missing, unsafe };

class PosixStore {
public:
    explicit PosixStore(const char* parent = "/data/console-shelf",
                        uid_t parent_owner = 0, mode_t parent_mode = 0777)
        : parent_path_(parent), expected_parent_owner_(parent_owner),
          expected_parent_mode_(parent_mode), expected_owner_(geteuid()) {}
    PosixStore(const PosixStore&) = delete;
    PosixStore& operator=(const PosixStore&) = delete;
    ~PosixStore() {
        if (root_fd_ >= 0) close(root_fd_);
        if (parent_fd_ >= 0) close(parent_fd_);
    }

    // A missing route directory is a legacy/default state; unsafe is not.
    // Retain the checked parent descriptor so a later create on this instance
    // cannot silently bind to a different parent path.
    Root open_root(bool create) {
        if (!parent_path_ || geteuid() != expected_owner_) return Root::unsafe;
        if (parent_fd_ < 0)
            parent_fd_ = ::open(parent_path_, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (parent_fd_ < 0 || !parent_identity()) return Root::unsafe;
        if (root_fd_ >= 0) return identity_fresh() ? Root::present : Root::unsafe;
        root_fd_ = isshelf_badge::checked_at::openat(
            parent_fd_, route_directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (root_fd_ < 0) {
            if (errno != ENOENT || !parent_identity()) return Root::unsafe;
            if (!create) return Root::missing;
            if (isshelf_badge::checked_at::mkdirat(parent_fd_, route_directory, 0700) != 0 ||
                fsync(parent_fd_) != 0) return Root::unsafe;
            root_fd_ = isshelf_badge::checked_at::openat(
                parent_fd_, route_directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        return root_fd_ >= 0 && identity_fresh() ? Root::present : Root::unsafe;
    }

    bool identity_fresh() const {
        if (parent_fd_ < 0 || root_fd_ < 0 || geteuid() != expected_owner_ ||
            !parent_identity()) return false;
        struct stat named{}, held{};
        return isshelf_badge::checked_at::fstatat(parent_fd_, route_directory, &named,
                                                  AT_SYMLINK_NOFOLLOW) == 0 &&
               fstat(root_fd_, &held) == 0 && same_identity(named, held) &&
               S_ISDIR(named.st_mode) && named.st_uid == expected_owner_ &&
               (named.st_mode & 07777u) == 0700u &&
               (held.st_mode & 07777u) == 0700u;
    }

    Input read(Source source, Preference& output) const {
        char leaf[64];
        if (!identity_fresh() || !leaf_for(source, leaf)) return Input::invalid;
        char body[maximum_record + 1];
        std::size_t used = 0;
        const Input result = read_leaf(leaf, body, sizeof(body), used);
        if (result != Input::valid) return result;
        Preference parsed;
        if (!parse_preference(std::string_view(body, used), parsed) ||
            parsed.system != source.system) return Input::invalid;
        output = parsed;
        return Input::valid;
    }

    // Nonce is supplied by a caller's CSPRNG. A reused nonce or partial temp
    // file is a consumed attempt; this method never deletes one automatically.
    Save write(Source source, const Preference& preference, std::string_view nonce) {
        char leaf[64], temp[112], body[maximum_record + 1];
        if (!identity_fresh() || !leaf_for(source, leaf) ||
            preference.system != source.system || !encode_preference(preference, body, sizeof(body)) ||
            !nonce_valid(nonce)) return Save::refused;
        const int length = std::snprintf(temp, sizeof(temp), "%s.%.*s.tmp", leaf,
                                          static_cast<int>(nonce.size()), nonce.data());
        if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(temp) ||
            !existing_target_safe(leaf)) return Save::refused;
        const int fd = isshelf_badge::checked_at::openat(
            root_fd_, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) return Save::refused;
        const std::size_t size = std::strlen(body);
        const bool written = ::write(fd, body, size) == static_cast<ssize_t>(size) &&
                             fsync(fd) == 0 && file_identity(temp, fd);
        const bool closed = close(fd) == 0;
        if (!written || !closed || !identity_fresh() || !existing_target_safe(leaf) ||
            fsync(root_fd_) != 0) return Save::refused;
        char checked[maximum_record + 1];
        std::size_t used = 0;
        Preference parsed;
        if (read_leaf(temp, checked, sizeof(checked), used) != Input::valid ||
            !parse_preference(std::string_view(checked, used), parsed) ||
            parsed.system != source.system ||
            std::string_view(checked, used) != std::string_view(body, size)) return Save::refused;
        if (isshelf_badge::checked_at::renameat(root_fd_, temp, root_fd_, leaf) != 0)
            return Save::refused;
        // Once rename succeeds, a failed sync/readback is unknown, not a
        // prompt to repeat the write with a new nonce.
        if (fsync(root_fd_) != 0 || !identity_fresh()) return Save::unknown;
        Preference readback;
        if (read(source, readback) != Input::valid ||
            readback.destination != preference.destination ||
            std::strcmp(readback.emulator_title, preference.emulator_title) != 0)
            return Save::unknown;
        return Save::saved;
    }

private:
    static bool same_identity(const struct stat& named, const struct stat& held) {
        return named.st_dev == held.st_dev && named.st_ino == held.st_ino &&
               named.st_ino != 0 && named.st_uid == held.st_uid &&
               (named.st_mode & S_IFMT) == (held.st_mode & S_IFMT);
    }
    static bool same_file_snapshot(const struct stat& before, const struct stat& after) {
        return same_identity(before, after) && before.st_size == after.st_size &&
               before.st_nlink == after.st_nlink &&
               before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
               before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
               before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
               before.st_ctim.tv_nsec == after.st_ctim.tv_nsec &&
               (before.st_mode & 07777u) == (after.st_mode & 07777u);
    }
    bool parent_identity() const {
        struct stat named{}, held{};
        return parent_fd_ >= 0 && lstat(parent_path_, &named) == 0 &&
               fstat(parent_fd_, &held) == 0 && same_identity(named, held) &&
               S_ISDIR(named.st_mode) && named.st_uid == expected_parent_owner_ &&
               (named.st_mode & 07777u) == expected_parent_mode_ &&
               (held.st_mode & 07777u) == expected_parent_mode_;
    }
    static bool leaf_for(Source source, char (&leaf)[64]) {
        if (!source_valid(source)) return false;
        const auto id = catalog[source.system].id;
        const int used = std::snprintf(leaf, sizeof(leaf), "route-%.*s.txt",
                                       static_cast<int>(id.size()), id.data());
        return used > 0 && static_cast<std::size_t>(used) < sizeof(leaf);
    }
    static bool nonce_valid(std::string_view nonce) {
        if (nonce.size() != 32) return false;
        for (char c : nonce)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        return true;
    }
    bool file_identity(const char* leaf, int fd) const {
        struct stat named{}, held{}, root{};
        return identity_fresh() &&
               isshelf_badge::checked_at::fstatat(root_fd_, leaf, &named,
                                                  AT_SYMLINK_NOFOLLOW) == 0 &&
               fstat(fd, &held) == 0 && fstat(root_fd_, &root) == 0 &&
               same_identity(named, held) && S_ISREG(named.st_mode) &&
               named.st_dev == root.st_dev && named.st_uid == expected_owner_ &&
               named.st_nlink == 1 && held.st_nlink == 1 &&
               (named.st_mode & 07777u) == 0600u &&
               (held.st_mode & 07777u) == 0600u;
    }
    bool existing_target_safe(const char* leaf) const {
        const int fd = isshelf_badge::checked_at::openat(
            root_fd_, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return errno == ENOENT && identity_fresh();
        const bool safe = file_identity(leaf, fd);
        const bool closed = close(fd) == 0;
        return safe && closed;
    }
    Input read_leaf(const char* leaf, char* body, std::size_t capacity,
                    std::size_t& used) const {
        used = 0;
        if (!identity_fresh()) return Input::invalid;
        const int fd = isshelf_badge::checked_at::openat(
            root_fd_, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return errno == ENOENT && identity_fresh() ? Input::absent : Input::invalid;
        struct stat before{}, after{};
        const bool safe = file_identity(leaf, fd) && fstat(fd, &before) == 0 &&
                          before.st_size >= 0 && static_cast<std::size_t>(before.st_size) < capacity;
        if (!safe) { close(fd); return Input::invalid; }
        const auto size = static_cast<std::size_t>(before.st_size);
        std::size_t offset = 0;
        while (offset < size) {
            const auto count = ::read(fd, body + offset, size - offset);
            if (count <= 0) break;
            offset += static_cast<std::size_t>(count);
        }
        const bool still_safe = file_identity(leaf, fd) && fstat(fd, &after) == 0 &&
                                same_file_snapshot(before, after);
        const bool closed = close(fd) == 0;
        if (offset != size || !still_safe || !closed || !identity_fresh()) return Input::invalid;
        used = size;
        return Input::valid;
    }
    const char* parent_path_;
    uid_t expected_parent_owner_;
    mode_t expected_parent_mode_;
    uid_t expected_owner_;
    int parent_fd_ = -1;
    int root_fd_ = -1;
};
} // namespace shelf::tile_route
#endif
