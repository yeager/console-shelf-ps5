// SPDX-License-Identifier: GPL-3.0-only
// isShelf's cooperative console-tile program. It validates the exact owned
// tile files, starts a one-shot loopback relay through elfldr when needed,
// queues one route, then requests a clean shell close so Home can become stable
// before the pinned destination opens.
#include "shelf/console_tile_forwarder_config.hpp"
#include "cjson/cJSON.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "tile_relay_payload.inc"

extern "C" {
int sceKernelSendNotificationRequest(std::uint32_t, void*, std::size_t, int);
int sceKernelDebugOutText(int, const char*);
int sceSystemServiceLoadExec(const char*, const char* const*);
int sceLncUtilInitialize();
int sceLncUtilGetAppIdOfRunningBigApp();
int sceLncUtilGetAppTitleId(std::uint32_t, char*);
int sceLncUtilKillApp(std::uint32_t);
}

namespace {
constexpr unsigned short relay_port = 10202;
constexpr unsigned short elfldr_port = 9021;

struct Notification {
    std::uint8_t reserved[45];
    char message[3075];
};

void log_line(const char* text) {
    char line[512];
    std::snprintf(line, sizeof(line), "[isShelf tile v3] %s\n", text);
    (void)sceKernelDebugOutText(0, line);
}

__attribute__((used)) const char clean_close_marker[] =
    "ISSHELF_CLEAN_TILE_CLOSE_V1";

[[noreturn]] void close_cleanly(const char* origin) {
    const int status = sceSystemServiceLoadExec("exit", nullptr);
    char detail[96];
    std::snprintf(detail, sizeof(detail),
                  "clean shell close requested: rc=0x%08x",
                  static_cast<unsigned>(status));
    log_line(detail);
    // LoadExec is asynchronous. Never return through the app CRT: that path
    // issues a forbidden process exit on retail PS5 and produces the system
    // "Something went wrong" dialog. Match isShelf's exact-title fallback if
    // the accepted shell close has not completed after three seconds.
    for (unsigned i = 0; i < 30; ++i)
        usleep(100000);
    if (origin && shelf::console_tile_relay_title_valid(origin) &&
        sceLncUtilInitialize() == 0) {
        const int app = sceLncUtilGetAppIdOfRunningBigApp();
        char title[16]{};
        if (app > 0 && sceLncUtilGetAppTitleId(
                           static_cast<std::uint32_t>(app), title) == 0 &&
            std::memchr(title, 0, sizeof(title)) &&
            std::strcmp(title, origin) == 0)
            (void)sceLncUtilKillApp(static_cast<std::uint32_t>(app));
    }
    for (;;)
        usleep(100000);
}

[[noreturn]] void fail(const char* reason, const char* origin = nullptr) {
    log_line(reason);
    static Notification notification{};
    std::snprintf(notification.message, sizeof(notification.message),
                  "isShelf shortcut: %s", reason);
    (void)sceKernelSendNotificationRequest(0, &notification,
                                           sizeof(notification), 0);
    close_cleanly(origin);
}

struct JsonOwner {
    cJSON* value = nullptr;
    ~JsonOwner() { cJSON_Delete(value); }
};

bool read_json(const char* path, char* bytes, std::size_t capacity,
               JsonOwner& output) {
    if (!path || !bytes || capacity < 2) return false;
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    const auto used = std::fread(bytes, 1, capacity - 1, file);
    const bool okay = std::ferror(file) == 0 && std::fclose(file) == 0 &&
                      used > 0 && used < capacity - 1;
    if (!okay) return false;
    bytes[used] = 0;
    const char* end = nullptr;
    output.value = cJSON_ParseWithLengthOpts(bytes, used + 1, &end, 1);
    return output.value && cJSON_IsObject(output.value) && end == bytes + used;
}

bool load_owned_route(char (&origin)[10],
                      char (&route)[shelf::console_tile_message_capacity + 1]) {
    char owner_bytes[1024], config_bytes[1024], param_bytes[8192];
    JsonOwner owner, config, param;
    if (!read_json("/app0/isshelf-console-tile.json", owner_bytes,
                   sizeof(owner_bytes), owner) ||
        !read_json("/app0/forwarder.json", config_bytes,
                   sizeof(config_bytes), config) ||
        !read_json("/app0/sce_sys/param.json", param_bytes,
                   sizeof(param_bytes), param))
        return false;
    return shelf::parse_console_tile_forwarder_config(
        owner.value, config.value, param.value, origin, route);
}

int connect_local(unsigned short port, int timeout_seconds) {
    const int value = socket(AF_INET, SOCK_STREAM, 0);
    if (value < 0) return -1;
    timeval timeout{timeout_seconds, 0};
    (void)setsockopt(value, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(value, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(value, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0) {
        close(value);
        return -1;
    }
    return value;
}

bool send_all(int socket_value, const unsigned char* input, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const auto count = send(socket_value, input + sent, size - sent, 0);
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

bool relay_running() {
    const int probe = connect_local(relay_port, 1);
    if (probe < 0) return false;
    close(probe);
    return true;
}

bool ensure_relay() {
    if (relay_running()) return true;
    const int loader = connect_local(elfldr_port, 5);
    if (loader < 0) return false;
    const bool sent = send_all(loader, kTileRelayPayload,
                               sizeof(kTileRelayPayload));
    (void)shutdown(loader, SHUT_WR);
    char sink[256];
    while (recv(loader, sink, sizeof(sink), 0) > 0) {}
    close(loader);
    if (!sent) return false;
    for (unsigned attempt = 0; attempt < 40; ++attempt) {
        usleep(100000);
        if (relay_running()) return true;
    }
    return false;
}

bool queue_route(const char* origin, const char* route) {
    unsigned char request[shelf::console_tile_relay_request_size];
    if (!shelf::encode_console_tile_relay_request(
            origin, route, request, sizeof(request)))
        return false;
    const int relay = connect_local(relay_port, 5);
    if (relay < 0 || !send_all(relay, request, sizeof(request))) {
        if (relay >= 0) close(relay);
        return false;
    }
    unsigned char reply[shelf::console_tile_relay_reply_size];
    std::size_t used = 0;
    while (used < sizeof(reply)) {
        const auto count = recv(relay, reply + used, sizeof(reply) - used, 0);
        if (count <= 0) break;
        used += static_cast<std::size_t>(count);
    }
    close(relay);
    shelf::ConsoleTileRelayReply status = shelf::ConsoleTileRelayReply::invalid;
    std::int32_t code = 0;
    return shelf::decode_console_tile_relay_reply(reply, used, status, code) &&
           status == shelf::ConsoleTileRelayReply::queued && code == 0;
}
} // namespace

int main() {
    char origin[10], route[shelf::console_tile_message_capacity + 1];
    if (!load_owned_route(origin, route))
        fail("shortcut files are invalid or do not match");
    if (!ensure_relay())
        fail("the cooperative launcher could not be started", origin);
    if (!queue_route(origin, route))
        fail("Home handoff was not accepted", origin);
    log_line("route queued; requesting a clean shell close");
    close_cleanly(origin);
}
