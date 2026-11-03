// SPDX-License-Identifier: GPL-3.0-only
// Localhost-only test of the unwired signed STATUS app boundary.
#include "shelf/managed_tile_signed_app_status.hpp"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace app = shelf::managed_tile_signed_app_status;
namespace api = shelf::managed_tile_api_v1;
namespace http = shelf::managed_tile_signed_http;
namespace pair = shelf::managed_tile_pairing_client;
namespace signed_status = shelf::managed_tile_signed_status;

namespace {

struct Context {
    unsigned entered = 0;
    unsigned left = 0;
    bool deny_lock = false;
    bool cancelled = false;
};

bool enter(void* raw) {
    auto& context = *static_cast<Context*>(raw);
    ++context.entered;
    return !context.deny_lock;
}

void leave(void* raw) { ++static_cast<Context*>(raw)->left; }

bool clock_ms(void*, std::uint64_t& value) {
    value = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    return true;
}

bool cancel(void* raw) { return static_cast<Context*>(raw)->cancelled; }

pair::Session session() {
    pair::Session result{};
    for (std::size_t i = 0; i < result.key.size(); ++i)
        result.key[i] = static_cast<unsigned char>(0x30u + i);
    result.next_counter = 9;
    result.active = true;
    return result;
}

std::size_t system_for(const char* id) {
    for (std::size_t i = 0; i < shelf::system_count; ++i)
        if (shelf::catalog[i].id == id) return i;
    std::abort();
}

std::string absent(const char* title) {
    return std::string("{\"status\":0,\"contract_version\":1,\"title_id\":\"") +
        title + "\",\"state\":\"absent\",\"create_nonce\":null,\"route\":null,"
        "\"emulator_title_id\":null,\"source_verified\":false,"
        "\"mount_verified\":false,\"registration_verified\":false,\"reason\":null}";
}

std::string conflict(const char* title) {
    return std::string("{\"status\":0,\"contract_version\":1,\"title_id\":\"") +
        title + "\",\"state\":\"conflict\",\"create_nonce\":null,\"route\":null,"
        "\"emulator_title_id\":null,\"source_verified\":null,"
        "\"mount_verified\":null,\"registration_verified\":null,"
        "\"reason\":\"legacy_pilot\"}";
}

app::Binding binding(pair::Session& opened, Context& context, unsigned short port) {
    app::Binding result{};
    result.enabled = true;
    result.service_identity_verified = true;
    result.capability_verified = true;
    result.session_owned = true;
    std::memset(result.service_sha256, 'a', 64);
    std::memset(result.session_service_sha256, 'a', 64);
    result.service_epoch = 7;
    result.session_epoch = 7;
    result.session = &opened;
    result.enter = enter;
    result.leave = leave;
    result.guard_context = &context;
    result.transport.port = port;
    result.transport.timeout_ms = 1000;
    result.transport.clock = clock_ms;
    result.transport.cancel = cancel;
    result.transport.context = &context;
    return result;
}

struct Listener {
    int fd = -1;
    unsigned short port = 0;

    Listener() {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        assert(bind_socket(address));
        assert(listen(fd, 1) == 0);
        socklen_t size = sizeof(address);
        assert(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0);
        port = ntohs(address.sin_port);
    }

    bool bind_socket(sockaddr_in& address) {
        return ::bind(fd, reinterpret_cast<sockaddr*>(&address),
                      sizeof(address)) == 0;
    }

    bool received_connection() const {
        pollfd item{fd, POLLIN, 0};
        return poll(&item, 1, 0) != 0;
    }

