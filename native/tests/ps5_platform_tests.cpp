// SPDX-License-Identifier: GPL-3.0-only
// Host mocks exercise the actual PS5 allocator/display cleanup code. They do
// not establish firmware or graphics-driver compatibility on a real console.
#include <array>
#include <atomic>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

extern "C" {
void* gs_malloc(std::size_t);
void* gs_realloc(void*, std::size_t);
void* gs_calloc(std::size_t, std::size_t);
void gs_free(void*);
const char* gs_vout_init();
void gs_vout_present(const void*, int);
int gs_vout_has_frame();
void gs_vout_shutdown();
int gs_request_close();
int gs_app_visibility(unsigned*);
[[noreturn]] void gs_wait_for_close();
}

namespace {
struct Allocation {
    std::int64_t id;
    void* mapped;
    std::size_t size;
};
std::array<Allocation, 16> allocations{};
std::int64_t next_id = 0;
int outstanding = 0, mappings = 0, opens = 0, fail = 0;
bool expect_pixels = false;
std::atomic<int> flips{0};
std::atomic<int> splash_hides{0};
std::array<int, 4> indices{};
void* registered[2]{};
std::jmp_buf shell_replaced;
bool closing_probe = false;
int close_calls = 0, close_status = 0, close_waits = 0;
int shell_replace_after = 2, lnc_initialize = 0, lnc_app = 0x2018;
int lnc_title_status = 0, lnc_kill_status = 0, lnc_kill_calls = 0;
const char* lnc_title = "PPSA99541";
int visibility_status = 0;
unsigned char overlay = 0, background_execution = 0;
} // namespace

