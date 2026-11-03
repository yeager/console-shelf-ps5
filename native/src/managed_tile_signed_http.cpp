// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/managed_tile_signed_http.hpp"

#include <cstdio>
#include <cstring>
#include <string_view>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace shelf::managed_tile_signed_http {
namespace {
namespace api = managed_tile_api_v1;
namespace pair = managed_tile_pairing_client;
namespace signed_status = managed_tile_signed_status;
namespace signed_create = managed_tile_signed_create;

constexpr std::size_t max_header_bytes = 8192;
constexpr std::size_t max_wire_bytes = max_header_bytes + pair::max_response_bytes + 4;
constexpr unsigned max_timeout_ms = 5000;
constexpr unsigned wait_slice_ms = 50;

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
void close_socket(Socket fd) { if (fd != invalid_socket) closesocket(fd); }
int socket_error() { return WSAGetLastError(); }
bool pending_error(int error) {
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}
bool retry_error(int error) { return error == WSAEWOULDBLOCK || error == WSAEINTR; }
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
void close_socket(Socket fd) { if (fd != invalid_socket) close(fd); }
int socket_error() { return errno; }
bool pending_error(int error) { return error == EINPROGRESS || error == EWOULDBLOCK; }
bool retry_error(int error) { return error == EAGAIN || error == EWOULDBLOCK || error == EINTR; }
#endif

struct Connection {
    Socket fd = invalid_socket;
    ~Connection() { close_socket(fd); }
};

bool options_valid(const Options& options) {
    return options.port != 0 && options.timeout_ms >= wait_slice_ms &&
           options.timeout_ms <= max_timeout_ms && options.clock;
}

struct Budget {
    const Options& options;
    std::uint64_t began = 0;
    std::uint64_t last = 0;
    unsigned idle_slices = 0;

    explicit Budget(const Options& source) : options(source) {}

    Transport start() {
        if (options.cancel && options.cancel(options.context)) return Transport::cancelled;
        if (!options.clock(options.context, began)) return Transport::io_unknown;
        last = began;
        return Transport::complete;
    }

    Transport check(std::uint64_t& remaining) {
        if (options.cancel && options.cancel(options.context)) return Transport::cancelled;
        std::uint64_t now = 0;
        if (!options.clock(options.context, now) || now < last || now < began)
            return Transport::io_unknown;
        last = now;
        const auto elapsed = now - began;
        if (elapsed >= options.timeout_ms ||
            idle_slices > (options.timeout_ms / wait_slice_ms) + 1)
            return Transport::timed_out;
        remaining = options.timeout_ms - elapsed;
        return Transport::complete;
    }
};

Transport wait_socket(Socket fd, bool for_write, Budget& budget) {
    for (;;) {
        std::uint64_t remaining = 0;
        const auto state = budget.check(remaining);
        if (state != Transport::complete) return state;
        fd_set ready;
        fd_set errors;
        FD_ZERO(&ready);
        FD_ZERO(&errors);
        FD_SET(fd, &ready);
        FD_SET(fd, &errors);
        const unsigned slice = remaining < wait_slice_ms
                                   ? static_cast<unsigned>(remaining)
                                   : wait_slice_ms;
        timeval timeout{static_cast<long>(slice / 1000),
                        static_cast<long>((slice % 1000) * 1000)};
        const int result = select(static_cast<int>(fd + 1),
                                  for_write ? nullptr : &ready,
                                  for_write ? &ready : nullptr,
                                  &errors, &timeout);
        if (result < 0) {
            if (retry_error(socket_error())) continue;
            return Transport::io_unknown;
        }
        if (FD_ISSET(fd, &errors)) return Transport::io_unknown;
        if (result > 0 && FD_ISSET(fd, &ready)) return Transport::complete;
        if (result == 0) ++budget.idle_slices;
    }
}

bool lower_hex_64(const char* value) {
    if (!value) return false;
    for (std::size_t i = 0; i < 64; ++i) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    }
    return value[64] == 0;
}

bool decimal_counter(const char* value) {
    if (!value || value[0] < '1' || value[0] > '9') return false;
    for (std::size_t i = 1; i <= 20; ++i) {
        if (value[i] == 0) return true;
        if (i == 20 || value[i] < '0' || value[i] > '9') return false;
    }
    return false;
}

