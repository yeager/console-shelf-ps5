// SPDX-License-Identifier: GPL-3.0-only
// One-shot loopback relay for isShelf console tiles. It acknowledges one
// bounded request while the tile is foreground, waits for stable Home, then
// launches exactly one validated destination. Emulator routes go straight to
// their pinned menu; Home and Library routes still open isShelf.
#include "shelf/console_tile_relay.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

extern "C" {
int sceUserServiceInitialize(void*);
int sceUserServiceGetForegroundUser(std::uint32_t*);
int sceSystemServiceGetAppIdOfRunningBigApp();
int sceSystemServiceGetAppTitleId(int, char*);
int sceLncUtilInitialize();
struct AppLaunchContext {
    std::uint32_t size;
    std::uint32_t user;
    std::uint32_t options;
    std::uint64_t crash_report;
    std::uint32_t check_flags;
};
int sceSystemServiceLaunchApp(const char*, const char**, AppLaunchContext*);
}
static_assert(sizeof(AppLaunchContext) == 32 &&
              offsetof(AppLaunchContext, check_flags) == 24,
              "Unexpected PS5 launch ABI");

namespace {
constexpr unsigned short relay_port = 10202;
constexpr unsigned short shadowmount_port = 10101;
constexpr std::size_t shadowmount_response_capacity = 32768;
constexpr char log_path[] = "/data/console-shelf/tile-relay-v3.log";

struct Context {
    std::uint32_t user = 0;
    int probe_status = 0;
    int launch_status = 0;
    unsigned release_probes = 0;
    int release_state = -2;
    int server = -1;
    char origin[10]{};
    char route[shelf::console_tile_message_capacity + 1]{};
    unsigned coalesced_requests = 0;
    unsigned unmount_attempts = 0;
    unsigned last_info_probe = 0;
    bool unmount_retry_pending = false;
    FILE* journal = nullptr;
    ~Context() {
        if (journal) std::fclose(journal);
    }
};

void log_line(Context& context, const char* format, ...) {
    if (!context.journal) return;
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(context.journal, format, arguments);
    va_end(arguments);
    std::fputc('\n', context.journal);
    std::fflush(context.journal);
}

int current(void* raw, char (&title)[10]) {
    auto& context = *static_cast<Context*>(raw);
    const int id = sceSystemServiceGetAppIdOfRunningBigApp();
    if (id == 0 || id == -1) return 0;
    if (id < 0) {
        context.probe_status = id;
        return -1;
    }
    char value[32]{};
    const int status = sceSystemServiceGetAppTitleId(id, value);
    context.probe_status = status;
    if (status != 0 || !std::memchr(value, 0, sizeof(value)) ||
        std::strlen(value) != 9)
        return -1;
    for (unsigned i = 0; i < 9; ++i)
        if (!((value[i] >= 'A' && value[i] <= 'Z') ||
              (value[i] >= '0' && value[i] <= '9')))
            return -1;
    std::memcpy(title, value, 10);
    return 1;
}

bool write_exact(int socket_value, const unsigned char* input,
                 std::size_t size);
bool read_exact(int socket_value, unsigned char* output, std::size_t size,
                std::size_t& used);
void reply(int socket_value, shelf::ConsoleTileRelayReply status, int code);
void wait(void* raw, unsigned milliseconds);

struct ShadowReply {
    unsigned status = 0;
    char body[shadowmount_response_capacity + 1]{};
    std::size_t body_bytes = 0;
};

bool shadowmount_post(const char* route, const char* origin,
                      ShadowReply& output) {
    if ((!route || (std::strcmp(route, "/api/v1/games/info") != 0 &&
                    std::strcmp(route, "/api/v1/games/unmount") != 0)) ||
        !origin || !shelf::console_tile_relay_title_valid(origin))
        return false;

    const int socket_value = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_value < 0) return false;
    timeval timeout{2, 0};
    (void)setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO,
                     &timeout, sizeof(timeout));
    (void)setsockopt(socket_value, SOL_SOCKET, SO_SNDTIMEO,
                     &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(shadowmount_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socket_value, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0) {
        close(socket_value);
        return false;
    }

    char body[64];
    const int body_size = std::snprintf(
        body, sizeof(body), "{\"title_id\":\"%s\"}", origin);
    char request[512];
    const int request_size = body_size > 0 &&
                                     static_cast<std::size_t>(body_size) < sizeof(body)
                                 ? std::snprintf(
                                       request, sizeof(request),
                                       "POST %s HTTP/1.1\r\n"
                                       "Host: 127.0.0.1:10101\r\n"
                                       "Content-Type: application/json\r\n"
                                       "Content-Length: %d\r\n"
                                       "Connection: close\r\n\r\n%s",
                                       route, body_size, body)
                                 : -1;
    if (request_size <= 0 ||
        static_cast<std::size_t>(request_size) >= sizeof(request) ||
        !write_exact(socket_value,
                     reinterpret_cast<const unsigned char*>(request),
                     static_cast<std::size_t>(request_size))) {
        close(socket_value);
        return false;
    }
    (void)shutdown(socket_value, SHUT_WR);

    char response[shadowmount_response_capacity + 1];
    std::size_t used = 0;
    bool complete = false;
    while (used < shadowmount_response_capacity) {
        const auto count = recv(socket_value, response + used,
                                shadowmount_response_capacity - used, 0);
        if (count == 0) {
            complete = true;
            break;
        }
        if (count < 0) break;
        used += static_cast<std::size_t>(count);
    }
    close(socket_value);
    if (!complete || used == 0 || used >= shadowmount_response_capacity ||
        std::memchr(response, 0, used))
        return false;
    response[used] = 0;
    char* content = std::strstr(response, "\r\n\r\n");
    if (!content || std::sscanf(response, "HTTP/%*u.%*u %u", &output.status) != 1)
        return false;
    content += 4;
    output.body_bytes = used - static_cast<std::size_t>(content - response);
    if (output.body_bytes == 0 ||
        output.body_bytes > shadowmount_response_capacity)
        return false;
    std::memcpy(output.body, content, output.body_bytes);
    output.body[output.body_bytes] = 0;
    return true;
}