extern "C" {
std::size_t sceKernelGetDirectMemorySize() {
    return 1024ull * 1024 * 1024;
}
int sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t size, std::size_t, int,
                                  std::int64_t* physical) {
    if (fail == 1)
        return -1;
    for (auto& a : allocations)
        if (!a.id) {
            a = {++next_id, nullptr, size};
            *physical = a.id;
            ++outstanding;
            return 0;
        }
    return -1;
}
int sceKernelMapDirectMemory(void** output, std::size_t size, int, int, std::int64_t physical, std::size_t) {
    if (fail == 2)
        return -1;
    for (auto& a : allocations)
        if (a.id == physical) {
            assert(a.size == size);
            a.mapped = reinterpret_cast<void*>(
                syscall(SYS_mmap, nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
            assert(a.mapped != MAP_FAILED);
            *output = a.mapped;
            ++mappings;
            return 0;
        }
    return -1;
}
int munmap(void* address, std::size_t size) noexcept {
    for (auto& a : allocations)
        if (a.mapped == address) {
            assert(a.size == size);
            a.mapped = nullptr;
            --mappings;
        }
    return static_cast<int>(syscall(SYS_munmap, address, size));
}
int sceKernelReleaseDirectMemory(std::int64_t physical, std::size_t size) {
    for (auto& a : allocations)
        if (a.id == physical) {
            assert(!a.mapped && a.size == size);
            a = {};
            --outstanding;
            return 0;
        }
    assert(false);
    return -1;
}
int sceKernelUsleep(std::uint32_t) {
    if (closing_probe && ++close_waits == shell_replace_after)
        std::longjmp(shell_replaced, 1);
    return 0;
}
int sceSystemServiceLoadExec(const char* path, const char* const* args) {
    assert(std::strcmp(path, "exit") == 0 && args == nullptr);
    ++close_calls;
    return close_status;
}
int sceSystemServiceGetStatus(void* output) {
    assert(reinterpret_cast<std::uintptr_t>(output) % 16 == 0);
    auto* bytes = static_cast<unsigned char*>(output);
    bytes[4] = overlay;
    bytes[5] = background_execution;
    return visibility_status;
}
int sceLncUtilInitialize(void) { return lnc_initialize; }
int sceLncUtilGetAppIdOfRunningBigApp(void) { return lnc_app; }
int sceLncUtilGetAppTitleId(std::uint32_t app_id, char* output) {
    assert(app_id == static_cast<std::uint32_t>(lnc_app));
    if (lnc_title_status == 0 && lnc_title)
        std::snprintf(output, 16, "%s", lnc_title);
    return lnc_title_status;
}
int sceLncUtilKillApp(std::uint32_t app_id) {
    assert(app_id == static_cast<std::uint32_t>(lnc_app));
    ++lnc_kill_calls;
    return lnc_kill_status;
}
int sceSystemServiceHideSplashScreen() {
    assert(flips >= 2); // Never reveal the initial cleared screen or failed startup.
    ++splash_hides;
    return 0;
}
int sceVideoOutOpen(std::int32_t, std::int32_t, std::int32_t, const void*) {
    ++opens;
    return 1;
}
int sceVideoOutClose(std::int32_t) {
    if (fail == 6)
        return -1;
    if (expect_pixels)
        assert(*static_cast<std::uint32_t*>(registered[1]) == 0xff123456);
    --opens;
    return 0;
}
int sceVideoOutSetFlipRate(std::int32_t, std::int32_t) {
    return fail == 3 ? -1 : 0;
}
void sceVideoOutSetBufferAttribute2(void*, std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint64_t,
                                    std::uint32_t, std::uint64_t) {}
struct Buffer {
    void* data;
    void* metadata;
    void* r0;
    void* r1;
};
int sceVideoOutRegisterBuffers2(std::int32_t, std::int32_t, std::int32_t, Buffer* buffers, std::int32_t count, void*,
                                std::int32_t, void*) {
    assert(count == 2);
    registered[0] = buffers[0].data;
    registered[1] = buffers[1].data;
    return fail == 4 ? -1 : 0;
}
int sceVideoOutSubmitFlip(std::int32_t, std::int32_t buffer, std::uint32_t, std::int64_t) {
    const int index = flips.load();
    assert(index < static_cast<int>(indices.size()));
    indices[static_cast<std::size_t>(index)] = buffer;
    flips.store(index + 1);
    return fail == 5 ? -1 : 0;
}
int sceVideoOutWaitVblank(std::int32_t) {
    return 0;
}
}

int main() {
    unsigned visibility = 99;
    assert(gs_app_visibility(&visibility) == 0 && visibility == 0);
    overlay = 1;
    assert(gs_app_visibility(&visibility) == 0 && visibility == 1);
    background_execution = 1;
    assert(gs_app_visibility(&visibility) == 0 && visibility == 3);
    overlay = 0;
    assert(gs_app_visibility(&visibility) == 0 && visibility == 2);
    visibility_status = -7;
    assert(gs_app_visibility(&visibility) == -7 && visibility == 2);
    visibility_status = 0;
    overlay = 4;
    assert(gs_app_visibility(&visibility) == -2 && visibility == 2);
    assert(gs_app_visibility(nullptr) == -1);
    overlay = background_execution = 0;
    assert(!gs_malloc(std::numeric_limits<std::size_t>::max()));
    assert(!gs_calloc(std::numeric_limits<std::size_t>::max(), 2));
    auto* small = static_cast<unsigned char*>(gs_malloc(32));
    assert(small);
    std::memset(small, 0x6b, 32);
    auto* large = static_cast<unsigned char*>(gs_realloc(small, 300000));
    assert(large && outstanding == 1 && mappings == 1);
    for (int i = 0; i < 32; ++i)
        assert(large[i] == 0x6b);
    assert(!gs_realloc(large, std::numeric_limits<std::size_t>::max()));
    assert(large[0] == 0x6b); // Failed realloc preserves the original allocation.
    gs_free(large);
    assert(outstanding == 0 && mappings == 0);
    for (fail = 1; fail <= 5; ++fail) {
        flips = 0;
        splash_hides = 0;
        assert(gs_vout_init());
        gs_vout_shutdown();
        assert(outstanding == 0 && mappings == 0 && opens == 0);
        assert(splash_hides == 0);
        gs_vout_shutdown(); // Repeated shutdown is harmless.
    }
    fail = 0;
    flips = 0;
    assert(!gs_vout_init());
    assert(gs_vout_has_frame() == 0); // The cleared buffer is not a rendered picture.
    assert(gs_vout_init());           // Cannot initialise the same display twice.
    assert(splash_hides == 0);
    auto* pixels = static_cast<std::uint32_t*>(std::calloc(1920 * 1080, sizeof(std::uint32_t)));
    assert(pixels);
    pixels[0] = 0x00123456;
    gs_vout_present(pixels, 20); // Invalid pitch is ignored, rather than read out of bounds.
    assert(flips == 1);
    expect_pixels = true;
    gs_vout_present(pixels, 1920 * 4);
    gs_vout_shutdown(); // Must join the worker before freeing its frame memory.
    assert(gs_vout_has_frame() == 1);
    assert(flips == 2 && indices[0] == 0 && indices[1] == 1);
    assert(splash_hides == 1);
    assert(outstanding == 0 && mappings == 0 && opens == 0);
    std::free(pixels);
    expect_pixels = false;
    flips = 0;
    assert(!gs_vout_init());
    assert(gs_vout_has_frame() == 0); // Reinitialisation cannot inherit the previous frame's acknowledgement.
    fail = 6;
    gs_vout_shutdown();
    assert(outstanding == 1 && mappings == 1 && opens == 1); // Never unmap an active display.
    fail = 0;
    gs_vout_shutdown();
    assert(outstanding == 0 && mappings == 0 && opens == 0);
    assert(gs_request_close() == 0 && close_calls == 1);
    closing_probe = true;
    if (setjmp(shell_replaced) == 0)
        gs_wait_for_close();
    assert(close_calls == 1 && close_waits == 2 && lnc_kill_calls == 0);
    // A stalled accepted request receives one exact-title self-close after
    // three seconds, then still waits instead of reaching the CRT exit path.
    close_waits = 0;
    shell_replace_after = 31;
    if (setjmp(shell_replaced) == 0)
        gs_wait_for_close();
    assert(close_waits == 31 && lnc_kill_calls == 1);
    close_waits = 0;
    lnc_title = "PPSA99203";
    if (setjmp(shell_replaced) == 0)
        gs_wait_for_close();
    assert(close_waits == 31 && lnc_kill_calls == 1); // A different title is never closed.
    lnc_title = "PPSA99541";
    closing_probe = false;
    close_status = -1;
    assert(gs_request_close() == -1 && close_calls == 2); // No implicit second launch or raw-exit fallback.
    std::puts("PASS: PS5 platform host mocks: allocator overflow/preservation, five display failure paths, first "
              "back-buffer selection, rendered-frame acknowledgement, read-only visibility flags, joined cleanup, "
              "failed-close memory retention, asynchronous shell exit and exact-title self-close fallback.");
}
