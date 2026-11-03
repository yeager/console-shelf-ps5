// SPDX-License-Identifier: GPL-3.0-only
// App-owned create-only job files. PS5 builds only; the UI write gate is closed.
#pragma once
#include "console_tile_job_dispatch.hpp"
#include "console_tile_job_directory.hpp"
#if defined(SHELF_PS5)
#include "../../../tools/console_badges/checked_at_ps5.hpp"
#include <atomic>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace shelf::tile_job {

// This is a cooperative sender guard, not a trust boundary: the observed
// /data/console-shelf parent is writable by other console processes.
inline std::atomic_flag process_send_lock = ATOMIC_FLAG_INIT;
inline constexpr char send_lock_leaf[] = "console-tile-send-v1.lock";

// Only exact generated job names may be used below the opened directory.
inline const char* owned_job_leaf(const char* path) {
    if (!path) return nullptr;
    constexpr JobFile kinds[] = {JobFile::request, JobFile::result, JobFile::sent};
    constexpr const char* names[] = {"console-tile-job-", "console-tile-result-",
                                     "console-tile-sent-"};
    constexpr std::size_t root_length = sizeof(job_root) - 1;
    if (std::strncmp(path, job_root, root_length) != 0 || path[root_length] != '/')
        return nullptr;
    const char* leaf = path + root_length + 1;
    for (std::size_t i = 0; i < 3; ++i) {
        const auto prefix = std::strlen(names[i]);
        if (std::strncmp(leaf, names[i], prefix) != 0 ||
            std::strlen(leaf) != prefix + 32 + 4 ||
            std::strcmp(leaf + prefix + 32, ".txt") != 0) continue;
        char nonce[33]{};
        std::memcpy(nonce, leaf + prefix, 32);
        char canonical[maximum_job_path]{};
        if (nonce_valid(nonce) && job_path(kinds[i], nonce, canonical) &&
            std::strcmp(path, canonical) == 0) return leaf;
    }
    return nullptr;
}

class PosixJobStore {
public:
    PosixJobStore() = default;
#if defined(SHELF_TILE_JOB_HOST_TEST)
    PosixJobStore(const char* data_path, std::uint64_t parent_owner)
        : data_path_(data_path), expected_parent_owner_(parent_owner) {}
#endif
    PosixJobStore(const PosixJobStore&) = delete;
    PosixJobStore& operator=(const PosixJobStore&) = delete;
    ~PosixJobStore() {
        if (send_lock_fd_ >= 0) {
            // A failed close leaves the local guard held until process exit.
            if (close(send_lock_fd_) == 0)
                process_send_lock.clear(std::memory_order_release);
        }
        if (root_fd_ >= 0) close(root_fd_);
        if (parent_fd_ >= 0) close(parent_fd_);
        if (data_fd_ >= 0) close(data_fd_);
    }

