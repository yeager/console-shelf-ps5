// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/launch_client.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
namespace shelf {
namespace {
std::uint64_t now() {
    const auto t = std::time(nullptr);
    return t > 0 ? static_cast<std::uint64_t>(t) : 0;
}
void path(char (&output)[2304], const char* root, const char* name) {
    std::snprintf(output, sizeof(output), "%s/%s", root, name);
}
bool replace(const char* temporary, const char* target) {
#ifdef _WIN32
    return MoveFileExA(temporary, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(temporary, target) == 0;
#endif
}
bool read(const char* name, char* data, std::size_t capacity, std::size_t& size) {
    size = 0;
    FILE* file = std::fopen(name, "rb");
    if (!file)
        return false;
    size = std::fread(data, 1, capacity, file);
    const bool okay = !std::ferror(file) && size < capacity;
    std::fclose(file);
    return okay;
}
bool write(const char* temporary, const char* target, const char* data) {
    FILE* file = std::fopen(temporary, "wb");
    if (!file)
        return false;
    const auto size = std::strlen(data);
    bool okay = std::fwrite(data, 1, size, file) == size;
    okay = std::fflush(file) == 0 && okay;
    okay = std::fclose(file) == 0 && okay;
    if (okay && replace(temporary, target))
        return true;
    std::remove(temporary);
    return false;
}
bool exists(const char* name) {
    FILE* file = std::fopen(name, "rb");
    if (!file)
        return false;
    std::fclose(file);
    return true;
}
bool old_file(const char* name) {
    struct stat info{};
    const auto time = now();
    return stat(name, &info) == 0 && info.st_mtime > 0 && time > static_cast<std::uint64_t>(info.st_mtime) &&
           time - static_cast<std::uint64_t>(info.st_mtime) > 30;
}
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid = INVALID_SOCKET;
void close_socket(Socket fd) {
    if (fd != invalid)
        closesocket(fd);
}
#else
using Socket = int;
constexpr Socket invalid = -1;
void close_socket(Socket fd) {
    if (fd != invalid)
        close(fd);
}
#endif
bool wait_socket(Socket fd, SDL_atomic_t& cancelled) {
    for (int i = 0; i < 60 && !SDL_AtomicGet(&cancelled); ++i) {
        fd_set ready, failed;
        FD_ZERO(&ready);
        FD_ZERO(&failed);
        FD_SET(fd, &ready);
        FD_SET(fd, &failed);
        timeval timeout{0, 50000};
        const int status = select(static_cast<int>(fd + 1), nullptr, &ready, &failed, &timeout);
        if (status < 0 || FD_ISSET(fd, &failed))
            return false;
        if (status > 0 && FD_ISSET(fd, &ready))
            return true;
    }
    return false;
}
bool transfer(unsigned short port, const unsigned char* data, std::size_t size, SDL_atomic_t& cancelled,
              unsigned max_duration_ms = 0) {
    const auto began = SDL_GetTicks();
    const auto expired = [&]() {
        return max_duration_ms && SDL_GetTicks() - began >= max_duration_ms;
    };
#ifdef _WIN32
    WSADATA state;
    if (WSAStartup(MAKEWORD(2, 2), &state) != 0)
        return false;
    struct Network {
        ~Network() {
            WSACleanup();
        }
    } network;
#endif
    Socket fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == invalid)
        return false;
    struct Connection {
        Socket fd;
        ~Connection() {
            close_socket(fd);
        }
    } connection{fd};
#ifndef _WIN32
    if (fd >= FD_SETSIZE)
        return false;
#endif
#ifdef _WIN32
    u_long nonblocking = 1;
    if (ioctlsocket(fd, FIONBIO, &nonblocking))
        return false;
#elif defined(SHELF_PS5)
    constexpr int so_nbio = 0x1200;
    const int nonblocking = 1;
    if (setsockopt(fd, SOL_SOCKET, so_nbio, &nonblocking, sizeof(nonblocking)))
        return false;
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK))
        return false;
#endif
    sockaddr_in address{};
#ifdef SHELF_PS5
    address.sin_len = sizeof(address);
#endif
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (expired()) return false;
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
#ifdef _WIN32
        const int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS)
            return false;
#else
        if (errno != EINPROGRESS)
            return false;
#endif
        if (expired() || !wait_socket(fd, cancelled) || expired())
            return false;
    }
    int error = 0;
#ifdef _WIN32
    int length = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) || error)
        return false;
#else
    socklen_t length = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) || error)
        return false;