int shadowmount_release(void* raw, const char* origin) {
    auto& context = *static_cast<Context*>(raw);
    ++context.release_probes;
    if (!origin || !shelf::console_tile_relay_title_valid(origin))
        return -1;

    const bool retry_unmount = shelf::console_tile_relay_should_retry_unmount(
        context.release_probes, context.unmount_attempts,
        context.unmount_retry_pending);
    if (context.unmount_attempts == 0 || retry_unmount) {
        ++context.unmount_attempts;
        context.unmount_retry_pending = false;
        ShadowReply unmount;
        if (shadowmount_post("/api/v1/games/unmount", origin, unmount)) {
            const auto result = shelf::parse_console_tile_unmount_probe(
                unmount.status,
                std::string_view(unmount.body, unmount.body_bytes), origin);
            log_line(context,
                     "ShadowMount unmount assist: origin=%s attempt=%u state=%s http=%u",
                     origin,
                     context.unmount_attempts,
                     result == shelf::ConsoleTileUnmountProbe::ready
                         ? "ready"
                         : result == shelf::ConsoleTileUnmountProbe::pending
                               ? "pending"
                               : "invalid",
                     unmount.status);
            if (result == shelf::ConsoleTileUnmountProbe::ready) {
                context.release_state = 1;
                return 1;
            }
            if (result == shelf::ConsoleTileUnmountProbe::pending &&
                context.unmount_attempts == 1) {
                context.unmount_retry_pending = true;
                context.release_state = 0;
                return 0;
            }
        } else {
            log_line(context,
                     "ShadowMount unmount assist: origin=%s attempt=%u state=unavailable",
                     origin, context.unmount_attempts);
        }
    }

    if (context.unmount_retry_pending)
        return 0;
    if (!shelf::console_tile_relay_should_poll_info(
            context.release_probes, context.last_info_probe))
        return 0;
    context.last_info_probe = context.release_probes;

    ShadowReply info;
    if (!shadowmount_post("/api/v1/games/info", origin, info))
        return 0;
    const auto probe = shelf::parse_console_tile_release_probe(
        info.status, std::string_view(info.body, info.body_bytes), origin);
    const int state = probe == shelf::ConsoleTileReleaseProbe::ready
                          ? 1
                          : probe == shelf::ConsoleTileReleaseProbe::pending ? 0 : -1;
    if (state != context.release_state) {
        context.release_state = state;
        log_line(context,
                 "ShadowMount release gate: origin=%s state=%s probes=%u http=%u",
                 origin, state > 0 ? "ready" : state == 0 ? "pending" : "invalid",
                 context.release_probes, info.status);
    }
    return state;
}