    bool open_root(bool create_if_missing) {
        if (root_fd_ >= 0) return identity_fresh();
        if (data_fd_ >= 0 || parent_fd_ >= 0) return false;
        data_fd_ = open(data_path_, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (data_fd_ < 0) return false;
        parent_fd_ = isshelf_badge::checked_at::openat(
            data_fd_, "console-shelf", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        // This parent is a pre-existing root-owned anchor. Creating it from
        // the app would leave an app-owned substitute that fails our pin.
        if (parent_fd_ < 0 || !parent_identity()) return false;
        root_fd_ = isshelf_badge::checked_at::openat(
            parent_fd_, job_directory_name,
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (root_fd_ < 0 && errno == ENOENT && create_if_missing) {
            if (isshelf_badge::checked_at::mkdirat(parent_fd_, job_directory_name, 0700) != 0 &&
                errno != EEXIST) return false;
            if (fsync(parent_fd_) != 0) return false;
            root_fd_ = isshelf_badge::checked_at::openat(
                parent_fd_, job_directory_name,
                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (root_fd_ < 0) return false;
        expected_owner_ = static_cast<std::uint64_t>(geteuid());
        return identity_fresh() && (!create_if_missing || fsync(root_fd_) == 0);
    }

    JobIo io() { return {this, entropy, read_file, create_file}; }
    int root_fd() const { return root_fd_; }
    bool acquire_send_lock() {
        if (!identity_fresh() || send_lock_fd_ >= 0 ||
            process_send_lock.test_and_set(std::memory_order_acquire)) return false;
        int fd = isshelf_badge::checked_at::openat(parent_fd_, send_lock_leaf,
            O_RDWR | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0 && errno == ENOENT)
            fd = isshelf_badge::checked_at::openat(parent_fd_, send_lock_leaf,
                O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) {
            process_send_lock.clear(std::memory_order_release);
            return false;
        }
        struct flock lock{};
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        lock.l_start = 0;
        lock.l_len = 0;
        if (!lock_identity(fd) || fsync(fd) != 0 || fsync(parent_fd_) != 0 ||
            fcntl(fd, F_SETLK, &lock) != 0 || !lock_identity(fd) ||
            !identity_fresh()) {
            if (close(fd) == 0)
                process_send_lock.clear(std::memory_order_release);
            return false;
        }
        send_lock_fd_ = fd;
        if (identity_fresh()) return true;
        if (close(send_lock_fd_) == 0)
            process_send_lock.clear(std::memory_order_release);
        send_lock_fd_ = -1;
        return false;
    }
    bool identity_fresh() const {
        return data_fd_ >= 0 && parent_fd_ >= 0 && root_fd_ >= 0 &&
               static_cast<std::uint64_t>(geteuid()) == expected_owner_ &&
               parent_identity() && child_identity() &&
               (send_lock_fd_ < 0 || lock_identity(send_lock_fd_));
    }

private:
    static DirectoryIdentity identity(const struct stat& info) {
        return {static_cast<std::uint64_t>(info.st_dev),
                static_cast<std::uint64_t>(info.st_ino),
                static_cast<std::uint64_t>(info.st_uid),
                static_cast<unsigned>(info.st_mode & 07777u),
                S_ISDIR(info.st_mode) != 0, S_ISLNK(info.st_mode) != 0};
    }
    bool parent_identity() const {
        if (data_fd_ < 0 || parent_fd_ < 0) return false;
        struct stat named{}, held{};
        return isshelf_badge::checked_at::fstatat(data_fd_, "console-shelf", &named,
                                                  AT_SYMLINK_NOFOLLOW) == 0 &&
               fstat(parent_fd_, &held) == 0 &&
               parent_identity_valid(identity(named), identity(held), expected_parent_owner_);
    }
    bool child_identity() const {
        if (parent_fd_ < 0 || root_fd_ < 0) return false;
        struct stat named{}, held{};
        return isshelf_badge::checked_at::fstatat(parent_fd_, job_directory_name,
                                                  &named, AT_SYMLINK_NOFOLLOW) == 0 &&
               fstat(root_fd_, &held) == 0 &&
               private_child_identity_valid(identity(named), identity(held),
                                            expected_owner_);
    }
    bool lock_identity(int fd) const {
        if (parent_fd_ < 0 || fd < 0) return false;
        struct stat parent{}, named{}, held{};
        return isshelf_badge::checked_at::fstatat(parent_fd_, send_lock_leaf,
                   &named, AT_SYMLINK_NOFOLLOW) == 0 &&
               fstat(parent_fd_, &parent) == 0 && fstat(fd, &held) == 0 &&
               S_ISREG(named.st_mode) && named.st_dev == parent.st_dev &&
               S_ISREG(held.st_mode) && named.st_dev == held.st_dev &&
               named.st_ino == held.st_ino && named.st_nlink == 1 &&
               held.st_nlink == 1 && named.st_size == 0 && held.st_size == 0 &&
               static_cast<std::uint64_t>(named.st_uid) == expected_owner_ &&
               static_cast<std::uint64_t>(held.st_uid) == expected_owner_ &&
               (named.st_mode & 07777u) == 0600u &&
               (held.st_mode & 07777u) == 0600u;
    }
    static FileStatus read_file(void* raw, const char* path, char* output,
                                std::size_t capacity, std::size_t& used) {
        used = 0;
        auto& store = *static_cast<PosixJobStore*>(raw);
        const char* leaf = owned_job_leaf(path);
        if (!store.identity_fresh() || !leaf || !output || !capacity) return FileStatus::error;
        const int fd = isshelf_badge::checked_at::openat(
            store.root_fd_, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            const bool missing = errno == ENOENT;
            return missing && store.identity_fresh() ? FileStatus::missing : FileStatus::error;
        }
        struct stat info{};
        const bool regular = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
                             info.st_size >= 0 && static_cast<std::uint64_t>(info.st_size) < capacity;
        if (!regular) { close(fd); return FileStatus::error; }
        const auto length = static_cast<std::size_t>(info.st_size);
        const auto count = ::read(fd, output, length);
        const bool closed = close(fd) == 0;
        if (count < 0 || static_cast<std::size_t>(count) != length || !closed ||
            !store.identity_fresh())
            return FileStatus::error;
        used = length;
        return FileStatus::present;
    }

    static bool create_file(void* raw, const char* path, std::string_view body) {
        auto& store = *static_cast<PosixJobStore*>(raw);
        const char* leaf = owned_job_leaf(path);
        if (!store.identity_fresh() || !leaf || body.empty() ||
            body.size() > maximum_receipt_bytes || fsync(store.root_fd_) != 0)
            return false;
        const int fd = isshelf_badge::checked_at::openat(
            store.root_fd_, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) return false;
        const bool written = ::write(fd, body.data(), body.size()) ==
                             static_cast<ssize_t>(body.size()) && fsync(fd) == 0;
        struct stat info{};
        const bool regular = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
                             info.st_size == static_cast<off_t>(body.size()) &&
                             static_cast<std::uint64_t>(info.st_uid) == store.expected_owner_ &&
                             (info.st_mode & 0777u) == 0600u;
        const bool closed = close(fd) == 0;
        const bool synced = fsync(store.root_fd_) == 0;
        // Failed writes remain consumed create-only attempts. Never unlink.
        return written && regular && closed && synced && store.identity_fresh();
    }

    static bool entropy(void*, unsigned char (&random)[16]) {
        const int fd = open("/dev/urandom", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return false;
        struct stat info{};
        const bool character = fstat(fd, &info) == 0 && S_ISCHR(info.st_mode);
        std::size_t offset = 0;
        while (character && offset < sizeof(random)) {
            const auto count = ::read(fd, random + offset, sizeof(random) - offset);
            if (count <= 0) break;
            offset += static_cast<std::size_t>(count);
        }
        return close(fd) == 0 && character && offset == sizeof(random);
    }

    int data_fd_ = -1;
    int parent_fd_ = -1;
    int root_fd_ = -1;
    int send_lock_fd_ = -1;
    std::uint64_t expected_owner_ = 0;
    const char* data_path_ = "/data";
    std::uint64_t expected_parent_owner_ = 0;
};

using PendingJobVisitor = bool (*)(void*, const char (&)[33]);
using LeaseVisitor = bool (*)(void*, std::size_t);
enum class SavedNameKind { request, sent, result, lease };
struct SavedName {
    SavedNameKind kind = SavedNameKind::request;
    char nonce[33]{};
    std::size_t system = system_count;
};
inline bool parse_saved_name(const char* name, SavedName& output) {
    if (!name) return false;
    std::size_t length = 0;
    while (length <= 80 && name[length]) ++length;
    if (length == 0 || length > 80) return false;
    struct Prefix { const char* text; JobFile file; SavedNameKind kind; };
    constexpr Prefix prefixes[] = {
        {"console-tile-job-", JobFile::request, SavedNameKind::request},
        {"console-tile-sent-", JobFile::sent, SavedNameKind::sent},
        {"console-tile-result-", JobFile::result, SavedNameKind::result},
    };
    for (const auto& prefix : prefixes) {
        const std::size_t bytes = std::strlen(prefix.text);
        if (length != bytes + 32 + 4 ||
            std::memcmp(name, prefix.text, bytes) != 0 ||
            std::memcmp(name + bytes + 32, ".txt", 4) != 0) continue;
        SavedName parsed;
        parsed.kind = prefix.kind;
        std::memcpy(parsed.nonce, name + bytes, 32);
        char path[maximum_job_path]{};
        const char* canonical = job_path(prefix.file, parsed.nonce, path)
            ? owned_job_leaf(path) : nullptr;
        if (!canonical || std::strcmp(canonical, name) != 0) return false;
        output = parsed;
        return true;
    }
    constexpr char lease_prefix[] = "console-tile-lease-";
    constexpr std::size_t lease_bytes = sizeof(lease_prefix) - 1;
    if (length != lease_bytes + 9 + 4 ||
        std::memcmp(name, lease_prefix, lease_bytes) != 0 ||
        std::memcmp(name + lease_bytes + 9, ".txt", 4) != 0) return false;
    for (std::size_t system = 0; system < system_count; ++system) {
        const char* title = owned_title_for(system);
        if (title && std::strlen(title) == 9 &&
            std::memcmp(name + lease_bytes, title, 9) == 0) {
            output.kind = SavedNameKind::lease;
            output.system = system;
            return true;
        }
    }
    return false;
}
inline bool each_saved_job(PosixJobStore& store, void* context, PendingJobVisitor visit,
                           LeaseVisitor lease_visit = nullptr) {
    if (!store.identity_fresh() || !visit) return false;
    const int fd = isshelf_badge::checked_at::openat(
        store.root_fd(), ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    DIR* directory = fdopendir(fd);
    if (!directory) { close(fd); return false; }
    std::size_t entries = 0;
    bool complete = true;
    for (;;) {
        errno = 0;
        auto* entry = readdir(directory);
        if (!entry) { if (errno != 0) complete = false; break; }
        if (++entries > 512) { complete = false; break; }
        const char* name = entry->d_name;
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) continue;
        SavedName parsed;
        if (!parse_saved_name(name, parsed)) {
            complete = false; break;
        }
        struct stat named{};
        if (isshelf_badge::checked_at::fstatat(store.root_fd(), name, &named,
                                               AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(named.st_mode) || named.st_nlink != 1 ||
            static_cast<std::uint64_t>(named.st_uid) !=
                static_cast<std::uint64_t>(geteuid()) ||
            (named.st_mode & 07777u) != 0600u) {
            complete = false; break;
        }
        if (parsed.kind == SavedNameKind::request) {
            if (!visit(context, parsed.nonce)) { complete = false; break; }
        } else if (parsed.kind == SavedNameKind::lease) {
            if (lease_visit && !lease_visit(context, parsed.system)) {
                complete = false; break;
            }
        } else {
            // A marker or receipt without its request must not be hidden by
            // a request-only scan before another send is admitted.
            char request_path[maximum_job_path]{};
            if (!job_path(JobFile::request, parsed.nonce, request_path)) {
                complete = false; break;
            }
            const char* request_leaf = owned_job_leaf(request_path);
            struct stat request_info{};
            if (!request_leaf || isshelf_badge::checked_at::fstatat(
                    store.root_fd(), request_leaf, &request_info, AT_SYMLINK_NOFOLLOW) != 0 ||
                !S_ISREG(request_info.st_mode)) {
                complete = false; break;
            }
        }
    }
    return closedir(directory) == 0 && complete && store.identity_fresh();
}

struct NewJobPreflight {
    PosixJobStore* store = nullptr;
    std::size_t system = system_count;
    EmulatorRouteCheck route_check = nullptr;
    const void* route_context = nullptr;
    bool completed[system_count]{};
    bool leases[system_count]{};

    static bool lease(void* raw, std::size_t system) {
        auto& scan = *static_cast<NewJobPreflight*>(raw);
        if (system >= system_count || scan.leases[system]) return false;
        scan.leases[system] = true;
        return true;
    }
    static bool visit(void* raw, const char (&nonce)[33]) {
        auto& scan = *static_cast<NewJobPreflight*>(raw);
        Dispatch prior;
        const auto io = scan.store->io();
        if (!prior.restore(io, nonce, scan.route_check, scan.route_context) ||
            prior.phase() != Phase::sent_unverified ||
            prior.request().action != Action::upsert) return false;
        Receipt receipt;
        if (prior.read_receipt(io, receipt, scan.route_check, scan.route_context) !=
                ReceiptStatus::accepted ||
            receipt.result != Result::upserted_source ||
            prior.request().intent.system == scan.system) return false;
        if (scan.completed[prior.request().intent.system]) return false;
        scan.completed[prior.request().intent.system] = true;
        return true;
    }
};

// The helper accepts one pending create at a time. A queued, unverified,
// malformed or same-title historical request must be resolved explicitly;
// another press may not manufacture a new nonce and consume a second send.
inline bool can_begin_new_upsert(PosixJobStore& store, std::size_t system,
                                 EmulatorRouteCheck route_check,
                                 const void* route_context) {
    if (system >= system_count || !store.identity_fresh()) return false;
    NewJobPreflight scan;
    scan.store = &store;
    scan.system = system;
    scan.route_check = route_check;
    scan.route_context = route_context;
    if (!each_saved_job(store, &scan, NewJobPreflight::visit,
                        NewJobPreflight::lease)) return false;
    for (std::size_t i = 0; i < system_count; ++i)
        if (scan.leases[i] && !scan.completed[i]) return false;
    return store.identity_fresh();
}
} // namespace shelf::tile_job
#endif
