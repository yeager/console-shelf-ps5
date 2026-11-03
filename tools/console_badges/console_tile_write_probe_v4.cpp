// SPDX-License-Identifier: GPL-3.0-only
// One-shot write-capability diagnostic. Never creates a title or touches a title DB.
// The pinned SDK's libc++ errno.h has a fallback ENOENT=9968. Load the target
// FreeBSD errno constants first, then refuse to build if that ordering drifts.
#include <sys/errno.h>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

static_assert(ENOENT == 2 && EEXIST == 17 && EIO == 5 && EINVAL == 22,
              "PS5 target errno constants differ from the pinned kernel ABI");

#ifndef ISSHELF_WRITE_PROBE_HOST_TEST
extern "C" int klog_puts(const char*);
extern "C" int klog_printf(const char*, ...);
#endif

namespace {
#ifdef ISSHELF_WRITE_PROBE_HOST_TEST
constexpr char receipt_root[] = ISSHELF_WRITE_PROBE_TEST_RECEIPT_ROOT;
constexpr char homebrew_root[] = ISSHELF_WRITE_PROBE_TEST_HOMEBREW_ROOT;
#else
constexpr char receipt_root[] = "/data/console-shelf";
constexpr char homebrew_root[] = "/data/homebrew";
#endif
constexpr char request_name[] = "console-tile-write-request-v4.txt";
constexpr char request_header[] = "format=isshelf-console-tile-write-probe/4\nnonce=";
constexpr char receipt_prefix[] = "console-tile-write-v4-";
constexpr char scratch_prefix[] = ".isshelf-write-probe-v4-";
constexpr char temp_name[] = "probe.tmp";
constexpr char final_name[] = "probe.dat";
constexpr std::size_t nonce_length = 32;

struct AtReturn { long value; int error; bool carry; };
constexpr AtReturn normalize_at(long raw, bool carry) {
    return {carry ? -1 : raw, carry ? static_cast<int>(raw) : 0, carry};
}
static_assert(normalize_at(2, true).value == -1 && normalize_at(2, true).error == ENOENT);
static_assert(normalize_at(2, false).value == 2 && normalize_at(2, false).error == 0);

#ifndef ISSHELF_WRITE_PROBE_HOST_TEST
static_assert(SYS_fstatat == 493 && SYS_mkdirat == 496 && SYS_openat == 499 &&
              SYS_renameat == 501 && SYS_unlinkat == 503,
              "Pinned PS5 raw at-syscall numbers changed");

// The SDK libc.a *at wrappers only execute `syscall; ret`. On PS5 FreeBSD an
// error leaves a positive errno in RAX and sets carry. Capture carry in the
// same asm block so an openat fd of 2 is never mistaken for ENOENT.
AtReturn raw_at(long number, long a1, long a2, long a3, long a4 = 0) {
    long value = number;
    bool carry = false;
    register long fourth __asm__("r10") = a4;
    __asm__ volatile("syscall" : "+a"(value), "=@ccc"(carry)
                     : "D"(a1), "S"(a2), "d"(a3), "r"(fourth)
                     : "rcx", "r11", "memory");
    return normalize_at(value, carry);
}
#endif

int checked_at(AtReturn value) {
    if (value.error) errno = value.error;
    return static_cast<int>(value.value);
}

int checked_fstatat(int fd, const char* path, struct stat* info, int flags) {
#ifdef ISSHELF_WRITE_PROBE_HOST_TEST
    const int result = ::fstatat(fd, path, info, flags);
    return checked_at(normalize_at(result < 0 ? errno : result, result < 0));
#else
    return checked_at(raw_at(SYS_fstatat, fd, reinterpret_cast<long>(path),
                             reinterpret_cast<long>(info), flags));
#endif
}
int checked_mkdirat(int fd, const char* path, mode_t mode) {
#ifdef ISSHELF_WRITE_PROBE_HOST_TEST
    const int result = ::mkdirat(fd, path, mode);
    return checked_at(normalize_at(result < 0 ? errno : result, result < 0));
#else
    return checked_at(raw_at(SYS_mkdirat, fd, reinterpret_cast<long>(path), mode));
#endif
}
int checked_openat(int fd, const char* path, int flags, mode_t mode = 0) {
#ifdef ISSHELF_WRITE_PROBE_HOST_TEST
    const int result = ::openat(fd, path, flags, mode);
    return checked_at(normalize_at(result < 0 ? errno : result, result < 0));
#else
    return checked_at(raw_at(SYS_openat, fd, reinterpret_cast<long>(path), flags, mode));
#endif
}
int checked_renameat(int old_fd, const char* old_name, int new_fd, const char* new_name) {
#ifdef ISSHELF_WRITE_PROBE_HOST_TEST
    const int result = ::renameat(old_fd, old_name, new_fd, new_name);
    return checked_at(normalize_at(result < 0 ? errno : result, result < 0));
#else
    return checked_at(raw_at(SYS_renameat, old_fd, reinterpret_cast<long>(old_name),
                             new_fd, reinterpret_cast<long>(new_name)));
#endif
}
int checked_unlinkat(int fd, const char* path, int flags) {
#ifdef ISSHELF_WRITE_PROBE_HOST_TEST
    const int result = ::unlinkat(fd, path, flags);
    return checked_at(normalize_at(result < 0 ? errno : result, result < 0));
#else
    return checked_at(raw_at(SYS_unlinkat, fd, reinterpret_cast<long>(path), flags));
#endif
}

enum Stage {
    receipt_root_open, request_read, receipt_reserve, receipt_mode,
    receipt_header_write, receipt_header_sync, homebrew_open, homebrew_stat,
    scratch_absence, scratch_mkdirat, scratch_openat, scratch_identity,
    scratch_private_mode, file_openat, file_write, file_fsync,
    file_seek, file_readback, file_close, renameat_op, directory_fsync,
    final_openat, final_readback, final_close, file_unlinkat,
    directory_resync, scratch_close, scratch_cleanup_identity, scratch_unlinkat,
    homebrew_fsync, homebrew_close, remaining_lstat, receipt_body_write,
    receipt_body_fsync, receipt_close, receipt_root_fsync, receipt_root_close,
    stage_count
};
constexpr const char* stage_names[] = {
    "receipt_root_open", "request_read", "receipt_reserve", "receipt_mode",
    "receipt_header_write", "receipt_header_sync", "homebrew_open", "homebrew_stat",
    "scratch_absence", "scratch_mkdirat", "scratch_openat", "scratch_identity",
    "scratch_private_mode", "file_openat", "file_write", "file_fsync",
    "file_seek", "file_readback", "file_close", "renameat",
    "directory_fsync", "final_openat", "final_readback", "final_close",
    "file_unlinkat", "directory_resync", "scratch_close", "scratch_cleanup_identity",
    "scratch_unlinkat", "homebrew_fsync", "homebrew_close", "remaining_lstat",
    "receipt_body_write", "receipt_body_fsync", "receipt_close",
    "receipt_root_fsync", "receipt_root_close"
};
static_assert(sizeof(stage_names) / sizeof(stage_names[0]) == stage_count);

struct Results {
    int errors[stage_count]{};
    int first_failure = -1;
    int first_errno = 0;
    Results() { for (int& error : errors) error = -1; }
    void set(Stage stage, int error) {
        errors[stage] = error;
        if (error && first_failure < 0) {
            first_failure = stage;
            first_errno = error;
#ifndef ISSHELF_WRITE_PROBE_HOST_TEST
            klog_printf("ISSHELF_WRITE_PROBE_V4 stage=%s errno=%d\n", stage_names[stage], error);
#endif
        }
    }
};

struct Buffer {
    char bytes[8192]{};
    std::size_t used = 0;
    bool append(const char* format, ...) {
        if (used >= sizeof(bytes)) return false;
        va_list args;
        va_start(args, format);
        const int count = std::vsnprintf(bytes + used, sizeof(bytes) - used, format, args);
        va_end(args);
        if (count < 0 || static_cast<std::size_t>(count) >= sizeof(bytes) - used) return false;
        used += static_cast<std::size_t>(count);
        return true;
    }
};

bool write_all(int fd, const char* bytes, std::size_t count, int& observed_errno) {
    while (count) {
        const auto written = write(fd, bytes, count);
        if (written <= 0) {
            observed_errno = written < 0 ? errno : EIO;
            return false;
        }
        bytes += written;
        count -= static_cast<std::size_t>(written);
    }
    observed_errno = 0;
    return true;
}

bool make_path(char* out, std::size_t capacity, const char* root, const char* name) {
    const int count = std::snprintf(out, capacity, "%s/%s", root, name);
    return count > 0 && static_cast<std::size_t>(count) < capacity;
}

bool read_nonce(char nonce[nonce_length + 1], int& observed_errno) {
    char path[512]{};
    if (!make_path(path, sizeof(path), receipt_root, request_name)) {
        observed_errno = ENAMETOOLONG;
        return false;
    }
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { observed_errno = errno; return false; }
    struct stat info{};
    char bytes[sizeof(request_header) - 1 + nonce_length + 1]{};
    const bool stat_ok = fstat(fd, &info) == 0;
    const int stat_error = stat_ok ? 0 : errno;
    const bool regular = stat_ok && S_ISREG(info.st_mode) &&
                         info.st_size == static_cast<off_t>(sizeof(bytes)) &&
                         (info.st_mode & 0004) != 0;
    const auto count = regular ? read(fd, bytes, sizeof(bytes)) : -1;
    const int read_error = regular && count < 0 ? errno : 0;
    const int close_error = close(fd) == 0 ? 0 : errno;
    if (!regular || count != static_cast<ssize_t>(sizeof(bytes)) || close_error ||
        std::memcmp(bytes, request_header, sizeof(request_header) - 1) != 0 ||
        bytes[sizeof(bytes) - 1] != '\n') {
        observed_errno = stat_error ? stat_error : read_error ? read_error :
                         close_error ? close_error : EINVAL;
        return false;
    }
    for (std::size_t i = 0; i < nonce_length; ++i) {
        const char digit = bytes[sizeof(request_header) - 1 + i];
        if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'))) {
            observed_errno = EINVAL;
            return false;
        }
        nonce[i] = digit;
    }
    nonce[nonce_length] = '\0';
    observed_errno = 0;
    return true;
}