int launch(void* raw, const char* route) {
    auto& context = *static_cast<Context*>(raw);
    std::uint32_t user = 0;
    shelf::ConsoleTileRelayLaunchPlan plan;
    if (!route || !shelf::console_tile_relay_launch_plan(route, plan) ||
        sceUserServiceGetForegroundUser(&user) != 0 || user != context.user)
        return context.launch_status = -1;
    const char* arguments[] = {plan.forward_route ? plan.argument : nullptr, nullptr};
    AppLaunchContext parameters{};
    parameters.size = sizeof(parameters);
    parameters.user = user;
    parameters.check_flags = 2; // SkipSystemUpdate; no update action is made.
    context.launch_status = sceSystemServiceLaunchApp(
        plan.title, plan.forward_route ? arguments : nullptr, &parameters);
    log_line(context, "launch %s once: rc=0x%08x route=%s",
             plan.title, static_cast<unsigned>(context.launch_status),
             plan.forward_route ? plan.argument : "direct-pinned-emulator");
    return context.launch_status;
}

bool read_exact(int socket_value, unsigned char* output, std::size_t size,
                std::size_t& used) {
    used = 0;
    while (used < size) {
        const auto count = recv(socket_value, output + used, size - used, 0);
        if (count == 0) return used == 0;
        if (count < 0) return false;
        used += static_cast<std::size_t>(count);
    }
    return true;
}

bool write_exact(int socket_value, const unsigned char* input, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const auto count = send(socket_value, input + sent, size - sent, 0);
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

void reply(int socket_value, shelf::ConsoleTileRelayReply status, int code) {
    unsigned char message[shelf::console_tile_relay_reply_size];
    shelf::encode_console_tile_relay_reply(status, code, message);
    (void)write_exact(socket_value, message, sizeof(message));
}

void service_duplicate_clients(Context& context) {
    if (context.server < 0 || !context.origin[0] || !context.route[0]) return;
    for (unsigned serviced = 0; serviced < 4; ++serviced) {
        fd_set ready_set;
        FD_ZERO(&ready_set);
        FD_SET(context.server, &ready_set);
        timeval immediate{0, 0};
        if (select(context.server + 1, &ready_set, nullptr, nullptr,
                   &immediate) <= 0)
            return;
        const int client = accept(context.server, nullptr, nullptr);
        if (client < 0) return;
        timeval client_timeout{0, 100000};
        (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                         &client_timeout, sizeof(client_timeout));
        (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                         &client_timeout, sizeof(client_timeout));
        unsigned char message[shelf::console_tile_relay_request_size];
        std::size_t used = 0;
        const bool read = read_exact(client, message, sizeof(message), used);
        if (read && used == 0) {
            close(client); // readiness probe from a repeated press
            continue;
        }
        char origin[10]{}, route[shelf::console_tile_message_capacity + 1]{};
        if (read && shelf::decode_console_tile_relay_request(
                        message, used, origin, route) &&
            std::strcmp(origin, context.origin) == 0 &&
            std::strcmp(route, context.route) == 0) {
            reply(client, shelf::ConsoleTileRelayReply::queued, 0);
            ++context.coalesced_requests;
            log_line(context,
                     "coalesced duplicate origin=%s route=%s count=%u",
                     origin, route, context.coalesced_requests);
        } else {
            reply(client, shelf::ConsoleTileRelayReply::service_unavailable,
                  -EBUSY);
        }
        close(client);
    }
}

void wait(void* raw, unsigned milliseconds) {
    auto& context = *static_cast<Context*>(raw);
    unsigned remaining = milliseconds;
    while (remaining) {
        service_duplicate_clients(context);
        const unsigned slice = remaining > 25 ? 25 : remaining;
        (void)usleep(slice * 1000);
        remaining -= slice;
    }
    service_duplicate_clients(context);
}
} // namespace

