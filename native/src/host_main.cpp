// SPDX-License-Identifier: GPL-3.0-only
// Shared native event loop. The host build never deploys or launches PS5 apps.
#define SDL_MAIN_HANDLED
#include "shelf/filesystem.hpp"
#include "shelf/ftp.hpp"
#include "shelf/graphics.hpp"
#include "shelf/artwork.hpp"
#include "shelf/console_tile_intent.hpp"
#include "shelf/console_tile_route_adoption.hpp"
#include "shelf/console_tile_remembered_route_runtime.hpp"
#include "shelf/console_tile_remembered_startup.hpp"
#include "shelf/console_tile_startup.hpp"
#include "shelf/launch_client.hpp"
#include "shelf/update_client.hpp"
#if defined(SHELF_PS5) && defined(SHELF_UI_TEST_BRIDGE)
#include "shelf/ui_test_remote_arm.hpp"
#include "shelf/console_tile_path_probe.hpp"
#include "shelf/console_tile_write_probe_v4.hpp"
#include "console_tile_path_probe_asset.hpp"
#include "console_tile_write_probe_v4_asset.hpp"
#include <fcntl.h>
#include <unistd.h>
#endif
#include "shelf/memory.hpp"
#include "shelf/navigation.hpp"
#include "shelf/pairing_capture_gate.hpp"
#if !defined(SHELF_PS5) || defined(SHELF_OWNER_SUPPORT_CONTROL) || defined(SHELF_UI_TEST_BRIDGE)
#define SHELF_HAS_UI_TEST_BRIDGE
#include "shelf/ui_test_bridge.hpp"
#include "shelf/ui_test_video.hpp"
#endif
#include <SDL.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#ifndef ISSHELF_VERSION
#define ISSHELF_VERSION "v0.1.1"
#endif
#ifdef SHELF_PS5
#include "platform/platform.h"
#include "shelf/console_tile_job_routes.hpp"
#include "shelf/console_tile_manager.hpp"
#include "shelf/console_tile_write_gate.hpp"
#include "shelf/managed_tile_app_client.hpp"
#if defined(SHELF_UI_TEST_BRIDGE)
#include "shelf/app_tile_source_probe.hpp"
#include "shelf/n64_source_trial.hpp"
#include "shelf/protected_ps2_source_read_probe.hpp"
#include "shelf/protected_ps2_direct_file_probe.hpp"
#include "shelf/console_tile_route_store_app.hpp"
#include "shelf/console_tile_route_path_probe.hpp"
#include "shelf/console_tile_settings_path_probe.hpp"
#endif
#include "shelf/memory.hpp"
#include "shelf_launch_asset.hpp"
#include <time.h>
// The public PS5 build owns only shortcuts carrying isShelf's exact source
// record. Owner-enabled menu support is separate from private diagnostics.
#if defined(SHELF_PS5_OWNED_TILE_WRITES)
static_assert(shelf::console_tile_writes_enabled,
              "Owned-tile builds must open the exact ownership write gate");
[[gnu::used]] static const char owned_tile_write_marker[] =
    "ISSHELF_OWNED_TILE_WRITES_V1";
#else
static_assert(!shelf::console_tile_writes_enabled,
              "Read-only PS5 builds must keep tile writes disabled");
#endif
[[gnu::used]] static const char update_client_marker[] =
    "ISSHELF_IN_APP_UPDATE_CLIENT_V1";
#if defined(SHELF_OWNER_SUPPORT_CONTROL)
[[gnu::used]] static const char owner_support_control_marker[] =
    "ISSHELF_OWNER_SUPPORT_CONTROL_V1";
#endif
static_assert(!shelf::managed_tile_app_client::requests_enabled,
              "Managed tile requests need credential and journal custody");
#endif
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif
namespace {
void startup(const char* stage, const char* detail = nullptr) {
#ifdef SHELF_PS5
    // Owned diagnostic file only: no account values or game paths are logged.
    if (FILE* file = std::fopen("/data/console-shelf/startup.log", "ab")) {
        std::fprintf(file, "%s%s%s\n", stage, detail ? ": " : "", detail ? detail : "");
        std::fflush(file);
        std::fclose(file);
    }
#else
    (void)stage;
    (void)detail;
#endif
}
struct LibraryScan {
    shelf::ReadOnlyFs source;
    shelf::Library* library = nullptr;
    const char* data_root = nullptr;
    SDL_atomic_t cancelled{0}, done{0};
    SDL_Thread* thread = nullptr;
    static bool stopping(void* raw) {
        return SDL_AtomicGet(&static_cast<LibraryScan*>(raw)->cancelled) != 0;
    }
    static shelf::FsStatus list(void* raw, const char* path, shelf::EntryVisitor visit, void* destination) {
        auto& scan = *static_cast<LibraryScan*>(raw);
        return stopping(raw) ? shelf::FsStatus::failed
                             : scan.source.list(scan.source.context, path, visit, destination);
    }
    static shelf::FsStatus stat(void* raw, const char* path, shelf::FileStat& result) {
        auto& scan = *static_cast<LibraryScan*>(raw);
        return stopping(raw) ? shelf::FsStatus::failed : scan.source.stat(scan.source.context, path, result);
    }
    static shelf::FsStatus read(void* raw, const char* path, std::uint64_t offset, void* out, std::size_t cap,
                                std::size_t& used) {
        auto& scan = *static_cast<LibraryScan*>(raw);
        return stopping(raw) ? shelf::FsStatus::failed
                             : scan.source.read(scan.source.context, path, offset, out, cap, used);
    }
    static int run(void* raw) {
        auto& scan = *static_cast<LibraryScan*>(raw);
        shelf::discover_library({raw, list, stat, read, scan.source.cached}, *scan.library);
        shelf::cache_artwork({raw, list, stat, read, scan.source.cached}, *scan.library, scan.data_root, stopping, raw);
        SDL_AtomicSet(&scan.done, 1);
        return 0;
    }
    bool start() {
        // Bounded metadata/network/decoder calls must not depend on the platform default.
        thread = SDL_CreateThreadWithStackSize(run, "Shelf library", 256 * 1024, this);
        return thread != nullptr;
    }
    bool ready() {
        return thread && SDL_AtomicGet(&done) != 0;
    }
    void stop() {
        SDL_AtomicSet(&cancelled, 1);
        if (thread) {
            SDL_WaitThread(thread, nullptr);
            thread = nullptr;
        }
    }
    ~LibraryScan() {
        stop();
    }
};
void accept_library(shelf::UiState& state, shelf::Library& library, shelf::ReadOnlyFtp& ftp, bool scan_only) {
    const bool preserve_selected_surface = state.page != shelf::Page::home &&
                                           state.selected_system < shelf::system_count;
    const auto selected_surface = state.selected_system;
    state.library_discovering = false;
    state.library = &library;
    state.inventory = library.inventory;
    startup(library.scan.complete ? "library scan complete" : "library scan partial");
    char info[180];
    std::snprintf(info, sizeof(info), "%zu games; %zu routes; %zu roots read; %zu unavailable", library.game_count,
                  library.route_count, library.scan.roots_read, library.scan.roots_denied);
    startup("library counts", info);
    std::snprintf(info, sizeof(info), "%zu examined; %zu rejected; %zu missing references; limit %d; ftp cache %zu dirs/%zu entries",
                  library.scan.files_examined, library.scan.rejected, library.scan.missing_references,
                  library.scan.limit_reached ? 1 : 0, ftp.directory_count, ftp.entry_count);
    startup("library scan details", info);
    if (scan_only)
        std::printf("Scan details: %s\n", info);
    const auto& diag = ftp.diagnostics;
    std::snprintf(info, sizeof(info), "%zu connections; %zu listings; %zu reads; first failure %s (%d)",
                  diag.connections, diag.listings, diag.reads, diag.first_failure, diag.error);
    startup("library connection", info);
    if (scan_only)
        std::printf("FTP diagnostics: %s\n", info);
    std::printf("Read-only library: %zu games, %zu emulator/core routes; scan %s.\n", library.game_count,
                library.route_count, library.scan.complete ? "complete" : "partial");
    std::snprintf(info, sizeof(info), "%zu sources; %zu cached; %zu missing; %zu unreadable", library.artwork.found,
                  library.artwork.cached, library.artwork.missing, library.artwork.failed);
    startup("library artwork", info);
    std::printf("Library artwork: %s.\n", info);
    for (std::size_t i = 0; i < shelf::system_count; ++i)
        if (state.inventory[i].game_count || state.inventory[i].emulator_count)
            std::printf("  %.*s: %zu games, %zu routes\n", static_cast<int>(shelf::catalog[i].id.size()),
                        shelf::catalog[i].id.data(), state.inventory[i].game_count, state.inventory[i].emulator_count);
    shelf::clear_ftp_cache(ftp);
    const auto shown = shelf::shown_systems(state);
    state.focus = std::min(state.focus, std::max(0, static_cast<int>(shown.count) - 1));
    if (shown.count && !preserve_selected_surface)
        state.selected_system = shown.indices[static_cast<std::size_t>(state.focus)];
    else if (preserve_selected_surface)
        state.selected_system = selected_surface;
    if (!library.scan.complete && !state.notice[0])
        std::snprintf(state.notice, sizeof(state.notice),
                      "Library scan incomplete. Keep ftpsrv running and reopen isShelf to try again.");
}
#ifdef SHELF_PS5
struct ConsoleTileWorker {
    enum class Job {
        none,
        probe,
        inspect,
        upsert,
        upsert_emulators,
        refresh_installed,
        remove_installed,
        remove,
        confirm_removed
    };
    shelf::ConsoleTileManager manager;
    shelf::managed_tile_app_client::Client managed;
    shelf::managed_tile_app_client::CreateAttempt create_attempt;
    const shelf::Library* library = nullptr;
    SDL_atomic_t done{0};
    SDL_Thread* thread = nullptr;
    Job job = Job::none;
    std::size_t system = shelf::system_count;
    shelf::ConsoleTileIntent intent;
    shelf::ConsoleTileOutcome outcome;
    shelf::ConsoleTileEmulatorBatchOutcome emulator_batch_outcome;
    shelf::ConsoleTileBatchOutcome batch_outcome;
    shelf::ConsoleTileRemovalBatchOutcome removal_batch_outcome;
    shelf::ConsoleTileAvailability availability;
    shelf::Settings settings_snapshot;
    bool connected = false;
    static int run(void* raw) {
        auto& worker = *static_cast<ConsoleTileWorker*>(raw);
        switch (worker.job) {
        case Job::probe:
#if defined(SHELF_PS5_OWNED_TILE_WRITES)
            worker.availability = worker.manager.availability();
#else
            worker.availability = worker.manager.catalog_read_availability();
            if (!worker.availability.ready() && worker.managed.admitted()) {
                worker.availability = {};
                worker.availability.reason = shelf::ConsoleTileAvailabilityReason::ready;
                std::snprintf(worker.availability.message,
                              sizeof(worker.availability.message),
                              "Managed shortcut status reader is bound; verify each result.");
            }
#endif
            worker.connected = worker.availability.ready();
            break;
        case Job::inspect:
        {
#if defined(SHELF_PS5_OWNED_TILE_WRITES)
            worker.outcome = worker.manager.inspect(worker.system);
#else
            const auto managed = worker.managed.status(worker.system);
            if (managed.state == shelf::managed_tile_app_client::State::observed) {
                worker.outcome = {};
                worker.outcome.intent.system = worker.system;
                const auto stage = managed.status.stage;
                worker.outcome.stage =
                    stage == shelf::managed_tile_api_v1::Stage::absent
                        ? shelf::ConsoleTileStage::absent
                    : stage == shelf::managed_tile_api_v1::Stage::installed
                        ? shelf::ConsoleTileStage::registered
                        : shelf::ConsoleTileStage::pending;
                // A catalog status is not proof of title ownership or TV
                // visibility, including for the four adopted pilots.
                worker.outcome.owned = false;
                std::snprintf(worker.outcome.message,
                              sizeof(worker.outcome.message),
                              "Managed service reports %s; verify the PS5 Home tile separately.",
                              stage == shelf::managed_tile_api_v1::Stage::absent ? "absent"
                              : stage == shelf::managed_tile_api_v1::Stage::installed ? "installed"
                              : "in progress");
            } else if (managed.state ==
                       shelf::managed_tile_app_client::State::credential_unavailable) {
                worker.outcome = worker.manager.inspect_catalog_title(worker.system);
                std::snprintf(worker.outcome.diagnostic,
                              sizeof(worker.outcome.diagnostic),
                              "Managed tile credential unbound; legacy read-only catalog status used.");
            } else {
                worker.hold_generic_operation(
                    managed.state == shelf::managed_tile_app_client::State::conflict
                        ? "Managed shortcut status conflicts with this catalog title."
                        : "Managed shortcut status is uncertain; no tile state confirmed.");
            }
#endif
            break;
        }
        case Job::upsert:
#if defined(SHELF_PS5_OWNED_TILE_WRITES)
            worker.outcome = worker.manager.upsert(worker.intent);
#else
            worker.create_attempt = worker.managed.create(worker.intent,
                                                           worker.library);
            worker.outcome = {};
            worker.outcome.intent = worker.intent;
            worker.outcome.owned = false;
            worker.outcome.stage =
                worker.create_attempt.state == shelf::managed_tile_app_client::State::receipt_needs_status
                    ? shelf::ConsoleTileStage::pending
                    : shelf::ConsoleTileStage::deferred;
            std::snprintf(worker.outcome.message, sizeof(worker.outcome.message),
                          worker.create_attempt.state == shelf::managed_tile_app_client::State::credential_unavailable
                              ? "Managed shortcut service credential is unavailable; no CREATE was sent."
                          : worker.create_attempt.state == shelf::managed_tile_app_client::State::journal_unavailable
                              ? "Managed shortcut journal is unavailable; no CREATE was sent."
                          : worker.create_attempt.state == shelf::managed_tile_app_client::State::receipt_needs_status
                              ? "Managed CREATE returned a receipt; exact status reconciliation is required."
                              : "Managed CREATE is held; no Home tile was confirmed.");
#endif
            break;
        case Job::upsert_emulators:
#if defined(SHELF_PS5_OWNED_TILE_WRITES)
        {
            std::array<shelf::ConsoleTileIntent, shelf::system_count> intents{};
            std::array<bool, shelf::system_count> selected{};
            if (worker.library && worker.library->scan.complete) {
                for (std::size_t system = 0; system < shelf::system_count; ++system) {
                    shelf::LaunchRequest launch;
                    if (shelf::plan_emulator(
                            *worker.library, system,
                            worker.settings_snapshot.preferred_emulator[system].data(),
                            launch) == shelf::LaunchPlan::unavailable)
                        continue;
                    auto& intent = intents[system];
                    intent.system = system;
                    if (shelf::console_tile_emulator_menu_selects_system(launch.title)) {
                        intent.destination = shelf::ConsoleTileDestination::emulator;
                        std::snprintf(intent.emulator_title,
                                      sizeof(intent.emulator_title), "%s", launch.title);
                    } else {
                        intent.destination = shelf::ConsoleTileDestination::library;
                    }
                    selected[system] = shelf::tile_job::intent_valid(
                        intent, shelf::tile_job::installed_emulator_route,
                        worker.library);
                }
            }
            worker.emulator_batch_outcome =
                worker.manager.upsert_emulators(intents, selected);
            break;
        }
#else
            worker.emulator_batch_outcome = {};
            worker.emulator_batch_outcome.failed_stage =
                shelf::ConsoleTileStage::unavailable;
            std::snprintf(worker.emulator_batch_outcome.message,
                          sizeof(worker.emulator_batch_outcome.message),
                          "Managed shortcut emulator batch is not integrated.");
            break;
#endif
        case Job::refresh_installed:
#if defined(SHELF_PS5_OWNED_TILE_WRITES)
            worker.batch_outcome = worker.manager.refresh_installed();
#else
            worker.batch_outcome = {};
            worker.batch_outcome.failed_stage = shelf::ConsoleTileStage::unavailable;
            std::snprintf(worker.batch_outcome.message,
                          sizeof(worker.batch_outcome.message),
                          "Managed shortcut bulk update is not integrated.");
#endif
            break;
        case Job::remove_installed:
#if defined(SHELF_PS5_OWNED_TILE_WRITES)
            worker.removal_batch_outcome = worker.manager.remove_installed();
#else
            worker.removal_batch_outcome = {};
            worker.removal_batch_outcome.failed_stage = shelf::ConsoleTileStage::unavailable;
            std::snprintf(worker.removal_batch_outcome.message,
                          sizeof(worker.removal_batch_outcome.message),
                          "Managed shortcut bulk removal is not integrated.");
#endif
            break;
        case Job::remove:
#if defined(SHELF_PS5_OWNED_TILE_WRITES)
            worker.outcome = worker.manager.remove(worker.system);
#else
            worker.hold_generic_operation("Managed shortcut removal is not integrated.");
#endif
            break;
        case Job::confirm_removed:
#if defined(SHELF_PS5_OWNED_TILE_WRITES)
            worker.outcome = worker.manager.confirm_removed(worker.system);
#else
            worker.hold_generic_operation("Managed shortcut removal has no verified result yet.");
#endif
            break;
        default: break;
        }
        SDL_AtomicSet(&worker.done, 1);
        return 0;
    }
    void hold_generic_operation(const char* message) {
        outcome = {};
        outcome.intent.system = system;
        outcome.stage = shelf::ConsoleTileStage::unavailable;
        std::snprintf(outcome.message, sizeof(outcome.message), "%s", message);
    }
    bool start(Job next, std::size_t selected = shelf::system_count,
               shelf::ConsoleTileIntent next_intent = {},
               const shelf::Library* next_library = nullptr,
               const shelf::Settings* next_settings = nullptr) {
        if (thread || next == Job::none)
            return false;
        job = next;
        system = selected;
        intent = next_intent;
        library = next_library;
        settings_snapshot = next_settings ? *next_settings : shelf::Settings{};
        connected = false;
        outcome = {};
        emulator_batch_outcome = {};
        batch_outcome = {};
        removal_batch_outcome = {};
        availability = {};
        SDL_AtomicSet(&done, 0);
        thread = SDL_CreateThreadWithStackSize(run, "Shelf shortcut", 256 * 1024, this);
        return thread != nullptr;
    }
    bool ready() { return thread && SDL_AtomicGet(&done) != 0; }
    void join() {
        if (thread) {
            SDL_WaitThread(thread, nullptr);
            thread = nullptr;
        }
    }
    ~ConsoleTileWorker() { join(); }
};
#ifdef SHELF_UI_TEST_BRIDGE
struct ConsoleTilePathProbeWorker {
    SDL_atomic_t cancelled{0}, done{0};
    SDL_Thread* thread = nullptr;
    bool complete = false, send_complete = false;
    bool write_probe = false;
    std::size_t receipt_bytes = 0;
    std::uint64_t receipt_fnv = 0;
    char nonce[33]{}, receipt_path[2304]{};

