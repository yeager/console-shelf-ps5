// SPDX-License-Identifier: GPL-3.0-only
// Localhost fixture only. No console address, durable key, or tile mutation.
#include "shelf/managed_tile_signed_http.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace http = shelf::managed_tile_signed_http;
namespace status = shelf::managed_tile_signed_status;
namespace create = shelf::managed_tile_signed_create;
namespace pair = shelf::managed_tile_pairing_client;
namespace api = shelf::managed_tile_api_v1;

namespace {
constexpr char nonce[] = "0123456789abcdef0123456789abcdef";

struct Context { bool cancelled = false; };

bool clock_ms(void*, std::uint64_t& value) {
    value = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    return true;
}

bool cancel(void* context) {
    return static_cast<Context*>(context)->cancelled;
}

http::Options options(unsigned short port, Context* context = nullptr,
                      unsigned timeout_ms = 1000) {
    http::Options result{};
    result.port = port;
    result.clock = clock_ms;
    result.timeout_ms = timeout_ms;
    result.context = context;
    result.cancel = context ? cancel : nullptr;
    return result;
}

pair::Session session() {
    pair::Session result{};
    for (std::size_t i = 0; i < result.key.size(); ++i)
        result.key[i] = static_cast<unsigned char>(0x30u + i);
    result.next_counter = 1;
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

std::string installed(const char* title) {
    return std::string("{\"status\":0,\"contract_version\":1,\"title_id\":\"") +
        title + "\",\"state\":\"installed\",\"create_nonce\":\"" + nonce +
        "\",\"route\":\"library\",\"emulator_title_id\":\"-\","
        "\"source_verified\":true,\"mount_verified\":true,"
        "\"registration_verified\":true,\"reason\":null}";
}

std::string receipt(const char* title) {
    return std::string("{\"status\":0,\"contract_version\":1,\"title_id\":\"") +
        title + "\",\"operation\":\"create\",\"nonce\":\"" + nonce +
        "\",\"state\":\"preparing\",\"replay\":false}";
}

template <typename Pending>
std::string response(const pair::Session& opened, const Pending& pending,
                     unsigned code, const std::string& body,
                     const std::string& extra = {}) {
    pair::Mac mac{};
    assert(pair::response_mac(opened, pending.counter, pending.request_mac,
                              code, body.data(), body.size(), mac));
    char hex[65]{};
    status::encode_mac_hex(mac, hex);
    pair::clear(mac.data(), mac.size());
    return "HTTP/1.1 " + std::to_string(code) + " OK\r\n"
        "Content-Type: application/json\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\nX-IsShelf-Response-MAC: " +
        hex + "\r\n" + extra + "Connection: close\r\n\r\n" + body;
}

std::size_t request_length(const std::string& request) {
    const std::string marker = "Content-Length: ";
    const auto start = request.find(marker);
    assert(start != std::string::npos);
    const auto end = request.find("\r\n", start);
    assert(end != std::string::npos);
    return static_cast<std::size_t>(
        std::stoul(request.substr(start + marker.size(), end - start - marker.size())));
}

struct Fixture {
    int listener = -1;
    unsigned short port = 0;
    std::thread worker;
    std::string captured;

    Fixture(std::string wire_response, std::size_t chunk = 0,
            unsigned delay_ms = 0) {
        listener = socket(AF_INET, SOCK_STREAM, 0);
        assert(listener >= 0);
        int reuse = 1;
        assert(setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                          &reuse, sizeof(reuse)) == 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        assert(bind(listener, reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)) == 0);
        assert(listen(listener, 1) == 0);
        socklen_t size = sizeof(address);
        assert(getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                           &size) == 0);
        port = ntohs(address.sin_port);
        worker = std::thread([this, wire_response, chunk, delay_ms] {
            const int connection = accept(listener, nullptr, nullptr);
            assert(connection >= 0);
            char buffer[512]{};
            std::size_t expected = 0;
            for (;;) {
                const auto read = recv(connection, buffer, sizeof(buffer), 0);
                assert(read > 0);
                captured.append(buffer, static_cast<std::size_t>(read));
                const auto divider = captured.find("\r\n\r\n");
                if (divider != std::string::npos) {
                    expected = divider + 4 + request_length(captured);
                    assert(captured.size() <= expected);
                    if (captured.size() == expected) break;
                }
                assert(captured.size() < 8192);
            }
            if (delay_ms)
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            if (chunk == 0) chunk_send(connection, wire_response.data(),
                                       wire_response.size());
            else {
                for (std::size_t at = 0; at < wire_response.size(); at += chunk) {
                    const auto count = std::min(chunk, wire_response.size() - at);
                    chunk_send(connection, wire_response.data() + at, count);
                }
            }
            shutdown(connection, SHUT_WR);
            close(connection);
            close(listener);
            listener = -1;
        });
    }

