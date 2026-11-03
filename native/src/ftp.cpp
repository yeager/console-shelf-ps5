// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/ftp.hpp"
#include "shelf/artwork.hpp"
#include "shelf/console_tile_job_pin.hpp"
#include "shelf/json.hpp"
#include "shelf/memory.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
namespace shelf {
namespace {
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
void close_socket(Socket fd) {
    if (fd != invalid_socket)
        closesocket(fd);
}
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
void close_socket(Socket fd) {
    if (fd != invalid_socket)
        close(fd);
}
#endif
constexpr std::size_t max_entries = 32768, max_directories = 4096;
struct Item {
    Item* next;
    FileStat stat;
    char name[path_capacity];
};
struct Directory {
    Directory* next;
    Item* first;
    FsStatus status;
    char path[path_capacity];
};
bool in_root(std::string_view path, std::string_view root) {
    return path == root ||
           (path.size() > root.size() && path.substr(0, root.size()) == root && path[root.size()] == '/');
}
bool allowed_path(const char* path) {
    if (!path || !valid_virtual_path(path))
        return false;
    if (std::strcmp(path, "/data/PSXS5") == 0 || std::strcmp(path, "/data/PCSX2") == 0)
        return true; // List metadata parents, without admitting their other files for reads.
    for (const char* root :
         {"/data/homebrew", "/user/app", "/data/PCSX2/games", "/data/PSXS5/games", "/data/SwanStationPS5/games",
          "/data/psswanstation/games", "/data/PSXS5/covers", "/data/PSXS5/library.txt",
          "/data/PCSX2/covers", "/data/PCSX2/chd-serials.txt", "/data/psswanstation/covers", "/data/SwanStationPS5/covers"})
        if (in_root(path, root))
            return true;
    return false;
}
bool readable(const char* path, std::uint64_t offset, std::size_t cap) {
    const std::string_view name(path);
    constexpr std::string_view tile_root = "/data/homebrew/";
    if (offset == 0 && cap <= 4096 && name.size() > tile_root.size() &&
        std::memcmp(name.data(), tile_root.data(), tile_root.size()) == 0) {
        const std::string_view rest(name.data() + tile_root.size(), name.size() - tile_root.size());
        const auto slash = rest.find('/');
        if (slash != rest.npos) {
            const std::string_view title(rest.data(), slash);
            const std::string_view leaf(rest.data() + slash, rest.size() - slash);
            if (tile_job::owned_title_known(title) &&
                (leaf == "/isshelf-console-tile.json" || leaf == "/forwarder.json"))
                return true;
        }
    }
    if (offset == 0 && cap <= 8 * 1024 * 1024 && valid_artwork_source(name)) return true;
    if (offset == 0 && cap <= 65536 && (name == "/data/PSXS5/library.txt" || name == "/data/PCSX2/chd-serials.txt")) return true;
    const auto dot = name.rfind('.');
    const auto ext = dot == name.npos ? std::string_view{} : std::string_view(name.data() + dot, name.size() - dot);
    char folded[16]{};
    if (ext.size() >= sizeof(folded))
        return false;
    for (std::size_t i = 0; i < ext.size(); ++i)
        folded[i] = ext[i] >= 'A' && ext[i] <= 'Z' ? static_cast<char>(ext[i] + 32) : ext[i];
    const std::string_view lower(folded);
    if (offset == 0 && cap <= 65536 && (lower == ".json" || lower == ".sfo" || lower == ".cue" || lower == ".m3u")) {
        // JSON is only public app metadata, never emulator settings or account files.
        return lower != ".json" || (name.size() >= 19 && name.substr(name.size() - 19) == "/sce_sys/param.json");
    }
    return cap <= 8 && (offset == 0 || offset == 0x8000) &&
           (lower == ".z64" || lower == ".v64" || lower == ".n64" || lower == ".iso" || lower == ".chd");
}
int socket_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}
bool cancelled(const ReadOnlyFtp& client) {
    return client.cancelled && client.cancelled(client.cancel_context);
}
bool wait_socket(Socket fd, bool write, ReadOnlyFtp& client) {
    for (int attempt = 0; attempt < 60; ++attempt) {
        if (cancelled(client)) {
            client.diagnostics.failed("cancelled", 0);
            return false;
        }
        fd_set ready, errors;
        FD_ZERO(&ready);
        FD_ZERO(&errors);
        FD_SET(fd, &ready);
        FD_SET(fd, &errors);
        timeval slice{0, 50000};
        const int result =
            select(static_cast<int>(fd + 1), write ? nullptr : &ready, write ? &ready : nullptr, &errors, &slice);
        if (result < 0 || FD_ISSET(fd, &errors)) {
            client.diagnostics.failed("socket wait", socket_error());
            return false;
        }
        if (result > 0 && FD_ISSET(fd, &ready))
            return true;
    }
    client.diagnostics.failed("socket timeout", 0);
    return false;
}
Socket connect_to(ReadOnlyFtp& client, unsigned short port, bool passive = false) {
    auto& diagnostics = client.diagnostics;
    const char* operation = passive ? "data socket" : "control socket";
    const auto failed = [&](const char* stage, Socket fd, int error) {
        diagnostics.failed(stage, error);
        close_socket(fd);
        return invalid_socket;
    };
    Socket fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == invalid_socket)
        return failed(operation, fd, socket_error());
#ifndef _WIN32
    if (fd >= FD_SETSIZE)
        return failed("socket descriptor limit", fd, fd);
#endif
#ifdef _WIN32
    const DWORD timeout = 3000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    const timeval timeout{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    sockaddr_in address{};
#ifdef SHELF_PS5
    address.sin_len = sizeof(address);
#endif
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, client.host, &address.sin_addr) != 1) {
        return failed("numeric address", fd, 0);
    }
