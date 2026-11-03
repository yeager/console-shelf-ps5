// SPDX-License-Identifier: GPL-3.0-only
// Version 2 read-only path capability diagnostic. This is a distinct one-shot
// from v1: it avoids libkernel_sys and records entry/failure in the SDK klog.
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef ISSHELF_PATH_PROBE_HOST_TEST
extern "C" int klog_puts(const char*);
extern "C" int klog_printf(const char*, ...);
#endif

namespace {
#ifdef ISSHELF_PATH_PROBE_HOST_TEST
constexpr char receipt_root[] = ISSHELF_PATH_PROBE_TEST_RECEIPT_ROOT;
constexpr char homebrew_root[] = ISSHELF_PATH_PROBE_TEST_HOMEBREW_ROOT;
constexpr char app_root[] = ISSHELF_PATH_PROBE_TEST_APP_ROOT;
constexpr char appmeta_root[] = ISSHELF_PATH_PROBE_TEST_APPMETA_ROOT;
#else
constexpr char receipt_root[] = "/data/console-shelf";
constexpr char homebrew_root[] = "/data/homebrew";
constexpr char app_root[] = "/user/app";
constexpr char appmeta_root[] = "/user/appmeta";
#endif
constexpr char request_name[] = "console-tile-path-request-v2.txt";
constexpr char request_header[] = "format=isshelf-console-tile-path-probe/2\nnonce=";
constexpr char receipt_prefix[] = "console-tile-path-v2-";
constexpr char receipt_suffix[] = ".txt";
constexpr std::size_t nonce_length = 32;
constexpr std::size_t max_directory_entries = 16384;

// 2 is intentionally not used by main. The SDK CRT can return 2 before main
// while loading a dependency; a missing MAIN_ENTERED line remains unknown.
int fail(int stage, int observed_errno) {
#ifndef ISSHELF_PATH_PROBE_HOST_TEST
    klog_printf("ISSHELF_PATH_PROBE_V2 stage=%d errno=%d\n", stage, observed_errno);
#else
    (void)observed_errno;
#endif
    return stage;
}

struct Candidate {
    const char* root;
    const char* title;
};
constexpr Candidate candidates[] = {
    {homebrew_root, "PPSA99541"},
    {homebrew_root, "PPSA99704"},
    {app_root, "PPSA99704"},
    {appmeta_root, "PPSA99704"},
    {homebrew_root, "PPSA99752"},
    {app_root, "PPSA99752"},
    {appmeta_root, "PPSA99752"},
};
constexpr const char* roots[] = {homebrew_root, app_root, appmeta_root};

struct PathObservation {
    int lstat_error = 0;
    int stat_error = 0;
    int open_error = 0;
    int fstat_error = 0;
    unsigned lstat_type = 0;
    unsigned stat_type = 0;
    unsigned fstat_type = 0;
    unsigned lstat_mode = 0;
    unsigned stat_mode = 0;
    unsigned fstat_mode = 0;
    unsigned same_lstat = 0;
};

struct RootObservation {
    PathObservation path;
    int dir_open_error = 0;
    int dir_read_error = 0;
    int dir_close_error = 0;
    unsigned entries_seen = 0;
    unsigned complete = 0;
    unsigned seen_99541 = 0;
    unsigned seen_99704 = 0;
    unsigned seen_99752 = 0;
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

PathObservation observe_path(const char* path) {
    PathObservation result;
    struct stat link_info{};
    struct stat target_info{};
    if (lstat(path, &link_info) != 0) result.lstat_error = errno;
    else {
        result.lstat_type = static_cast<unsigned>(link_info.st_mode & S_IFMT);
        result.lstat_mode = static_cast<unsigned>(link_info.st_mode & 07777);
    }
    if (stat(path, &target_info) != 0) result.stat_error = errno;
    else {
        result.stat_type = static_cast<unsigned>(target_info.st_mode & S_IFMT);
        result.stat_mode = static_cast<unsigned>(target_info.st_mode & 07777);
    }
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) result.open_error = errno;
    else {
        struct stat opened{};
        if (fstat(fd, &opened) != 0) result.fstat_error = errno;
        else {
            result.fstat_type = static_cast<unsigned>(opened.st_mode & S_IFMT);
            result.fstat_mode = static_cast<unsigned>(opened.st_mode & 07777);
            result.same_lstat = result.lstat_error == 0 &&
                opened.st_dev == link_info.st_dev && opened.st_ino == link_info.st_ino;
        }
        close(fd);
    }
    return result;
}

RootObservation observe_root(const char* path) {
    RootObservation result;
    result.path = observe_path(path);
    const int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        result.dir_open_error = errno;
        return result;
    }
    DIR* dir = fdopendir(fd);
    if (!dir) {
        result.dir_open_error = errno;
        close(fd);
        return result;
    }
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(dir);
        if (!entry) {
            result.dir_read_error = errno;
            result.complete = result.dir_read_error == 0;
            break;
        }
        if (result.entries_seen == max_directory_entries) break;
        ++result.entries_seen;
        result.seen_99541 |= std::strcmp(entry->d_name, "PPSA99541") == 0;
        result.seen_99704 |= std::strcmp(entry->d_name, "PPSA99704") == 0;
        result.seen_99752 |= std::strcmp(entry->d_name, "PPSA99752") == 0;
    }
    if (closedir(dir) != 0) result.dir_close_error = errno;
    return result;
}