    static void chunk_send(int fd, const char* bytes, std::size_t size) {
        while (size) {
            const auto sent = send(fd, bytes, size, MSG_NOSIGNAL);
            if (sent <= 0) return; // Timeout client may have closed already.
            bytes += sent;
            size -= static_cast<std::size_t>(sent);
        }
    }

    void finish() { if (worker.joinable()) worker.join(); }
    ~Fixture() { finish(); }
};

std::string request(const char* path, unsigned short port,
                    const char* counter, const char* mac,
                    const char* body, std::size_t body_bytes) {
    return std::string("POST ") + path + " HTTP/1.1\r\nHost: 127.0.0.1:" +
        std::to_string(port) + "\r\nContent-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body_bytes) +
        "\r\nX-IsShelf-Counter: " + counter +
        "\r\nX-IsShelf-Request-MAC: " + mac +
        "\r\nConnection: close\r\n\r\n" +
        std::string(body, body_bytes);
}

bool journal(void* context, const shelf::ConsoleTileIntent&, const char[33]) {
    ++*static_cast<unsigned*>(context);
    return true;
}

void status_round_trip() {
    auto opened = session();
    status::PendingStatus pending;
    const auto system = system_for("snes");
    assert(status::prepare_status(opened, system, pending));
    const auto body = absent(shelf::tile_job::owned_title_for(system));
    Fixture fixture(response(opened, pending, 200, body), 7);
    const auto expected = request(api::status_route, fixture.port,
                                  pending.counter_header,
                                  pending.request_mac_header,
                                  pending.body, pending.body_bytes);
    api::Status observed{};
    http::Transport transport{};
    assert(http::exchange_status(options(fixture.port), opened, pending,
                                 observed, &transport) == status::Outcome::valid);
    fixture.finish();
    assert(transport == http::Transport::complete);
    assert(observed.stage == api::Stage::absent && !pending.active);
    assert(fixture.captured == expected);
}

void create_receipt_and_reconcile() {
    auto opened = session();
    const auto system = system_for("snes");
    const shelf::ConsoleTileIntent intent{
        system, shelf::ConsoleTileDestination::library, {}};
    create::PendingCreate pending;
    unsigned journal_calls = 0;
    assert(create::prepare_create(opened, intent, nullptr, nonce, journal,
                                  &journal_calls, pending) == create::Prepare::ready);
    assert(journal_calls == 1);
    const auto body = receipt(shelf::tile_job::owned_title_for(system));
    Fixture fixture(response(opened, pending, 202, body), 5);
    const auto expected = request(api::create_route, fixture.port,
                                  pending.counter_header,
                                  pending.request_mac_header,
                                  pending.body, pending.body_bytes);
    create::Generation generation;
    api::Operation received;
    http::Transport transport{};
    assert(http::exchange_create(options(fixture.port), opened, pending,
                                 generation, received, &transport) ==
           create::Outcome::receipt_needs_status);
    fixture.finish();
    assert(transport == http::Transport::complete && fixture.captured == expected);
    assert(!pending.ready && generation.journaled && generation.may_have_sent);
    assert(received.receipt == api::ReceiptState::needs_status);

    status::PendingStatus read;
    assert(create::prepare_reconcile_status(opened, generation, read));
    const auto installed_body = installed(shelf::tile_job::owned_title_for(system));
    Fixture second(response(opened, read, 200, installed_body), 11);
    api::Status installed_status;
    assert(http::exchange_reconcile_status(options(second.port), opened, read,
               installed_status, &transport) == create::Reconcile::installed_reported);
    second.finish();
    assert(transport == http::Transport::complete && !read.active);
    assert(installed_status.stage == api::Stage::installed);
}