    static bool read_receipt(ConsoleTilePathProbeWorker& worker) {
        const int fd = open(worker.receipt_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return false;
        struct stat info{};
        char bytes[8193];
        const bool regular = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
                             info.st_size > 0 && info.st_size <= 8192;
        const auto count = regular ? read(fd, bytes, static_cast<std::size_t>(info.st_size)) : -1;
        const bool closed = close(fd) == 0;
        if (!regular || count != info.st_size || !closed) return false;
        const std::string_view body(bytes, static_cast<std::size_t>(count));
        if (worker.write_probe ? !shelf::console_tile_write_v4_receipt_valid(body, worker.nonce)
                               : !shelf::console_tile_probe_receipt_valid(body, worker.nonce)) return false;
        worker.receipt_bytes = body.size();
        worker.receipt_fnv = shelf::console_tile_probe_fnv(body);
        return true;
    }
    static int run(void* raw) {
        auto& worker = *static_cast<ConsoleTilePathProbeWorker*>(raw);
        // One send, even if the loader's socket reply is lost. A complete
        // nonce-bound receipt is the only success signal.
        worker.send_complete = worker.write_probe ?
            shelf::send_probe_elf_once(console_tile_write_probe_v4_payload,
                                       console_tile_write_probe_v4_payload_bytes, worker.cancelled) :
            shelf::send_probe_elf_once(console_tile_path_probe_payload,
                                       console_tile_path_probe_payload_bytes, worker.cancelled);
        const auto began = SDL_GetTicks();
        while (!SDL_AtomicGet(&worker.cancelled) && SDL_GetTicks() - began < 8000) {
            if (read_receipt(worker)) {
                worker.complete = true;
                break;
            }
            SDL_Delay(50);
        }
        SDL_AtomicSet(&worker.done, 1);
        return 0;
    }
    bool start(const char* root, const char* session, bool& request_created, bool write_mode = false) {
        request_created = false;
        if (thread || std::strcmp(root, shelf::console_tile_probe_root) != 0 ||
            !shelf::console_tile_probe_nonce(session)) return false;
        char request_text[96], request_path[128];
        std::size_t used = 0;
        const bool paths_ready = write_mode ?
            shelf::console_tile_write_v4_request(session, request_text, used) &&
            shelf::console_tile_write_v4_request_path(request_path) &&
            shelf::console_tile_write_v4_receipt_path(session, receipt_path) :
            shelf::console_tile_probe_request(session, request_text, used) &&
            shelf::console_tile_probe_request_path(request_path) &&
            shelf::console_tile_probe_receipt_path(session, receipt_path);
        if (!paths_ready)
            return false;
        const int root_fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (root_fd < 0) return false;
        // A receipt left from an earlier attempt cannot prove this dispatch.
        // Refuse it before creating the one-shot request, including symlinks
        // and access errors; never delete or overwrite prior evidence.
        const int earlier = open(receipt_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (earlier >= 0) { close(earlier); close(root_fd); return false; }
        if (errno != ENOENT) { close(root_fd); return false; }
        // Check directory-sync support before consuming the create-only request.
        if (fsync(root_fd) != 0) { close(root_fd); return false; }
        // This nonce is not secret. The separate payload runtime may have a
        // different UID. Request read access without group/world write, then
        // verify the actual mode after umask before sending any ELF bytes.
        const int fd = open(request_path,
                            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd < 0) { close(root_fd); return false; }
        request_created = true;
        const bool written = write(fd, request_text, used) == static_cast<ssize_t>(used) && fsync(fd) == 0;
        struct stat request_info{};
        const bool readable = written && fstat(fd, &request_info) == 0 &&
                              S_ISREG(request_info.st_mode) &&
                              request_info.st_size == static_cast<off_t>(used) &&
                              shelf::console_tile_probe_request_mode_allowed(request_info.st_mode);
        const bool file_closed = close(fd) == 0;
        const bool root_synced = fsync(root_fd) == 0;
        const bool root_closed = close(root_fd) == 0;
        if (!readable || !file_closed || !root_synced || !root_closed) return false;
        std::memcpy(nonce, session, sizeof(nonce));
        write_probe = write_mode;
        receipt_bytes = 0;
        receipt_fnv = 0;
        complete = send_complete = false;
        SDL_AtomicSet(&cancelled, 0);
        SDL_AtomicSet(&done, 0);
        thread = SDL_CreateThreadWithStackSize(run, "Shelf tile probe", 96 * 1024, this);
        return thread != nullptr;
    }
    bool ready() const { return thread && SDL_AtomicGet(const_cast<SDL_atomic_t*>(&done)) != 0; }
    void stop() {
        SDL_AtomicSet(&cancelled, 1);
        if (thread) { SDL_WaitThread(thread, nullptr); thread = nullptr; }
    }
    ~ConsoleTilePathProbeWorker() { stop(); }
};
#endif
bool console_tile_shared_assets_ready(std::size_t system) {
    if (system >= shelf::system_count)
        return false;
    const auto readable = [](const char* path) {
        FILE* file = std::fopen(path, "rb");
        if (!file) return false;
        return std::fclose(file) == 0;
    };
    char image[256];
    const auto relative = shelf::catalog[system].image;
    const int used = std::snprintf(image, sizeof(image), "%s/%.*s", shelf::console_tile_shared_assets,
                                   static_cast<int>(relative.size()), relative.data());
    return used > 0 && static_cast<std::size_t>(used) < sizeof(image) &&
           readable("/data/homebrew/PPSA99541/native/assets/fonts/Poppins-Regular.ttf") && readable(image);
}
void apply_console_tile_intent_to_ui(shelf::UiState& state, const shelf::ConsoleTileIntent& intent) {
    const auto result = shelf::apply_console_tile_intent(state, intent);
    const char* stage = nullptr;
    switch (result) {
    case shelf::ConsoleTileApply::library: stage = "console tile library"; break;
    case shelf::ConsoleTileApply::home: stage = "console tile home"; break;
    case shelf::ConsoleTileApply::home_unavailable: stage = "console tile home unavailable"; break;
    case shelf::ConsoleTileApply::emulator: stage = "console tile emulator"; break;
    case shelf::ConsoleTileApply::emulator_unavailable: stage = "console tile emulator unavailable"; break;
    case shelf::ConsoleTileApply::invalid: return;
    }
    startup(stage, shelf::catalog[intent.system].id.data());
}
std::uint64_t monotonic_ms() {
    struct timespec time{};
    return clock_gettime(CLOCK_MONOTONIC, &time) == 0
               ? static_cast<std::uint64_t>(time.tv_sec) * 1000 + static_cast<std::uint64_t>(time.tv_nsec) / 1000000
               : 0;
}
struct PlatformExit {
    ~PlatformExit() {
        const auto begin = monotonic_ms();
        startup("display shutdown starting");
        gs_vout_shutdown();
        char elapsed[80];
        std::snprintf(elapsed, sizeof(elapsed), "%llu ms", static_cast<unsigned long long>(monotonic_ms() - begin));
        startup("display shutdown complete", elapsed);
        startup("shell close requesting");
        const int status = gs_request_close();
        char message[48];
        std::snprintf(message, sizeof(message), "0x%08x", static_cast<unsigned>(status));
        startup("shell close requested", message);
        gs_wait_for_close();
    }
};
#endif
bool directory(const char* path) {
#ifdef _WIN32
    return _mkdir(path) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0700) == 0 || errno == EEXIST;
#endif
}
bool filename(const char* value) {
    if (!value[0] || std::strstr(value, ".."))
        return false;
    for (const char* at = value; *at; ++at)
        if (!((*at >= 'a' && *at <= 'z') || (*at >= 'A' && *at <= 'Z') || (*at >= '0' && *at <= '9') || *at == '.' ||
              *at == '_' || *at == '-'))
            return false;
    return true;
}
bool report(const char* path, const shelf::UiState& state) {
    FILE* file = std::fopen(path, "wb");
    if (!file)
        return false;
    const auto& p = state.settings.preferences;
    const auto shown = shelf::shown_systems(state);
    std::fprintf(
        file, "{\"page\":%d,\"tab\":%d,\"focus\":%d,\"selected\":\"%.*s\",\"shown\":%zu,\"layout\":\"%s\",\"hidden\":[",
        static_cast<int>(state.page), static_cast<int>(state.tab), state.settings_focus,
        static_cast<int>(shelf::catalog[state.selected_system].id.size()),
        shelf::catalog[state.selected_system].id.data(), shown.count,
        p.layout == shelf::Layout::grid ? "grid" : p.layout == shelf::Layout::swipe ? "swipe" : "shelf");
    bool comma = false;
    for (std::size_t i = 0; i < shelf::system_count; ++i)
        if (p.hidden[i]) {
            std::fprintf(file, "%s\"%.*s\"", comma ? "," : "", static_cast<int>(shelf::catalog[i].id.size()),
                         shelf::catalog[i].id.data());
            comma = true;
        }
    std::fprintf(file,
                 "],\"queryLength\":%zu,\"pickerQueryLength\":%zu,\"launchConnected\":%s,\"libraryGames\":%zu,"
                 "\"libraryRoutes\":%zu,\"artworkCached\":%zu,\"artworkMissing\":%zu,\"artworkFailed\":%zu,\"monochromeMissing\":%s,\"shelfSingle\":%s,\"reducedMotion\":%s,"
                 "\"shelfPage\":%d,\"shelfSelected\":%zu,\"tvMode\":%d,\"tvTextLength\":%zu,\"favoriteGames\":%zu,"
                 "\"tileRemoving\":%s,\"tileRequested\":%s,\"tileAction\":%d,\"tileFocus\":%d,"
                 "\"tileRemovalBatchReady\":%s,\"tileRemovalCount\":%zu}\n",
                 std::strlen(state.query), std::strlen(state.console_query), state.launch_connected ? "true" : "false",
                 state.library ? state.library->game_count : 0, state.library ? state.library->route_count : 0,
                 state.library ? state.library->artwork.cached : 0, state.library ? state.library->artwork.missing : 0,
                 state.library ? state.library->artwork.failed : 0,
                 p.monochrome_missing ? "true" : "false", p.shelf_single ? "true" : "false",
                 p.reduced_motion ? "true" : "false", state.focus / 8,
                 static_cast<std::size_t>(std::count(p.shelf_selected.begin(), p.shelf_selected.end(), true)),
                 static_cast<int>(p.tv_mode), std::strlen(state.settings.tv_text),
                 static_cast<std::size_t>(std::count_if(state.settings.game_favorites.begin(), state.settings.game_favorites.end(),
                                                       [](std::uint64_t key) { return key != 0; })),
                 state.tile_removing[state.selected_system] ? "true" : "false",
                 state.tile_requested ? "true" : "false", static_cast<int>(state.pending_tile.action),
                 state.tile_focus,
                 state.tile_removal_batch_ready ? "true" : "false",
                 state.tile_removal_count);
    return std::fclose(file) == 0;
}
bool mapped_button(Uint8 button, const shelf::UiState& state, shelf::Action& action) {
    using shelf::Action;
    switch (button) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        action = Action::up;
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        action = Action::down;
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        action = Action::left;
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        action = Action::right;
        break;
    case SDL_CONTROLLER_BUTTON_A:
        action = Action::confirm;
        break;
    case SDL_CONTROLLER_BUTTON_B:
        action = Action::back;
        break;
    case SDL_CONTROLLER_BUTTON_X:
        if (state.page != shelf::Page::keyboard && state.page != shelf::Page::home)
            return false;
        action = state.page == shelf::Page::keyboard ? Action::erase : Action::layout;
        break;
    case SDL_CONTROLLER_BUTTON_Y:
        action = shelf::triangle_action(state.page);
        break;
    case SDL_CONTROLLER_BUTTON_START:
        action = Action::settings;
        break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        action = Action::previous_tab;
        break;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        action = Action::next_tab;
        break;
    default:
        return false;
    }
    return true;
}
bool script_event(const char* token) {
    if (std::strncmp(token, "wait:", 5) == 0) {
        char* end = nullptr;
        const auto delay = std::strtoul(token + 5, &end, 10);
        if (!token[5] || *end || delay > 2000)
            return false;
        SDL_Delay(static_cast<Uint32>(delay));
        return true;
    }
    if (std::strcmp(token, "loading") == 0 || std::strcmp(token, "reveal") == 0 || std::strcmp(token, "prepare") == 0 ||
        std::strcmp(token, "launch-art") == 0)
        return true;
#ifndef SHELF_PS5
    if (std::strcmp(token, "mock-tile-removing") == 0)
        return true;
    if (std::strcmp(token, "mock-tile-removal-queued") == 0)
        return true;
    if (std::strcmp(token, "mock-tile-all-installed") == 0)
        return true;
    if (std::strcmp(token, "mock-tile-removal-batch-queued") == 0)
        return true;
    if (std::strcmp(token, "mock-shortcut-route") == 0)
        return true;
    if (std::strcmp(token, "mock-pairing-sensitive") == 0)
        return true;
#ifdef SHELF_TILE_WRITE_QUARANTINE_TEST
    if (std::strcmp(token, "mock-tile-status-error") == 0)
        return true;
#endif
#endif
    if (std::strcmp(token, "circle-down") == 0 || std::strcmp(token, "circle-up") == 0) {
        SDL_Event event{};
        event.type = std::strcmp(token, "circle-down") == 0 ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
        event.cbutton.button = SDL_CONTROLLER_BUTTON_B;
        SDL_PushEvent(&event);
        return true;
    }
    if (std::strncmp(token, "shot:", 5) == 0 || std::strncmp(token, "report:", 7) == 0)
        return true;
    if (std::strncmp(token, "frame:", 6) == 0) {
        char* end = nullptr;
        const auto value = std::strtoul(token + 6, &end, 10);
        return token[6] && *end == 0 && value <= 2000;
    }
    const char* names[] = {"up",     "down",     "left",     "right",        "confirm", "back",
                           "layout", "favorite", "settings", "previous-tab", "tab"};
    const Uint8 buttons[] = {SDL_CONTROLLER_BUTTON_DPAD_UP,
                             SDL_CONTROLLER_BUTTON_DPAD_DOWN,
                             SDL_CONTROLLER_BUTTON_DPAD_LEFT,
                             SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
                             SDL_CONTROLLER_BUTTON_A,
                             SDL_CONTROLLER_BUTTON_B,
                             SDL_CONTROLLER_BUTTON_X,
                             SDL_CONTROLLER_BUTTON_Y,
                             SDL_CONTROLLER_BUTTON_START,
                             SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
                             SDL_CONTROLLER_BUTTON_RIGHTSHOULDER};
    for (int i = 0; i < 11; ++i)
        if (std::strcmp(token, names[i]) == 0) {
            SDL_Event event{};
            event.type = SDL_CONTROLLERBUTTONDOWN;
            event.cbutton.button = buttons[i];
            SDL_PushEvent(&event);
            event.type = SDL_CONTROLLERBUTTONUP;
            SDL_PushEvent(&event);
            return true;
        }
    if (std::strncmp(token, "text:", 5) == 0)
        return token[5] != 0;
    SDL_Event event{};
    event.type = SDL_KEYDOWN;
    if (std::strcmp(token, "consoles") == 0)
        event.key.keysym.sym = SDLK_F3;
    else if (std::strcmp(token, "filters") == 0)
        event.key.keysym.sym = SDLK_F2;
    else if (std::strcmp(token, "search") == 0)
        event.key.keysym.sym = SDLK_F4;
    else if (std::strcmp(token, "erase") == 0)
        event.key.keysym.sym = SDLK_BACKSPACE;
    else
        return false;
    SDL_PushEvent(&event);
    return true;
}
} // namespace

