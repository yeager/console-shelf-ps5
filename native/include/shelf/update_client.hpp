// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "update_protocol.hpp"
#include <SDL.h>
#include <mutex>

#ifdef SHELF_PS5
namespace fe { class HttpsClient; }
#endif

namespace shelf {
enum class UpdateWorkState {
    idle,
    checking,
    current,
    newer,
    available,
    preparing,
    ready,
    failed,
    cancelled,
};
struct UpdateWorkOutcome {
    UpdateWorkState state = UpdateWorkState::idle;
    UpdateRelease release;
    char message[192]{};
    char manifest_sha256[65]{};
};

class UpdateClient {
    enum class Task { none, check, prepare };
    SDL_atomic_t cancelled_{0}, state_{0}, progress_{0};
    SDL_Thread* thread_ = nullptr;
    Task task_ = Task::none;
    UpdateWorkOutcome outcome_;
    UpdateRelease release_;
    char data_root_[2304]{};
    char ca_path_[2304]{};
    char current_[32]{};
#ifdef SHELF_PS5
    // stop() aborts the one in-flight socket before joining the worker. The
    // mutex keeps the stack-owned client alive across the cross-thread call.
    std::mutex https_mutex_;
    fe::HttpsClient* active_https_ = nullptr;
#endif

    static int run(void* context);
    int run_check();
    int run_prepare();
    bool begin(Task task, UpdateWorkState state);

  public:
    UpdateClient() = default;
    UpdateClient(const UpdateClient&) = delete;
    UpdateClient& operator=(const UpdateClient&) = delete;
    ~UpdateClient();
    bool check(const char* data_root, const char* ca_path, const char* current);
    bool prepare(const UpdateRelease& release, const char* data_root, const char* ca_path);
    bool busy() const;
    int progress() const;
    bool take(UpdateWorkOutcome& output);
    void stop();
};

// A BEGIN/COMMIT journal is never interpreted as success by the app. It is a
// prompt to run the bundled helper, which either rolls back or finishes the
// exact transaction before relaunching isShelf.
bool update_recovery_pending(const char* data_root, char (&tag)[32],
                             char (&manifest_sha256)[65], bool& committed);
// Consumes only a strictly shaped helper result from the app-owned update
// folder. Invalid or partial files remain available for diagnosis.
bool take_update_result(const char* data_root, char (&notice)[192]);
} // namespace shelf
