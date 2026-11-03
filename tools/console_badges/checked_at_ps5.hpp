// SPDX-License-Identifier: GPL-3.0-only
// FreeBSD/PS5 *at syscall return adapter. The pinned SDK v0.42 libc.a wrappers
// return raw RAX and discard carry, so positive errno and positive file
// descriptors cannot be distinguished through those wrappers.
#pragma once

#if !defined(ISSHELF_PS1_WRITER_HOST_TEST) && !defined(ISSHELF_PS1_API_PROBE_HOST_TEST)
// The SDK libc++ errno.h otherwise supplies fallback values such as ENOENT=9968.
#include <sys/errno.h>
#endif
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if !defined(ISSHELF_PS1_WRITER_HOST_TEST) && !defined(ISSHELF_PS1_API_PROBE_HOST_TEST)
#include <sys/syscall.h>
#endif

namespace isshelf_badge::checked_at {
struct Return {
    long value;
    int error;
};

constexpr Return normalize(long raw, bool carry) {
    return {carry ? -1 : raw, carry ? static_cast<int>(raw) : 0};
}
static_assert(normalize(2, true).value == -1 && normalize(2, true).error == 2,
              "Positive ENOENT must be an error when carry is set");
static_assert(normalize(2, false).value == 2 && normalize(2, false).error == 0,
              "A successful descriptor 2 must remain a success");

#if !defined(ISSHELF_PS1_WRITER_HOST_TEST) && !defined(ISSHELF_PS1_API_PROBE_HOST_TEST)
static_assert(ENOENT == 2 && EEXIST == 17,
              "Pinned PS5 target errno values changed");
static_assert(SYS_fstatat == 493 && SYS_mkdirat == 496 && SYS_openat == 499 &&
              SYS_renameat == 501 && SYS_unlinkat == 503,
              "Pinned PS5 *at syscall numbers changed");

__attribute__((noinline)) inline Return raw(long number, long a1, long a2,
                                            long a3, long a4 = 0) {
    long value = number;
    long first = a1;
    long second = a2;
    long third = a3;
    bool carry = false;
    register long fourth __asm__("r10") = a4;
    // PS5's syscall entry may overwrite caller-saved argument registers. In
    // particular, a live path pointer in r8 was lost across openat. Tell the
    // optimizer that these registers do not survive the boundary.
    __asm__ volatile("syscall" : "+a"(value), "=@ccc"(carry),
                     "+D"(first), "+S"(second), "+d"(third), "+r"(fourth)
                     : : "rcx", "r8", "r9", "r11", "memory");
    return normalize(value, carry);
}

inline int checked(Return result) {
    if (result.error) errno = result.error;
    return static_cast<int>(result.value);
}
#endif

inline int fstatat(int dirfd, const char* path, struct stat* info, int flags) {
#if defined(ISSHELF_PS1_WRITER_HOST_TEST) || defined(ISSHELF_PS1_API_PROBE_HOST_TEST)
    return ::fstatat(dirfd, path, info, flags);
#else
    return checked(raw(SYS_fstatat, dirfd, reinterpret_cast<long>(path),
                       reinterpret_cast<long>(info), flags));
#endif
}
inline int mkdirat(int dirfd, const char* path, mode_t mode) {
#if defined(ISSHELF_PS1_WRITER_HOST_TEST) || defined(ISSHELF_PS1_API_PROBE_HOST_TEST)
    return ::mkdirat(dirfd, path, mode);
#else
    return checked(raw(SYS_mkdirat, dirfd, reinterpret_cast<long>(path), mode));
#endif
}
inline int openat(int dirfd, const char* path, int flags, mode_t mode = 0) {
#if defined(ISSHELF_PS1_WRITER_HOST_TEST) || defined(ISSHELF_PS1_API_PROBE_HOST_TEST)
    return ::openat(dirfd, path, flags, mode);
#else
    return checked(raw(SYS_openat, dirfd, reinterpret_cast<long>(path), flags, mode));
#endif
}
inline int renameat(int oldfd, const char* oldname, int newfd, const char* newname) {
#if defined(ISSHELF_PS1_WRITER_HOST_TEST) || defined(ISSHELF_PS1_API_PROBE_HOST_TEST)
    return ::renameat(oldfd, oldname, newfd, newname);
#else
    return checked(raw(SYS_renameat, oldfd, reinterpret_cast<long>(oldname),
                       newfd, reinterpret_cast<long>(newname)));
#endif
}
inline int unlinkat(int dirfd, const char* path, int flags) {
#if defined(ISSHELF_PS1_WRITER_HOST_TEST) || defined(ISSHELF_PS1_API_PROBE_HOST_TEST)
    return ::unlinkat(dirfd, path, flags);
#else
    return checked(raw(SYS_unlinkat, dirfd, reinterpret_cast<long>(path), flags));
#endif
}
} // namespace isshelf_badge::checked_at