int main(int argc, char** argv) {
#ifdef SHELF_PS5
    PlatformExit platform_exit;
    startup("main entered", "versioned console tile workflow; settings fit the room layout");
    char missing[1024];
    if (gs_missing_imports(missing, sizeof(missing)) != 0) {
        startup("missing imports", missing);
        char message[1200];
        std::snprintf(message, sizeof(message), "isShelf cannot start: missing console functions: %s", missing);
        std::fprintf(stderr, "%s\n", message);
        gs_notify(message);
        return 1;
    }
    startup("imports ready");
    if (SDL_SetMemoryFunctions(gs_malloc, gs_calloc, gs_realloc, gs_free) != 0) {
        startup("SDL allocator rejected", SDL_GetError());
        gs_notify("isShelf could not initialise its memory allocator.");
        return 1;
    }
    startup("SDL allocator ready");
    const char* assets = "/app0";
    const char* data = "/data/console-shelf";
    shelf::ConsoleTileIntent console_tile_intent;
    bool console_tile_marker = false;
    const auto console_tile_input = shelf::select_console_tile_intent(
        argc, argv, shelf::console_tile_marker_path, console_tile_intent, &console_tile_marker);
    if (console_tile_input == shelf::ConsoleTileInput::invalid) {
        startup("console tile input invalid");
        gs_notify("isShelf shortcut data is invalid or conflicting. Open the main isShelf tile instead.");
        return 1;
    }
    if (console_tile_marker) {
        if (!console_tile_shared_assets_ready(console_tile_intent.system)) {
            startup("console tile shared assets missing");
            gs_notify("isShelf shortcut needs the main isShelf installation. Reopen the main tile first.");
            return 1;
        }
        assets = shelf::console_tile_shared_assets;
    }
#else
    const char* assets = nullptr;
    const char* data = nullptr;
#endif
    const char* captures = nullptr;
    const char* script_arg = nullptr;
    bool headless = false;
    bool animate_script = false;
    const char* library_host = nullptr;
    const char* library_root = nullptr;
    unsigned short library_port = 2121;
    bool scan_only = false;
    bool background_requested = false;
#ifndef SHELF_PS5
    unsigned mock_pairing_after_ms = 0;
#endif
#ifdef SHELF_HAS_UI_TEST_BRIDGE
    bool ui_test_requested = false;
#endif
#ifndef SHELF_PS5
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--asset-root") == 0 && i + 1 < argc)
            assets = argv[++i];
        else if (std::strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc)
            data = argv[++i];
        else if (std::strcmp(argv[i], "--capture-dir") == 0 && i + 1 < argc)
            captures = argv[++i];
        else if (std::strcmp(argv[i], "--script") == 0 && i + 1 < argc)
            script_arg = argv[++i];
        else if (std::strcmp(argv[i], "--headless") == 0)
            headless = true;
        else if (std::strcmp(argv[i], "--animate-script") == 0)
            animate_script = true;
        else if (std::strcmp(argv[i], "--library-ftp") == 0 && i + 1 < argc)
            library_host = argv[++i];
        else if (std::strcmp(argv[i], "--library-root") == 0 && i + 1 < argc)
            library_root = argv[++i];
        else if (std::strcmp(argv[i], "--library-port") == 0 && i + 1 < argc) {
            char* end = nullptr;
            const auto port = std::strtoul(argv[++i], &end, 10);
            if (!*argv[i] || *end || port == 0 || port > 65535)
                return 1;
            library_port = static_cast<unsigned short>(port);
        } else if (std::strcmp(argv[i], "--scan-only") == 0)
            scan_only = true;
        else if (std::strcmp(argv[i], "--background-scan") == 0)
            background_requested = true;
        else if (std::strcmp(argv[i], "--ui-test") == 0)
            ui_test_requested = true;
        else if (std::strcmp(argv[i], "--mock-pairing-after-ms") == 0 && i + 1 < argc) {
            char* end = nullptr;
            const auto delay = std::strtoul(argv[++i], &end, 10);
            if (!*argv[i] || *end || delay == 0 || delay > 5000)
                return 1;
            mock_pairing_after_ms = static_cast<unsigned>(delay);
        }
        else {
            std::fprintf(stderr, "Unknown or incomplete host option: %s\n", argv[i]);
            return 1;
        }
    }
#else
    // Console storage paths are owned constants, never supplied by launch arguments.
#ifdef SHELF_UI_TEST_BRIDGE
    char remote_arm_nonce[33]{};
    const auto remote_arm = shelf::parse_ui_test_remote_arm(
        argc, argv, console_tile_input, console_tile_marker, remote_arm_nonce);
    if (remote_arm == shelf::UiTestRemoteArmInput::valid) {
        if (shelf::ui_test_main_title_param("/app0/sce_sys/param.json")) {
            ui_test_requested = true;
            startup("private remote arm admitted", "fixed-title one-shot");
        } else
            startup("private remote arm ignored", "main title metadata unavailable");
    } else if (remote_arm == shelf::UiTestRemoteArmInput::invalid) {
        startup("private remote arm ignored", "invalid or shortcut launch argument");
        char shape[96];
        std::snprintf(shape, sizeof(shape), "argc %d tile_input %d marker %d",
                      argc, static_cast<int>(console_tile_input), console_tile_marker ? 1 : 0);
        startup("private remote arm shape", shape);
        // Log only bounded argument shapes, never the nonce or full argv.
        for (int i = 0; argv && i < argc && i < 4; ++i) {
            if (!argv[i]) {
                startup("private remote arm arg", "null");
                continue;
            }
            unsigned length = 0;
            while (length < 96 && argv[i][length]) ++length;
            const char* kind = !argv[i][0] ? "empty" :
                std::strncmp(argv[i], "--isshelf-ui-test-arm",
                             sizeof("--isshelf-ui-test-arm") - 1) == 0 ? "arm" :
                std::strncmp(argv[i], "--isshelf-console-tile",
                             sizeof("--isshelf-console-tile") - 1) == 0 ? "tile" :
                argv[i][0] == '-' ? "option" : "program";
            std::snprintf(shape, sizeof(shape), "index %d class %s length %u%s",
                          i, kind, length, length == 96 ? "+" : "");
            startup("private remote arm arg", shape);
        }
    }
#else
    (void)argc;
    (void)argv;
#endif
    library_host = "127.0.0.1";
#endif
#ifdef SHELF_HAS_UI_TEST_BRIDGE
    if (ui_test_requested && (script_arg || scan_only)) {
        std::fprintf(stderr, "UI test control requires the interactive event loop.\n");
        return 1;
    }
#endif
    if (library_host && library_root) {
        std::fprintf(stderr, "Choose one explicit library source.\n");
        return 1;
    }
    if (!assets || !data || std::strlen(assets) > 2000 || std::strlen(data) > 2000 ||
        (script_arg && std::strlen(script_arg) > 8191)) {
        std::fprintf(stderr, "Use --asset-root and --data-dir; optional --headless --script and --capture-dir.\n");
        return 1;
    }
    if (!directory(data)) {
        char error[64];
        std::snprintf(error, sizeof(error), "errno=%d", errno);
        startup("owned directory unavailable", error);
        std::fprintf(stderr, "Cannot create the requested owned data directory.\n");
#ifdef SHELF_PS5
        gs_notify("isShelf cannot access its own settings folder: /data/console-shelf.");
#endif
        return 1;
    }
    if (!captures)
        captures = data;
    if (std::strlen(captures) > 2000 || !directory(captures)) {
        startup("capture directory unavailable");
        std::fprintf(stderr, "Cannot create capture directory.\n");
        return 1;
    }
    startup("owned data ready");
    char settings_path[2304];
    std::snprintf(settings_path, sizeof(settings_path), "%s/settings.ini", data);
    shelf::UiState state;
#ifdef SHELF_PS5
    state.launch_connected = true;
#endif
    struct LibraryOwner {
        shelf::Library* value = nullptr;
        ~LibraryOwner() {
            if (value) {
                value->~Library();
                shelf::release(value);
            }
        }
    } library;
    shelf::ReadOnlyFtp ftp;
    shelf::ReadOnlyTree tree;
    LibraryScan scan;
    const bool background_scan = !scan_only && (background_requested || (!headless && !script_arg));
    if (library_host || library_root) {
        const bool ready = library_host ? shelf::init_read_only_ftp(ftp, library_host, library_port)
                                        : shelf::init_read_only_tree(tree, library_root);
        if (!ready) {
            startup("library source init failed");
            std::fprintf(stderr, "Library source could not initialise.\n");
            return 1;
        }
        void* memory = shelf::allocate(sizeof(shelf::Library));
        if (!memory) {
            std::fprintf(stderr, "Library memory allocation failed.\n");
            return 1;
        }
        library.value = new (memory) shelf::Library{};
        scan.source = library_host ? shelf::ftp_filesystem(ftp) : shelf::tree_filesystem(tree);
        scan.library = library.value;
        scan.data_root = data;
        ftp.cancelled = LibraryScan::stopping;
        ftp.cancel_context = &scan;
        if (!background_scan) {
            startup("library scan starting");
            LibraryScan::run(&scan);
            accept_library(state, *library.value, ftp, scan_only);
            if (scan_only)
                return library.value->scan.complete ? 0 : 2;
        } else {
            state.presentation.loading = true;
            state.library_discovering = true;
        }
    } else if (scan_only)
        return 1;
    const auto loaded = shelf::load_settings(settings_path, state.settings);
    if (loaded == shelf::SettingsLoad::backup)
        std::snprintf(state.notice, sizeof(state.notice), "Settings recovered from the last valid backup.");
    if (loaded == shelf::SettingsLoad::invalid)
        std::snprintf(state.notice, sizeof(state.notice),
                      "Settings could not be read. Defaults are in use; the original file is kept.");
    if (state.library && !state.library->scan.complete && !state.notice[0])
        std::snprintf(state.notice, sizeof(state.notice),
                      "Library scan incomplete. Keep ftpsrv running and reopen isShelf to try again.");
#ifdef SHELF_PS5
    char update_result_notice[192]{};
    if (shelf::take_update_result(data, update_result_notice)) {
        std::snprintf(state.notice, sizeof(state.notice), "%s", update_result_notice);
        startup("in-app update result", update_result_notice);
        char message[256];
        std::snprintf(message, sizeof(message), "isShelf update: %s", update_result_notice);
        gs_notify(message);
    }
#endif
    SDL_SetMainReady();
#ifdef SHELF_PS5
    startup("video starting");
    SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");
    const char* video_error = gs_vout_init();
    if (video_error) {
        startup("video failed", video_error);
        gs_notify(video_error);
        return 1;
    }
    startup("video ready");
#else
    if (headless)
        SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
#endif
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    startup("SDL starting");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS
#ifndef SHELF_PS5
                 | SDL_INIT_VIDEO
