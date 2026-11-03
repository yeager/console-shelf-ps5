// SPDX-License-Identifier: GPL-3.0-only
// Shelf's cooperative launcher. Public launch ABI/architecture references:
// Jewel Case helper/jc-launch.c at 765297d2decd1be6c8076261f484e36724250bd7
// OpenOrbis include/orbis/_types/sys_service.h at 06fcf7fd108fa080035cdd3259ed22eb07e0508c,
// websrv src/ps5/sys.c at 7748df9d2a43822a5ebfda149f43d4f2696d640b,
// and ShadowMountPlus src/sm_shellcore_flags.c at 9ffcbe131575153266e951ab473bb6fd100d8c80.
// One exact isShelf-origin close is allowed only after a committed handoff has
// remained stuck for five seconds. No arbitrary process kill, external
// file-write requests, new server, payload autoload or firmware-update call.
// Tile removal may send one fixed loopback uninstall request after Shelf closes.
// Bulk artwork refresh may send one bounded, validated list of owned-title
// uninstalls followed by exactly one scan. Residual cleanup accepts only the
// complete preferred-title sequence and preflights every global registration
// metadata/icon pair before its first uninstall; neither mode can select an
// arbitrary title.
#include "shelf/launch_protocol.hpp"
#include "shelf/catalog.generated.hpp"
#include "shelf/update_helper.hpp"
#include "shelf/update_relaunch.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

extern "C" {
int sceUserServiceInitialize(void*);
int sceUserServiceGetForegroundUser(std::int32_t*);
int sceSystemServiceGetAppIdOfRunningBigApp();
int sceSystemServiceGetAppTitleId(int, char*);
int sceLncUtilInitialize();
int sceLncUtilKillApp(int);
struct AppLaunchContext {
    std::uint32_t size;
    std::int32_t user;
    std::uint32_t options;
    std::uint64_t crash_report;
    std::uint64_t check_flags;
};
int sceSystemServiceLaunchApp(const char*, const char**, AppLaunchContext*);
int sceLncUtilLaunchApp(const char*, const char**, AppLaunchContext*);
int sceKernelOpenEventFlag(std::intptr_t*, const char*);
int sceKernelPollEventFlag(std::intptr_t, std::uint64_t, unsigned, std::uint64_t*);
int sceKernelCloseEventFlag(std::intptr_t);
int sceKernelSendNotificationRequest(std::int32_t, void*, std::size_t, std::int32_t);
}
static_assert(sizeof(AppLaunchContext) == 32 && offsetof(AppLaunchContext, user) == 4 &&
              offsetof(AppLaunchContext, check_flags) == 24, "Unexpected launch ABI");