    ~Listener() { if (fd >= 0) close(fd); }
};

std::string header(const std::string& request, const char* name) {
    const auto marker = std::string(name) + ": ";
    const auto at = request.find(marker);
    assert(at != std::string::npos);
    const auto end = request.find("\r\n", at);
    assert(end != std::string::npos);
    return request.substr(at + marker.size(), end - at - marker.size());
}

void send_all(int fd, const std::string& raw) {
    std::size_t offset = 0;
    while (offset < raw.size()) {
        const auto sent = send(fd, raw.data() + offset, raw.size() - offset,
                               MSG_NOSIGNAL);
        if (sent <= 0) return; // A timed-out client may have closed.
        offset += static_cast<std::size_t>(sent);
    }
}

enum class Behavior { signed_absent, signed_conflict, unsigned_ok, stale_counter,
                      delayed };

struct Server {
    Listener listener;
    std::thread worker;
    std::string captured;
    pair::Session opened;

    Server(Behavior behavior, pair::Session server_session,
           std::size_t system) : opened(server_session) {
        worker = std::thread([this, behavior, system] {
            pollfd waiting{listener.fd, POLLIN, 0};
            assert(poll(&waiting, 1, 5000) == 1);
            const int connection = accept(listener.fd, nullptr, nullptr);
            assert(connection >= 0);
            char chunk[512]{};
            std::size_t expected = 0;
            while (true) {
                const auto size = recv(connection, chunk, sizeof(chunk), 0);
                assert(size > 0);
                captured.append(chunk, static_cast<std::size_t>(size));
                const auto divider = captured.find("\r\n\r\n");
                if (divider != std::string::npos) {
                    expected = divider + 4 +
                        static_cast<std::size_t>(std::stoul(
                            header(captured, "Content-Length")));
                    assert(captured.size() <= expected);
                    if (captured.size() == expected) break;
                }
                assert(captured.size() < 8192);
            }
            assert(captured.find(std::string("POST ") + api::status_route +
                                 " HTTP/1.1\r\n") == 0);
            const auto title = shelf::tile_job::owned_title_for(system);
            const auto body = behavior == Behavior::signed_conflict
                ? conflict(title) : absent(title);
            const auto counter = static_cast<std::uint64_t>(std::stoull(
                header(captured, "X-IsShelf-Counter")));
            if (opened.next_counter <= counter)
                opened.next_counter = counter + 1; // Fake service accepted this request.
            pair::Mac request_mac{};
            const auto mac_hex = header(captured, "X-IsShelf-Request-MAC");
            assert(signed_status::decode_mac_hex(mac_hex.data(), mac_hex.size(),
                                                 request_mac));
            std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n";
            if (behavior != Behavior::unsigned_ok) {
                pair::Mac response_mac{};
                const auto signed_counter = behavior == Behavior::stale_counter
                    ? counter - 1 : counter;
                assert(pair::response_mac(opened, signed_counter, request_mac,
                                          200, body.data(), body.size(),
                                          response_mac));
                char response_hex[65]{};
                signed_status::encode_mac_hex(response_mac, response_hex);
                response += std::string("X-IsShelf-Response-MAC: ") +
                    response_hex + "\r\n";
                pair::clear(response_mac.data(), response_mac.size());
            }
            pair::clear(request_mac.data(), request_mac.size());
            response += "Connection: close\r\n\r\n" + body;
            if (behavior == Behavior::delayed)
                std::this_thread::sleep_for(std::chrono::milliseconds(180));
            send_all(connection, response);
            shutdown(connection, SHUT_WR);
            close(connection);
        });
    }