#endif
                 ) < 0) {
        startup("SDL failed", SDL_GetError());
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    startup("SDL ready");
    SDL_Window* window = nullptr;
#ifndef SHELF_PS5
    window = headless
                 ? nullptr
                 : SDL_CreateWindow("isShelf - native host build", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!headless && !window) {
        std::fprintf(stderr, "Window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
#endif
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, 1920, 1080, 32, SDL_PIXELFORMAT_ABGR8888);
    SDL_Renderer* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
    startup("graphics starting");
    if (!renderer || !shelf::graphics_init(renderer, assets, data)) {
        startup("graphics failed", SDL_GetError());
        std::fprintf(stderr, "Native renderer or packaged fonts could not load.\n");
        if (renderer)
            SDL_DestroyRenderer(renderer);
        if (surface)
            SDL_FreeSurface(surface);
        if (window)
            SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    startup("graphics ready");
    std::array<SDL_GameController*, 8> controllers{};
    if (!headless)
        for (int i = 0; i < SDL_NumJoysticks() && i < 8; ++i)
            if (SDL_IsGameController(i))
                controllers[static_cast<std::size_t>(i)] = SDL_GameControllerOpen(i);
    SDL_StartTextInput();
    char script[8192]{};
    if (script_arg)
        std::strcpy(script, script_arg);
    else if (headless
#ifdef SHELF_HAS_UI_TEST_BRIDGE
             && !ui_test_requested
#endif
    )
        std::strcpy(script, "shot:home.png");
    char* next = script[0] ? script : nullptr;
    bool scripting = next != nullptr;
    int result = 0;
    shelf::NavigationInput navigation;
    shelf::CloseGesture close_gesture;
    shelf::CloseHint close_hint;
    shelf::LaunchClient launcher;
#ifdef SHELF_PS5
    shelf::UpdateClient updater;
    shelf::UpdateRelease offered_update{};
    char recovery_update_tag[32]{}, recovery_manifest_sha256[65]{};
    bool recovery_committed = false;
    bool update_initialized = false, update_preparing = false;
    int update_download_quarter = 0;
    bool update_download_finished_notice = false;
    bool update_preparing_files_notice = false;
    char update_ca_path[2304];
    std::snprintf(update_ca_path, sizeof(update_ca_path), "%s/assets/update-ca.pem", assets);
#endif
    shelf::LaunchVisibilityGate launch_visibility;
    bool ui_test_probing = false;
#ifdef SHELF_HAS_UI_TEST_BRIDGE
    bool ui_test_probe_done = false;
#endif
#ifdef SHELF_PS5
    ConsoleTileWorker tile_worker;
    std::size_t tile_inspected_system = shelf::system_count;
    std::size_t tile_probe_picker_system = shelf::system_count;
    state.tile_busy = tile_worker.start(ConsoleTileWorker::Job::probe);
#endif
    bool redraw = true;
    bool first_frame = true;
    bool scan_started = false;
    const auto opened_at = scripting ? 0u : SDL_GetTicks();
    const bool automatic_intro = !scripting && state.presentation.loading;
    bool library_accepted = !background_scan || !scan.library, opening_prepared = !automatic_intro;
#ifdef SHELF_PS5
    bool console_tile_applied = false;
#endif
    shelf::OpeningGate opening_gate;
    bool intro_started = !automatic_intro, previous_visible = false;
    bool visibility_sampled = false, frame_acknowledged = false, opening_logged = false;
    bool opening_inventory_fallback_logged = false;
    int last_visibility_status = 0;
    unsigned last_visibility_flags = 0, visibility_notes = 0;
    unsigned visible_time = 0, last_visibility_at = opened_at;
    unsigned intro_frames = 0, reveal_frames = 0;
    unsigned circle_at = 0;
    bool circle_tracked = false, circle_committed_logged = false;
#if defined(SHELF_PS5) && defined(SHELF_CAPTURE_STARTUP)
    bool captured_loading = false, captured_reveal = false;
#endif
    unsigned reveal_at = 0;
    bool reveal_pending = false;
    unsigned measured_frames = 0, max_render_ms = 0, max_present_ms = 0;
    unsigned script_time = 0;
    unsigned tv_redraw_at = 0;
#ifdef SHELF_HAS_UI_TEST_BRIDGE
    shelf::UiTestBridge ui_test;
    shelf::UiTestVideo ui_video;
#if defined(SHELF_PS5) && defined(SHELF_UI_TEST_BRIDGE)
    ConsoleTilePathProbeWorker probe_worker;
#endif
    shelf::UiTestRequest ui_test_request;
    std::uint64_t ui_revision = 1;
    unsigned ui_test_started = 0, ui_test_polled = 0, ui_test_capture_at = 0;
    bool ui_test_pending = false, ui_test_applied = false, ui_test_saved = true;
    bool ui_test_finish = false;
    bool ui_test_closing = false, ui_test_close_finished = false;
    std::size_t ui_test_probe_bytes = 0;
    std::uint64_t ui_test_probe_fnv = 0;
    shelf::UiTestPs2SourceReport ui_test_ps2_source{};
    bool ui_test_ps2_ran = false;
    shelf::UiTestPs2DirectReport ui_test_ps2_direct{};
    bool ui_test_ps2_direct_ran = false;
    shelf::UiTestRouteStoreReport ui_test_route_store{};
    bool ui_test_route_store_ran = false;
    shelf::tile_route::RoutePathReport ui_test_route_path{};
    bool ui_test_route_path_ran = false;
    shelf::tile_route::SettingsPathReport ui_test_settings_path{};
    bool ui_test_settings_path_ran = false;
    constexpr SDL_JoystickID remote_close_controller = -2;
    const char* ui_test_result = "ready";
    SDL_JoystickID ui_test_controller = -1;
    bool ui_test_left = false, ui_test_right = false, ui_test_chord = false;
#if defined(SHELF_UI_TEST_BRIDGE)
    constexpr bool ui_test_private_diagnostics = true;
#else
    constexpr bool ui_test_private_diagnostics = false;
#endif
    auto local_ui_changed = [&]() {
        ++ui_revision;
        if (ui_test_pending && ui_test_request.sequence && !ui_test_closing && !ui_test_probing)
            ui_test_result = "interrupted";
    };
    auto arm_ui_test = [&]() {
        if (!shelf::pairing_capture_allowed(state.page))
            return;
        char session[33];
        std::snprintf(session, sizeof(session), "%016llx%016llx",
                      static_cast<unsigned long long>(SDL_GetPerformanceCounter()),
                      static_cast<unsigned long long>(std::time(nullptr)));
        if (!ui_test.start(data, session, ui_test_private_diagnostics)) {
            std::snprintf(state.notice, sizeof(state.notice), "Remote support could not open its own folder.");
            redraw = true;
            return;
        }
        ui_test_started = SDL_GetTicks();
        ui_test_polled = ui_test_started;
        ui_test_request = {};
        std::strcpy(ui_test_request.name, "snapshot");
        ui_test_pending = true;
        ui_test_applied = false;
        ui_test_saved = true;
        ui_test_finish = false;
        ui_test_closing = false;
        ui_test_close_finished = false;
        ui_test_probing = false;
        ui_test_probe_done = false;
        ui_test_probe_bytes = 0;
        ui_test_probe_fnv = 0;
        ui_test_ps2_source = {};
        ui_test_ps2_ran = false;
        ui_test_ps2_direct = {};
        ui_test_ps2_direct_ran = false;
        ui_test_route_store = {};
        ui_test_route_store_ran = false;
        ui_test_route_path = {};
        ui_test_route_path_ran = false;
        ui_test_settings_path = {};
        ui_test_settings_path_ran = false;
        ui_test_result = "ready";
        ui_test_capture_at = ui_test_started + 500;
        ++ui_revision;
        if (!ui_video.start(data, ui_test.session(), ui_revision))
            startup("private preview unavailable", "menu control remains available");
        std::snprintf(state.notice, sizeof(state.notice),
                      ui_test_private_diagnostics
                          ? "Private test control enabled. L3 + R3 disables it."
                          : "Remote support enabled. L3 + R3 disables it; private diagnostics stay unavailable.");
        startup(ui_test_private_diagnostics ? "ISSHELF_PRIVATE_UI_TEST_BRIDGE_V1"
                                            : "owner remote support enabled",
                "physical L3 + R3 opt-in; bounded menu actions only");
        redraw = true;
    };
    if (ui_test_requested)
        arm_ui_test();
#ifdef SHELF_UI_TEST_BRIDGE
    if (!ui_test_requested) {
        std::snprintf(state.notice, sizeof(state.notice), "Private UI test build: click L3 + R3 to enable menu control.");
        // This retained marker is independently checked before release packaging.
        startup("ISSHELF_PRIVATE_UI_TEST_BRIDGE_V1", "disabled until owner clicks L3 + R3");
    }
#endif
#endif
#ifdef SHELF_PS5
    bool update_eligible = console_tile_input == shelf::ConsoleTileInput::absent;
#ifdef SHELF_UI_TEST_BRIDGE
    update_eligible = update_eligible && !ui_test_requested;
#endif
#endif
    while (!state.exit_requested) {
        char* token = nullptr;
        if (scripting) {
            if (!next)
                break;
            token = next;
            char* comma = std::strchr(next, ',');
            if (comma) {
                *comma = 0;
                next = comma + 1;
            } else
                next = nullptr;
            if (!script_event(token)) {
                std::fprintf(stderr, "Unknown scripted action.\n");
                result = 1;
                break;
            }
            // Feed scripted UTF-8 straight to the UI. SDL2-compat backed by
            // SDL3 crashes when a synthetic SDL_TEXTINPUT event is queued.
            if (std::strncmp(token, "text:", 5) == 0)
                shelf::text_input(state, token + 5);
            if (std::strncmp(token, "frame:", 6) == 0)
                script_time += static_cast<unsigned>(std::strtoul(token + 6, nullptr, 10));
            if (std::strcmp(token, "loading") == 0)
                state.presentation.loading = true;
            if (std::strcmp(token, "launch-art") == 0)
                state.presentation.artwork = true;
#ifndef SHELF_PS5
            if (std::strcmp(token, "mock-pairing-sensitive") == 0)
                state.page = shelf::Page::pairing_code;
            if (std::strcmp(token, "mock-tile-removing") == 0) {
                state.tile_removing[state.selected_system] = true;
                state.tile_installed[state.selected_system] = false;
                state.tile_removal_handoff[state.selected_system] = false;
                state.tile_connected = true;
                state.tile_focus = 0;
            }
            if (std::strcmp(token, "mock-tile-removal-queued") == 0) {
                state.tile_removing[state.selected_system] = true;
                state.tile_installed[state.selected_system] = false;
                state.tile_removal_handoff[state.selected_system] = true;
                state.tile_connected = true;
                state.tile_focus = 0;
                state.tile_residual_sweep = false;
                state.tile_removal_count = 0;
                state.tile_removal_titles[0] = 0;
                (void)shelf::queue_console_tile_removal(state, "PPSA99701");
                std::snprintf(state.notice, sizeof(state.notice),
                              "Removal queued. Close isShelf so ShadowMount can update PS5 Home.");
            }
            if (std::strcmp(token, "mock-tile-all-installed") == 0) {
                state.tile_installed.fill(true);
                state.tile_removing.fill(false);
                state.tile_removal_handoff.fill(false);
                state.tile_removal_batch_ready = false;
                state.tile_residual_sweep = false;
                state.tile_removal_count = 0;
                state.tile_removal_titles[0] = 0;
                state.tile_connected = true;
                state.tile_focus = 0;
            }
            if (std::strcmp(token, "mock-tile-removal-batch-queued") == 0) {
                state.tile_installed.fill(false);
                state.tile_removing.fill(true);
                state.tile_removal_handoff.fill(true);
                state.tile_removal_batch_ready = false;
                state.tile_residual_sweep = false;
                state.tile_removal_count = 0;
                state.tile_removal_titles[0] = 0;
                state.tile_connected = true;
                state.tile_focus = 0;
                for (std::size_t system = 0; system < shelf::system_count; ++system) {
                    char title[10]{};
                    std::snprintf(title, sizeof(title), "PPSA%05zu", 99700 + system);
                    if (!shelf::queue_console_tile_removal(state, title)) {
                        std::fprintf(stderr, "Bulk removal fixture unavailable.\n");
                        result = 1;
                        break;
                    }
                }
                if (result != 0)
                    break;
                state.tile_removal_batch_ready = true;
                std::snprintf(state.notice, sizeof(state.notice),
                              "All owned tiles are queued. Close isShelf so ShadowMount can update PS5 Home.");
            }
            if (std::strcmp(token, "mock-shortcut-route") == 0) {
                const shelf::ConsoleTileIntent source{shelf::find_system("ps2"),
                                                      shelf::ConsoleTileDestination::home, {}};
                if (!shelf::begin_console_tile_route_choice(state, source)) {
                    std::fprintf(stderr, "Shortcut route fixture unavailable.\n");
                    result = 1;
                    break;
                }
            }
#ifdef SHELF_TILE_WRITE_QUARANTINE_TEST
            if (std::strcmp(token, "mock-tile-status-error") == 0) {
                state.tile_connected = false;
                state.tile_observation_system = state.selected_system;
                std::snprintf(state.tile_observation, sizeof(state.tile_observation),
                              "ShadowMount loopback version request failed (connection or HTTP framing).");
                state.notice[0] = '\0';
            }
#endif
#endif
            if (std::strcmp(token, "reveal") == 0) {
                state.presentation.loading = false;
                state.presentation.reveal = 0;
                reveal_pending = true;
            }
            redraw = true;
        }
        const unsigned now = scripting ? script_time : SDL_GetTicks();
#ifdef SHELF_PS5
        const bool update_surface_ready = !state.presentation.loading &&
                                          state.presentation.reveal >= 1 &&
                                          !state.exit_requested && !close_gesture.active();
        if (update_eligible && update_surface_ready && !update_initialized) {
            update_initialized = true;
            startup(update_client_marker, ISSHELF_VERSION);
            if (shelf::update_recovery_pending(data, recovery_update_tag,
                                               recovery_manifest_sha256,
                                               recovery_committed)) {
                state.update_prompt = shelf::UpdatePrompt::recovery;
                state.update_focus = 0;
                std::snprintf(state.update_tag, sizeof(state.update_tag), "%s",
                              recovery_update_tag);
                std::snprintf(state.update_message, sizeof(state.update_message), "%s",
                              recovery_committed
                                  ? "The update committed before power was interrupted; finish its cleanup now."
                                  : "Verified update files were preserved and can safely continue.");
                startup("in-app update recovery available",
                        recovery_committed ? "committed cleanup" : "resumable transaction");
                redraw = true;
            } else if (updater.check(data, update_ca_path, ISSHELF_VERSION)) {
                startup("in-app update check started", ISSHELF_VERSION);
            } else {
                startup("in-app update check unavailable");
            }
        }
        if (state.update_cancel_requested) {
            state.update_cancel_requested = false;
            updater.stop();
            update_preparing = false;
            state.update_prompt = shelf::UpdatePrompt::none;
            std::snprintf(state.notice, sizeof(state.notice),
                          "Update paused safely. Choose Update now later to resume it.");
            startup("in-app update paused", "partial files retained for resume");
            gs_notify("isShelf update paused safely. The partial download was kept for later.");
            redraw = true;
        }
        if (state.update_accept_requested) {
            state.update_accept_requested = false;
            if (state.update_prompt == shelf::UpdatePrompt::available) {
                if (updater.prepare(offered_update, data, update_ca_path)) {
                    update_preparing = true;
                    update_download_quarter = 0;
                    update_download_finished_notice = false;
                    update_preparing_files_notice = false;
                    state.update_prompt = shelf::UpdatePrompt::preparing;
                    state.update_progress = 0;
                    std::snprintf(state.update_message, sizeof(state.update_message),
                                  "Downloading and verifying the official production package.");
                    startup("owner approved in-app update", offered_update.tag);
                    char message[160];
                    std::snprintf(message, sizeof(message),
                                  "Downloading isShelf %s (resumable): 0%%", offered_update.tag);
                    gs_notify(message);
                } else {
                    state.update_prompt = shelf::UpdatePrompt::none;
                    std::snprintf(state.notice, sizeof(state.notice),
                                  "The update could not start. Nothing was changed.");
                }
                redraw = true;
            } else if (state.update_prompt == shelf::UpdatePrompt::recovery) {
                state.update_prompt = shelf::UpdatePrompt::none;
                state.pending_launch = {};
                state.pending_launch.mode = shelf::LaunchMode::update_recover;
                std::snprintf(state.pending_launch.title, sizeof(state.pending_launch.title), "%s",
                              shelf::shelf_title_id);
                std::snprintf(state.pending_launch.rom, sizeof(state.pending_launch.rom), "%s",
                              recovery_update_tag);
                std::snprintf(state.pending_launch.core, sizeof(state.pending_launch.core), "%s",
                              recovery_manifest_sha256);
                state.launch_requested = true;
                startup("owner approved update resume", recovery_update_tag);
                gs_notify("isShelf update recovery approved. isShelf will close, finish safely, and reopen.");
                redraw = true;
            }
        }
        if (update_preparing) {
            const int progress = updater.progress();
            if (progress != state.update_progress) {
                state.update_progress = progress;
                redraw = true;
            }
            if (progress >= 5 && progress < 50) {
                const int download_percent = std::clamp((progress - 5) * 100 / 45, 0, 99);
                const int quarter = std::min(3, download_percent / 25);
                if (quarter > update_download_quarter) {
                    update_download_quarter = quarter;
                    char message[160];
                    std::snprintf(message, sizeof(message), "Downloading isShelf %s: %d%%",
                                  offered_update.tag, quarter * 25);
                    gs_notify(message);
                }
            }
            if (progress >= 50 && !update_download_finished_notice) {
                update_download_finished_notice = true;
                gs_notify("isShelf download complete. Checking its SHA-256 signature...");
            }
            if (progress > 50 && !update_preparing_files_notice) {
                update_preparing_files_notice = true;
                gs_notify("isShelf package verified. Preparing the installation files...");
            }
        }
        shelf::UpdateWorkOutcome update_outcome;
        if (updater.take(update_outcome)) {
            const bool was_preparing = update_preparing;
            update_preparing = false;
            if (update_outcome.state == shelf::UpdateWorkState::available) {
                offered_update = update_outcome.release;
                state.update_prompt = shelf::UpdatePrompt::available;
                state.update_focus = 0;
                std::snprintf(state.update_tag, sizeof(state.update_tag), "%s", offered_update.tag);
                std::snprintf(state.update_message, sizeof(state.update_message), "%s",
                              update_outcome.message);
                startup("production update available", offered_update.tag);
                char message[160];
                std::snprintf(message, sizeof(message),
                              "isShelf %s is available. Choose Update now to install it.",
                              offered_update.tag);
                gs_notify(message);
            } else if (update_outcome.state == shelf::UpdateWorkState::ready) {
                state.update_prompt = shelf::UpdatePrompt::none;
                state.pending_launch = {};
                state.pending_launch.mode = shelf::LaunchMode::self_update;
                std::snprintf(state.pending_launch.title, sizeof(state.pending_launch.title), "%s",
                              shelf::shelf_title_id);
                std::snprintf(state.pending_launch.rom, sizeof(state.pending_launch.rom), "%s",
                              update_outcome.release.tag);
                std::snprintf(state.pending_launch.core, sizeof(state.pending_launch.core), "%s",
                              update_outcome.manifest_sha256);
                state.launch_requested = true;
                startup("verified update staged", update_outcome.release.tag);
                char message[192];
                std::snprintf(message, sizeof(message),
                              "isShelf %s is verified and ready. It will close to install, then reopen.",
                              update_outcome.release.tag);
                gs_notify(message);
            } else if (update_outcome.state == shelf::UpdateWorkState::cancelled) {
                state.update_prompt = shelf::UpdatePrompt::none;
                std::snprintf(state.notice, sizeof(state.notice),
                              "Update paused safely. It can resume later.");
            } else if (update_outcome.state == shelf::UpdateWorkState::failed) {
                state.update_prompt = shelf::UpdatePrompt::none;
                startup(was_preparing ? "update preparation failed" : "background update check failed",
                        update_outcome.message);
                if (was_preparing)
                    std::snprintf(state.notice, sizeof(state.notice), "%s", update_outcome.message);
                if (was_preparing) {
                    char message[256];
                    std::snprintf(message, sizeof(message),
                                  "isShelf update stopped: %s Nothing was installed.",
                                  update_outcome.message);
                    gs_notify(message);
                }
            } else {
                startup("background update check complete", update_outcome.message);
            }
            redraw = true;
        }
#endif
#ifndef SHELF_PS5
        if (mock_pairing_after_ms && !scripting &&
            static_cast<unsigned>(now - opened_at) >= mock_pairing_after_ms) {
            state.page = shelf::Page::pairing_code;
            mock_pairing_after_ms = 0;
            redraw = true;
        }
#endif
        if (automatic_intro && (state.presentation.loading || state.presentation.reveal < 1)) {
            unsigned flags = 0;
            int visibility_status = 0;
            bool rendered_frame = !first_frame;
#ifdef SHELF_PS5
            visibility_status = gs_app_visibility(&flags);
            rendered_frame = gs_vout_has_frame() != 0;
#endif
            opening_gate.sample(now, visibility_status, flags);
            if (visibility_notes < 8 && (!visibility_sampled || visibility_status != last_visibility_status ||
                                         flags != last_visibility_flags)) {
                char info[120];
                std::snprintf(info, sizeof(info), "%u ms; result 0x%08x; overlay %u; background %u", now - opened_at,
                              static_cast<unsigned>(visibility_status), flags & 1, (flags >> 1) & 1);
                startup("opening visibility", info);
                ++visibility_notes;
            }
            visibility_sampled = true;
            last_visibility_status = visibility_status;
            last_visibility_flags = flags;
            if (rendered_frame && !frame_acknowledged) {
                frame_acknowledged = true;
                char info[64];
                std::snprintf(info, sizeof(info), "%u ms after graphics ready", now - opened_at);
                startup("first rendered flip acknowledged", info);
            }
            const bool visible = rendered_frame && opening_prepared && opening_gate.ready(now);
            if (visible && previous_visible)
                visible_time += now - last_visibility_at;
            previous_visible = visible;
            last_visibility_at = now;
            if (visible && !intro_started) {
                intro_started = true;
                char info[120];
                std::snprintf(info, sizeof(info), "%u ms; status %s", now - opened_at,
                              opening_gate.used_fallback() ? "unavailable; timed fallback" : "foreground; no overlay");
                startup("opening entrance started", info);
            }
        }
        const unsigned visual_now = automatic_intro ? visible_time : now;
        const unsigned visual_start = automatic_intro ? 0 : opened_at;
        if (scan.ready()) {
            scan.stop();
            accept_library(state, *library.value, ftp, false);
#ifdef SHELF_HAS_UI_TEST_BRIDGE
            local_ui_changed();
#endif
            library_accepted = true;
            redraw = true;
        }
#ifdef SHELF_PS5
        if (tile_worker.ready()) {
            const auto finished = tile_worker.job;
            const auto selected = tile_worker.system;
            const auto outcome = tile_worker.outcome;
            const auto emulator_batch_outcome = tile_worker.emulator_batch_outcome;
            const auto batch_outcome = tile_worker.batch_outcome;
            const auto removal_batch_outcome = tile_worker.removal_batch_outcome;
            const auto requested_intent = tile_worker.intent;
            const bool connected = tile_worker.connected;
            tile_worker.join();
            state.tile_busy = false;
            state.tile_batch_busy = false;
            auto upsert_route_sync = shelf::tile_route::UpsertRouteSync::not_applicable;
            if (finished == ConsoleTileWorker::Job::probe) {
                state.tile_connected = connected;
                startup(connected ? (shelf::console_tile_writes_enabled ? "console tile job sender ready" : "console tile status reader ready")
                                  : (shelf::console_tile_writes_enabled ? "console tile job sender unavailable" : "console tile status reader unavailable"),
                        tile_worker.availability.message);
                if (!connected && state.page == shelf::Page::console_tile) {
                    if (!shelf::console_tile_writes_enabled) {
                        state.tile_observation_system = selected < shelf::system_count
                            ? selected : state.selected_system;
                        std::snprintf(state.tile_observation, sizeof(state.tile_observation),
                                      "%s", tile_worker.availability.message);
                        state.notice[0] = '\0';
                    } else
                        std::snprintf(state.notice, sizeof(state.notice), "%s",
                                      tile_worker.availability.message);
                }
            } else if (finished == ConsoleTileWorker::Job::upsert_emulators) {
                bool routes_saved = emulator_batch_outcome.committed == 0;
                if (emulator_batch_outcome.committed) {
                    shelf::Settings readback;
                    routes_saved = shelf::tile_route::remember_shortcut_routes(
                        settings_path, state.settings,
                        emulator_batch_outcome.intents,
                        emulator_batch_outcome.committed_systems,
                        state.library, readback);
                    if (routes_saved) {
                        state.settings = readback;
                        state.changed = false;
                    }
                }
                for (std::size_t system = 0; system < shelf::system_count; ++system)
                    if (emulator_batch_outcome.committed_systems[system]) {
                        state.tile_installed[system] = true;
                        state.tile_removing[system] = false;
                        state.tile_removal_handoff[system] = false;
                    }
                state.tile_removal_batch_ready = false;
                state.tile_residual_sweep = false;
                state.tile_emulator_batch_created = emulator_batch_outcome.created;
                state.tile_emulator_batch_updated = emulator_batch_outcome.updated;
                state.tile_emulator_batch_close_ready =
                    emulator_batch_outcome.complete &&
                    emulator_batch_outcome.close_required && routes_saved;
                if (!routes_saved)
                    std::snprintf(state.notice, sizeof(state.notice),
                                  "Emulator tiles changed, but their launch choices could not be saved. Run the action again.");
                else
                    std::snprintf(state.notice, sizeof(state.notice), "%s",
                                  emulator_batch_outcome.message);
                startup(emulator_batch_outcome.complete && routes_saved
                            ? "console tile emulator batch complete"
                            : "console tile emulator batch stopped",
                        routes_saved ? emulator_batch_outcome.message
                                     : "committed route settings readback failed");
            } else if (finished == ConsoleTileWorker::Job::refresh_installed) {
                std::snprintf(state.notice, sizeof(state.notice), "%s",
                              batch_outcome.message);
                if (batch_outcome.handoff_required) {
                    if (shelf::begin_console_tile_refresh_handoff(
                            state, batch_outcome.handoff_titles,
                            sizeof(batch_outcome.handoff_titles)))
                        startup("console tile bulk refresh handoff",
                                "cooperative close requested");
                    else
                        std::snprintf(state.notice, sizeof(state.notice),
                                      "Shortcut sources were updated, but the bounded Home artwork handoff could not start.");
                }
                startup(batch_outcome.complete
                            ? "console tile bulk update complete"
                            : "console tile bulk update stopped",
                        batch_outcome.message);
            } else if (finished == ConsoleTileWorker::Job::remove_installed) {
                std::snprintf(state.notice, sizeof(state.notice), "%s",
                              removal_batch_outcome.message);
                state.tile_residual_sweep = removal_batch_outcome.residual_sweep;
                if (state.tile_residual_sweep) {
                    state.tile_removal_count = 0;
                    state.tile_removal_titles[0] = 0;
                }
                for (std::size_t system = 0; system < shelf::system_count; ++system) {
                    if (!removal_batch_outcome.queued_systems[system])
                        continue;
                    state.tile_installed[system] = false;
                    state.tile_removing[system] = true;
                    state.tile_removal_handoff[system] = true;
                }
                if (removal_batch_outcome.handoff_required) {
                    if (shelf::queue_console_tile_removals(
                            state, removal_batch_outcome.handoff_titles,
                            sizeof(removal_batch_outcome.handoff_titles)))
                        state.tile_removal_batch_ready = state.tile_removal_count != 0;
                    else
                        std::snprintf(state.notice, sizeof(state.notice),
                                      "Tile sources were isolated, but the bounded post-close removal list could not be prepared.");
                    if (!state.tile_removal_batch_ready)
                        state.tile_residual_sweep = false;
                } else if (removal_batch_outcome.complete)
                    shelf::clear_console_tile_removal_state(state);
                else
                    state.tile_residual_sweep = false;
                if (state.selected_system < shelf::system_count &&
                    state.tile_removing[state.selected_system])
                    state.tile_focus = 0;
                startup(removal_batch_outcome.complete
                            ? "console tile bulk removal queued"
                            : "console tile bulk removal stopped",
                        removal_batch_outcome.message);
            } else if (selected < shelf::system_count) {
                tile_inspected_system = selected;
                if (!shelf::console_tile_writes_enabled &&
                    finished == ConsoleTileWorker::Job::inspect) {
                    state.tile_observation_system = selected;
                    std::snprintf(state.tile_observation, sizeof(state.tile_observation),
                                  "%s", outcome.message);
                }
                state.tile_installed[selected] = outcome.owned &&
                    outcome.stage != shelf::ConsoleTileStage::removed_pending &&
                    outcome.stage != shelf::ConsoleTileStage::conflict &&
                    outcome.stage != shelf::ConsoleTileStage::failed;
                state.tile_removing[selected] = outcome.stage == shelf::ConsoleTileStage::removed_pending ||
                    (finished == ConsoleTileWorker::Job::confirm_removed &&
                     outcome.stage == shelf::ConsoleTileStage::failed && outcome.owned);
                state.tile_removal_handoff[selected] =
                    state.tile_removing[selected] && outcome.handoff_required;
                if (state.tile_removing[selected] && state.page == shelf::Page::console_tile &&
                    state.selected_system == selected)
                    state.tile_focus = 0;
                if (finished == ConsoleTileWorker::Job::upsert) {
                    shelf::Settings readback;
                    upsert_route_sync = shelf::tile_route::remember_upsert_route(
                        settings_path, state.settings, requested_intent, outcome,
                        state.library, readback);
                    if (upsert_route_sync == shelf::tile_route::UpsertRouteSync::saved) {
                        state.settings = readback;
                        state.changed = false;
                        startup("console tile launch choice synchronized",
                                shelf::catalog[selected].id.data());
                    } else if (upsert_route_sync == shelf::tile_route::UpsertRouteSync::failed) {
                        startup("console tile launch choice synchronization failed",
                                shelf::catalog[selected].id.data());
                    }
                }
                if (finished != ConsoleTileWorker::Job::inspect ||
                    (shelf::console_tile_writes_enabled &&
                     state.page == shelf::Page::console_tile && state.selected_system == selected)) {
                    if (upsert_route_sync == shelf::tile_route::UpsertRouteSync::failed)
                        std::snprintf(state.notice, sizeof(state.notice),
                                      "Shortcut changed, but its launch choice could not be saved. Try again.");
                    else
                        std::snprintf(state.notice, sizeof(state.notice), "%s", outcome.message);
                }
                if (outcome.stage == shelf::ConsoleTileStage::removed_pending &&
                    outcome.owned && outcome.handoff_required && outcome.title_id[0]) {
                    state.tile_residual_sweep = false;
                    if (shelf::queue_console_tile_removal(state, outcome.title_id))
                        startup("console tile removal queued",
                                "waiting for one owner-selected post-close handoff");
                    else
                        std::snprintf(state.notice, sizeof(state.notice),
                                      "Pending removal was found, but its bounded post-close queue could not be prepared.");
                }
                startup("console tile result", outcome.message);
                if (outcome.diagnostic[0])
                    startup("console tile allocator", outcome.diagnostic);
            }
            redraw = true;
        }
        if (state.page != shelf::Page::console_tile) {
            tile_inspected_system = shelf::system_count;
            tile_probe_picker_system = shelf::system_count;
            state.tile_observation_system = shelf::system_count;
            state.tile_observation[0] = '\0';
        }
        if (state.tile_requested && !tile_worker.thread
#if defined(SHELF_UI_TEST_BRIDGE)
            && !ui_test_probing
#endif
            ) {
            const auto request = state.pending_tile;
            state.tile_requested = false;
            const auto job = request.action == shelf::ConsoleTileAction::remove
                                 ? ConsoleTileWorker::Job::remove
                                 : request.action == shelf::ConsoleTileAction::confirm_removed
                                 ? ConsoleTileWorker::Job::confirm_removed
                             : request.action == shelf::ConsoleTileAction::remove_installed
                                 ? ConsoleTileWorker::Job::remove_installed
                             : request.action == shelf::ConsoleTileAction::upsert_emulators
                                 ? ConsoleTileWorker::Job::upsert_emulators
                             : request.action == shelf::ConsoleTileAction::refresh_installed
                                 ? ConsoleTileWorker::Job::refresh_installed
                                 : ConsoleTileWorker::Job::upsert;
            const auto selected = job == ConsoleTileWorker::Job::upsert_emulators ||
                                          job == ConsoleTileWorker::Job::refresh_installed ||
                                          job == ConsoleTileWorker::Job::remove_installed
                                      ? shelf::system_count
                                      : request.intent.system;
            if (state.tile_connected && tile_worker.start(job, selected,
                                                          request.intent, state.library,
                                                          &state.settings)) {
                state.tile_busy = true;
                state.tile_batch_busy = job == ConsoleTileWorker::Job::upsert_emulators ||
                                        job == ConsoleTileWorker::Job::refresh_installed ||
                                        job == ConsoleTileWorker::Job::remove_installed;
                startup("console tile request",
                        request.action == shelf::ConsoleTileAction::remove ? "remove"
                        : request.action == shelf::ConsoleTileAction::confirm_removed ? "confirm removed"
                        : request.action == shelf::ConsoleTileAction::remove_installed ? "remove installed"
                        : request.action == shelf::ConsoleTileAction::upsert_emulators ? "upsert emulators"
                        : request.action == shelf::ConsoleTileAction::refresh_installed ? "refresh installed"
                                                                                       : "upsert");
            } else
                std::snprintf(state.notice, sizeof(state.notice), "PS5 shortcut service is unavailable. Try reopening isShelf.");
            redraw = true;
        } else if (state.page == shelf::Page::console_tile && !state.tile_connected && !state.tile_busy &&
#if defined(SHELF_UI_TEST_BRIDGE)
                   !ui_test_probing &&
#endif
                   !tile_worker.thread && tile_probe_picker_system != state.selected_system &&
                   tile_worker.start(ConsoleTileWorker::Job::probe, state.selected_system)) {
            tile_probe_picker_system = state.selected_system;
            state.tile_busy = true;
            redraw = true;
        } else if (state.page == shelf::Page::console_tile && state.tile_connected && !state.tile_busy &&
#if defined(SHELF_UI_TEST_BRIDGE)
                   !ui_test_probing &&
#endif
                   !tile_worker.thread && tile_inspected_system != state.selected_system &&
                   tile_worker.start(ConsoleTileWorker::Job::inspect, state.selected_system)) {
            state.tile_busy = true;
            redraw = true;
        }
        if (state.route_save_requested) {
            const auto choice = state.pending_route_save;
            const bool apply_after = state.route_save_apply_after;
            state.route_save_requested = false;
            shelf::Settings readback;
            if (shelf::tile_route::remember_shortcut_route(settings_path, state.settings,
                                                          choice, state.library, readback)) {
                state.settings = readback;
                state.changed = false;
                state.route_save_status = shelf::RouteSaveStatus::saved;
                std::snprintf(state.notice, sizeof(state.notice),
                              "Saved this console's Home shortcut choice.");
                if (apply_after) {
                    const auto applied = shelf::apply_console_tile_intent(state, choice);
                    if (applied == shelf::ConsoleTileApply::library ||
                        applied == shelf::ConsoleTileApply::home ||
                        applied == shelf::ConsoleTileApply::emulator) {
                        state.shortcut_route_system = shelf::system_count;
                        state.shortcut_route_ready = false;
                        startup("console tile saved route", shelf::catalog[choice.system].id.data());
                    } else {
                        state.page = shelf::Page::shortcut_route;
                        state.launch_requested = false;
                        std::snprintf(state.notice, sizeof(state.notice),
                                      "Choice saved, but its destination changed. Choose again.");
                    }
                }
            } else {
                state.route_save_status = shelf::RouteSaveStatus::unavailable;
                std::snprintf(state.notice, sizeof(state.notice),
                              "Could not confirm this choice; nothing launched. Choose again.");
            }
            state.route_save_busy = false;
            state.route_save_apply_after = false;
            redraw = true;
        }
        if (console_tile_input == shelf::ConsoleTileInput::valid && !console_tile_applied) {
            const auto route = shelf::tile_route::resolve_remembered_launch(
                console_tile_input, console_tile_marker, console_tile_intent,
                argc, argv, state.settings, state.library);
            const bool early_surface =
                shelf::tile_route::can_select_surface_before_library(route);
            const bool presentation_ready = !state.presentation.loading &&
                                            state.presentation.reveal >= 1;
            if ((library_accepted || early_surface) &&
                (early_surface || presentation_ready ||
                 shelf::tile_route::can_handoff_before_reveal(route))) {
                if (route.state == shelf::tile_route::adoption::State::choose_route ||
                    route.state == shelf::tile_route::adoption::State::stale_emulator) {
                    if (shelf::begin_console_tile_route_choice(state, console_tile_intent))
                        startup("console tile route choice", shelf::catalog[console_tile_intent.system].id.data());
                    else {
                        startup("console tile route choice invalid");
                        state.exit_requested = true;
                    }
                } else if (shelf::tile_route::adoption::applicable(route))
                    apply_console_tile_intent_to_ui(state, route.intent);
                else {
                    startup("console tile route input invalid");
                    gs_notify("isShelf shortcut route is invalid. Open the main isShelf tile instead.");
                    state.exit_requested = true;
                }
                console_tile_applied = true;
                redraw = true;
            }
        }
#endif
#if defined(SHELF_PS5) && defined(SHELF_UI_TEST_BRIDGE)
        if (probe_worker.ready()) {
            probe_worker.stop();
            ui_test_result = probe_worker.complete ? "probe-recorded" : "probe-unknown";
            ui_test_probe_bytes = probe_worker.complete ? probe_worker.receipt_bytes : 0;
            ui_test_probe_fnv = probe_worker.complete ? probe_worker.receipt_fnv : 0;
            ui_test_capture_at = SDL_GetTicks();
            ui_test_probe_done = true;
            startup(probe_worker.complete ? "console tile probe receipt verified"
                                          : "console tile probe outcome unknown",
                    probe_worker.complete ? (probe_worker.write_probe ? "nonce-bound v4 receipt"
                                                                     : "nonce-bound v2 receipt")
                                          : (probe_worker.send_complete ? "ELF transfer complete; no verified receipt"
                                                                        : "ELF transfer unconfirmed; no verified receipt"));
            redraw = true;
        }
#endif
        // A normal scan publishes inventory before Home is revealed, so an
        // installed/favourites view never looks empty for a few seconds. The
        // bounded fallback still exposes responsive controls if FTP is slow or
        // unavailable; Home then labels discovery honestly until it completes.
        if (automatic_intro && state.presentation.loading && opening_prepared && intro_started &&
            shelf::opening_inventory_ready(library_accepted, visual_now - visual_start)) {
            if (!library_accepted && !opening_inventory_fallback_logged) {
                opening_inventory_fallback_logged = true;
                startup("opening inventory fallback", "scan still running after 6000 ms foreground time");
            }
            state.presentation.loading = false;
            state.presentation.reveal = 0;
            reveal_pending = true;
            redraw = true;
        }
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (automatic_intro && state.presentation.loading &&
                (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_KEYDOWN)) {
                const bool first_owner_escape = !opening_gate.used_owner_input();
                opening_gate.allow_owner_input(SDL_GetTicks());
                if (first_owner_escape && opening_gate.used_owner_input())
                    startup("opening owner-input escape", "visibility gate released after stable physical input");
            }
#ifdef SHELF_HAS_UI_TEST_BRIDGE
            if (event.type == SDL_KEYDOWN || event.type == SDL_TEXTINPUT ||
                event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP)
                local_ui_changed();
            if ((event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP) &&
                (event.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK ||
                 event.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK)) {
                if (!ui_test_left && !ui_test_right)
                    ui_test_controller = event.cbutton.which;
                if (ui_test_controller == event.cbutton.which) {
                    const bool down = event.type == SDL_CONTROLLERBUTTONDOWN;
                    (event.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK ? ui_test_left : ui_test_right) = down;
                    if (ui_test_left && ui_test_right && !ui_test_chord &&
                        !state.launch_busy && !close_gesture.active() && !ui_test_probing) {
                        if (ui_test.active()) {
                            ui_test.stop();
                            ui_video.stop();
                            ui_test_pending = true;
                            ui_test_result = "owner-stopped";
                            ui_test_capture_at = SDL_GetTicks();
                            std::snprintf(state.notice, sizeof(state.notice), "Remote support disabled.");
                            redraw = true;
                        } else
                            arm_ui_test();
                        ui_test_chord = true;
                    }
                    if (!ui_test_left && !ui_test_right)
                        ui_test_chord = false;
                }
                continue;
            }
#endif
            shelf::Action action = shelf::Action::up;
            bool mapped = false;
            if (event.type == SDL_QUIT)
                state.exit_requested = true;
            else if (event.type == SDL_TEXTINPUT && !ui_test_probing) {
                shelf::text_input(state, event.text.text);
                redraw = true;
            } else if (event.type == SDL_WINDOWEVENT) {
#ifdef SHELF_HAS_UI_TEST_BRIDGE
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                    local_ui_changed();
#endif
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                    navigation.cancel();
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                    close_gesture.cancel();
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                    close_hint.dismiss();
                redraw = true;
            } else if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                if (event.cbutton.button == SDL_CONTROLLER_BUTTON_B) {
                    close_hint.dismiss();
                    state.launch_requested = false;
                    if (state.launch_busy) {
                        launcher.cancel();
                        state.launch_busy = false;
                        std::snprintf(state.notice, sizeof(state.notice), "Launch cancelled.");
                    }
                    if (!close_gesture.active()) {
                        circle_at = now;
                        circle_tracked = true;
                        startup("Circle hold received", "3000 ms threshold");
                    }
                    close_gesture.press(event.cbutton.which, now);
                    navigation.cancel();
                    redraw = true;
                } else
                    mapped = mapped_button(event.cbutton.button, state, action);
                if (mapped && shelf::NavigationInput::directional(action))
                    mapped = navigation.button(action, true, event.cbutton.which, SDL_GetTicks(), action);
            } else if (event.type == SDL_CONTROLLERBUTTONUP) {
                if (event.cbutton.button == SDL_CONTROLLER_BUTTON_B) {
                    mapped = close_gesture.release(event.cbutton.which, now);
                    if (circle_tracked) {
                        char duration[80];
                        std::snprintf(duration, sizeof(duration), "%u ms; %s", now - circle_at,
                                      close_gesture.closing()  ? "committed"
                                      : close_gesture.active() ? "other controller"
                                                               : "released early");
                        startup("Circle released", duration);
                        if (!close_gesture.active())
                            circle_tracked = false;
                    }
                    action = shelf::Action::back;
                    redraw = true;
                } else if (mapped_button(event.cbutton.button, state, action) &&
                           shelf::NavigationInput::directional(action))
                    navigation.button(action, false, event.cbutton.which, SDL_GetTicks(), action);
            } else if (event.type == SDL_CONTROLLERDEVICEADDED && !headless) {
                for (auto& controller : controllers)
                    if (!controller && SDL_IsGameController(event.cdevice.which)) {
                        controller = SDL_GameControllerOpen(event.cdevice.which);
                        break;
                    }
            } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
#ifdef SHELF_HAS_UI_TEST_BRIDGE
                if (ui_test_controller == event.cdevice.which) {
                    ui_test_left = ui_test_right = ui_test_chord = false;
                    ui_test_controller = -1;
                    local_ui_changed();
                }
#endif
                navigation.disconnected(event.cdevice.which);
                close_gesture.cancel(event.cdevice.which);
                for (auto& controller : controllers)
                    if (controller &&
                        SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) == event.cdevice.which) {
                        SDL_GameControllerClose(controller);
                        controller = nullptr;
                    }
            } else if (event.type == SDL_CONTROLLERAXISMOTION) {
                if (event.caxis.axis != SDL_CONTROLLER_AXIS_LEFTX && event.caxis.axis != SDL_CONTROLLER_AXIS_LEFTY)
                    continue;
                mapped = navigation.axis(event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX, event.caxis.value,
                                         event.caxis.which, SDL_GetTicks(), action);
#ifdef SHELF_HAS_UI_TEST_BRIDGE
                if (mapped)
                    local_ui_changed();
#endif
            } else if (event.type == SDL_KEYDOWN) {
                mapped = true;
                switch (event.key.keysym.sym) {
                case SDLK_UP:
                    action = shelf::Action::up;
                    break;
                case SDLK_DOWN:
                    action = shelf::Action::down;
                    break;
                case SDLK_LEFT:
                    action = shelf::Action::left;
                    break;
                case SDLK_RIGHT:
                    action = shelf::Action::right;
                    break;
                case SDLK_RETURN:
                    action = shelf::Action::confirm;
                    break;
                case SDLK_ESCAPE:
                    action = shelf::Action::back;
                    break;
                case SDLK_F1:
                    action = shelf::Action::settings;
                    break;
                case SDLK_F2:
                    action = shelf::Action::filters;
                    break;
                case SDLK_F3:
                    action = shelf::Action::consoles;
                    break;
                case SDLK_F4:
                    action = shelf::Action::search;
                    break;
                case SDLK_PAGEUP:
                    action = shelf::Action::previous_tab;
                    break;
                case SDLK_PAGEDOWN:
                    action = shelf::Action::next_tab;
                    break;
                case SDLK_TAB:
                    action =
                        (event.key.keysym.mod & KMOD_SHIFT) ? shelf::Action::previous_tab : shelf::Action::next_tab;
                    break;
                case SDLK_BACKSPACE:
                    action = shelf::Action::erase;
                    break;
                default:
                    mapped = false;
                }
            }
            if (mapped && !ui_test_probing && !state.presentation.loading && !close_gesture.closing() &&
                (!state.launch_busy || action == shelf::Action::back)) {
                if (action != shelf::Action::back)
                    close_hint.dismiss();
                if (action == shelf::Action::back) {
                    state.launch_requested = false;
                    if (state.launch_busy) {
                        launcher.cancel();
                        state.launch_busy = false;
                    }
                }
                if (!shelf::NavigationInput::directional(action))
                    navigation.cancel();
                shelf::input(state, action);
                redraw = true;
            }
        }