#endif
    while (size) {
        if (SDL_AtomicGet(&cancelled) || expired() || !wait_socket(fd, cancelled) || expired())
            return false;
        const auto chunk = std::min<std::size_t>(size, 65536);
#ifdef MSG_NOSIGNAL
        constexpr int send_flags = MSG_NOSIGNAL;
#else
        constexpr int send_flags = 0;
#endif
        const auto sent = send(fd, reinterpret_cast<const char*>(data), static_cast<int>(chunk), send_flags);
        if (sent <= 0)
            return false;
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
#ifdef _WIN32
    return shutdown(fd, SD_SEND) == 0;
#else
    return shutdown(fd, SHUT_WR) == 0;
#endif
}
} // namespace
bool send_probe_elf_once(const unsigned char* bytes, std::size_t size, SDL_atomic_t& cancelled,
                         unsigned short test_port) {
    if (!bytes || size < 64 || size > 1048576 ||
        std::memcmp(bytes, "\x7f" "ELF\x02\x01\x01\x00", 8) != 0 || !test_port)
        return false;
#ifdef SHELF_PS5
    if (test_port != 9021) return false;
#endif
    return transfer(test_port, bytes, size, cancelled, 5000);
}
LaunchClient::~LaunchClient() {
    stop();
}
LaunchClientState LaunchClient::state() const {
    return static_cast<LaunchClientState>(SDL_AtomicGet(const_cast<SDL_atomic_t*>(&result)));
}
void LaunchClient::failed(const char* message) {
    if (SDL_AtomicGet(&cancelled))
        return;
    std::snprintf(failure, sizeof(failure), "%s", message);
    const int previous = SDL_AtomicGet(&result);
    if (previous != static_cast<int>(LaunchClientState::cancelled) &&
        previous != static_cast<int>(LaunchClientState::committed))
        SDL_AtomicCAS(&result, previous, static_cast<int>(LaunchClientState::failed));
}
bool LaunchClient::read_status(LaunchStatus& status) {
    char target[2304], data[256];
    std::size_t size;
    path(target, root, "launch-status.txt");
    return read(target, data, sizeof(data), size) && parse_launch_status({data, size}, now(), status) &&
           std::strcmp(status.id, request.id) == 0 && status.issued == request.issued;
}
bool LaunchClient::write_status(LaunchStage stage) {
    LaunchStatus status;
    std::memcpy(status.id, request.id, sizeof(status.id));
    status.issued = request.issued;
    status.stage = stage;
    char data[256], target[2304], temporary[2304];
    path(target, root, "launch-status.txt");
    path(temporary, root, "launch-client-status.tmp");
    return encode_launch_status(status, data, sizeof(data)) && write(temporary, target, data);
}
void LaunchClient::discard_owned_request() {
    if (!owns_request)
        return;
    char target[2304], data[launch_message_capacity], encoded[launch_message_capacity];
    std::size_t size;
    path(target, root, "launch-request.txt");
    if (encode_launch_request(request, request.issued, encoded, sizeof(encoded)) &&
        read(target, data, sizeof(data), size) && size == std::strlen(encoded) && std::memcmp(data, encoded, size) == 0)
        std::remove(target);
}
void LaunchClient::cancel_owned_status() {
    LaunchStatus status;
    if (owns_request && read_status(status) &&
        (status.stage == LaunchStage::sending || status.stage == LaunchStage::ready))
        write_status(LaunchStage::cancelled);
}
int LaunchClient::run(void* raw) {
    auto& client = *static_cast<LaunchClient*>(raw);
    char target[2304], temporary[2304], data[launch_message_capacity];
    path(target, client.root, "launch-request.txt");
    if (exists(target)) {
        path(temporary, client.root, "launch-request.previous");
        if (!old_file(target) || !replace(target, temporary)) {
            client.failed("An earlier launch is still pending. Wait a moment and try again.");
            return 1;
        }
    }
    path(target, client.root, "launch-status.txt");
    LaunchStatus previous;
    std::size_t size;
    if (read(target, data, 256, size) && parse_launch_status({data, size}, now(), previous) &&
        (previous.stage == LaunchStage::ready || previous.stage == LaunchStage::committed ||
         previous.stage == LaunchStage::sending)) {
        client.failed("An earlier launch is still pending. Wait a moment and try again.");
        return 1;
    }
    if (SDL_AtomicGet(&client.cancelled))
        return 0;
    path(target, client.root, "launch-request.txt");
    path(temporary, client.root, "launch-request.tmp");
    if (!encode_launch_request(client.request, now(), data, sizeof(data)) ||
        !client.write_status(LaunchStage::sending) || !write(temporary, target, data)) {
        client.failed("isShelf could not prepare its launch request.");
        return 1;
    }
    client.owns_request = true;
    if (!transfer(client.port, client.payload, client.payload_bytes, client.cancelled)) {
        client.discard_owned_request();
        client.cancel_owned_status();
        if (!SDL_AtomicGet(&client.cancelled))
            client.failed("The loader is unavailable. Open Payload Manager and try again.");
        return 1;
    }
    const auto began = SDL_GetTicks();
    while (SDL_GetTicks() - began < 5000 && !SDL_AtomicGet(&client.cancelled)) {
        LaunchStatus reply;
        if (client.read_status(reply)) {
            if (reply.stage == LaunchStage::ready) {
                if (SDL_AtomicGet(&client.cancelled))
                    break;
                SDL_AtomicCAS(&client.result, static_cast<int>(LaunchClientState::sending),
                              static_cast<int>(LaunchClientState::ready));
                return 0;
            }
            if (reply.stage != LaunchStage::sending) {
                client.failed(reply.stage == LaunchStage::foreground_changed
                                  ? "The launcher could not confirm isShelf is the active app. Diagnostic details were saved."
                                  : "The launcher could not prepare the handoff. Diagnostic details were saved.");
                return 1;
            }
        }
        SDL_Delay(50);
    }
    client.discard_owned_request();
    client.cancel_owned_status();
    if (!SDL_AtomicGet(&client.cancelled))
        client.failed("The launcher did not answer. Keep Payload Manager available and try again.");
    return 1;
}
bool LaunchClient::start(const LaunchRequest& next, const char* directory, const unsigned char* bytes, std::size_t size,
                         unsigned short loader_port) {
    if (state() == LaunchClientState::sending || state() == LaunchClientState::ready ||
        state() == LaunchClientState::committed)
        return false;
    stop();
    SDL_AtomicSet(&cancelled, 0);
    SDL_AtomicSet(&result, static_cast<int>(LaunchClientState::idle));
    if (!directory || !*directory || std::strlen(directory) >= sizeof(root) || !bytes || size < 64 || size > 1048576 ||
        std::memcmp(bytes,
                    "\x7f"
                    "ELF\x02\x01\x01\x00",
                    8) ||
        !loader_port || !launch_request_valid(next, now())) {
        failed("Invalid launch request or bundled helper.");
        return false;
    }
#ifdef SHELF_PS5
    if (std::strcmp(directory, "/data/console-shelf") != 0 || loader_port != 9021) {
        failed("Invalid console launch destination.");
        return false;
    }
#endif
    request = next;
    std::snprintf(root, sizeof(root), "%s", directory);
    failure[0] = 0;
    payload = bytes;
    payload_bytes = size;
    port = loader_port;
    owns_request = false;
    SDL_AtomicSet(&cancelled, 0);
    SDL_AtomicSet(&result, static_cast<int>(LaunchClientState::sending));
    thread = SDL_CreateThread(run, "Shelf launch", this);
    if (!thread) {
        failed("isShelf could not start its launcher task.");
        return false;
    }
    return true;
}
bool LaunchClient::commit() {
    if (state() != LaunchClientState::ready || SDL_AtomicGet(&cancelled))
        return false;
    LaunchStatus reply;
    if (!read_status(reply) || reply.stage != LaunchStage::ready || !write_status(LaunchStage::committed)) {
        failed("The handoff is no longer ready. Please try again.");
        return false;
    }
    SDL_AtomicSet(&result, static_cast<int>(LaunchClientState::committed));
    return true;
}
void LaunchClient::cancel() {
    if (state() == LaunchClientState::committed)
        return;
    SDL_AtomicSet(&cancelled, 1);
    if (!thread || state() == LaunchClientState::ready) {
        discard_owned_request();
        cancel_owned_status();
    }
    SDL_AtomicSet(&result, static_cast<int>(LaunchClientState::cancelled));
}
void LaunchClient::stop() {
    if (state() != LaunchClientState::committed)
        cancel();
    if (thread) {
        SDL_WaitThread(thread, nullptr);
        thread = nullptr;
    }
    if (state() != LaunchClientState::committed) {
        discard_owned_request();
        cancel_owned_status();
    }
    owns_request = false;
}
} // namespace shelf