bool route_allowed(const char* route) {
    return route && (std::strcmp(route, api::status_route) == 0 ||
                     std::strcmp(route, api::create_route) == 0);
}

bool ascii_equal_ci(std::string_view candidate, std::string_view expected) {
    if (candidate.size() != expected.size()) return false;
    for (std::size_t i = 0; i < candidate.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(candidate[i]);
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<unsigned char>(ch + 32);
        if (ch != static_cast<unsigned char>(expected[i])) return false;
    }
    return true;
}

std::string_view trim_ows(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.remove_suffix(1);
    return value;
}

bool parse_decimal(std::string_view value, std::size_t& result) {
    result = 0;
    if (value.empty() || value.size() > 4) return false;
    for (const char ch : value) {
        if (ch < '0' || ch > '9') return false;
        result = result * 10 + static_cast<unsigned>(ch - '0');
        if (result > pair::max_response_bytes) return false;
    }
    return true;
}

bool parse_reply(const char* raw, std::size_t bytes, Reply& reply) {
    reply = {};
    if (!raw || bytes < 16 || bytes > max_wire_bytes) return false;
    std::size_t divider = bytes;
    for (std::size_t i = 0; i + 3 < bytes; ++i)
        if (std::memcmp(raw + i, "\r\n\r\n", 4) == 0) { divider = i; break; }
    if (divider == bytes || divider > max_header_bytes) return false;
    std::size_t first_end = divider;
    for (std::size_t i = 0; i + 1 < divider; ++i)
        if (raw[i] == '\r' && raw[i + 1] == '\n') { first_end = i; break; }
    if (first_end == divider || first_end < 12 ||
        std::memcmp(raw, "HTTP/1.1 ", 9) != 0 ||
        raw[9] < '1' || raw[9] > '5' ||
        raw[10] < '0' || raw[10] > '9' ||
        raw[11] < '0' || raw[11] > '9' ||
        (first_end > 12 && raw[12] != ' ')) return false;
    const int status = (raw[9] - '0') * 100 + (raw[10] - '0') * 10 + raw[11] - '0';
    bool have_length = false;
    bool have_type = false;
    bool have_mac = false;
    std::size_t content_length = 0;
    char mac_hex[65]{};
    std::size_t lines = 0;
    for (std::size_t pos = first_end + 2; pos < divider;) {
        if (++lines > 64) return false;
        std::size_t end = pos;
        while (end + 1 < divider && !(raw[end] == '\r' && raw[end + 1] == '\n'))
            ++end;
        if (end != divider && !(raw[end] == '\r' && raw[end + 1] == '\n'))
            end = divider;
        if (end > divider || end == pos) return false;
        std::size_t colon = pos;
        while (colon < end && raw[colon] != ':') ++colon;
        if (colon == pos || colon == end) return false;
        for (std::size_t i = pos; i < colon; ++i) {
            const unsigned char ch = static_cast<unsigned char>(raw[i]);
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  ch == '-')) return false;
        }
        const std::string_view name(raw + pos, colon - pos);
        const auto value = trim_ows(std::string_view(raw + colon + 1,
                                                     end - colon - 1));
        for (const char ch : value)
            if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f)
                return false;
        if (ascii_equal_ci(name, "content-length")) {
            if (have_length || !parse_decimal(value, content_length)) return false;
            have_length = true;
        } else if (ascii_equal_ci(name, "content-type")) {
            if (have_type || value != "application/json") return false;
            have_type = true;
        } else if (ascii_equal_ci(name, "x-isshelf-response-mac")) {
            if (have_mac || value.size() != 64) return false;
            std::memcpy(mac_hex, value.data(), 64);
            if (!lower_hex_64(mac_hex)) return false;
            have_mac = true;
        } else if (ascii_equal_ci(name, "transfer-encoding") ||
                   ascii_equal_ci(name, "content-encoding")) {
            return false;
        }
        pos = end == divider ? divider : end + 2;
    }
    if (!have_length || !have_type || !have_mac ||
        divider + 4 + content_length != bytes) return false;
    reply.http_status = status;
    reply.body_bytes = content_length;
    if (content_length)
        std::memcpy(reply.body, raw + divider + 4, content_length);
    std::memcpy(reply.response_mac_hex, mac_hex, sizeof(mac_hex));
    reply.complete = true;
    return true;
}