#ifdef SHELF_HAS_UI_TEST_BRIDGE
        // This runs after input and before any render or bridge dispatch. A
        // pending preview job can only contain a pre-pairing frame; stop joins
        // that worker before the first sensitive frame is drawn.
        if (!shelf::pairing_capture_allowed(state.page)) {
            ui_video.stop();
            ui_test.stop();
            ui_test_pending = false;
            ui_test_finish = false;
            state.ui_test_active = false;
        }
#endif
#ifdef SHELF_PS5
        if (state.launch_requested && !state.exit_requested && !close_gesture.active()
#ifdef SHELF_UI_TEST_BRIDGE
            && !ui_test_probing
#endif
            ) {
            state.launch_requested = false;
            auto& request = state.pending_launch;
            const auto epoch = std::time(nullptr);
            request.issued = epoch > 0 ? static_cast<std::uint64_t>(epoch) : 0;
            std::snprintf(request.id, sizeof(request.id), "%016llx%016llx",
                          static_cast<unsigned long long>(SDL_GetPerformanceCounter()),
                          static_cast<unsigned long long>(request.issued));
            if (launcher.start(request, data, shelf_launch_payload, sizeof(shelf_launch_payload))) {
                launch_visibility.reset();
                state.launch_busy = true;
                navigation.cancel();
                startup("launcher request started", shelf::launch_mode_name(request.mode));
            } else {
                std::snprintf(state.notice, sizeof(state.notice), "%s", launcher.error());
                startup("launcher request refused");
            }
            redraw = true;
        }
        if (state.launch_busy) {
            const auto status = launcher.state();
            if (state.exit_requested || close_gesture.active()) {
                launcher.cancel();
                state.launch_busy = false;
            } else if (status == shelf::LaunchClientState::ready) {
                unsigned visibility = 0;
                const auto visibility_status = gs_app_visibility(&visibility);
                const auto decision = launch_visibility.sample(now, visibility_status, visibility);
                if (decision == shelf::LaunchVisibilityDecision::commit && launcher.commit()) {
                    if (state.pending_launch.mode == shelf::LaunchMode::self_update ||
                        state.pending_launch.mode == shelf::LaunchMode::update_recover)
                        gs_notify("isShelf is closing now to install the update. It will reopen automatically.");
                    state.presentation.launching = true;
                    circle_at = now;
                    close_gesture.begin_close(now);
                    startup("launcher committed", "cooperative close; no process kill");
                    state.launch_busy = false;
                    redraw = true;
                } else if (decision == shelf::LaunchVisibilityDecision::cancel ||
                           decision == shelf::LaunchVisibilityDecision::commit) {
                    launcher.cancel();
                    std::snprintf(state.notice, sizeof(state.notice), "%s",
                                  decision == shelf::LaunchVisibilityDecision::commit
                                      ? launcher.error()
                                      : "Launch cancelled because isShelf did not stay in the foreground.");
                    startup("launcher foreground wait ended", "no launch was sent");
                    state.launch_busy = false;
                    redraw = true;
                }
            } else if (status == shelf::LaunchClientState::failed || status == shelf::LaunchClientState::cancelled) {
                std::snprintf(state.notice, sizeof(state.notice), "%s",
                              status == shelf::LaunchClientState::failed ? launcher.error() : "Launch cancelled.");
                state.launch_busy = false;
                redraw = true;
                startup("launcher did not commit");
            }
        }
