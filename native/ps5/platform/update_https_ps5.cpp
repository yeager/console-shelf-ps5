// PS5 HTTPS platform for isShelf's updater.
// Adapted from PS5SX2's fe_https_ps5.cpp and installer net.c.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fe_https.h"

#include <atomic>
#include <cerrno>
#include <cpuid.h>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

extern "C" {
int sceNetResolverCreate(const char* name, int memid, int flags);
int sceNetResolverStartNtoa(int rid, const char* hostname, struct in_addr* addr,
                            int timeout, int retry, int flags);
int sceNetResolverDestroy(int rid);
}

namespace fe {
namespace {
std::string hex(int value) {
    char output[16]{};
    std::snprintf(output, sizeof(output), "%#x", static_cast<unsigned>(value));
    return output;
}

bool resolve(int pool, const std::string& host, in_addr& address, std::string& error) {
    if (::inet_pton(AF_INET, host.c_str(), &address) == 1) return true;
    const int resolver = sceNetResolverCreate("isshelf-https", pool, 0);
    if (resolver < 0) {
        error = "no DNS resolver (" + hex(resolver) + ")";
        return false;
    }
    const int result = sceNetResolverStartNtoa(resolver, host.c_str(), &address,
                                               0, 0, 0);
    (void)sceNetResolverDestroy(resolver);
    if (result < 0) {
        error = "can't find " + host + " (DNS " + hex(result) + ")";
        return false;
    }
    return true;
}

int connect_tcp(int pool, const std::string& host, std::uint16_t port,
                int connect_timeout_ms, int io_timeout_ms, std::string& error) {
    in_addr address{};
    if (!resolve(pool, host, address, error)) return -1;
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) {
        error = "no socket (errno " + std::to_string(errno) + ")";
        return -1;
    }
#ifdef SO_NOSIGPIPE
    const int no_sigpipe = 1;
    (void)::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe,
                      sizeof(no_sigpipe));
#endif
    const int receive_buffer = 1024 * 1024;
    (void)::setsockopt(descriptor, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                      sizeof(receive_buffer));
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(port);
    destination.sin_addr = address;
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0) {
        const int socket_error = errno;
        ::close(descriptor);
        error = "socket setup failed for " + host + " (errno " +
                std::to_string(socket_error) + ")";
        return -1;
    }
    int result = ::connect(descriptor, reinterpret_cast<const sockaddr*>(&destination),
                           sizeof(destination));
    if (result != 0 && errno == EINPROGRESS) {
        pollfd event{descriptor, POLLOUT, 0};
        do {
            result = ::poll(&event, 1, connect_timeout_ms);
        } while (result < 0 && errno == EINTR);
        if (result == 1) {
            int socket_error = 0;
            socklen_t length = sizeof(socket_error);
            result = ::getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socket_error,
                                  &length) == 0 && socket_error == 0 ? 0 : -1;
            if (socket_error) errno = socket_error;
        } else {
            if (result == 0) errno = ETIMEDOUT;
            result = -1;
        }
    }
    if (result != 0 || ::fcntl(descriptor, F_SETFL, flags & ~O_NONBLOCK) != 0) {
        const int socket_error = errno;
        ::close(descriptor);
        char ip[INET_ADDRSTRLEN]{};
        (void)::inet_ntop(AF_INET, &address, ip, sizeof(ip));
        error = "no connection to " + host + " (" + ip + ":" +
                std::to_string(port) + ", errno " +
                std::to_string(socket_error) + ")";
        return -1;
    }
    timeval timeout{io_timeout_ms / 1000, (io_timeout_ms % 1000) * 1000};
    (void)::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)::setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    return descriptor;
}

bool sysctl_random(unsigned char* output, std::size_t size) {
    while (size) {
        int mib[2] = {CTL_KERN, KERN_ARND};
        std::size_t received = size > 256 ? 256 : size;
        if (::sysctl(mib, 2, output, &received, nullptr, 0) != 0 || !received)
            return false;
        output += received;
        size -= received;
    }
    return true;
}

bool urandom_random(unsigned char* output, std::size_t size) {
    const int descriptor = ::open("/dev/urandom", O_RDONLY);
    if (descriptor < 0) return false;
    bool okay = true;
    while (size) {
        const ssize_t received = ::read(descriptor, output, size);
        if (received <= 0) {
            okay = false;
            break;
        }
        output += received;
        size -= static_cast<std::size_t>(received);
    }
    ::close(descriptor);
    return okay;
}

bool rdrand_random(unsigned char* output, std::size_t size) {
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) || !(ecx & (1u << 30))) return false;
    std::uint64_t last = 0;
    bool have_last = false;
    while (size) {
        std::uint64_t value = 0;
        unsigned char okay = 0;
        for (int attempt = 0; attempt < 16 && !okay; ++attempt)
            __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(okay));
        if (!okay || value == 0 || value == ~0ull || (have_last && value == last))
            return false;
        last = value;
        have_last = true;
        const std::size_t take = size < sizeof(value) ? size : sizeof(value);
        std::memcpy(output, &value, take);
        output += take;
        size -= take;
    }
    return true;
}
} // namespace

HttpsPlatform MakeConsoleHttpsPlatform(int net_pool,
                                       std::function<void(const std::string&)> log) {
    HttpsPlatform platform;
    platform.connect = [net_pool](const std::string& host, std::uint16_t port,
                                 int connect_timeout_ms, int io_timeout_ms,
                                 std::string& error) {
        return connect_tcp(net_pool, host, port, connect_timeout_ms, io_timeout_ms,
                           error);
    };
    platform.random = [log](void* buffer, std::size_t size) {
        static std::atomic<int> reported{0};
        auto* output = static_cast<unsigned char*>(buffer);
        const int source = sysctl_random(output, size) ? 1 :
                           urandom_random(output, size) ? 2 :
                           rdrand_random(output, size) ? 3 : 0;
        if (source && reported.exchange(source) != source && log) {
            static const char* names[] = {"", "kernel random", "/dev/urandom", "CPU RDRAND"};
            log(std::string("[https] random source: ") + names[source]);
        }
        if (!source && log) log("[https] no secure random source available");
        return source != 0;
    };
    platform.log = std::move(log);
    return platform;
}
} // namespace fe