Transport connect_loopback(const Options& options, Budget& budget,
                           Connection& connection) {
    connection.fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connection.fd == invalid_socket) return Transport::io_unknown;
#ifndef _WIN32
    if (connection.fd >= FD_SETSIZE) return Transport::io_unknown;
#endif
#ifdef _WIN32
    u_long nonblocking = 1;
    if (ioctlsocket(connection.fd, FIONBIO, &nonblocking))
        return Transport::io_unknown;
#elif defined(SHELF_PS5)
    constexpr int native_nonblocking = 0x1200;
    const int nonblocking = 1;
    if (setsockopt(connection.fd, SOL_SOCKET, native_nonblocking,
                   &nonblocking, sizeof(nonblocking))) return Transport::io_unknown;
#else
    const int flags = fcntl(connection.fd, F_GETFL, 0);
    if (flags < 0 || fcntl(connection.fd, F_SETFL, flags | O_NONBLOCK))
        return Transport::io_unknown;
#endif
    sockaddr_in address{};
#ifdef SHELF_PS5
    address.sin_len = sizeof(address);
#endif
    address.sin_family = AF_INET;
    address.sin_port = htons(options.port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(connection.fd, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0) {
        if (!pending_error(socket_error())) return Transport::io_unknown;
        const auto waited = wait_socket(connection.fd, true, budget);
        if (waited != Transport::complete) return waited;
        int error = 0;
#ifdef _WIN32
        int size = sizeof(error);
        if (getsockopt(connection.fd, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&error), &size) || error)
            return Transport::io_unknown;
#else
        socklen_t size = sizeof(error);
        if (getsockopt(connection.fd, SOL_SOCKET, SO_ERROR, &error, &size) || error)
            return Transport::io_unknown;
#endif
    }
    return Transport::complete;
}

Transport send_all(Connection& connection, const char* bytes,
                   std::size_t size, Budget& budget) {
    std::size_t sent = 0;
    while (sent < size) {
        const auto waited = wait_socket(connection.fd, true, budget);
        if (waited != Transport::complete) return waited;
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const int amount = static_cast<int>(send(connection.fd, bytes + sent,
                                                  static_cast<int>(size - sent),
                                                  flags));
        if (amount < 0 && retry_error(socket_error())) continue;
        if (amount <= 0) return Transport::io_unknown;
        sent += static_cast<std::size_t>(amount);
    }
    return Transport::complete;
}

Transport receive(Connection& connection, Budget& budget, Reply& reply) {
    // Reserve one byte beyond the maximum valid response. An exact-limit
    // response must still be allowed to reach EOF for framing validation.
    char raw[max_wire_bytes + 1]{};
    std::size_t bytes = 0;
    for (;;) {
        const auto waited = wait_socket(connection.fd, false, budget);
        if (waited != Transport::complete) return waited;
        if (bytes == sizeof(raw)) return Transport::protocol_unknown;
        const int amount = static_cast<int>(recv(connection.fd, raw + bytes,
                                                  static_cast<int>(sizeof(raw) - bytes), 0));
        if (amount < 0 && retry_error(socket_error())) continue;
        if (amount < 0) return Transport::io_unknown;
        if (amount == 0) {
            const bool valid = parse_reply(raw, bytes, reply);
            pair::clear(raw, sizeof(raw));
            return valid ? Transport::complete : Transport::protocol_unknown;
        }
        bytes += static_cast<std::size_t>(amount);
        if (bytes > sizeof(raw)) return Transport::protocol_unknown;
    }
}

} // namespace

Transport post(const Options& options, const char* route, const char* body,
               std::size_t body_bytes, const char* counter_header,
               const char* request_mac_hex, Reply& reply) {
    reply = {};
    // A future caller cannot use this low-level route to evade the current
    // PS5 write gate. CREATE is exercisable only by the host fixture build.
#if defined(SHELF_PS5) || !defined(SHELF_TILE_WRITE_QUARANTINE_TEST)
    if (route && std::strcmp(route, api::create_route) == 0)
        return Transport::invalid;
#endif
    if (!options_valid(options) || !route_allowed(route) || !body ||
        body_bytes == 0 || body_bytes > pair::max_body_bytes ||
        !decimal_counter(counter_header) || !lower_hex_64(request_mac_hex) ||
        std::memchr(body, 0, body_bytes)) return Transport::invalid;
    Budget budget(options);
    const auto started = budget.start();
    if (started != Transport::complete) return started;
#ifdef _WIN32
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock)) return Transport::io_unknown;
    struct Network { ~Network() { WSACleanup(); } } network;