#endif
        if (state.close_hint_requested) {
            state.close_hint_requested = false;
            if (!close_gesture.active())
                close_hint.show(now);
        }
        if (close_hint.update(now, state.presentation))
            redraw = true;
        close_gesture.update(now, state.presentation);
        if (close_gesture.closing()) {
#if defined(SHELF_PS5) && defined(SHELF_UI_TEST_BRIDGE)
            if (ui_test_probing)
                SDL_AtomicSet(&probe_worker.cancelled, 1);
#endif
            if (!circle_committed_logged) {
                char duration[80];
                std::snprintf(duration, sizeof(duration), "%u ms after button event", now - circle_at);
                startup(state.presentation.launching ? "handoff fade started" : "Circle hold committed", duration);
                circle_committed_logged = true;
            }
            navigation.cancel();
            SDL_AtomicSet(&scan.cancelled, 1);
        }
        if (close_gesture.finished(now)) {
            char duration[80];
            std::snprintf(duration, sizeof(duration), "%u ms after button event", now - circle_at);
            startup(state.presentation.launching ? "handoff fade complete" : "closing fade complete", duration);
#ifdef SHELF_HAS_UI_TEST_BRIDGE
            if (ui_test_closing)
                ui_test_close_finished = true;
#endif
            state.exit_requested = true;
        }
        if (state.presentation.loading) {
            const float progress = intro_started ? std::min(
                1.0f, (visual_now - visual_start) /
                          static_cast<float>(shelf::opening_entrance_ms)) : 0;
            state.presentation.appear = progress * progress * (3 - 2 * progress);
        }
        if (!state.presentation.loading && state.presentation.reveal < 1 && !reveal_pending) {
            const float progress = std::min(
                1.0f, (visual_now - reveal_at) /
                          static_cast<float>(shelf::opening_reveal_ms));
            state.presentation.reveal = progress * progress * (3 - 2 * progress);
        }
        shelf::Action repeated{};
        if (!scripting && !ui_test_probing && !state.presentation.loading && !close_gesture.closing() &&
            navigation.tick(now, repeated)) {
            shelf::input(state, repeated);
#ifdef SHELF_HAS_UI_TEST_BRIDGE
            local_ui_changed();
#endif
            redraw = true;
        }