void status_failures() {
    const auto system = system_for("ps1");
    const auto title = shelf::tile_job::owned_title_for(system);
    for (unsigned case_id = 0; case_id < 8; ++case_id) {
        auto opened = session();
        status::PendingStatus pending;
        assert(status::prepare_status(opened, system, pending));
        const auto body = absent(title);
        std::string raw = response(opened, pending, 200, body);
        http::Transport expected = http::Transport::protocol_unknown;
        if (case_id == 0) { // Truncated body.
            raw.pop_back();
        } else if (case_id == 1) { // Extra body byte.
            raw.push_back('!');
        } else if (case_id == 2) { // Unsigned response.
            const auto start = raw.find("X-IsShelf-Response-MAC: ");
            assert(start != std::string::npos);
            raw.erase(start, raw.find("\r\n", start) + 2 - start);
        } else if (case_id == 3) { // Duplicate MAC.
            const auto marker = std::string("X-IsShelf-Response-MAC: ") +
                std::string(64, '0') + "\r\n";
            raw.insert(raw.find("Connection: close"), marker);
        } else if (case_id == 4) { // Framing ambiguity.
            raw.insert(raw.find("Connection: close"),
                       "Transfer-Encoding: chunked\r\n");
        } else if (case_id == 5) { // Authenticated framing, bad MAC.
            const auto at = raw.find("X-IsShelf-Response-MAC: ") + 24;
            assert(at < raw.size());
            raw[at] = raw[at] == 'a' ? 'b' : 'a';
            expected = http::Transport::complete;
        } else if (case_id == 6) { // Signed error is not a trusted STATUS state.
            raw = response(opened, pending, 503, body);
            expected = http::Transport::complete;
        } else { // A body larger than the wire budget is rejected.
            raw = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                  "Content-Length: 4097\r\nX-IsShelf-Response-MAC: " +
                  std::string(64, '0') + "\r\n\r\n" +
                  std::string(pair::max_response_bytes + 1, 'a');
        }
        Fixture fixture(raw);
        api::Status observed;
        http::Transport transport{};
        assert(http::exchange_status(options(fixture.port), opened, pending,
                                     observed, &transport) == status::Outcome::unknown);
        fixture.finish();
        assert(transport == expected);
        assert(!pending.active && observed.stage == api::Stage::held_unknown);
    }
}

void timeout_and_cancel() {
    auto opened = session();
    status::PendingStatus pending;
    assert(status::prepare_status(opened, system_for("ps2"), pending));
    Fixture slow("", 0, 160);
    api::Status observed;
    http::Transport transport{};
    assert(http::exchange_status(options(slow.port, nullptr, 50), opened,
                                 pending, observed, &transport) ==
           status::Outcome::unknown);
    slow.finish();
    assert(transport == http::Transport::timed_out && !pending.active);

    Context context{};
    context.cancelled = true;
    assert(status::prepare_status(opened, system_for("ps2"), pending));
    assert(http::exchange_status(options(10101, &context), opened, pending,
                                 observed, &transport) == status::Outcome::unknown);
    assert(transport == http::Transport::cancelled && !pending.active);

    // A journaled CREATE with no response must retain its generation and
    // require a later signed STATUS, even when cancellation came first.
    create::PendingCreate write;
    const shelf::ConsoleTileIntent intent{
        system_for("snes"), shelf::ConsoleTileDestination::library, {}};
    unsigned calls = 0;
    assert(create::prepare_create(opened, intent, nullptr, nonce, journal,
                                  &calls, write) == create::Prepare::ready);
    create::Generation generation;
    api::Operation receipt_value;
    assert(http::exchange_create(options(10101, &context), opened, write,
                                 generation, receipt_value, &transport) ==
           create::Outcome::uncertain);
    assert(transport == http::Transport::cancelled &&
           generation.journaled && generation.may_have_sent && !write.ready);
    status::PendingStatus reconciliation;
    assert(create::prepare_reconcile_status(opened, generation, reconciliation));
}