int main() {
    Context context;
    context.journal = std::fopen(log_path, "ab");
    log_line(context,
             "relay identity: isshelf-main=PPSA99541;handoff=shadowmount-unmount-assisted-retry5s");
    const int user_init = sceUserServiceInitialize(nullptr);
    const int lnc_init = sceLncUtilInitialize();
    const int user_status = sceUserServiceGetForegroundUser(&context.user);
    log_line(context,
             "relay start: pid=%d user_init=0x%08x lnc_init=0x%08x user_status=0x%08x",
             static_cast<int>(getpid()), static_cast<unsigned>(user_init),
             static_cast<unsigned>(lnc_init), static_cast<unsigned>(user_status));
    const bool lnc_ready = lnc_init == 0 ||
                           static_cast<std::uint32_t>(lnc_init) == 0x80940018u;
    if (!lnc_ready || user_status != 0 || context.user > 0x7fffffffu)
        return 2;

    const int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) return 3;
    const int enabled = 1;
    (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    sockaddr_in address{};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(relay_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(server, 4) != 0) {
        close(server);
        return 4;
    }

    char origin[10]{}, route[shelf::console_tile_message_capacity + 1]{};
    bool queued = false;
    for (unsigned attempt = 0; attempt < 32 && !queued; ++attempt) {
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(server, &ready);
        timeval timeout{10, 0};
        const int selected = select(server + 1, &ready, nullptr, nullptr, &timeout);
        if (selected <= 0) break;
        const int client = accept(server, nullptr, nullptr);
        if (client < 0) continue;
        timeval client_timeout{3, 0};
        (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                         &client_timeout, sizeof(client_timeout));
        (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                         &client_timeout, sizeof(client_timeout));
        unsigned char message[shelf::console_tile_relay_request_size];
        std::size_t used = 0;
        const bool read = read_exact(client, message, sizeof(message), used);
        if (read && used == 0) {
            close(client); // readiness probe
            continue;
        }
        if (!read || !shelf::decode_console_tile_relay_request(
                         message, used, origin, route)) {
            reply(client, shelf::ConsoleTileRelayReply::invalid, -EINVAL);
            close(client);
            continue;
        }
        char foreground[10]{};
        const int active = current(&context, foreground);
        if (active != 1 || std::strcmp(foreground, origin) != 0) {
            reply(client, shelf::ConsoleTileRelayReply::foreground_mismatch,
                  context.probe_status);
            close(client);
            continue;
        }
        reply(client, shelf::ConsoleTileRelayReply::queued, 0);
        close(client);
        queued = true;
        log_line(context, "accepted origin=%s route=%s", origin, route);
    }
    if (!queued) {
        close(server);
        return 5;
    }
    context.server = server;
    std::memcpy(context.origin, origin, sizeof(context.origin));
    std::memcpy(context.route, route, sizeof(context.route));

    const auto result = shelf::cooperative_console_tile_relay(
        origin, route, {&context, current, wait, shadowmount_release, launch});
    context.server = -1;
    close(server);
    log_line(context,
             "relay result=%u probe=0x%08x release_probes=%u duplicates=%u launch=0x%08x",
             static_cast<unsigned>(result), static_cast<unsigned>(context.probe_status),
             context.release_probes, context.coalesced_requests,
             static_cast<unsigned>(context.launch_status));
    return result == shelf::ConsoleTileRelayResult::launch_requested ? 0 : 6;
}