#ifdef SHELF_HAS_UI_TEST_BRIDGE
        // L3 + R3 may have armed control after this loop's older `now` sample.
        // Re-read the clock after input; the bounded helper also refuses stale
        // samples without breaking normal unsigned clock wrap.
        const std::uint32_t control_now = SDL_GetTicks();
        if (ui_test.active() && !ui_test_pending && shelf::ui_test_elapsed(control_now, ui_test_polled, 250)) {
            ui_test_polled = control_now;
            if (ui_test.poll(ui_test_request)) {
                ui_test_pending = true;
                ui_test_applied = false;
                ui_test_saved = true;
                ui_test_finish = false;
                ui_test_result = "ok";
                ui_test_probe_bytes = 0;
                ui_test_probe_fnv = 0;
                ui_test_ps2_source = {};
                ui_test_ps2_ran = false;
                ui_test_ps2_direct = {};
                ui_test_ps2_direct_ran = false;
                ui_test_route_store = {};
                ui_test_route_store_ran = false;
                ui_test_route_path = {};
                ui_test_route_path_ran = false;
                ui_test_settings_path = {};
                ui_test_settings_path_ran = false;
                ui_test_probe_done = false;
                ui_test_capture_at = control_now + 500;
                if (ui_test_request.operation == shelf::UiTestOperation::stop) {
                    ui_test_finish = true;
                    ui_test_result = "stopped";
                    ui_video.stop();
                } else if (ui_test_request.revision != ui_revision)
                    ui_test_result = "stale";
                else if (state.presentation.loading || state.launch_busy || close_gesture.active() ||
                         state.exit_requested || state.presentation.reveal < 1 ||
#ifdef SHELF_PS5
                         ((ui_test_request.operation == shelf::UiTestOperation::probe_paths ||
                           ui_test_request.operation == shelf::UiTestOperation::launch_confirm ||
                           ui_test_request.operation == shelf::UiTestOperation::probe_writes_v4 ||
                           ui_test_request.operation == shelf::UiTestOperation::probe_app_source ||
                           ui_test_request.operation == shelf::UiTestOperation::create_n64_source ||
                           ui_test_request.operation == shelf::UiTestOperation::probe_ps2_source ||
                           ui_test_request.operation == shelf::UiTestOperation::probe_ps2_direct ||
                           ui_test_request.operation == shelf::UiTestOperation::probe_route_store ||
                           ui_test_request.operation == shelf::UiTestOperation::probe_route_path ||
                           ui_test_request.operation == shelf::UiTestOperation::probe_settings_path ||
                           ui_test_request.operation == shelf::UiTestOperation::probe_job_root ||
                           ui_test_request.operation == shelf::UiTestOperation::create_job_root) &&
                          (!library_accepted || (console_tile_input == shelf::ConsoleTileInput::valid &&
                                                  !console_tile_applied))) ||
#endif
                         ((ui_test_request.operation == shelf::UiTestOperation::close ||
                           ui_test_request.operation == shelf::UiTestOperation::launch_confirm ||
                           ui_test_request.operation == shelf::UiTestOperation::probe_paths ||
                           ui_test_request.operation == shelf::UiTestOperation::probe_writes_v4 ||
                           ui_test_request.operation == shelf::UiTestOperation::probe_app_source ||
                           ui_test_request.operation == shelf::UiTestOperation::create_n64_source ||
                           ui_test_request.operation == shelf::UiTestOperation::probe_ps2_source ||
                           ui_test_request.operation == shelf::UiTestOperation::probe_ps2_direct ||
                           ui_test_request.operation == shelf::UiTestOperation::probe_route_store ||
                           ui_test_request.operation == shelf::UiTestOperation::probe_route_path ||
                           ui_test_request.operation == shelf::UiTestOperation::probe_settings_path ||
                           ui_test_request.operation == shelf::UiTestOperation::probe_job_root ||
                           ui_test_request.operation == shelf::UiTestOperation::create_job_root) &&
                          (state.launch_requested || state.tile_requested || state.tile_busy ||
                           state.route_save_requested || state.route_save_busy
#ifdef SHELF_PS5
                           || tile_worker.thread
#endif
                           )))
                    ui_test_result = "busy";
                else if (ui_test_request.operation == shelf::UiTestOperation::close) {
                    // A remote close uses the same three-second Circle gesture
                    // and fade as a physical hold. Keep its one consumed request
                    // pending until cooperative cleanup has actually completed.
                    close_hint.dismiss();
                    navigation.cancel();
                    circle_at = control_now;
                    close_gesture.press(remote_close_controller, control_now);
                    ui_test_closing = true;
                    ++ui_revision;
                    startup("remote Circle hold received", "3000 ms threshold");
                }
                else if (ui_test_request.operation == shelf::UiTestOperation::launch_confirm) {
#if defined(SHELF_PS5) && defined(SHELF_UI_TEST_BRIDGE)
                    unsigned visibility = 0;
                    if (gs_app_visibility(&visibility) != 0 || visibility != 0)
                        ui_test_result = "busy";
                    else if (!shelf::apply_ui_test_launch_confirm(state))
                        ui_test_result = "launch-blocked";
                    else {
                        // End remote control before the normal launcher loop
                        // consumes this selected emulator or exact queued-
                        // removal handoff request.
                        navigation.cancel();
                        close_hint.dismiss();
                        ui_video.stop();
                        ui_test_finish = true;
                        ui_test_capture_at = control_now;
                        ui_test_result = "launch-requested";
                        ui_test_applied = true;
                        ++ui_revision;
                        startup("remote guarded handoff confirmed", "one-shot UI test session ended");
                    }
#else
                    ui_test_result = "unsupported";
#endif
                }
                else if (ui_test_request.operation == shelf::UiTestOperation::probe_paths ||
                         ui_test_request.operation == shelf::UiTestOperation::probe_writes_v4) {
#if defined(SHELF_PS5) && defined(SHELF_UI_TEST_BRIDGE)
                    unsigned visibility = 0;
                    if (gs_app_visibility(&visibility) != 0 || visibility != 0)
                        ui_test_result = "busy";
                    else {
                        bool request_created = false;
                        const bool write_probe = ui_test_request.operation == shelf::UiTestOperation::probe_writes_v4;
                        if (probe_worker.start(data, ui_test.session(), request_created, write_probe)) {
                            ui_test_probing = true;
                            ui_test_applied = true;
                            ++ui_revision;
                            navigation.cancel();
                            close_hint.dismiss();
                            std::snprintf(state.notice, sizeof(state.notice),
                                          "%s", write_probe ? "Checking private shortcut scratch writes. Leave isShelf open."
                                                             : "Checking PS5 shortcut paths. Leave isShelf open.");
                            startup(write_probe ? "console tile write probe dispatched"
                                                : "console tile path probe dispatched",
                                    "one ELF send to localhost 9021");
                        } else {
                            ui_test_applied = request_created;
                            ui_test_result = request_created ? "probe-unknown" : "probe-unavailable";
                            startup(write_probe ? "console tile write probe refused"
                                                : "console tile path probe refused",
                                    request_created ? "request retained" : "request not created");
                        }
                    }
#else
                    ui_test_result = "unsupported";
#endif
                }
                else if (ui_test_request.operation == shelf::UiTestOperation::probe_job_root ||
                         ui_test_request.operation == shelf::UiTestOperation::create_job_root)
                    ui_test_result = "unsupported";
                else if (ui_test_request.operation == shelf::UiTestOperation::probe_app_source) {
#if defined(SHELF_PS5) && defined(SHELF_UI_TEST_BRIDGE)
                    unsigned visibility = 0;
                    if (gs_app_visibility(&visibility) != 0 || visibility != 0)
                        ui_test_result = "busy";
                    else {
                        const auto source = shelf::app_tile_source_probe::run_once(data, ui_test.session());
                        ui_test_applied = source.attempted;
                        ui_test_probe_bytes = source.complete ? source.receipt_bytes : 0;
                        ui_test_probe_fnv = source.complete ? source.receipt_fnv : 0;
                        ui_test_result = source.complete ? "source-recorded" :
                                         source.attempted ? "source-held" : "source-unavailable";
                        ++ui_revision;
                        startup("app-owned tile source scratch probe", source.failure);
                    }
#else
                    ui_test_result = "unsupported";
#endif
                }
                else if (ui_test_request.operation == shelf::UiTestOperation::create_n64_source) {
#if defined(SHELF_PS5) && defined(SHELF_UI_TEST_BRIDGE)
                    unsigned visibility = 0;
                    if (gs_app_visibility(&visibility) != 0 || visibility != 0)
                        ui_test_result = "busy";
                    else {
                        const auto source = shelf::n64_source_trial::run_once(
                            data, shelf::n64_source_trial::fixed_pack, ui_test.session());
                        ui_test_applied = source.attempted;
                        ui_test_probe_bytes = source.complete ? source.receipt_bytes : 0;
                        ui_test_probe_fnv = source.complete ? source.receipt_fnv : 0;
                        ui_test_result = source.complete ? "console-source-recorded" :
                                         source.attempted ? "console-source-held" : "console-source-unavailable";
                        ++ui_revision;
                        startup("app-owned N64 source-only trial", source.failure);
                    }
#else
                    ui_test_result = "unsupported";
#endif
                }
                else if (ui_test_request.operation == shelf::UiTestOperation::probe_ps2_source) {
#if defined(SHELF_PS5) && defined(SHELF_UI_TEST_BRIDGE)
                    unsigned visibility = 0;
                    if (gs_app_visibility(&visibility) != 0 || visibility != 0)
                        ui_test_result = "busy";
                    else {
                        const auto source = shelf::protected_ps2_source_read_probe::inspect(
                            "/", 0, ::geteuid());
                        ui_test_ps2_source.complete = source.complete;
                        ui_test_ps2_source.failure = source.failure;
                        ui_test_ps2_source.uid = source.uid;
                        ui_test_ps2_source.euid = source.euid;
                        ui_test_ps2_source.observed_nodes = source.observed_nodes;
                        ui_test_ps2_source.failed_node = source.failed_node;
                        ui_test_ps2_source.failed_pass = source.failed_pass;
                        ui_test_ps2_source.system_error = source.system_error;
                        ui_test_ps2_source.files_read = source.files_read;
                        for (std::size_t i = 0; i < source.observed_nodes; ++i) {
                            const auto& node = source.nodes[i];
                            ui_test_ps2_source.nodes[i] = {
                                static_cast<std::uint64_t>(node.device),
                                static_cast<std::uint64_t>(node.inode),
                                static_cast<std::uint64_t>(node.owner),
                                static_cast<std::uint64_t>(node.group),
                                static_cast<std::uint64_t>(node.mode),
                                static_cast<std::int64_t>(node.size)};
                        }
                        ui_test_ps2_ran = true;
                        ui_test_result = source.complete ? "ps2-source-readable" :
                                                           "ps2-source-unavailable";
                        ui_test_capture_at = SDL_GetTicks();
                        redraw = true;
                    }
#else
                    ui_test_result = "unsupported";
#endif
                }
                else if (ui_test_request.operation == shelf::UiTestOperation::probe_ps2_direct) {
#if defined(SHELF_PS5) && defined(SHELF_UI_TEST_BRIDGE)
                    unsigned visibility = 0;
                    if (gs_app_visibility(&visibility) != 0 || visibility != 0)
                        ui_test_result = "busy";
                    else {
                        const auto direct = shelf::protected_ps2_direct_file_probe::inspect();
                        ui_test_ps2_direct.complete = direct.complete;
                        ui_test_ps2_direct.target_opened = direct.target_opened;
                        ui_test_ps2_direct.content_matched = direct.content_matched;
                        ui_test_ps2_direct.mount_distinct = direct.mount_distinct;
                        ui_test_ps2_direct.failure = direct.failure;
                        ui_test_ps2_direct.system_error = direct.system_error;
                        ui_test_ps2_direct.uid = direct.uid;
                        ui_test_ps2_direct.euid = direct.euid;
                        ui_test_ps2_direct.bytes_read = direct.bytes_read;
                        ui_test_ps2_direct.root_device = direct.root_device;
                        ui_test_ps2_direct.system_device = direct.system_device;
                        ui_test_ps2_direct.target_device = direct.target_device;
                        ui_test_ps2_direct.target_inode = direct.target_inode;
                        ui_test_ps2_direct.target_owner = direct.target_owner;
                        ui_test_ps2_direct.target_mode = direct.target_mode;
                        ui_test_ps2_direct.target_size = direct.target_size;
                        ui_test_ps2_direct_ran = true;
                        ui_test_result = direct.complete ? "ps2-direct-matched" : "ps2-direct-unavailable";
                        ui_test_capture_at = SDL_GetTicks();
                        redraw = true;
                    }
#else
                    ui_test_result = "unsupported";
#endif
                }
                else if (ui_test_request.operation == shelf::UiTestOperation::probe_route_store) {
#if defined(SHELF_PS5) && defined(SHELF_UI_TEST_BRIDGE)
                    unsigned visibility = 0;
                    if (gs_app_visibility(&visibility) != 0 || visibility != 0)
                        ui_test_result = "busy";
                    else {
                        errno = 0;
                        shelf::tile_route::AppStore store;
                        const auto root = store.open_root(false);
                        const int observed_error = errno;
                        ui_test_route_store.root = root == shelf::tile_route::AppRoot::present ? "present" :
                                                   root == shelf::tile_route::AppRoot::missing ? "missing" : "unsafe";
                        ui_test_route_store.uid = static_cast<std::uint64_t>(::getuid());
                        ui_test_route_store.euid = static_cast<std::uint64_t>(::geteuid());
                        ui_test_route_store.system_error = root == shelf::tile_route::AppRoot::present ? 0 :
                                                           observed_error >= 0 && observed_error <= 65535 ?
                                                               observed_error : 0;
                        ui_test_route_store_ran = true;
                        ui_test_result = root == shelf::tile_route::AppRoot::present ? "route-store-present" :
                                         root == shelf::tile_route::AppRoot::missing ? "route-store-missing" :
                                                                                      "route-store-unsafe";
                        ui_test_capture_at = SDL_GetTicks();
                        redraw = true;
                    }
#else
                    ui_test_result = "unsupported";
#endif
                }
                else if (ui_test_request.operation == shelf::UiTestOperation::probe_route_path) {
#if defined(SHELF_PS5) && defined(SHELF_UI_TEST_BRIDGE)
                    unsigned visibility = 0;
                    if (gs_app_visibility(&visibility) != 0 || visibility != 0)
                        ui_test_result = "busy";
                    else {
                        ui_test_route_path = shelf::tile_route::probe_route_path();
                        ui_test_route_path_ran = true;
                        ui_test_result = "route-path-observed";
                        ui_test_capture_at = SDL_GetTicks();
                        redraw = true;
                    }
#else
                    ui_test_result = "unsupported";
#endif
                }
                else if (ui_test_request.operation == shelf::UiTestOperation::probe_settings_path) {
#if defined(SHELF_PS5) && defined(SHELF_UI_TEST_BRIDGE)
                    unsigned visibility = 0;
                    if (gs_app_visibility(&visibility) != 0 || visibility != 0)
                        ui_test_result = "busy";
                    else {
                        ui_test_settings_path = shelf::tile_route::probe_settings_path();
                        ui_test_settings_path_ran = true;
                        ui_test_result = "settings-path-observed";
                        ui_test_capture_at = SDL_GetTicks();
                        redraw = true;
                    }
#else
                    ui_test_result = "unsupported";
#endif
                }
                else if (ui_test_request.operation == shelf::UiTestOperation::action) {
                    // Trial state prevents a remote Confirm from leaving isShelf.
                    if (!shelf::apply_ui_test_action(state, ui_test_request.action))
                        ui_test_result = "launch-blocked";
                    else {
                        navigation.cancel();
                        close_hint.dismiss();
                        ++ui_revision;
                        ui_test_applied = true;
                    }
                }
                redraw = true;
            }
        }
        if (ui_test_closing && !close_gesture.active()) {
            // Focus loss cancels an uncommitted hold, just like physical Circle.
            ui_test_closing = false;
            ui_test_result = "interrupted";
            ui_test_capture_at = control_now;
        }
        if (ui_test_pending && !ui_test_closing && (!ui_test_probing || ui_test_probe_done) &&
            shelf::ui_test_elapsed(control_now, ui_test_capture_at, 0))
            redraw = true;
        if (ui_video.due(control_now))
            redraw = true;
        state.ui_test_active = ui_test.active() && !ui_test_finish;