    void finish() { if (worker.joinable()) worker.join(); }
    ~Server() { finish(); }
};

void no_network_when_unbound() {
    auto opened = session();
    Context context{};
    Listener listener;
    const auto system = system_for("ps2");
    const auto valid = binding(opened, context, listener.port);
    const auto before = opened.next_counter;
    for (unsigned condition = 0; condition < 11; ++condition) {
        auto candidate = valid;
        if (condition == 0) candidate.enabled = false;
        if (condition == 1) candidate.service_identity_verified = false;
        if (condition == 2) candidate.capability_verified = false;
        if (condition == 3) candidate.session_owned = false;
        if (condition == 4) candidate.session_epoch = 8;
        if (condition == 5) candidate.session_service_sha256[0] = 'b';
        if (condition == 6) candidate.session = nullptr;
        if (condition == 7) candidate.transport.clock = nullptr;
        if (condition == 8) candidate.service_sha256[0] = 'A';
        if (condition == 9) {
            std::memset(candidate.service_sha256, '0', 64);
            std::memset(candidate.session_service_sha256, '0', 64);
        }
        if (condition == 10) candidate.service_epoch = 0;
        const auto read = app::read_status(candidate, system);
        assert(read.state == app::State::unbound && read.counter == 0);
        assert(opened.next_counter == before && !listener.received_connection());
    }
    assert(context.entered == 0 && context.left == 0);
    const auto invalid = app::read_status(valid, shelf::system_count);
    assert(invalid.state == app::State::invalid_request &&
           !listener.received_connection());
    opened.active = false;
    const auto inactive = app::read_status(valid, system);
    assert(inactive.state == app::State::unbound &&
           opened.next_counter == before && !listener.received_connection());
#ifndef SHELF_PS5
    assert(context.entered == 1 && context.left == 1);
#endif
}

#ifndef SHELF_PS5
void signed_round_trips() {
    const auto system = system_for("snes");
    for (auto behavior : {Behavior::signed_absent, Behavior::signed_conflict,
                          Behavior::unsigned_ok, Behavior::stale_counter,
                          Behavior::delayed}) {
        auto opened = session();
        Context context{};
        Server server(behavior, opened, system);
        auto bound = binding(opened, context, server.listener.port);
        if (behavior == Behavior::delayed) bound.transport.timeout_ms = 50;
        const auto read = app::read_status(bound, system);
        server.finish();
        assert(context.entered == 1 && context.left == 1);
        assert(opened.next_counter == 10 && read.counter == 9);
        assert(server.captured.find("X-IsShelf-Request-MAC: ") !=
               std::string::npos);
        if (behavior == Behavior::signed_absent) {
            assert(read.state == app::State::observed &&
                   read.transport == http::Transport::complete &&
                   read.status.stage == api::Stage::absent);
        } else if (behavior == Behavior::signed_conflict) {
            assert(read.state == app::State::conflict &&
                   read.transport == http::Transport::complete &&
                   read.status.stage == api::Stage::conflict);
        } else {
            assert(read.state == app::State::unknown &&
                   read.status.title_id[0] == 0);
            if (behavior == Behavior::unsigned_ok)
                assert(read.transport == http::Transport::protocol_unknown);
            else if (behavior == Behavior::stale_counter)
                assert(read.transport == http::Transport::complete);
            else
                assert(read.transport == http::Transport::timed_out);
        }
    }
}

void cancelled_after_counter_reservation() {
    auto opened = session();
    Context context{};
    context.cancelled = true;
    Listener listener;
    const auto bound = binding(opened, context, listener.port);
    const auto read = app::read_status(bound, system_for("ps2"));
    assert(read.state == app::State::unknown &&
           read.transport == http::Transport::cancelled &&
           read.counter == 9 && opened.next_counter == 10 &&
           context.entered == 1 && context.left == 1 &&
           !listener.received_connection());
}

void denied_serialization_is_unbound() {
    auto opened = session();
    Context context{};
    context.deny_lock = true;
    Listener listener;
    const auto bound = binding(opened, context, listener.port);
    const auto read = app::read_status(bound, system_for("ps2"));
    assert(read.state == app::State::unbound && read.counter == 0 &&
           opened.next_counter == 9 && context.entered == 1 &&
           context.left == 0 && !listener.received_connection());
}
#else
void ps5_compile_guard() {
    auto opened = session();
    Context context{};
    Listener listener;
    const auto bound = binding(opened, context, listener.port);
    const auto read = app::read_status(bound, system_for("ps2"));
    assert(read.state == app::State::unbound && read.counter == 0 &&
           opened.next_counter == 9 && context.entered == 0 &&
           !listener.received_connection());
}
#endif

} // namespace

int main() {
    no_network_when_unbound();
#ifndef SHELF_PS5
    signed_round_trips();
    cancelled_after_counter_reservation();
    denied_serialization_is_unbound();
#else
    ps5_compile_guard();
#endif
}