namespace {
constexpr char exact_origin_close_marker[] =
    "ISSHELF_EXACT_ORIGIN_CLOSE_V1";
constexpr char exact_update_relaunch_marker[] =
    "ISSHELF_EXACT_UPDATE_RELAUNCH_V4";
[[gnu::used]] constexpr char update_debug_notice_marker[] =
    "ISSHELF_UPDATE_DEBUG_NOTICES_V1";
constexpr char request_path[] = "/data/console-shelf/launch-request.txt";
constexpr char status_path[] = "/data/console-shelf/launch-status.txt";
constexpr char status_temporary[] = "/data/console-shelf/launch-status.tmp";
struct DebugNotification {
    std::int32_t type;
    std::int32_t request_id;
    std::int32_t priority;
    std::int32_t message_id;
    std::int32_t target_id;
    std::int32_t user_id;
    std::int32_t reserved_a;
    std::int32_t reserved_b;
    std::int32_t app_id;
    std::int32_t error_number;
    std::int32_t reserved_c;
    char use_icon_uri;
    char message[1024];
    char uri[1024];
    char reserved_text[1024];
    char padding[3];
};
static_assert(sizeof(DebugNotification) == 0xc30 &&
              offsetof(DebugNotification, message) == 0x2d,
              "Unexpected PS5 debug notification ABI");

void debug_notice(const char* text) {
    static DebugNotification notification{};
    std::memset(&notification, 0, sizeof(notification));
    notification.target_id = -1;
    std::snprintf(notification.message, sizeof(notification.message), "%s", text ? text : "");
    (void)sceKernelSendNotificationRequest(0, &notification, sizeof(notification), 0);
}

void update_stage_notice(void*, shelf::UpdateTransactionStage stage, const char* tag) {
    char message[256];
    const char* version = tag && tag[0] ? tag : "the selected version";
    switch (stage) {
    case shelf::UpdateTransactionStage::verifying:
        std::snprintf(message, sizeof(message), "isShelf update: verifying staged files...");
        break;
    case shelf::UpdateTransactionStage::recovering:
        std::snprintf(message, sizeof(message), "isShelf update: resuming the interrupted transaction...");
        break;
    case shelf::UpdateTransactionStage::installing:
        std::snprintf(message, sizeof(message), "Installing isShelf %s...", version);
        break;
    case shelf::UpdateTransactionStage::restoring:
        std::snprintf(message, sizeof(message), "isShelf update: restoring the previous program files...");
        break;
    case shelf::UpdateTransactionStage::finishing:
        std::snprintf(message, sizeof(message), "Finishing isShelf %s installation...", version);
        break;
    }
    debug_notice(message);
}

struct Context {
    const shelf::LaunchRequest* request = nullptr;
    int origin_app = 0, target_app = 0;
    std::int32_t user = -1;
    int launch_status = 0;
    int probe_status = 0;
    int update_relaunch_observation = -2;
    unsigned update_relaunch_attempts = 0;
    std::intptr_t focus_handle = -1;
    bool focus_open = false;
    FILE* journal = nullptr;
    ~Context() {
        if (focus_open)
            sceKernelCloseEventFlag(focus_handle);
        if (journal)
            std::fclose(journal);
    }
};
std::uint64_t now(void*) {
    const auto value = std::time(nullptr);
    return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}
bool status(const shelf::LaunchRequest& request, shelf::LaunchStage state, int code) {
    shelf::LaunchStatus reply;
    std::memcpy(reply.id, request.id, sizeof(reply.id));
    reply.issued = request.issued;
    reply.stage = state;
    reply.code = static_cast<std::uint32_t>(code);
    char message[256];
    if (!shelf::encode_launch_status(reply, message, sizeof(message)))
        return false;
    // Never replace a newer request's reply, even if this helper finishes late.
    FILE* prior = std::fopen(status_path, "rb");
    if (!prior)
        return false;
    char existing[256];
    const auto count = std::fread(existing, 1, sizeof(existing), prior);
    const bool bad = std::ferror(prior) != 0;
    std::fclose(prior);
    shelf::LaunchStatus owner;
    if (bad || !shelf::launch_status_owned_by_request({existing, count}, request, owner))
        return false;
    FILE* file = std::fopen(status_temporary, "wb");
    if (!file)
        return false;
    bool okay = std::fwrite(message, 1, std::strlen(message), file) == std::strlen(message);
    okay = std::fflush(file) == 0 && okay;
    okay = std::fclose(file) == 0 && okay;
    if (okay && std::rename(status_temporary, status_path) == 0)
        return true;
    std::remove(status_temporary);
    return false;
}
int current(void* raw, char (&title)[10]) {
    auto& context = *static_cast<Context*>(raw);
    const int id = sceSystemServiceGetAppIdOfRunningBigApp();
    if (context.journal) {
        std::fprintf(context.journal, "current app: 0x%08x\n", static_cast<unsigned>(id));
        std::fflush(context.journal);
    }
    // -1 and zero are the no-app values used by the reference helper. Other
    // errors must not become permission to launch over an unknown application.
    if (id == 0 || id == -1)
        return 0;
    if (id < 0) {
        context.probe_status = id;
        return -1;
    }
    char value[32]{};
    const int title_status = sceSystemServiceGetAppTitleId(id, value);
    context.probe_status = title_status;
    if (context.journal) {
        std::fprintf(context.journal, "title query: 0x%08x; title: %.31s\n", static_cast<unsigned>(title_status), value);
        std::fflush(context.journal);
    }
    // During a cooperative close the PS5 can retain the exact isShelf app ID
    // for one or more probes after its title has become temporarily
    // unavailable. The protocol treats this as neither idle nor permission to
    // proceed: it must subsequently observe a stable no-app interval.
    if (static_cast<unsigned>(title_status) == 0x80940004u &&
        context.origin_app && context.origin_app == id) {
        if (context.journal) {
            std::fprintf(context.journal,
                         "exact origin release pending: app=0x%08x\n",
                         static_cast<unsigned>(id));
            std::fflush(context.journal);
        }
        return shelf::handoff_origin_releasing;
    }
    if (title_status != 0 || !std::memchr(value, 0, sizeof(value)) ||
        std::strlen(value) != 9)
        return -1;
    for (unsigned i = 0; i < 9; ++i)
        if (!((value[i] >= 'A' && value[i] <= 'Z') || (value[i] >= '0' && value[i] <= '9')))
            return -1;
    if (std::strcmp(value, shelf::shelf_title_id) == 0) {
        if (context.origin_app && context.origin_app != id)
            return -1;
        context.origin_app = id;
    }
    if (context.request && std::strcmp(value, context.request->title) == 0) {
        if (context.target_app && context.target_app != id)
            return -1;
        context.target_app = id;
    }
    std::memcpy(title, value, 10);
    return 1;
}
bool ready(void* raw, const char* id) {
    const auto& context = *static_cast<Context*>(raw);
    return context.request && std::strcmp(id, context.request->id) == 0 &&
           status(*context.request, shelf::LaunchStage::ready, 0);
}
shelf::HandoffConsent consent(void* raw) {
    const auto& context = *static_cast<Context*>(raw);
    FILE* file = std::fopen(status_path, "rb");
    if (!file || !context.request) {
        if (file)
            std::fclose(file);
        return shelf::HandoffConsent::cancelled;
    }
    char message[256];
    const auto size = std::fread(message, 1, sizeof(message), file);
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    shelf::LaunchStatus reply;
    if (failed || !shelf::parse_launch_status({message, size}, now(nullptr), reply) ||
        std::strcmp(reply.id, context.request->id) != 0 || reply.issued != context.request->issued)
        return shelf::HandoffConsent::cancelled;
    if (reply.stage == shelf::LaunchStage::committed)
        return shelf::HandoffConsent::committed;
    return reply.stage == shelf::LaunchStage::ready ? shelf::HandoffConsent::pending : shelf::HandoffConsent::cancelled;
}
int close_origin(void* raw) {
    auto& context = *static_cast<Context*>(raw);
    if (!context.request || !context.origin_app ||
        consent(raw) != shelf::HandoffConsent::committed)
        return -1;
    const int app = sceSystemServiceGetAppIdOfRunningBigApp();
    char title[32]{};
    const int title_status = app > 0 ? sceSystemServiceGetAppTitleId(app, title) : -1;
    if (app != context.origin_app || title_status != 0 ||
        !std::memchr(title, 0, sizeof(title)) ||
        std::strcmp(title, shelf::shelf_title_id) != 0)
        return -1;
    const int result = sceLncUtilKillApp(app);
    if (context.journal) {
        std::fprintf(context.journal,
                     "exact origin close: app=0x%08x title=%s result=0x%08x attempts=1\n",
                     static_cast<unsigned>(app), title, static_cast<unsigned>(result));
        std::fflush(context.journal);
    }
    return result;
}
void wait(void*, unsigned milliseconds) {
    usleep(milliseconds * 1000);
}
shelf::UpdateRelaunchState update_relaunch_state(void* raw) {
    auto& context = *static_cast<Context*>(raw);
    const int id = sceSystemServiceGetAppIdOfRunningBigApp();
    shelf::UpdateRelaunchState state = shelf::UpdateRelaunchState::unknown;
    char title[32]{};
    int title_status = -1;
    if (id == 0 || id == -1) {
        state = shelf::UpdateRelaunchState::idle;
    } else if (id > 0) {
        title_status = sceSystemServiceGetAppTitleId(id, title);
        if (title_status == 0 && std::memchr(title, 0, sizeof(title)) &&
            std::strlen(title) == 9) {
            state = std::strcmp(title, shelf::shelf_title_id) == 0
                ? shelf::UpdateRelaunchState::target
                : shelf::UpdateRelaunchState::other_app;
        }
    }
    const int observed = static_cast<int>(state);
    if (context.journal && observed != context.update_relaunch_observation) {
        std::fprintf(context.journal,
                     "isShelf update relaunch observation: state=%d app=0x%08x title_status=0x%08x title=%.31s\n",
                     observed, static_cast<unsigned>(id), static_cast<unsigned>(title_status), title);
        std::fflush(context.journal);
        context.update_relaunch_observation = observed;
    }
    return state;
}
int launch_update_target(void* raw) {
    auto& context = *static_cast<Context*>(raw);
    AppLaunchContext params{};
    params.size = sizeof(params);
    std::int32_t user = -1;
    const int user_status = sceUserServiceGetForegroundUser(&user);
    int result = user_status;
    if (user_status == 0 && user >= 0 && user == context.user) {
        debug_notice("Starting isShelf...");
        params.user = user;
        result = sceLncUtilLaunchApp(shelf::shelf_title_id, nullptr, &params);
    } else if (user_status == 0) {
        result = -EPERM;
    }
    ++context.update_relaunch_attempts;
    if (context.journal) {
        std::fprintf(context.journal,
                     "isShelf update relaunch attempt %u: sceLncUtilLaunchApp(%s, user %d)="
                     "0x%08x user_query=0x%08x settle_ms=%u observe_ms=%u single_request=1\n",
                     context.update_relaunch_attempts, shelf::shelf_title_id,
                     user, static_cast<unsigned>(result), static_cast<unsigned>(user_status),
                     shelf::update_relaunch_settle_ms,
                     shelf::update_relaunch_observe_ms);
        std::fflush(context.journal);
    }
    return result;
}
enum class ResidualRegistration { absent, exact, conflict, failed };
enum class PathState { missing, present, failed };
struct Descriptor {
    int value = -1;
    Descriptor() = default;
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    ~Descriptor() { if (value >= 0) close(value); }
};
PathState path_state(const char* path) {
    struct stat info{};
    if (stat(path, &info) == 0)
        return PathState::present;
    return errno == ENOENT ? PathState::missing : PathState::failed;
}
bool same_identity(const struct stat& named, const struct stat& held) {
    return named.st_dev == held.st_dev && named.st_ino == held.st_ino &&
           named.st_uid == held.st_uid && named.st_gid == held.st_gid &&
           named.st_mode == held.st_mode;
}
shelf::MaintenanceEndpointProbe probe_shadowmount(void*) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return shelf::MaintenanceEndpointProbe::failed;
    timeval timeout{1, 0};
    const bool configured =
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0 &&
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0;
    sockaddr_in address{};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(10101);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int connected = configured
        ? connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address))
        : -1;
    const int error = connected == 0 ? 0 : errno;
    close(fd);
    if (connected == 0)
        return shelf::MaintenanceEndpointProbe::ready;
    return error == ECONNREFUSED || error == ETIMEDOUT
        ? shelf::MaintenanceEndpointProbe::unavailable
        : shelf::MaintenanceEndpointProbe::failed;
}
bool bind_path(const char* path, bool directory, bool platform_owned,
               Descriptor& descriptor, std::size_t* bytes = nullptr) {
    struct stat named{};
    if (!path || stat(path, &named) != 0 ||
        (directory ? !S_ISDIR(named.st_mode) : !S_ISREG(named.st_mode)))
        return false;
    if (platform_owned &&
        (named.st_uid != 0 || named.st_gid != 0 ||
         (named.st_mode & 0777) != static_cast<mode_t>(directory ? 0777 : 0666)))
        return false;
    int flags = O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC;
    if (directory) flags |= O_DIRECTORY;
    descriptor.value = open(path, flags);
    if (descriptor.value < 0)
        return false;
    struct stat held{};
    if (fstat(descriptor.value, &held) != 0 || !same_identity(named, held) ||
        (directory ? !S_ISDIR(held.st_mode) : !S_ISREG(held.st_mode)) ||
        held.st_size < 0)
        return false;
    if (bytes) *bytes = static_cast<std::size_t>(held.st_size);
    return true;
}
bool read_exact(int fd, unsigned char* output, std::size_t bytes) {
    std::size_t used = 0;
    while (used < bytes) {
        const auto count = read(fd, output + used, bytes - used);
        if (count <= 0)
            return false;
        used += static_cast<std::size_t>(count);
    }
    return true;
}
bool files_equal(const char* left_path, bool left_platform,
                 const char* right_path, bool right_platform,
                 std::size_t limit) {
    Descriptor left, right;
    std::size_t left_size = 0, right_size = 0;
    if (!bind_path(left_path, false, left_platform, left, &left_size) ||
        !bind_path(right_path, false, right_platform, right, &right_size) ||
        !left_size || left_size != right_size || left_size > limit)
        return false;
    constexpr std::size_t block = 4096;
    auto* memory = static_cast<unsigned char*>(std::malloc(block * 2));
    if (!memory)
        return false;
    bool equal = true;
    std::size_t remaining = left_size;
    while (equal && remaining) {
        const auto amount = remaining < block ? remaining : block;
        equal = read_exact(left.value, memory, amount) &&
                read_exact(right.value, memory + block, amount) &&
                std::memcmp(memory, memory + block, amount) == 0;
        remaining -= amount;
    }
    std::free(memory);
    return equal;
}
bool read_small_platform_file(const char* path, char*& output,
                              std::size_t& bytes) {
    output = nullptr;
    bytes = 0;
    Descriptor file;
    if (!bind_path(path, false, true, file, &bytes) || !bytes || bytes > 4096)
        return false;
    output = static_cast<char*>(std::malloc(bytes));
    if (!output)
        return false;
    if (read_exact(file.value, reinterpret_cast<unsigned char*>(output), bytes))
        return true;
    std::free(output);
    output = nullptr;
    bytes = 0;
    return false;
}
bool title_path(char (&output)[256], const char* prefix, const char* title,
                const char* suffix = "") {
    const int used = std::snprintf(output, sizeof(output), "%s%s%s",
                                   prefix, title, suffix);
    return used > 0 && static_cast<std::size_t>(used) < sizeof(output);
}
ResidualRegistration residual_registration(const char* title,
                                           std::size_t expected_system) {
    std::size_t system = 0;
    if (!title || !shelf::tile_residual_title_index(title, system) ||
        system != expected_system || system >= shelf::catalog.size())
        return ResidualRegistration::failed;
    char active[256], app[256], appmeta[256], sce[256];
    if (!title_path(active, "/data/homebrew/", title) ||
        !title_path(app, "/user/app/", title) ||
        !title_path(appmeta, "/user/appmeta/", title) ||
        !title_path(sce, "/user/app/", title, "/sce_sys"))
        return ResidualRegistration::failed;
    const auto active_state = path_state(active);
    const auto app_state = path_state(app);
    const auto meta_state = path_state(appmeta);
    if (active_state == PathState::failed || app_state == PathState::failed ||
        meta_state == PathState::failed)
        return ResidualRegistration::failed;
    if (active_state != PathState::missing)
        return ResidualRegistration::conflict;
    if (app_state == PathState::missing && meta_state == PathState::missing)
        return ResidualRegistration::absent;
    if (app_state != PathState::present || meta_state != PathState::present)
        return ResidualRegistration::conflict;
    Descriptor app_directory, meta_directory, sce_directory;
    if (!bind_path(app, true, true, app_directory) ||
        !bind_path(appmeta, true, true, meta_directory) ||
        !bind_path(sce, true, true, sce_directory))
        return ResidualRegistration::conflict;

    char app_param[256], meta_param[256];
    if (!title_path(app_param, "/user/app/", title,
                    "/sce_sys/param.json") ||
        !title_path(meta_param, "/user/appmeta/", title, "/param.json") ||
        !files_equal(app_param, true, meta_param, true, 4096))
        return ResidualRegistration::conflict;
    char* metadata = nullptr;
    std::size_t metadata_bytes = 0;
    if (!read_small_platform_file(app_param, metadata, metadata_bytes))
        return ResidualRegistration::failed;
    const bool metadata_exact = shelf::tile_residual_param_identity(
        {metadata, metadata_bytes}, title);
    std::free(metadata);
    if (!metadata_exact)
        return ResidualRegistration::conflict;

    char bundle_icon[256], app_icon[256], sce_icon[256], meta_icon[256];
    const auto id = shelf::catalog[system].id;
    const int bundle_used = std::snprintf(
        bundle_icon, sizeof(bundle_icon),
        "/data/homebrew/PPSA99541/assets/console-tiles/icons/%.*s.png",
        static_cast<int>(id.size()), id.data());
    if (bundle_used <= 0 || static_cast<std::size_t>(bundle_used) >= sizeof(bundle_icon) ||
        !title_path(app_icon, "/user/app/", title, "/icon0.png") ||
        !title_path(sce_icon, "/user/app/", title, "/sce_sys/icon0.png") ||
        !title_path(meta_icon, "/user/appmeta/", title, "/icon0.png") ||
        !files_equal(bundle_icon, false, app_icon, true, 2 * 1024 * 1024) ||
        !files_equal(bundle_icon, false, sce_icon, true, 2 * 1024 * 1024) ||
        !files_equal(bundle_icon, false, meta_icon, true, 2 * 1024 * 1024))
        return ResidualRegistration::conflict;
    return ResidualRegistration::exact;
}
int shadowmount_exchange(const char* outgoing, char* incoming,
                         std::size_t capacity, std::size_t& used) {
    if (!outgoing || !incoming || capacity < 2)
        return -EINVAL;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -(errno ? errno : EIO);
    struct Socket {
        int value;
        ~Socket() { close(value); }
    } socket_owner{fd};
    timeval timeout{5, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0)
        return -(errno ? errno : EIO);
    sockaddr_in address{};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(10101);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        return -(errno ? errno : EIO);
    std::size_t sent = 0;
    const std::size_t outgoing_size = std::strlen(outgoing);
    while (sent < outgoing_size) {
        const auto count = send(fd, outgoing + sent, outgoing_size - sent, 0);
        if (count <= 0)
            return -(errno ? errno : EIO);
        sent += static_cast<std::size_t>(count);
    }
    if (shutdown(fd, SHUT_WR) != 0)
        return -(errno ? errno : EIO);
    used = 0;
    while (used < capacity - 1) {
        const auto count = recv(fd, incoming + used, capacity - 1 - used, 0);
        if (count < 0)
            return -(errno ? errno : EIO);
        if (count == 0)
            break;
        used += static_cast<std::size_t>(count);
    }
    if (used == capacity - 1) {
        char extra = 0;
        const auto count = recv(fd, &extra, 1, 0);
        if (count != 0)
            return count > 0 ? -EOVERFLOW : -(errno ? errno : EIO);
    }
    incoming[used] = 0;
    return 0;
}
shelf::TileUninstallHttpResult uninstall_tile(Context& context,
                                              const shelf::LaunchRequest& request) {
    char outgoing[512];
    if (!shelf::encode_tile_uninstall_http_request(request, outgoing, sizeof(outgoing)))
        return shelf::TileUninstallHttpResult::invalid;
    char incoming[4097];
    std::size_t used = 0;
    const int exchanged = shadowmount_exchange(outgoing, incoming, sizeof(incoming), used);
    if (exchanged != 0) {
        if (context.journal) {
            std::fprintf(context.journal,
                         "ShadowMount uninstall transport: code=%d title=%s\n",
                         exchanged, request.title);
            std::fflush(context.journal);
        }
        return shelf::TileUninstallHttpResult::invalid;
    }
    const auto result = shelf::tile_uninstall_http_response({incoming, used}, request.title);
    const char* label = result == shelf::TileUninstallHttpResult::accepted ? "accepted" :
                        result == shelf::TileUninstallHttpResult::game_active ? "game_active" :
                        result == shelf::TileUninstallHttpResult::scanner_busy ? "scanner_busy" : "invalid";
    if (context.journal) {
        std::fprintf(context.journal, "ShadowMount uninstall: result=%s bytes=%zu title=%s\n",
                     label, used, request.title);
        std::fflush(context.journal);
    }
    return result;
}
int scan_tiles(Context& context) {
    char outgoing[256];
    if (!shelf::encode_tile_scan_http_request(outgoing, sizeof(outgoing)))
        return -EINVAL;
    char incoming[4097];
    std::size_t used = 0;
    const int exchanged = shadowmount_exchange(outgoing, incoming, sizeof(incoming), used);
    if (exchanged != 0)
        return exchanged;
    const bool accepted = shelf::tile_scan_http_response_valid({incoming, used});
    if (context.journal) {
        std::fprintf(context.journal, "ShadowMount scan: accepted=%u bytes=%zu attempts=1\n",
                     accepted ? 1u : 0u, used);
        std::fflush(context.journal);
    }
    return accepted ? 0 : -EIO;
}
bool residual_manual_list_clear(Context& context) {
    char outgoing[256];
    if (!shelf::encode_tile_manual_list_http_request(outgoing,
                                                      sizeof(outgoing)))
        return false;
    char incoming[8193];
    std::size_t used = 0;
    const int exchanged = shadowmount_exchange(outgoing, incoming,
                                                sizeof(incoming), used);
    const bool clear = exchanged == 0 &&
        shelf::tile_residual_manual_list_valid({incoming, used});
    if (context.journal) {
        std::fprintf(context.journal,
                     "ShadowMount residual manual-list preflight: clear=%u bytes=%zu\n",
                     clear ? 1u : 0u, used);
        std::fflush(context.journal);
    }
    return clear;
}
int maintain_tiles(Context& context, const shelf::LaunchRequest& request) {
    std::size_t expected = 0;
    const bool residual = request.mode == shelf::LaunchMode::tile_residual;
    const bool titles_valid = residual
        ? shelf::launch_tile_residual_titles_valid(request.title, request.rom,
                                                    &expected)
        : shelf::launch_tile_refresh_titles_valid(request.title, request.rom,
                                                   &expected);
    if ((request.mode != shelf::LaunchMode::tile_remove &&
         request.mode != shelf::LaunchMode::tile_refresh && !residual) ||
        !titles_valid)
        return -EINVAL;
    std::array<ResidualRegistration, shelf::catalog.size()> residual_states{};
    std::size_t residual_exact = 0, residual_absent = 0;
    if (residual) {
        if (!residual_manual_list_clear(context))
            return -EACCES;
        std::size_t system = 0;
        const auto preflight = [&](std::string_view title) {
            if (system >= residual_states.size())
                return false;
            char value[10]{};
            if (title.size() != 9)
                return false;
            std::memcpy(value, title.data(), title.size());
            const auto state = residual_registration(value, system);
            residual_states[system] = state;
            if (context.journal) {
                const char* label = state == ResidualRegistration::exact ? "exact" :
                                    state == ResidualRegistration::absent ? "absent" :
                                    state == ResidualRegistration::conflict ? "conflict" : "failed";
                std::fprintf(context.journal,
                             "Residual registration preflight: system=%zu title=%s state=%s\n",
                             system, value, label);
                std::fflush(context.journal);
            }
            ++system;
            if (state == ResidualRegistration::exact)
                ++residual_exact;
            else if (state == ResidualRegistration::absent)
                ++residual_absent;
            else
                return false;
            return true;
        };
        bool okay = preflight(request.title);
        std::string_view remaining(request.rom);
        while (okay && !remaining.empty()) {
            const auto comma = remaining.find(',');
            const auto length = comma == remaining.npos
                ? remaining.size() : comma;
            okay = preflight(std::string_view(remaining.data(), length));
            if (comma == remaining.npos)
                remaining = {};
            else
                remaining.remove_prefix(comma + 1);
        }
        if (context.journal) {
            std::fprintf(context.journal,
                         "Residual registration preflight summary: exact=%zu absent=%zu expected=%zu complete=%u\n",
                         residual_exact, residual_absent, expected,
                         okay && system == expected ? 1u : 0u);
            std::fflush(context.journal);
        }
        if (!okay || system != expected || residual_exact + residual_absent != expected)
            return -EEXIST;
        // Close the preflight-to-mutation gap for the source registry as well:
        // no removal starts if a protected title entered the manual list while
        // the global registration set was being inspected.
        if (!residual_manual_list_clear(context))
            return -EACCES;
    }
    shelf::LaunchRequest single = request;
    single.mode = shelf::LaunchMode::tile_uninstall;
    single.rom[0] = 0;
    single.core[0] = 0;
    std::size_t completed = 0;
    std::size_t uninstall_requests = 0;
    std::size_t skipped_absent = 0;
    std::size_t transient_retries = 0;
    const auto remove = [&](std::string_view title, std::size_t system) {
        if (title.size() != 9)
            return -EINVAL;
        if (residual && (system >= residual_states.size() ||
                         residual_states[system] == ResidualRegistration::absent)) {
            if (system >= residual_states.size())
                return -EINVAL;
            ++completed;
            ++skipped_absent;
            return 0;
        }
        if (residual) {
            char value[10]{};
            std::memcpy(value, title.data(), title.size());
            const auto current_registration = residual_registration(value, system);
            if (context.journal) {
                const char* label = current_registration == ResidualRegistration::exact ? "exact" :
                                    current_registration == ResidualRegistration::absent ? "absent" :
                                    current_registration == ResidualRegistration::conflict ? "conflict" : "failed";
                std::fprintf(context.journal,
                             "Residual registration mutation guard: system=%zu title=%s state=%s\n",
                             system, value, label);
                std::fflush(context.journal);
            }
            if (current_registration == ResidualRegistration::absent) {
                ++completed;
                ++skipped_absent;
                return 0;
            }
            if (current_registration != ResidualRegistration::exact)
                return current_registration == ResidualRegistration::conflict
                    ? -EEXIST : -EIO;
            char active[10]{};
            if (current(&context, active) != 0)
                return -EBUSY;
        }
        std::memcpy(single.title, title.data(), title.size());
        single.title[title.size()] = 0;
        constexpr unsigned max_attempts = 6;
        constexpr unsigned retry_wait_ms = 5000;
        for (unsigned attempt = 1; attempt <= max_attempts; ++attempt) {
            const auto response = uninstall_tile(context, single);
            if (response == shelf::TileUninstallHttpResult::accepted) {
                ++completed;
                ++uninstall_requests;
                return 0;
            }
            if (response != shelf::TileUninstallHttpResult::game_active &&
                response != shelf::TileUninstallHttpResult::scanner_busy)
                return -EIO;
            if (attempt == max_attempts)
                return -EBUSY;
            ++transient_retries;
            if (context.journal) {
                std::fprintf(context.journal,
                             "ShadowMount transient wait: title=%s attempt=%u/%u idle_ms=%u\n",
                             single.title, attempt, max_attempts, retry_wait_ms);
                std::fflush(context.journal);
            }
            // A retry is allowed only for ShadowMount's two exact reviewed
            // transient receipts, while SystemService continuously proves
            // that no big app started during the bounded idle wait.
            for (unsigned waited = 0; waited < retry_wait_ms; waited += 250) {
                char active[10]{};
                if (current(&context, active) != 0)
                    return -EBUSY;
                usleep(250 * 1000);
            }
        }
        return -EBUSY;
    };
    std::size_t system = 0;
    int result = remove(request.title, system++);
    std::string_view remaining(request.rom);
    while (result == 0 && !remaining.empty()) {
        // ShadowMount performs title removals asynchronously. Keep one bounded
        // request in flight at a calm cadence so its work queue cannot be
        // flooded by a 52-title removal or artwork batch. Only exact reviewed transient
        // receipts can enter the bounded same-title branch above.
        // A full second avoids needlessly colliding with ordinary scanner
        // cycles; exact transient collisions use the bounded branch above.
        usleep(1000 * 1000);
        const auto comma = remaining.find(',');
        const auto length = comma == remaining.npos ? remaining.size() : comma;
        result = remove(std::string_view(remaining.data(), length), system++);
        if (comma == remaining.npos)
            remaining = {};
        else
            remaining.remove_prefix(comma + 1);
    }
    if (result == 0 && completed == expected)
        result = scan_tiles(context);
    if (context.journal) {
        if (residual)
            std::fprintf(context.journal,
                         "ShadowMount residual removal: completed=%zu expected=%zu exact=%zu absent=%zu uninstall_requests=%zu scan=%u interval_ms=1000 transient_retries=%zu retry_wait_ms=5000 retry_limit=5\n",
                         completed, expected, residual_exact, skipped_absent,
                         uninstall_requests, result == 0 ? 1u : 0u,
                         transient_retries);
        else
            std::fprintf(context.journal,
                         request.mode == shelf::LaunchMode::tile_remove
                             ? "ShadowMount bulk removal: completed=%zu expected=%zu scan=%u interval_ms=1000 transient_retries=%zu retry_wait_ms=5000 retry_limit=5\n"
                             : "ShadowMount bulk artwork refresh: completed=%zu expected=%zu scan=%u interval_ms=1000 transient_retries=%zu retry_wait_ms=5000 retry_limit=5\n",
                         completed, expected, result == 0 ? 1u : 0u,
                         transient_retries);
        std::fflush(context.journal);
    }
    return result;
}
int launch(void* raw, const shelf::LaunchRequest& request) {
    auto& context = *static_cast<Context*>(raw);
    std::int32_t user = -1;
    if (sceUserServiceGetForegroundUser(&user) != 0 || user != context.user ||
        !shelf::launch_request_valid(request, now(nullptr)) || consent(raw) != shelf::HandoffConsent::committed)
        return context.launch_status = -1;
    if (request.mode == shelf::LaunchMode::tile_uninstall ||
        request.mode == shelf::LaunchMode::tile_remove ||
        request.mode == shelf::LaunchMode::tile_refresh ||
        request.mode == shelf::LaunchMode::tile_residual) {
        // ShellCore can report no running big app slightly before
        // ShadowMount clears its own game-active lifecycle flag. Give that
        // bounded state three additional seconds to settle; never retry a
        // refused maintenance request.
        if (context.journal) {
            std::fprintf(context.journal, "ShadowMount post-close settle: 3000 ms\n");
            std::fflush(context.journal);
        }
        usleep(3000 * 1000);
        const shelf::MaintenanceEndpointOps readiness_ops{
            &context, probe_shadowmount, current, wait};
        const auto readiness =
            shelf::wait_for_maintenance_endpoint(readiness_ops);
        if (context.journal) {
            const char* label =
                readiness == shelf::MaintenanceEndpointResult::ready ? "ready" :
                readiness == shelf::MaintenanceEndpointResult::foreground_changed ? "foreground_changed" :
                readiness == shelf::MaintenanceEndpointResult::timeout ? "timeout" :
                readiness == shelf::MaintenanceEndpointResult::failed ? "failed" : "invalid";
            std::fprintf(context.journal,
                         "ShadowMount pre-mutation readiness: result=%s max_wait_ms=8000\n",
                         label);
            std::fflush(context.journal);
        }
        if (readiness != shelf::MaintenanceEndpointResult::ready)
            return context.launch_status =
                readiness == shelf::MaintenanceEndpointResult::foreground_changed ? -EBUSY :
                readiness == shelf::MaintenanceEndpointResult::timeout ? -ETIMEDOUT : -EIO;
        if (!shelf::launch_request_valid(request, now(nullptr)) ||
            consent(raw) != shelf::HandoffConsent::committed)
            return context.launch_status = -EPERM;
    }
    if (request.mode == shelf::LaunchMode::tile_uninstall)
        return context.launch_status =
            uninstall_tile(context, request) == shelf::TileUninstallHttpResult::accepted ? 0 : -EIO;
    if (request.mode == shelf::LaunchMode::tile_remove ||
        request.mode == shelf::LaunchMode::tile_refresh ||
        request.mode == shelf::LaunchMode::tile_residual)
        return context.launch_status = maintain_tiles(context, request);
    if (request.mode == shelf::LaunchMode::self_update ||
        request.mode == shelf::LaunchMode::update_recover) {
        if (context.journal) {
            std::fprintf(context.journal, "isShelf update notifications: marker=%s\n",
                         update_debug_notice_marker);
            std::fflush(context.journal);
        }
        const int transaction = shelf::apply_update_transaction(
            request, context.journal, update_stage_notice, nullptr);
        shelf::UpdateRelaunchOutcome reopened;
        if (transaction >= 0) {
            char message[160];
            std::snprintf(message, sizeof(message),
                          "isShelf %s installed. Please don't open anything for %u seconds; "
                          "isShelf will reopen automatically.",
                          request.rom, shelf::update_relaunch_settle_ms / 1000u);
            debug_notice(message);
            reopened = shelf::relaunch_updated_shelf(
                {&context, update_relaunch_state, launch_update_target, wait});
            if (reopened.result != shelf::UpdateRelaunchResult::launched)
                debug_notice("isShelf was updated but could not reopen automatically. Start it from PS5 Home.");
        } else {
            debug_notice("isShelf update stopped safely. Open isShelf again for details.");
        }
        if (context.journal) {
            std::fprintf(context.journal,
                         "isShelf update: transaction=0x%08x relaunch_result=%d relaunch_code=0x%08x "
                         "attempts=%u settle_ms=%u observe_ms=%u marker=%s\n",
                         static_cast<unsigned>(transaction), static_cast<int>(reopened.result),
                         static_cast<unsigned>(reopened.last_code), reopened.attempts,
                         shelf::update_relaunch_settle_ms,
                         shelf::update_relaunch_observe_ms,
                         exact_update_relaunch_marker);
            std::fflush(context.journal);
        }
        if (transaction < 0)
            return context.launch_status = transaction;
        if (reopened.result == shelf::UpdateRelaunchResult::launched)
            return context.launch_status = 0;
        if (reopened.result == shelf::UpdateRelaunchResult::foreground_changed)
            return context.launch_status = -EBUSY;
        if (reopened.result == shelf::UpdateRelaunchResult::not_observed)
            return context.launch_status = -ETIMEDOUT;
        if (reopened.result == shelf::UpdateRelaunchResult::refused && reopened.last_code < 0)
            return context.launch_status = reopened.last_code;
        return context.launch_status = -EIO;
    }
    auto args = shelf::launch_arguments(request);
    AppLaunchContext params{};
    params.size = sizeof(params);
    params.user = user;
    // Public LaunchApp flag SkipSystemUpdate. This launches only a selected
    // installed title; the helper has no update/reboot/install functionality.
    params.check_flags = 2;
    context.launch_status = sceSystemServiceLaunchApp(request.title, args[0] ? args.data() : nullptr, &params);
    if (context.journal) {
        std::fprintf(context.journal, "SystemService launch: 0x%08x; check_flags: 0x%016llx\n",
                     static_cast<unsigned>(context.launch_status),
                     static_cast<unsigned long long>(params.check_flags));
        std::fflush(context.journal);
    }
    return context.launch_status;
}
int focused(void* raw, const shelf::LaunchRequest& request) {
    auto& context = *static_cast<Context*>(raw);
    char title[10]{};
    std::int32_t user = -1;
    if (!context.target_app || current(raw, title) != 1 || std::strcmp(title, request.title) != 0 ||
        sceUserServiceGetForegroundUser(&user) != 0 || user != context.user ||
        !shelf::launch_request_valid(request, now(nullptr)) || consent(raw) != shelf::HandoffConsent::committed)
        return -1;
    if (!context.focus_open) {
        const int opened = sceKernelOpenEventFlag(&context.focus_handle, "SceShellCoreUtilAppFocus");
        if (opened < 0) {
            if (context.journal)
                std::fprintf(context.journal, "read-only focus flag unavailable: 0x%08x\n", static_cast<unsigned>(opened));
            return -1;
        }
        context.focus_open = true;
    }
    std::uint64_t value = 0;
    int observed = sceKernelPollEventFlag(context.focus_handle, UINT64_MAX, 2u, &value);
    // OR over every bit reports EBUSY for zero; never use clear/set wait modes.
    if (static_cast<std::uint32_t>(observed) == 0x80020010u) {
        value = 0;
        observed = 0;
    }
    if (context.journal) {
        std::fprintf(context.journal, "read-only shell focus: rc=0x%08x value=0x%016llx target=0x%08x\n",
                     static_cast<unsigned>(observed), static_cast<unsigned long long>(value),
                     static_cast<unsigned>(context.target_app));
        std::fflush(context.journal);
    }
    if (observed < 0 || value > UINT32_MAX)
        return -1;
    return static_cast<std::uint32_t>(value) == static_cast<std::uint32_t>(context.target_app) ? 1 : 0;
}
shelf::LaunchStage result_stage(shelf::HandoffResult result) {
    switch (result) {
    case shelf::HandoffResult::invalid:
        return shelf::LaunchStage::invalid;
    case shelf::HandoffResult::foreground_changed:
        return shelf::LaunchStage::foreground_changed;
    case shelf::HandoffResult::reply_failed:
        return shelf::LaunchStage::reply_failed;
    case shelf::HandoffResult::close_timeout:
        return shelf::LaunchStage::close_timeout;
    case shelf::HandoffResult::started:
        return shelf::LaunchStage::started;
    case shelf::HandoffResult::unconfirmed:
        return shelf::LaunchStage::unconfirmed;
    case shelf::HandoffResult::refused:
        return shelf::LaunchStage::refused;
    case shelf::HandoffResult::cancelled:
        return shelf::LaunchStage::cancelled;
    case shelf::HandoffResult::foreground_observed:
        return shelf::LaunchStage::foreground_observed;
    }
    return shelf::LaunchStage::invalid;
}
} // namespace
int main() {
    char message[shelf::launch_message_capacity];
    FILE* incoming = std::fopen(request_path, "rb");
    if (!incoming)
        return 1;
    const auto used = std::fread(message, 1, sizeof(message), incoming);
    const bool error = std::ferror(incoming) != 0;
    std::fclose(incoming);
    shelf::LaunchRequest request;
    if (error || !shelf::parse_launch_request({message, used}, now(nullptr), request))
        return 1;
    // Only the reader that successfully consumes this request may acknowledge
    // it or launch anything. A duplicate helper has nothing left to consume.
    if (unlink(request_path) != 0)
        return 1;
    Context context;
    context.request = &request;
    context.journal = std::fopen("/data/console-shelf/launch-helper.log", "wb");
    sceUserServiceInitialize(nullptr);
    const int initialized = sceLncUtilInitialize();
    const int user_status = sceUserServiceGetForegroundUser(&context.user);
    if (context.journal) {
        std::fprintf(context.journal,
#ifdef SHELF_STANDARD_SDK_RUNTIME
                     "ui-16 standard SDK helper; exact origin close: %s; init: 0x%08x; user query: 0x%08x\n",
#else
                     "ui-16 restricted helper; exact origin close: %s; init: 0x%08x; user query: 0x%08x\n",
#endif
                     exact_origin_close_marker, static_cast<unsigned>(initialized),
                     static_cast<unsigned>(user_status));
        std::fflush(context.journal);
    }
    if (initialized < 0 || user_status != 0 || context.user < 0) {
        status(request, shelf::LaunchStage::service_unavailable, user_status ? user_status : initialized);
        return 1;
    }
    const auto result = shelf::cooperative_handoff(
        request, {&context, current, ready, wait, close_origin, launch, now, consent, focused});
    status(request, result_stage(result),
           result == shelf::HandoffResult::foreground_changed ? context.probe_status : context.launch_status);
    return result == shelf::HandoffResult::foreground_observed ||
                    ((request.mode == shelf::LaunchMode::tile_uninstall ||
                     request.mode == shelf::LaunchMode::tile_remove ||
                     request.mode == shelf::LaunchMode::tile_refresh ||
                      request.mode == shelf::LaunchMode::tile_residual ||
                      request.mode == shelf::LaunchMode::self_update ||
                      request.mode == shelf::LaunchMode::update_recover) &&
                    result == shelf::HandoffResult::started)
               ? 0 : 1;
}