#ifdef _WIN32
    u_long nonblocking = 1;
    if (ioctlsocket(fd, FIONBIO, &nonblocking)) {
        return failed("socket nonblocking", fd, socket_error());
    }
#elif defined(SHELF_PS5)
    // Native titles reject fcntl on sockets. The console's own socket option
    // configures this mode without changing process or system permissions.
    constexpr int native_nonblocking = 0x1200; // SO_NBIO
    const int nonblocking = 1;
    if (setsockopt(fd, SOL_SOCKET, native_nonblocking, &nonblocking, sizeof(nonblocking)))
        return failed("socket nonblocking option", fd, socket_error());
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    if (fd >= FD_SETSIZE) {
        return failed("socket descriptor limit", fd, static_cast<int>(fd));
    }
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
        return failed("socket nonblocking", fd, socket_error());
    }
#endif
    const int connected = connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (connected != 0) {
#ifdef _WIN32
        const int connect_error = WSAGetLastError();
        if (connect_error != WSAEWOULDBLOCK && connect_error != WSAEINPROGRESS) {
            return failed(passive ? "data connect" : "control connect", fd, connect_error);
        }
#else
        if (errno != EINPROGRESS) {
            return failed(passive ? "data connect" : "control connect", fd, socket_error());
        }
#endif
        if (!wait_socket(fd, true, client))
            return failed("socket connect wait", fd, 0);
        int error = 0;
#ifdef _WIN32
        int error_size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &error_size) || error) {
            return failed("socket connect result", fd, error ? error : socket_error());
        }
#else
        socklen_t error_size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) || error) {
            return failed("socket connect result", fd, error ? error : socket_error());
        }
