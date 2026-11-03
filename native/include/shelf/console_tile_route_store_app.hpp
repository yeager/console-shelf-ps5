// SPDX-License-Identifier: GPL-3.0-only
// Offline app-process adapter for route-only preferences. It uses imported
// POSIX functions, never the checked_at raw syscall wrappers. The parent is
// writable by other local processes on the owner's PS5, so records are
// untrusted UI input. Neither a record nor this store authorizes tile writes.
#pragma once
#include "console_tile_route_preference.hpp"
#if !defined(_WIN32)
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace shelf::tile_route {
inline constexpr char app_route_directory[] = "tile-routes-v1";
enum class AppRoot { present, missing, unsafe };
enum class AppSave { saved, refused, unknown };

class AppStore {
public:
    explicit AppStore(const char* parent = "/data/console-shelf",
                      uid_t parent_owner = 0, mode_t parent_mode = 0777)
        : parent_path_(parent), parent_owner_(parent_owner),
          parent_mode_(parent_mode), file_owner_(geteuid()) {
        const int used = parent_path_ && parent_path_[0] == '/'
            ? std::snprintf(root_path_, sizeof(root_path_), "%s/%s", parent_path_, app_route_directory)
            : -1;
        path_valid_ = used > 0 && static_cast<std::size_t>(used) < sizeof(root_path_) &&
                      std::strstr(parent_path_, "..") == nullptr;
    }
    AppStore(const AppStore&) = delete;
    AppStore& operator=(const AppStore&) = delete;
    ~AppStore() {
        if (root_fd_ >= 0) ::close(root_fd_);
        if (parent_fd_ >= 0) ::close(parent_fd_);
    }
#ifdef ISSHELF_APP_ROUTE_STORE_HOST_TEST
    void test_fail_post_mkdir_sync_once() { fail_post_mkdir_sync_ = true; }
    void test_fail_post_rename_sync_once() { fail_post_rename_sync_ = true; }
#endif

    // Missing is a legacy/default state. A replaced or inaccessible root is
    // unsafe, never equivalent to a missing preference. The parent descriptor
    // is retained across missing->create on one instance.
    AppRoot open_root(bool create) {
        if (unusable_ || !path_valid_ || geteuid() != file_owner_) return unsafe();
        if (parent_fd_ < 0)
            parent_fd_ = ::open(parent_path_, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (parent_fd_ < 0 || !parent_identity()) return unsafe();
        if (root_fd_ >= 0) return identity_fresh() ? AppRoot::present : unsafe();
        root_fd_ = ::open(root_path_, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (root_fd_ < 0) {
            if (errno != ENOENT || !parent_identity()) return unsafe();
            if (!create) return AppRoot::missing;
            if (::mkdir(root_path_, 0700) != 0) return unsafe();
#ifdef ISSHELF_APP_ROUTE_STORE_HOST_TEST
            if (fail_post_mkdir_sync_) {
                fail_post_mkdir_sync_ = false;
                errno = EIO;
                return unsafe();
            }
#endif
            if (!parent_identity() || ::fsync(parent_fd_) != 0) return unsafe();
            root_fd_ = ::open(root_path_, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        return root_fd_ >= 0 && identity_fresh() ? AppRoot::present : unsafe();
    }

    bool identity_fresh() const {
        if (unusable_ || root_fd_ < 0 || geteuid() != file_owner_ || !parent_identity()) return false;
        struct stat named{}, held{}, parent{};
        return ::lstat(root_path_, &named) == 0 && ::fstat(root_fd_, &held) == 0 &&
               ::fstat(parent_fd_, &parent) == 0 && same_identity(named, held) &&
               S_ISDIR(named.st_mode) && named.st_uid == file_owner_ &&
               named.st_dev == parent.st_dev &&
               (named.st_mode & 07777u) == 0700u &&
               (held.st_mode & 07777u) == 0700u;
    }

    Input read(Source source, Preference& output) const {
        char path[384], body[maximum_record + 1];
        if (!identity_fresh() || !path_for(source, path, sizeof(path))) return Input::invalid;
        std::size_t used = 0;
        const Input result = read_path(path, body, sizeof(body), used);
        if (result != Input::valid) return result;
        Preference parsed;
        if (!parse_preference(std::string_view(body, used), parsed) ||
            parsed.system != source.system) return Input::invalid;
        output = parsed;
        return Input::valid;
    }

    // A caller supplies one fresh CSPRNG nonce. A failed or uncertain attempt
    // leaves its temp file for inspection and is never retried automatically.
    AppSave write(Source source, const Preference& preference, std::string_view nonce) {
        char path[384], temp[440], body[maximum_record + 1];
        if (write_uncertain_ || !identity_fresh() || !path_for(source, path, sizeof(path)) ||
            preference.system != source.system ||
            !encode_preference(preference, body, sizeof(body)) || !nonce_valid(nonce) ||
            !existing_target_safe(path)) return AppSave::refused;
        const int used = std::snprintf(temp, sizeof(temp), "%s.%.*s.tmp", path,
                                       static_cast<int>(nonce.size()), nonce.data());
        if (used <= 0 || static_cast<std::size_t>(used) >= sizeof(temp)) return AppSave::refused;
        const int fd = ::open(temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) return AppSave::refused;
        const std::size_t size = std::strlen(body);
        const bool written = ::write(fd, body, size) == static_cast<ssize_t>(size) &&
                             ::fsync(fd) == 0 && file_identity(temp, fd);
        const bool closed = ::close(fd) == 0;
        if (!written || !closed || !identity_fresh() || !existing_target_safe(path) ||
            ::fsync(root_fd_) != 0) return AppSave::refused;
        char checked[maximum_record + 1];
        std::size_t checked_size = 0;
        Preference parsed;
        if (read_path(temp, checked, sizeof(checked), checked_size) != Input::valid ||
            checked_size != size || std::memcmp(checked, body, size) != 0 ||
            !parse_preference(std::string_view(checked, checked_size), parsed) ||
            parsed.system != source.system) return AppSave::refused;
        // rename is an imported path-based call. A writable ancestor leaves a
        // local replacement race, so post-rename ambiguity is always unknown.
        if (::rename(temp, path) != 0) return uncertain();
#ifdef ISSHELF_APP_ROUTE_STORE_HOST_TEST
        if (fail_post_rename_sync_) {
            fail_post_rename_sync_ = false;
            errno = EIO;
            return uncertain();
        }
#endif
        if (::fsync(root_fd_) != 0 || !identity_fresh()) return uncertain();
        std::size_t final_size = 0;
        if (read_path(path, checked, sizeof(checked), final_size) != Input::valid ||
            final_size != size || std::memcmp(checked, body, size) != 0)
            return uncertain();
        return AppSave::saved;
    }

private:
    AppRoot unsafe() {
        unusable_ = true;
        return AppRoot::unsafe;
    }
    AppSave uncertain() {
        write_uncertain_ = true;
        return AppSave::unknown;
    }
    static bool same_identity(const struct stat& named, const struct stat& held) {
        return named.st_dev == held.st_dev && named.st_ino == held.st_ino &&
               named.st_ino != 0 && named.st_uid == held.st_uid &&
               (named.st_mode & S_IFMT) == (held.st_mode & S_IFMT);
    }
    static bool same_snapshot(const struct stat& before, const struct stat& after) {
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
        return parent_fd_ >= 0 && ::lstat(parent_path_, &named) == 0 &&
               ::fstat(parent_fd_, &held) == 0 && same_identity(named, held) &&
               S_ISDIR(named.st_mode) && named.st_uid == parent_owner_ &&
               (named.st_mode & 07777u) == parent_mode_ &&
               (held.st_mode & 07777u) == parent_mode_;
    }
    bool path_for(Source source, char* output, std::size_t capacity) const {
        if (!path_valid_ || !source_valid(source) || !output) return false;
        const auto id = catalog[source.system].id;
        const int used = std::snprintf(output, capacity, "%s/route-%.*s.txt", root_path_,
                                       static_cast<int>(id.size()), id.data());
        return used > 0 && static_cast<std::size_t>(used) < capacity;
    }
    static bool nonce_valid(std::string_view nonce) {
        if (nonce.size() != 32) return false;
        for (char c : nonce)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        return true;
    }
    bool file_identity(const char* path, int fd) const {
        struct stat named{}, held{}, root{};
        return identity_fresh() && ::lstat(path, &named) == 0 &&
               ::fstat(fd, &held) == 0 && ::fstat(root_fd_, &root) == 0 &&
               same_identity(named, held) && S_ISREG(named.st_mode) &&
               named.st_dev == root.st_dev && named.st_uid == file_owner_ &&
               named.st_nlink == 1 && held.st_nlink == 1 &&
               (named.st_mode & 07777u) == 0600u &&
               (held.st_mode & 07777u) == 0600u;
    }
    bool existing_target_safe(const char* path) const {
        const int fd = ::open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return errno == ENOENT && identity_fresh();
        const bool safe = file_identity(path, fd);
        const bool closed = ::close(fd) == 0;
        return safe && closed;
    }
    Input read_path(const char* path, char* body, std::size_t capacity,
                    std::size_t& used) const {
        used = 0;
        if (!identity_fresh()) return Input::invalid;
        const int fd = ::open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return errno == ENOENT && identity_fresh() ? Input::absent : Input::invalid;
        struct stat before{}, after{};
        const bool safe = file_identity(path, fd) && ::fstat(fd, &before) == 0 &&
                          before.st_size >= 0 && static_cast<std::size_t>(before.st_size) < capacity;
        if (!safe) { ::close(fd); return Input::invalid; }
        const auto size = static_cast<std::size_t>(before.st_size);
        std::size_t offset = 0;
        while (offset < size) {
            const auto count = ::read(fd, body + offset, size - offset);
            if (count <= 0) break;
            offset += static_cast<std::size_t>(count);
        }
        const bool still_safe = file_identity(path, fd) && ::fstat(fd, &after) == 0 &&
                                same_snapshot(before, after);
        const bool closed = ::close(fd) == 0;
        if (offset != size || !still_safe || !closed || !identity_fresh()) return Input::invalid;
        used = size;
        return Input::valid;
    }
    const char* parent_path_;
    uid_t parent_owner_;
    mode_t parent_mode_;
    uid_t file_owner_;
    char root_path_[320]{};
    bool path_valid_ = false;
    bool unusable_ = false;
    bool write_uncertain_ = false;
#ifdef ISSHELF_APP_ROUTE_STORE_HOST_TEST
    bool fail_post_mkdir_sync_ = false;
    bool fail_post_rename_sync_ = false;
#endif
    int parent_fd_ = -1;
    int root_fd_ = -1;
};
} // namespace shelf::tile_route
#endif