#endif
        if (state.changed && shelf::pairing_capture_allowed(state.page)) {
            if (!shelf::save_settings(settings_path, state.settings)) {
                std::snprintf(state.notice, sizeof(state.notice),
                              "Settings could not be saved. Your previous settings are kept.");
                result = 1;
#ifdef SHELF_HAS_UI_TEST_BRIDGE
                local_ui_changed();
                if (ui_test_pending) {
                    ui_test_saved = false;
                    ui_test_result = "save-failed";
                }
#endif
            }
            state.changed = false;
        }
        if (!scripting && state.settings.preferences.layout == shelf::Layout::shelf &&
            (state.settings.preferences.tv_mode == shelf::TvMode::clock || state.settings.preferences.tv_mode == shelf::TvMode::favorites) &&
            static_cast<unsigned>(now - tv_redraw_at) >= 1000) {
            redraw = true;
            tv_redraw_at = now;
        }
        if (redraw || shelf::graphics_animating() || state.presentation.loading || state.presentation.reveal < 1 ||
            close_gesture.active()) {
            const auto frame_start = SDL_GetTicks();
            const bool preparing =
                (token && std::strcmp(token, "prepare") == 0) ||
                (automatic_intro && state.presentation.loading && !opening_prepared);
            state.presentation.preparing = preparing;
            const bool covered_reveal = reveal_pending && !state.presentation.loading;
            shelf::graphics_render(state, animate_script ? script_time : SDL_GetTicks(),
                                   !preparing && (animate_script || (!scripting && !headless)));
            const auto render_end = SDL_GetTicks();
            if (shelf::graphics_errors()) {
                startup("render resource failed");
                std::fprintf(stderr, "A packaged image/font or rendering resource failed to load.\n");
                result = 1;
                break;
            }
            if (window) {
                SDL_Surface* target = SDL_GetWindowSurface(window);
                if (target) {
                    SDL_BlitScaled(surface, nullptr, target, nullptr);
                    SDL_UpdateWindowSurface(window);
                }
            }
#ifdef SHELF_PS5
            gs_vout_present(surface->pixels, surface->pitch);
#ifdef SHELF_CAPTURE_STARTUP
            // Two bounded, owned diagnostic pictures let us distinguish correct
            // drawing from a delayed display handoff. No game/account files are read.
            bool captured = false;
            if (shelf::pairing_capture_allowed(state.page) && !captured_loading &&
                state.presentation.loading && intro_started && state.presentation.appear >= 1) {
                captured_loading = captured = true;
                startup(shelf::pairing_capture_if_allowed(state.page, [&] {
                            return shelf::graphics_capture("/data/console-shelf/diagnostic-loading.png");
                        })
                            ? "loading diagnostic picture saved"
                            : "loading diagnostic picture failed");
            }
            if (shelf::pairing_capture_allowed(state.page) && !captured_reveal &&
                !state.presentation.loading && state.presentation.reveal >= .4f &&
                state.presentation.reveal < .8f) {
                captured_reveal = captured = true;
                startup(shelf::pairing_capture_if_allowed(state.page, [&] {
                            return shelf::graphics_capture("/data/console-shelf/diagnostic-reveal.png");
                        })
                            ? "reveal diagnostic picture saved"
                            : "reveal diagnostic picture failed");
            }
            if (captured)
                last_visibility_at = SDL_GetTicks();
#endif
#endif
#ifdef SHELF_HAS_UI_TEST_BRIDGE
            if (shelf::pairing_capture_allowed(state.page) && ui_video.due(SDL_GetTicks()))
                ui_video.submit(surface->pixels, surface->pitch, ui_revision, SDL_GetTicks());
            if (shelf::pairing_capture_allowed(state.page) && ui_test_pending && !ui_test_closing &&
                (!ui_test_probing || ui_test_probe_done) &&
                shelf::ui_test_elapsed(SDL_GetTicks(), ui_test_capture_at, 0)) {
                const bool captured = shelf::ui_test_discrete_frame_required(ui_test_request) &&
                                      shelf::pairing_capture_if_allowed(state.page, [&] {
                                          return shelf::graphics_capture(ui_test.frame_path());
                                      });
                if (ui_test_finish)
                    ui_test.stop();
                const bool published = ui_test.publish(ui_test_request, ui_revision, ui_test_result,
                                                       ui_test_applied, ui_test_saved, state, captured,
                                                       ui_test_probe_bytes, ui_test_probe_fnv, nullptr,
                                                       ui_test_ps2_ran ? &ui_test_ps2_source : nullptr,
                                                       ui_test_ps2_direct_ran ? &ui_test_ps2_direct : nullptr,
                                                       ui_test_route_store_ran ? &ui_test_route_store : nullptr,
                                                       ui_test_route_path_ran ? &ui_test_route_path : nullptr,
                                                       ui_test_settings_path_ran ? &ui_test_settings_path : nullptr);
                ui_test_pending = false;
                ui_test_applied = false;
                ui_test_probing = false;
                ui_test_probe_done = false;
                ui_test_ps2_ran = false;
                ui_test_ps2_direct_ran = false;
                ui_test_route_store_ran = false;
                ui_test_route_path_ran = false;
                ui_test_settings_path_ran = false;
                if (!published) {
                    ui_test.stop();
                    ui_video.stop();
                }
            }
#endif
            if (preparing) {
                state.presentation.preparing = false;
                opening_prepared = true;
                last_visibility_at = SDL_GetTicks();
                char duration[100];
                std::snprintf(duration, sizeof(duration), "%u ms render; %u ms after graphics ready",
                              render_end - frame_start, last_visibility_at - opened_at);
                startup("covered scene prepared", duration);
            }
            // Bounded timing evidence, not a claim about displayed frame rate.
            if (measured_frames < 16) {
                if (measured_frames) {
                    max_render_ms = std::max(max_render_ms, render_end - frame_start);
                    max_present_ms = std::max(max_present_ms, SDL_GetTicks() - render_end);
                }
                if (++measured_frames == 16) {
                    char timing[100];
                    std::snprintf(timing, sizeof(timing), "max render %u ms; frame handoff %u ms (15 samples)",
                                  max_render_ms, max_present_ms);
                    startup("frame costs", timing);
                }
            }
            if (first_frame) {
                startup("first frame submitted");
                first_frame = false;
            }
            if (state.presentation.loading && intro_started)
                ++intro_frames;
            if (!state.presentation.loading && state.presentation.reveal < 1)
                ++reveal_frames;
            if (automatic_intro && !opening_logged && !state.presentation.loading && state.presentation.reveal >= 1) {
                char info[150];
                std::snprintf(info, sizeof(info),
                              "%u ms wall; %u ms foreground clock; %u entrance frames; %u reveal frames",
                              SDL_GetTicks() - opened_at, visible_time, intro_frames, reveal_frames);
                startup("opening animation submitted", info);
                opening_logged = true;
            }
            if (state.presentation.loading && scan.library && !scan_started) {
                scan_started = true;
                startup("library scan starting", "background after first submitted frame");
                if (!scan.start()) {
                    startup("library worker unavailable", SDL_GetError());
                    library.value->scan.complete = false;
                    accept_library(state, *library.value, ftp, false);
                    library_accepted = true;
                }
            }
            if (covered_reveal) {
                // Image decoding and texture preparation must not consume the reveal's time.
                reveal_at = automatic_intro ? visible_time : scripting ? script_time : SDL_GetTicks();
                // Exclude this covered frame's image decoding from the foreground animation clock too.
                last_visibility_at = SDL_GetTicks();
                reveal_pending = false;
                startup("opening reveal ready", "prepared frame; 520 ms blend");
            }
            redraw = false;
        }
        if (token && (std::strncmp(token, "shot:", 5) == 0 || std::strncmp(token, "report:", 7) == 0)) {
            const bool shot = std::strncmp(token, "shot:", 5) == 0;
            const char* name = token + (shot ? 5 : 7);
            if (!filename(name)) {
                std::fprintf(stderr, "Invalid capture filename.\n");
                result = 1;
                break;
            }
            char output[2304];
            std::snprintf(output, sizeof(output), "%s/%s", captures, name);
            if (!shelf::pairing_capture_if_allowed(state.page, [&] {
                    return shot ? shelf::graphics_capture(output) : report(output, state);
                })) {
                std::fprintf(stderr, "Capture/report failed.\n");
                result = 1;
                break;
            }
        }
        if (!scripting)
            SDL_Delay(16);
    }
#ifdef SHELF_HAS_UI_TEST_BRIDGE
#if defined(SHELF_PS5) && defined(SHELF_UI_TEST_BRIDGE)
    if (ui_test_probing) {
        probe_worker.stop();
        ui_test_probing = false;
        ui_test_probe_done = false;
        ui_test_result = "probe-unknown";
        ui_test_probe_bytes = 0;
        ui_test_probe_fnv = 0;
    }
#endif
    ui_video.stop();
    if (shelf::pairing_capture_allowed(state.page) && !ui_test_closing &&
        (ui_test.active() || ui_test_pending)) {
        ui_test.stop();
        ui_test.publish(ui_test_request, ui_revision,
                        ui_test_pending && (ui_test_request.operation == shelf::UiTestOperation::probe_paths ||
                                            ui_test_request.operation == shelf::UiTestOperation::probe_writes_v4)
                            ? ui_test_result : "closed",
                        ui_test_applied, ui_test_saved, state, false,
                        ui_test_probe_bytes, ui_test_probe_fnv);
    }
#endif
    scan.stop();
#ifdef SHELF_PS5
    tile_worker.join();
#endif
    launcher.stop();
#ifdef SHELF_PS5
    updater.stop();
#endif
    for (auto* controller : controllers)
        if (controller)
            SDL_GameControllerClose(controller);
    shelf::graphics_shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_FreeSurface(surface);
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();
#ifdef SHELF_HAS_UI_TEST_BRIDGE
    if (shelf::pairing_capture_allowed(state.page) && ui_test_closing) {
        // This receipt proves the app completed its own cleanup. On PS5 the
        // subsequent shell close request is asynchronous, so an installer must
        // still observe process absence before changing program files.
        ui_test.stop();
        const bool published = ui_test.publish(
            ui_test_request, ui_revision, ui_test_close_finished ? "cleanup-complete" : "interrupted",
            ui_test_close_finished, ui_test_saved, state, false);
        startup(published ? "remote close receipt published" : "remote close receipt failed");
    }
#endif
    std::printf("Native host session finished: %zu systems in view.\n", shelf::shown_systems(state).count);
    return result;
}