bool read_exact(int fd, const char* expected, std::size_t size, int& observed_errno) {
    char found[128]{};
    if (size > sizeof(found)) { observed_errno = EINVAL; return false; }
    std::size_t used = 0;
    while (used < size) {
        const auto count = read(fd, found + used, size - used);
        if (count <= 0) {
            observed_errno = count < 0 ? errno : EIO;
            return false;
        }
        used += static_cast<std::size_t>(count);
    }
    char extra = 0;
    const auto tail = read(fd, &extra, 1);
    if (tail < 0) { observed_errno = errno; return false; }
    if (tail != 0 || std::memcmp(found, expected, size) != 0) {
        observed_errno = EIO;
        return false;
    }
    observed_errno = 0;
    return true;
}

int early_fail(Stage stage, int error) {
#ifndef ISSHELF_WRITE_PROBE_HOST_TEST
    klog_printf("ISSHELF_WRITE_PROBE_V4 stage=%s errno=%d\n", stage_names[stage], error);
#else
    (void)error;
#endif
    return 40 + static_cast<int>(stage);
}
} // namespace

int main() {
#ifndef ISSHELF_WRITE_PROBE_HOST_TEST
    klog_puts("ISSHELF_WRITE_PROBE_V4_MAIN_ENTERED");
#endif
    // The request and output root are pre-existing, exactly as with read-only v2.
    const int root_fd = open(receipt_root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root_fd < 0) return early_fail(receipt_root_open, errno);
    struct stat root_info{};
    if (fstat(root_fd, &root_info) != 0 || !S_ISDIR(root_info.st_mode)) {
        const int error = errno ? errno : ENOTDIR;
        close(root_fd);
        return early_fail(receipt_root_open, error);
    }
    char nonce[nonce_length + 1]{};
    int error = 0;
    if (!read_nonce(nonce, error)) {
        close(root_fd);
        return early_fail(request_read, error);
    }
    char receipt_name[96]{};
    char scratch_name[96]{};
    char receipt_path[512]{};
    char scratch_path[512]{};
    if (std::snprintf(receipt_name, sizeof(receipt_name), "%s%s.txt", receipt_prefix, nonce) <= 0 ||
        std::snprintf(scratch_name, sizeof(scratch_name), "%s%s", scratch_prefix, nonce) <= 0 ||
        !make_path(receipt_path, sizeof(receipt_path), receipt_root, receipt_name) ||
        !make_path(scratch_path, sizeof(scratch_path), homebrew_root, scratch_name)) {
        close(root_fd);
        return early_fail(receipt_reserve, ENAMETOOLONG);
    }
    const int receipt_fd = open(receipt_path,
                                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (receipt_fd < 0) {
        const int saved = errno;
        close(root_fd);
        return early_fail(receipt_reserve, saved);
    }
    Results result;
    result.set(receipt_root_open, 0);
    result.set(request_read, 0);
    result.set(receipt_reserve, 0);
    result.set(receipt_mode, fchmod(receipt_fd, 0644) == 0 ? 0 : errno);
    char header[256]{};
    const int header_size = std::snprintf(header, sizeof(header),
        "format=isshelf-console-tile-write-probe/4\nnonce=%s\nruntime=standard-sdk-v0.42\n",
        nonce);
    if (header_size <= 0 || static_cast<std::size_t>(header_size) >= sizeof(header)) {
        result.set(receipt_header_write, ENOBUFS);
    } else {
        write_all(receipt_fd, header, static_cast<std::size_t>(header_size), error);
        result.set(receipt_header_write, error);
    }
    result.set(receipt_header_sync, fsync(receipt_fd) == 0 ? 0 : errno);
    int homebrew_fd = -1;
    int scratch_fd = -1;
    int file_fd = -1;
    bool owned_scratch = false;
    bool temp_created = false;
    bool renamed = false;
    bool main_success = result.first_failure < 0;
    struct stat owned_info{};
    char payload[96]{};
    const int payload_size = std::snprintf(payload, sizeof(payload), "isshelf-write-probe-v4:%s\n", nonce);
    if (payload_size <= 0 || static_cast<std::size_t>(payload_size) >= sizeof(payload)) {
        main_success = false;
    }
    if (main_success) {
        homebrew_fd = open(homebrew_root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        result.set(homebrew_open, homebrew_fd >= 0 ? 0 : errno);
        main_success = homebrew_fd >= 0;
    }
    struct stat homebrew_info{};
    if (main_success) {
        const bool okay = fstat(homebrew_fd, &homebrew_info) == 0 && S_ISDIR(homebrew_info.st_mode);
        result.set(homebrew_stat, okay ? 0 : errno ? errno : ENOTDIR);
        main_success = okay;
    }
    if (main_success) {
        struct stat prior{};
        const int check = checked_fstatat(homebrew_fd, scratch_name, &prior, AT_SYMLINK_NOFOLLOW);
        const int saved = check == 0 ? EEXIST : errno;
        result.set(scratch_absence, check < 0 && saved == ENOENT ? 0 : saved);
        main_success = check < 0 && saved == ENOENT;
    }
    if (main_success) {
        const int made = checked_mkdirat(homebrew_fd, scratch_name, 0700);
        result.set(scratch_mkdirat, made == 0 ? 0 : errno);
        owned_scratch = made == 0;
        main_success = owned_scratch;
    }
    if (main_success) {
        scratch_fd = checked_openat(homebrew_fd, scratch_name,
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        result.set(scratch_openat, scratch_fd >= 0 ? 0 : errno);
        main_success = scratch_fd >= 0;
    }
    if (main_success) {
        struct stat opened{};
        struct stat named{};
        const bool opened_ok = fstat(scratch_fd, &opened) == 0;
        const int opened_error = opened_ok ? 0 : errno;
        const bool named_ok = checked_fstatat(homebrew_fd, scratch_name, &named,
                                     AT_SYMLINK_NOFOLLOW) == 0;
        const int named_error = named_ok ? 0 : errno;
        const bool identity = opened_ok && named_ok && S_ISDIR(opened.st_mode) &&
                              S_ISDIR(named.st_mode) && opened.st_dev == named.st_dev &&
                              opened.st_ino == named.st_ino;
        result.set(scratch_identity, identity ? 0 :
                   opened_error ? opened_error : named_error ? named_error : ESTALE);
        if (identity) owned_info = opened;
        main_success = identity;
    }
    if (main_success) {
        const bool private_mode = fchmod(scratch_fd, 0700) == 0;
        result.set(scratch_private_mode, private_mode ? 0 : errno);
        main_success = private_mode;
    }
    if (main_success) {
        file_fd = checked_openat(scratch_fd, temp_name,
                         O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        result.set(file_openat, file_fd >= 0 ? 0 : errno);
        temp_created = file_fd >= 0;
        main_success = temp_created;
    }
    if (main_success) {
#ifdef ISSHELF_WRITE_PROBE_TEST_FAIL_AFTER_TEMP
        error = EIO;
#else
        write_all(file_fd, payload, static_cast<std::size_t>(payload_size), error);
#endif
        result.set(file_write, error);
        main_success = error == 0;
    }
    if (main_success) {
        const int synced = fsync(file_fd);
        result.set(file_fsync, synced == 0 ? 0 : errno);
        main_success = synced == 0;
    }
    if (main_success) {
        const bool sought = lseek(file_fd, 0, SEEK_SET) == 0;
        result.set(file_seek, sought ? 0 : errno);
        main_success = sought;
    }
    if (main_success) {
        main_success = read_exact(file_fd, payload, static_cast<std::size_t>(payload_size), error);
        result.set(file_readback, error);
    }
    if (file_fd >= 0) {
        const int closed = close(file_fd);
        result.set(file_close, closed == 0 ? 0 : errno);
        main_success = main_success && closed == 0;
        file_fd = -1;
    }
    if (main_success) {
        const int moved = checked_renameat(scratch_fd, temp_name, scratch_fd, final_name);
        result.set(renameat_op, moved == 0 ? 0 : errno);
        renamed = moved == 0;
        main_success = renamed;
    }
    if (main_success) {
        const int synced = fsync(scratch_fd);
        result.set(directory_fsync, synced == 0 ? 0 : errno);
        main_success = synced == 0;
    }
    if (main_success) {
        const int final_fd = checked_openat(scratch_fd, final_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        result.set(final_openat, final_fd >= 0 ? 0 : errno);
        main_success = final_fd >= 0;
        if (final_fd >= 0) {
            if (main_success) {
                main_success = read_exact(final_fd, payload,
                                          static_cast<std::size_t>(payload_size), error);
                result.set(final_readback, error);
            }
            const int closed = close(final_fd);
            result.set(final_close, closed == 0 ? 0 : errno);
            main_success = main_success && closed == 0;
        }
    }
    // The scratch folder is private and was reserved exclusively. If any stage
    // fails, cleanup uses only the held directory handles. A failed cleanup
    // leaves an inert dot-prefixed folder for explicit inspection.
    if (scratch_fd >= 0) {
        if (temp_created || renamed) {
            const int removed = checked_unlinkat(scratch_fd, renamed ? final_name : temp_name, 0);
            result.set(file_unlinkat, removed == 0 ? 0 : errno);
            main_success = main_success && removed == 0;
        }
        const int synced = fsync(scratch_fd);
        result.set(directory_resync, synced == 0 ? 0 : errno);
        main_success = main_success && synced == 0;
        const int closed = close(scratch_fd);
        result.set(scratch_close, closed == 0 ? 0 : errno);
        main_success = main_success && closed == 0;
        scratch_fd = -1;
    }
    if (owned_scratch && homebrew_fd >= 0 && result.errors[scratch_identity] == 0) {
        struct stat named{};
        const int checked = checked_fstatat(homebrew_fd, scratch_name, &named, AT_SYMLINK_NOFOLLOW);
        const int check_error = checked == 0 && S_ISDIR(named.st_mode) &&
                                named.st_dev == owned_info.st_dev &&
                                named.st_ino == owned_info.st_ino ? 0 :
                                checked < 0 ? errno : ESTALE;
        result.set(scratch_cleanup_identity, check_error);
        if (check_error == 0) {
            const int removed = checked_unlinkat(homebrew_fd, scratch_name, AT_REMOVEDIR);
            result.set(scratch_unlinkat, removed == 0 ? 0 : errno);
            main_success = main_success && removed == 0;
        } else {
            main_success = false;
        }
    }
    if (homebrew_fd >= 0) {
        const int synced = fsync(homebrew_fd);
        result.set(homebrew_fsync, synced == 0 ? 0 : errno);
        main_success = main_success && synced == 0;
        const int closed = close(homebrew_fd);
        result.set(homebrew_close, closed == 0 ? 0 : errno);
        main_success = main_success && closed == 0;
    }
    struct stat remaining{};
    const int remaining_check = lstat(scratch_path, &remaining);
    const int remaining_error = remaining_check == 0 ? 0 : errno;
    result.set(remaining_lstat, remaining_check == 0 ? EEXIST :
               remaining_error == ENOENT ? 0 : remaining_error);
    const char* remaining_status = remaining_check == 0 ? "present" :
                                   remaining_error == ENOENT ? "absent" : "unknown";
    if (remaining_check == 0 || remaining_error != ENOENT) main_success = false;
    Buffer body;
    bool body_ok = body.append("scratch=%s\nscratch_remaining=%s\n"
                               "scratch_remaining_errno=%d\n"
                               "valid_title_id_write_attempted=0\n"
                               "homebrew_scratch_write_attempted=%d\n"
                               "homebrew_scratch_created=%d\n"
                               "database_open_attempted=0\nnetwork_attempted=0\n",
                               scratch_path, remaining_status, remaining_error,
                               result.errors[scratch_mkdirat] == -1 ? 0 : 1,
                               owned_scratch ? 1 : 0);
    // Receipt completion stages happen after this serialization. They are
    // represented by process exit and klog, never falsely pre-filled as pass.
    for (int i = 0; i < receipt_body_write; ++i) {
        body_ok = body_ok && body.append("stage_%s_errno=%d\n", stage_names[i], result.errors[i]);
    }
    body_ok = body_ok && body.append("first_failure=%s\nfirst_errno=%d\n"
                                     "transaction_complete=%d\nrecord_complete=1\n",
                                     result.first_failure < 0 ? "none" : stage_names[result.first_failure],
                                     result.first_errno, main_success ? 1 : 0);
    if (!body_ok) {
        result.set(receipt_body_write, ENOBUFS);
    } else {
        write_all(receipt_fd, body.bytes, body.used, error);
        result.set(receipt_body_write, error);
    }
    result.set(receipt_body_fsync, fsync(receipt_fd) == 0 ? 0 : errno);
    result.set(receipt_close, close(receipt_fd) == 0 ? 0 : errno);
    result.set(receipt_root_fsync, fsync(root_fd) == 0 ? 0 : errno);
    result.set(receipt_root_close, close(root_fd) == 0 ? 0 : errno);
#ifndef ISSHELF_WRITE_PROBE_HOST_TEST
    if (result.first_failure < 0 && main_success)
        klog_puts("ISSHELF_WRITE_PROBE_V4_RECORD_WRITTEN");
#endif
    return result.first_failure < 0 && main_success ? 0 :
           40 + (result.first_failure < 0 ? static_cast<int>(remaining_lstat) : result.first_failure);
}