bool read_nonce(char nonce[nonce_length + 1], int& observed_errno) {
    char request_path[512]{};
    const int path_size = std::snprintf(request_path, sizeof(request_path), "%s/%s",
                                        receipt_root, request_name);
    if (path_size <= 0 || static_cast<std::size_t>(path_size) >= sizeof(request_path)) {
        observed_errno = ENAMETOOLONG;
        return false;
    }
    const int fd = open(request_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
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
    const bool closed = close(fd) == 0;
    const int close_error = closed ? 0 : errno;
    if (!regular || count != static_cast<ssize_t>(sizeof(bytes)) || !closed ||
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
    return true;
}

bool append_path(Buffer& body, const char* label, const char* path, const PathObservation& value) {
    return body.append(
        "%s=%s\n%s_lstat_errno=%d\n%s_lstat_type=%u\n%s_lstat_mode=%o\n"
        "%s_stat_errno=%d\n%s_stat_type=%u\n%s_stat_mode=%o\n"
        "%s_open_errno=%d\n%s_fstat_errno=%d\n%s_fstat_type=%u\n"
        "%s_fstat_mode=%o\n%s_fstat_same_lstat=%u\n",
        label, path, label, value.lstat_error, label, value.lstat_type,
        label, value.lstat_mode, label, value.stat_error, label, value.stat_type,
        label, value.stat_mode, label, value.open_error, label, value.fstat_error,
        label, value.fstat_type, label, value.fstat_mode, label, value.same_lstat);
}

bool write_all(int fd, const char* bytes, std::size_t count) {
    while (count) {
        const auto written = write(fd, bytes, count);
        if (written <= 0) return false;
        bytes += written;
        count -= static_cast<std::size_t>(written);
    }
    return true;
}
} // namespace

int main() {
#ifndef ISSHELF_PATH_PROBE_HOST_TEST
    klog_puts("ISSHELF_PATH_PROBE_V2_MAIN_ENTERED");
#endif
    // The fixed output directory must already exist. Only the one receipt is created.
    const int root_fd = open(receipt_root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root_fd < 0) return fail(21, errno);
    struct stat output_info{};
    const bool root_stat_ok = fstat(root_fd, &output_info) == 0;
    if (!root_stat_ok || !S_ISDIR(output_info.st_mode)) {
        const int error = root_stat_ok ? ENOTDIR : errno;
        close(root_fd);
        return fail(22, error);
    }
    char nonce[nonce_length + 1]{};
    int request_error = 0;
    if (!read_nonce(nonce, request_error)) {
        close(root_fd);
        return fail(23, request_error);
    }
    Buffer body;
    if (!body.append("format=isshelf-console-tile-path-probe/2\nnonce=%s\n"
                     "runtime=standard-sdk-v0.42\nrequest_unlinked=0\n", nonce)) {
        close(root_fd);
        return fail(24, ENOBUFS);
    }
    for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]); ++i) {
        const auto observed = observe_root(roots[i]);
        char label[16]{};
        if (std::snprintf(label, sizeof(label), "root_%u", i) <= 0 ||
            !append_path(body, label, roots[i], observed.path) ||
            !body.append("%s_dir_open_errno=%d\n%s_dir_read_errno=%d\n"
                         "%s_dir_close_errno=%d\n%s_entries_seen=%u\n%s_complete=%u\n"
                         "%s_seen_PPSA99541=%u\n%s_seen_PPSA99704=%u\n%s_seen_PPSA99752=%u\n",
                         label, observed.dir_open_error, label, observed.dir_read_error,
                         label, observed.dir_close_error, label, observed.entries_seen,
                         label, observed.complete, label, observed.seen_99541,
                         label, observed.seen_99704, label, observed.seen_99752)) {
            close(root_fd);
            return fail(25, ENOBUFS);
        }
    }
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        char path[128]{};
        char label[16]{};
        const int path_size = std::snprintf(path, sizeof(path), "%s/%s",
                                            candidates[i].root, candidates[i].title);
        if (path_size <= 0 || static_cast<std::size_t>(path_size) >= sizeof(path) ||
            std::snprintf(label, sizeof(label), "path_%u", i) <= 0 ||
            !append_path(body, label, path, observe_path(path))) {
            close(root_fd);
            return fail(26, ENOBUFS);
        }
    }
    if (!body.append("title_root_write_attempted=0\ndatabase_open_attempted=0\n"
                     "database_write_attempted=0\nnetwork_attempted=0\nrecord_complete=1\n")) {
        close(root_fd);
        return fail(27, ENOBUFS);
    }
    char filename[96]{};
    const int name_size = std::snprintf(filename, sizeof(filename), "%s%s%s",
                                         receipt_prefix, nonce, receipt_suffix);
    if (name_size <= 0 || static_cast<std::size_t>(name_size) >= sizeof(filename)) {
        close(root_fd);
        return fail(28, ENAMETOOLONG);
    }
    char receipt_path[512]{};
    const int path_size = std::snprintf(receipt_path, sizeof(receipt_path), "%s/%s",
                                        receipt_root, filename);
    if (path_size <= 0 || static_cast<std::size_t>(path_size) >= sizeof(receipt_path)) {
        close(root_fd);
        return fail(28, ENAMETOOLONG);
    }
    const int receipt_fd = open(receipt_path,
                                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (receipt_fd < 0) {
        const int error = errno;
        close(root_fd);
        return fail(29, error);
    }
    if (fchmod(receipt_fd, 0644) != 0) {
        const int error = errno;
        close(receipt_fd);
        close(root_fd);
        return fail(30, error);
    }
    const bool written = write_all(receipt_fd, body.bytes, body.used);
    const int write_error = written ? 0 : errno;
    const bool file_synced = written && fsync(receipt_fd) == 0;
    const int file_sync_error = file_synced ? 0 : errno;
    const bool file_closed = close(receipt_fd) == 0;
    const int file_close_error = file_closed ? 0 : errno;
    const bool directory_synced = fsync(root_fd) == 0;
    const int directory_sync_error = directory_synced ? 0 : errno;
    const bool root_closed = close(root_fd) == 0;
    const int root_close_error = root_closed ? 0 : errno;
    // A failed/incomplete receipt remains in place for inspection; no file is overwritten.
    if (!written) return fail(31, write_error);
    if (!file_synced) return fail(32, file_sync_error);
    if (!file_closed) return fail(33, file_close_error);
    if (!directory_synced) return fail(34, directory_sync_error);
    if (!root_closed) return fail(35, root_close_error);
#ifndef ISSHELF_PATH_PROBE_HOST_TEST
    klog_puts("ISSHELF_PATH_PROBE_V2_RECORD_WRITTEN");
#endif
    return 0;
}