void unsigned_create_is_uncertain() {
    auto opened = session();
    const auto system = system_for("snes");
    const shelf::ConsoleTileIntent intent{
        system, shelf::ConsoleTileDestination::library, {}};
    create::PendingCreate pending;
    unsigned journal_calls = 0;
    assert(create::prepare_create(opened, intent, nullptr, nonce, journal,
                                  &journal_calls, pending) == create::Prepare::ready);
    const auto body = receipt(shelf::tile_job::owned_title_for(system));
    auto raw = response(opened, pending, 202, body);
    const auto start = raw.find("X-IsShelf-Response-MAC: ");
    assert(start != std::string::npos);
    raw.erase(start, raw.find("\r\n", start) + 2 - start);
    Fixture fixture(raw);
    create::Generation generation;
    api::Operation observed;
    http::Transport transport{};
    assert(http::exchange_create(options(fixture.port), opened, pending,
                                 generation, observed, &transport) ==
           create::Outcome::uncertain);
    fixture.finish();
    assert(transport == http::Transport::protocol_unknown &&
           generation.journaled && generation.may_have_sent &&
           observed.receipt == api::ReceiptState::held_unknown);
    status::PendingStatus read;
    assert(create::prepare_reconcile_status(opened, generation, read));
}

void catalog_limits_and_preflight() {
    assert(shelf::system_count == 52);
    auto opened = session();
    std::size_t mutable_count = 0;
    unsigned calls = 0;
    for (std::size_t system = 0; system < shelf::system_count; ++system) {
        status::PendingStatus read;
        assert(status::prepare_status(opened, system, read));
        assert(read.active && read.system == system);
        status::clear(read);
        const shelf::ConsoleTileIntent intent{
            system, shelf::ConsoleTileDestination::library, {}};
        create::PendingCreate write;
        const auto result = create::prepare_create(
            opened, intent, nullptr, nonce, journal, &calls, write);
        if (api::mutable_title(system)) {
            ++mutable_count;
            assert(result == create::Prepare::ready && write.ready);
        } else {
            assert(result == create::Prepare::invalid_request && !write.ready);
        }
    }
    assert(mutable_count == 48 && calls == 48);
    http::Reply reply;
    constexpr char mac[] =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    assert(http::post(options(10101), "/api/v1/isshelf/tiles/remove", "{}", 2,
                      "1", mac, reply) == http::Transport::invalid);
    assert(http::post(options(10101), api::create_route, "{}", 2,
                      "0", mac, reply) == http::Transport::invalid);
    auto no_clock = options(10101);
    no_clock.clock = nullptr;
    assert(http::post(no_clock, api::status_route, "{}", 2,
                      "1", mac, reply) == http::Transport::invalid);

    std::string too_big(pair::max_body_bytes + 1, 'a');
    assert(http::post(options(10101), api::status_route, too_big.data(),
                      too_big.size(), "1", mac, reply) ==
           http::Transport::invalid);

    // The direct transport handles the largest allowed request without
    // treating its raw HTTP completion as an authenticated STATUS result.
    std::string maximum(pair::max_body_bytes, 'a');
    const std::string raw = std::string("HTTP/1.1 200 OK\r\n") +
        "Content-Type: application/json\r\nContent-Length: 2\r\n"
        "X-IsShelf-Response-MAC: " + mac +
        "\r\nConnection: close\r\n\r\n{}";
    Fixture fixture(raw);
    assert(http::post(options(fixture.port), api::status_route, maximum.data(),
                      maximum.size(), "1", mac, reply) ==
           http::Transport::complete);
    fixture.finish();
    assert(reply.complete && reply.body_bytes == 2 &&
           fixture.captured.size() ==
               request(api::status_route, fixture.port, "1", mac,
                       maximum.data(), maximum.size()).size());
}
} // namespace

int main() {
    status_round_trip();
    create_receipt_and_reconcile();
    status_failures();
    timeout_and_cancel();
    unsigned_create_is_uncertain();
    catalog_limits_and_preflight();
    std::puts("PASS: bounded signed loopback STATUS and receipt-only CREATE (52/48)");
}