#endif
    }
    // Keep nonblocking mode. Bounded readiness waits also make startup cancellation responsive.
    return fd;
}
struct Connection {
    Socket control = invalid_socket, data = invalid_socket;
    char buffer[4096]{};
    int begin = 0, end = 0;
    bool clean_eof = false;
    FtpDiagnostics* diagnostics = nullptr;
    ReadOnlyFtp* client = nullptr;
    ~Connection() {
        close_socket(data);
        close_socket(control);
    }
    bool line(char* output, std::size_t cap) {
        std::size_t used = 0;
        clean_eof = false;
        while (used + 1 < cap) {
            if (begin == end) {
                end = client && wait_socket(control, false, *client)
                          ? static_cast<int>(recv(control, buffer, sizeof(buffer), 0))
                          : -1;
                begin = 0;
                if (end <= 0) {
                    if (end < 0 && diagnostics)
                        diagnostics->failed("receive", socket_error());
                    clean_eof = end == 0 && used == 0;
                    return false;
                }
            }
            const char value = buffer[begin++];
            if (!value)
                return false;
            if (value == '\n') {
                if (used && output[used - 1] == '\r')
                    --used;
                output[used] = 0;
                return true;
            }
            output[used++] = value;
        }
        return false;
    }
    int reply(char* output, std::size_t cap) {
        if (!line(output, cap) || std::strlen(output) < 3)
            return 0;
        char code[4]{};
        std::memcpy(code, output, 3);
        for (int i = 0; i < 3; ++i)
            if (code[i] < '0' || code[i] > '9')
                return 0;
        if (output[3] == '-') {
            unsigned lines = 0;
            do {
                if (++lines > 32 || !line(output, cap))
                    return 0;
            } while (std::strncmp(output, code, 3) || output[3] != ' ');
        }
        return std::atoi(code);
    }
    int command(const char* verb, const char* argument = nullptr, char* result = nullptr, std::size_t cap = 0) {
        char request[path_capacity + 64], response[2048];
        if (argument && (std::strchr(argument, '\r') || std::strchr(argument, '\n')))
            return 0;
        const int size =
            std::snprintf(request, sizeof(request), "%s%s%s\r\n", verb, argument ? " " : "", argument ? argument : "");
        if (size <= 0 || size >= static_cast<int>(sizeof(request)))
            return 0;
        int sent = 0;
        while (sent < size) {
            if (!client || !wait_socket(control, true, *client))
                return 0;
            const int n = static_cast<int>(send(control, request + sent, size - sent,
#ifdef MSG_NOSIGNAL
                                                MSG_NOSIGNAL
#else
                                                0
#endif
                                                ));
            if (n <= 0) {
                if (diagnostics)
                    diagnostics->failed("send", socket_error());
                return 0;
            }
            sent += n;
        }
        return reply(result ? result : response, result ? cap : sizeof(response));
    }
    bool open(ReadOnlyFtp& source) {
        client = &source;
        diagnostics = &source.diagnostics;
        control = connect_to(source, source.port);
        if (control == invalid_socket)
            return false;
        char reply_text[2048];
        const int greeting = reply(reply_text, sizeof(reply_text));
        if (greeting != 220) {
            diagnostics->failed("control greeting", greeting);
            return false;
        }
        const int user = command("USER", "anonymous");
        if (!(user == 230 || (user == 331 && command("PASS", "") == 230))) {
            diagnostics->failed("anonymous login", user);
            return false;
        }
        const int type = command("TYPE", "I");
        if (type != 200) {
            diagnostics->failed("binary transfer mode", type);
            return false;
        }
        ++diagnostics->connections;
        return true;
    }
    bool passive(ReadOnlyFtp& source) {
        char response[2048];
        unsigned port = 0;
        if (command("EPSV", nullptr, response, sizeof(response)) == 229) {
            const char* start = std::strchr(response, '(');
            if (start && std::strlen(start) > 5 && start[1] == start[2] && start[2] == start[3]) {
                char* end_port = nullptr;
                port = static_cast<unsigned>(std::strtoul(start + 4, &end_port, 10));
                if (!end_port || *end_port != start[1])
                    port = 0;
            }
        } else if (command("PASV", nullptr, response, sizeof(response)) == 227) {
            const char* start = std::strchr(response, '(');
            unsigned a, b, c, d, hi, lo;
            if (start && std::sscanf(start, "(%u,%u,%u,%u,%u,%u)", &a, &b, &c, &d, &hi, &lo) == 6 && a <= 255 &&
                b <= 255 && c <= 255 && d <= 255 && hi <= 255 && lo <= 255)
                port = hi * 256 + lo;
        }
        if (!port || port > 65535) {
            if (diagnostics)
                diagnostics->failed("passive reply", 0);
            return false;
        }
        // Never follow a server-provided IP address or hostname.
        data = connect_to(source, static_cast<unsigned short>(port), true);
        return data != invalid_socket;
    }
};
FsStatus transfer_status(int code) {
    return code == 550 ? FsStatus::missing : FsStatus::failed;
}
Directory* get_directory(ReadOnlyFtp& client, const char* path) {
    if (cancelled(client))
        return nullptr;
    for (auto* dir = static_cast<Directory*>(client.directories); dir; dir = dir->next)
        if (std::strcmp(dir->path, path) == 0)
            return dir;
    if (client.directory_count >= max_directories)
        return nullptr;
    auto* dir = static_cast<Directory*>(allocate_zero(1, sizeof(Directory)));
    if (!dir)
        return nullptr;
    std::strcpy(dir->path, path);
    dir->status = FsStatus::failed;
    dir->next = static_cast<Directory*>(client.directories);
    client.directories = dir;
    ++client.directory_count;
    Connection connection;
    if (!connection.open(client) || !connection.passive(client))
        return dir;
    char response[2048];
    const int code = connection.command("LIST", path, response, sizeof(response));
    if (code != 125 && code != 150) {
        dir->status = std::strstr(response, "denied") || std::strstr(response, "permitted") ? FsStatus::denied
                                                                                            : transfer_status(code);
        return dir;
    }
    const Socket saved = connection.control;
    // A fast server may send both 150 and 226 in one control read. Directory
    // data has its own buffer; preserve unread control replies across the switch.
    char pending_control[4096];
    const int pending_bytes = connection.end - connection.begin;
    std::memcpy(pending_control, connection.buffer + connection.begin, static_cast<std::size_t>(pending_bytes));
    connection.begin = connection.end = 0;
    connection.control = connection.data;
    connection.data = invalid_socket;
    char line[2048];
    bool valid = true, eof = false;
    for (;;) {
        // Reuse the bounded line reader for directory data; EOF must be clean.
        if (!connection.line(line, sizeof(line))) {
            eof = connection.clean_eof;
            break;
        }
        const char* at = line;
        const char* fields[8]{};
        std::size_t lengths[8]{};
        for (int i = 0; i < 8; ++i) {
            while (*at == ' ' || *at == '\t')
                ++at;
            fields[i] = at;
            while (*at && *at != ' ' && *at != '\t')
                ++at;
            lengths[i] = static_cast<std::size_t>(at - fields[i]);
        }
        while (*at == ' ' || *at == '\t')
            ++at;
        if (!lengths[0] || !lengths[7] || !*at || lengths[4] > 20) {
            valid = false;
            break;
        }
        if (line[0] == 'l')
            continue;
        if (line[0] != 'd' && line[0] != '-') {
            valid = false;
            break;
        }
        if (std::strcmp(at, ".") == 0 || std::strcmp(at, "..") == 0)
            continue;
        if (!valid_utf8(at) || std::strchr(at, '/') || std::strchr(at, '\\') || std::strlen(at) >= path_capacity) {
            valid = false;
            break;
        }
        std::uint64_t size = 0;
        for (std::size_t i = 0; i < lengths[4]; ++i) {
            const char digit = fields[4][i];
            if (digit < '0' || digit > '9' || size > (UINT64_MAX - static_cast<unsigned>(digit - '0')) / 10) {
                valid = false;
                break;
            }
            size = size * 10 + static_cast<unsigned>(digit - '0');
        }
        if (!valid)
            break;
        if (client.entry_count >= max_entries) {
            dir->status = FsStatus::limit;
            break;
        }
        auto* item = static_cast<Item*>(allocate_zero(1, sizeof(Item)));
        if (!item) {
            valid = false;
            break;
        }
        std::strcpy(item->name, at);
        item->stat.directory = line[0] == 'd';
        item->stat.size = size;
        item->next = dir->first;
        dir->first = item;
        ++client.entry_count;
    }
    close_socket(connection.control);
    connection.control = saved;
    std::memcpy(connection.buffer, pending_control, static_cast<std::size_t>(pending_bytes));
    connection.begin = 0;
    connection.end = pending_bytes;
    const int finish = connection.reply(response, sizeof(response));
    if (dir->status != FsStatus::limit)
        dir->status = valid && eof && (finish == 226 || finish == 250) ? FsStatus::ok : FsStatus::failed;
    if (dir->status == FsStatus::ok)
        ++client.diagnostics.listings;
    else
        client.diagnostics.failed(valid && eof ? "listing completion" : "listing data", finish);
    return dir;
}
FsStatus ftp_list(void* raw, const char* path, EntryVisitor visitor, void* destination) {
    if (!allowed_path(path) || !visitor)
        return FsStatus::invalid;
    auto* dir = get_directory(*static_cast<ReadOnlyFtp*>(raw), path);
    if (!dir)
        return FsStatus::limit;
    if (dir->status != FsStatus::ok)
        return dir->status;
    for (auto* item = dir->first; item; item = item->next)
        if (!visitor(destination, item->name, item->stat))
            return FsStatus::limit;
    return FsStatus::ok;
}
FsStatus ftp_stat(void* raw, const char* path, FileStat& result) {
    if (!allowed_path(path))
        return FsStatus::invalid;
    // A root is established by a successful listing, never by assumed existence.
    const std::string_view value(path);
    const auto split = value.rfind('/');
    char parent[path_capacity];
    std::memcpy(parent, path, split);
    parent[split] = 0;
    if (!allowed_path(parent)) {
        const auto* dir = get_directory(*static_cast<ReadOnlyFtp*>(raw), path);
        if (!dir)
            return FsStatus::limit;
        if (dir->status == FsStatus::ok) {
            result = {};
            result.directory = true;
        }
        return dir->status;
    }
    const auto* dir = get_directory(*static_cast<ReadOnlyFtp*>(raw), parent);
    if (!dir)
        return FsStatus::limit;
    if (dir->status != FsStatus::ok)
        return dir->status;
    for (const auto* item = dir->first; item; item = item->next)
        if (std::strcmp(item->name, path + split + 1) == 0) {
            result = item->stat;
            return FsStatus::ok;
        }
    return FsStatus::missing;
}
FsStatus ftp_read(void* raw, const char* path, std::uint64_t offset, void* output, std::size_t cap, std::size_t& used) {
    used = 0;
    if (!allowed_path(path) || !readable(path, offset, cap) || !output || !cap)
        return FsStatus::denied;
    FileStat stat;
    const auto status = ftp_stat(raw, path, stat);
    if (status != FsStatus::ok)
        return status;
    if (stat.directory || offset > stat.size)
        return FsStatus::invalid;
    Connection connection;
    auto& client = *static_cast<ReadOnlyFtp*>(raw);
    if (!connection.open(client) || !connection.passive(client))
        return FsStatus::failed;
    if (offset) {
        char position[32];
        std::snprintf(position, sizeof(position), "%llu", static_cast<unsigned long long>(offset));
        if (connection.command("REST", position) != 350)
            return FsStatus::failed;
    }
    const int code = connection.command("RETR", path);
    if (code != 125 && code != 150)
        return transfer_status(code);
    const auto need = static_cast<std::size_t>(std::min<std::uint64_t>(cap, stat.size - offset));
    while (used < need) {
        if (!wait_socket(connection.data, false, client))
            return FsStatus::failed;
        const int size = static_cast<int>(
            recv(connection.data, static_cast<char*>(output) + used, static_cast<int>(need - used), 0));
        if (size <= 0)
            return FsStatus::failed;
        used += static_cast<std::size_t>(size);
    }
    // A bounded header read deliberately closes its dedicated connection early.
    ++client.diagnostics.reads;
    return FsStatus::ok;
}
} // namespace
ReadOnlyFtp::~ReadOnlyFtp() {
    clear_ftp_cache(*this);
#ifdef _WIN32
    if (network_ready)
        WSACleanup();
#endif
}
void clear_ftp_cache(ReadOnlyFtp& client) {
    auto* dir = static_cast<Directory*>(client.directories);
    while (dir) {
        auto* next = dir->next;
        auto* item = dir->first;
        while (item) {
            auto* after = item->next;
            release(item);
            item = after;
        }
        release(dir);
        dir = next;
    }
    client.directories = nullptr;
    client.entry_count = client.directory_count = 0;
}
bool init_read_only_ftp(ReadOnlyFtp& client, const char* host, unsigned short port) {
    if (!host || std::strlen(host) >= sizeof(client.host) || !port)
        return false;
#ifdef _WIN32
    if (!client.network_ready) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            return false;
        client.network_ready = true;
    }
#endif
    in_addr address{};
    if (inet_pton(AF_INET, host, &address) != 1)
        return false;
    std::strcpy(client.host, host);
    client.port = port;
    client.network_ready = true;
    return true;
}
ReadOnlyFs ftp_filesystem(ReadOnlyFtp& client) {
    return {&client, ftp_list, ftp_stat, ftp_read};
}
} // namespace shelf
