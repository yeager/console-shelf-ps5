// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/ui_test_video.hpp"
#include "shelf/ui_test_bridge.hpp"
#include "shelf/ui_test_preview.hpp"
#include "shelf/memory.hpp"
#include "stb/stb_image_write.h"
#include <SDL.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <new>
#include <sys/stat.h>

namespace shelf {
namespace {
constexpr int preview_width = ui_test_preview::width, preview_height = ui_test_preview::height;
constexpr std::size_t rgb_bytes = ui_test_preview::rgb_bytes;
constexpr std::size_t frame_limit = 524288;
constexpr std::uint64_t fnv_offset = 14695981039346656037ull, fnv_prime = 1099511628211ull;
bool fixed_path(char (&out)[2304], const char* root, const char* filename) {
    const int used = std::snprintf(out, sizeof(out), "%s/%s", root, filename);
    return used > 0 && static_cast<std::size_t>(used) < sizeof(out);
}
bool discard(const char* file) { return std::remove(file) == 0 || errno == ENOENT; }
bool replace(const char* from, const char* to) { return discard(to) && std::rename(from, to) == 0; }
bool valid_session(const char* session) {
    if (!session || std::strlen(session) != 32)
        return false;
    for (unsigned i = 0; i < 32; ++i)
        if (!((session[i] >= '0' && session[i] <= '9') || (session[i] >= 'a' && session[i] <= 'f')))
            return false;
    return true;
}
struct BoundedJpeg {
    FILE* file = nullptr;
    SDL_atomic_t* stopping = nullptr;
    bool okay = true;
    std::size_t bytes = 0;
    std::uint64_t fnv = fnv_offset;
    static void write(void* raw, void* data, int count) {
        auto& out = *static_cast<BoundedJpeg*>(raw);
        if (!out.okay)
            return;
        if (count < 0 || SDL_AtomicGet(out.stopping) || static_cast<std::size_t>(count) > frame_limit - out.bytes) {
            out.okay = false;
            return;
        }
        const auto size = static_cast<std::size_t>(count);
        if (std::fwrite(data, 1, size, out.file) != size) {
            out.okay = false;
            return;
        }
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i)
            out.fnv = (out.fnv ^ bytes[i]) * fnv_prime;
        out.bytes += size;
    }
};
} // namespace

struct UiTestVideo::Worker {
    char directory[2304]{}, session[33]{};
    unsigned char* rgb = nullptr;
    SDL_Thread* thread = nullptr;
    SDL_atomic_t stopping{0}, job{0}, dropped{0};
    bool enabled = false;
    std::uint32_t last_attempt = 0;
    // Job fields are transferred by the job atomic; only the event thread
    // writes them while idle and only the worker reads them while encoding.
    std::uint64_t job_revision = 0;
    std::uint32_t job_captured = 0;
    // Last published frame fields are worker-owned until it has been joined.
    std::uint64_t generation = 0, revision = 0, fnv = 0;
    std::uint32_t captured = 0, encode_ms = 0;
    std::size_t bytes = 0;
    int slot = 0;

    bool manifest(bool active, std::uint64_t next_generation, std::uint64_t next_revision, int next_slot,
                  std::size_t next_bytes, std::uint64_t next_fnv, std::uint32_t next_captured,
                  std::uint32_t next_encode) {
        char pending[2304], target[2304];
        if (!fixed_path(pending, directory, "ui-test-video.new.json") ||
            !fixed_path(target, directory, "ui-test-video.json"))
            return false;
        FILE* output = std::fopen(pending, "wb");
        if (!output)
            return false;
        const int written = std::fprintf(output,
            "{\"protocol\":1,\"sessionTimeoutMs\":0,\"active\":%s,\"session\":\"%s\",\"generation\":%llu,\"uiRevision\":%llu,"
            "\"slot\":%d,\"width\":960,\"height\":540,\"bytes\":%zu,\"fnv64\":\"%016llx\","
            "\"capturedAtMs\":%u,\"encodeMs\":%u,\"dropped\":%d,\"targetFPS\":2}\n",
            active ? "true" : "false", session, static_cast<unsigned long long>(next_generation),
            static_cast<unsigned long long>(next_revision), next_slot, next_bytes,
            static_cast<unsigned long long>(next_fnv), next_captured, next_encode, SDL_AtomicGet(&dropped));
        const bool flushed = written > 0 && std::fflush(output) == 0 && !std::ferror(output);
        const bool closed = std::fclose(output) == 0;
        const bool okay = flushed && closed && replace(pending, target);
        if (!okay)
            discard(pending);
        return okay;
    }
    bool manifest(bool active) {
        return manifest(active, generation, revision, slot, bytes, fnv, captured, encode_ms);
    }
    void encode() {
        const std::uint32_t began = SDL_GetTicks();
        const std::uint64_t next_generation = generation + 1;
        const int next_slot = static_cast<int>((next_generation - 1) % 2);
        char pending[2304], target[2304];
        const char* pending_name = next_slot ? "ui-test-video-1.new.jpg" : "ui-test-video-0.new.jpg";
        const char* target_name = next_slot ? "ui-test-video-1.jpg" : "ui-test-video-0.jpg";
        if (!fixed_path(pending, directory, pending_name) || !fixed_path(target, directory, target_name)) {
            SDL_AtomicAdd(&dropped, 1);
            return;
        }
        FILE* output = std::fopen(pending, "wb");
        if (!output) {
            SDL_AtomicAdd(&dropped, 1);
            return;
        }
        BoundedJpeg encoded{output, &stopping};
        const bool completed = stbi_write_jpg_to_func(BoundedJpeg::write, &encoded, preview_width, preview_height,
                                                       3, rgb, 70) != 0;
        const bool flushed = std::fflush(output) == 0 && !std::ferror(output);
        const bool closed = std::fclose(output) == 0;
        const auto duration = static_cast<std::uint32_t>(SDL_GetTicks() - began);
        const bool okay = completed && encoded.okay && flushed && closed && encoded.bytes >= 24 &&
                          !SDL_AtomicGet(&stopping) && replace(pending, target) &&
                          manifest(true, next_generation, job_revision, next_slot, encoded.bytes,
                                   encoded.fnv, job_captured, duration);
        if (okay) {
            generation = next_generation;
            revision = job_revision;
            slot = next_slot;
            bytes = encoded.bytes;
            fnv = encoded.fnv;
            captured = job_captured;
            encode_ms = duration;
        } else {
            discard(pending);
            SDL_AtomicAdd(&dropped, 1);
        }
    }
    static int run(void* raw) {
        auto& worker = *static_cast<Worker*>(raw);
        while (!SDL_AtomicGet(&worker.stopping)) {
            if (SDL_AtomicCAS(&worker.job, 1, 2)) {
                worker.encode();
                SDL_AtomicSet(&worker.job, 0);
            } else
                SDL_Delay(5);
        }
        return 0;
    }
};