#endif
    char header[512]{};
    const int printed = std::snprintf(
        header, sizeof(header),
        "POST %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nX-IsShelf-Counter: %s\r\n"
        "X-IsShelf-Request-MAC: %s\r\nConnection: close\r\n\r\n",
        route, static_cast<unsigned>(options.port), body_bytes,
        counter_header, request_mac_hex);
    if (printed <= 0 || static_cast<std::size_t>(printed) >= sizeof(header))
        return Transport::invalid;
    char request[sizeof(header) + pair::max_body_bytes]{};
    std::memcpy(request, header, static_cast<std::size_t>(printed));
    std::memcpy(request + printed, body, body_bytes);
    pair::clear(header, sizeof(header));
    Connection connection;
    auto outcome = connect_loopback(options, budget, connection);
    if (outcome == Transport::complete)
        outcome = send_all(connection, request,
                           static_cast<std::size_t>(printed) + body_bytes,
                           budget);
    pair::clear(request, sizeof(request));
    if (outcome == Transport::complete)
        outcome = receive(connection, budget, reply);
    if (outcome != Transport::complete) reply = {};
    return outcome;
}

signed_status::Outcome exchange_status(
    const Options& options, const pair::Session& session,
    signed_status::PendingStatus& pending, api::Status& status,
    Transport* transport) {
    Reply reply;
    const auto result = pending.active
        ? post(options, api::status_route, pending.body, pending.body_bytes,
               pending.counter_header, pending.request_mac_header, reply)
        : Transport::invalid;
    if (transport) *transport = result;
    return signed_status::consume_status(
        session, pending, result == Transport::complete ? reply.http_status : -1,
        result == Transport::complete ? reply.body : nullptr,
        result == Transport::complete ? reply.body_bytes : 0,
        result == Transport::complete ? reply.response_mac_hex : nullptr,
        result == Transport::complete ? 64 : 0, status);
}

signed_create::Outcome exchange_create(
    const Options& options, const pair::Session& session,
    signed_create::PendingCreate& pending,
    signed_create::Generation& generation, api::Operation& receipt,
    Transport* transport) {
#if defined(SHELF_PS5) || !defined(SHELF_TILE_WRITE_QUARANTINE_TEST)
    // Keep the already-journaled pending object untouched: no send attempt
    // began and its counter remains burned by prepare_create.
    (void)options;
    (void)session;
    (void)pending;
    generation = {};
    receipt = {};
    if (transport) *transport = Transport::invalid;
    return signed_create::Outcome::invalid;
#else
    if (!signed_create::begin_send(pending)) {
        if (transport) *transport = Transport::invalid;
        generation = {};
        receipt = {};
        return signed_create::Outcome::invalid;
    }
    Reply reply;
    const auto result = post(options, api::create_route,
                             pending.body, pending.body_bytes,
                             pending.counter_header, pending.request_mac_header,
                             reply);
    if (transport) *transport = result;
    return signed_create::consume_create(
        session, pending, result == Transport::complete ? reply.http_status : -1,
        result == Transport::complete ? reply.body : nullptr,
        result == Transport::complete ? reply.body_bytes : 0,
        result == Transport::complete ? reply.response_mac_hex : nullptr,
        result == Transport::complete ? 64 : 0, generation, receipt);
#endif
}

signed_create::Reconcile exchange_reconcile_status(
    const Options& options, const pair::Session& session,
    signed_status::PendingStatus& pending, api::Status& status,
    Transport* transport) {
    Reply reply;
    const auto result = pending.active && pending.generation_specific
        ? post(options, api::status_route, pending.body, pending.body_bytes,
               pending.counter_header, pending.request_mac_header, reply)
        : Transport::invalid;
    if (transport) *transport = result;
    return signed_create::consume_reconcile_status(
        session, pending, result == Transport::complete ? reply.http_status : -1,
        result == Transport::complete ? reply.body : nullptr,
        result == Transport::complete ? reply.body_bytes : 0,
        result == Transport::complete ? reply.response_mac_hex : nullptr,
        result == Transport::complete ? 64 : 0, status);
}

} // namespace shelf::managed_tile_signed_http