UiTestVideo::~UiTestVideo() { stop(); }
bool UiTestVideo::start(const char* data_root, const char* session32, std::uint64_t ui_revision) {
    stop();
    if (!data_root || !*data_root || std::strlen(data_root) > 2000 || !valid_session(session32))
        return false;
    for (const auto* at = reinterpret_cast<const unsigned char*>(data_root); *at; ++at)
        if (*at < 32 || *at == 127)
            return false;
    struct stat info{};
    if (stat(data_root, &info) != 0 || (info.st_mode & S_IFMT) != S_IFDIR)
        return false;
    void* memory = allocate(sizeof(Worker));
    if (!memory)
        return false;
    auto* worker = new (memory) Worker{};
    std::memcpy(worker->directory, data_root, std::strlen(data_root) + 1);
    std::memcpy(worker->session, session32, sizeof(worker->session));
    worker->revision = ui_revision;
    worker->last_attempt = SDL_GetTicks() - 500;
    worker->rgb = static_cast<unsigned char*>(allocate(rgb_bytes));
    char owned[2304];
    bool ready = worker->rgb != nullptr;
    for (const char* name : {"ui-test-video.json", "ui-test-video.new.json", "ui-test-video-0.jpg",
                              "ui-test-video-1.jpg", "ui-test-video-0.new.jpg", "ui-test-video-1.new.jpg"})
        ready = fixed_path(owned, worker->directory, name) && discard(owned) && ready;
    if (ready)
        ready = worker->manifest(true);
    if (ready)
        worker->thread = SDL_CreateThreadWithStackSize(Worker::run, "Shelf preview", 256 * 1024, worker);
    if (!ready || !worker->thread) {
        worker->manifest(false);
        release(worker->rgb);
        worker->~Worker();
        release(worker);
        return false;
    }
    worker->enabled = true;
    worker_ = worker;
    return true;
}
void UiTestVideo::stop() {
    if (!worker_)
        return;
    auto* worker = worker_;
    worker_ = nullptr;
    worker->enabled = false;
    SDL_AtomicSet(&worker->stopping, 1);
    if (worker->thread)
        SDL_WaitThread(worker->thread, nullptr);
    worker->manifest(false);
    release(worker->rgb);
    worker->~Worker();
    release(worker);
}
bool UiTestVideo::active() const { return worker_ && worker_->enabled; }
bool UiTestVideo::due(std::uint32_t now) const {
    return active() && ui_test_elapsed(now, worker_->last_attempt, 500);
}
bool UiTestVideo::submit(const void* pixels, int pitch, std::uint64_t ui_revision, std::uint32_t now) {
    if (!due(now))
        return false;
    auto& worker = *worker_;
    worker.last_attempt = now;
    if (SDL_AtomicGet(&worker.job) ||
        !ui_test_preview::downsample_abgr_2x(pixels, pitch, worker.rgb, rgb_bytes)) {
        SDL_AtomicAdd(&worker.dropped, 1);
        return false;
    }
    worker.job_revision = ui_revision;
    worker.job_captured = now;
    SDL_AtomicSet(&worker.job, 1);
    return true;
}
} // namespace shelf
