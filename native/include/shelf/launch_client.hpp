// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "launch_protocol.hpp"
#include <SDL.h>
#include <cstddef>

namespace shelf {
// Sends exactly one bounded ELF to the fixed loopback payload loader. It does
// not create a game launch request, commit a handoff, or request app exit.
bool send_probe_elf_once(const unsigned char* bytes, std::size_t size, SDL_atomic_t& cancelled,
                         unsigned short test_port = 9021);
enum class LaunchClientState { idle, sending, ready, failed, cancelled, committed };
// Console integration uses only its fixed owned directory and the existing
// loopback ELF loader. Host tests supply a synthetic packet to a local fixture.
class LaunchClient {
    SDL_Thread* thread = nullptr;
    SDL_atomic_t result{0}, cancelled{0};
    LaunchRequest request;
    const unsigned char* payload = nullptr;
    std::size_t payload_bytes = 0;
    unsigned short port = 9021;
    char root[2048]{}, failure[192]{};
    bool owns_request = false;
    static int run(void*);
    bool write_status(LaunchStage);
    bool read_status(LaunchStatus&);
    void discard_owned_request();
    void cancel_owned_status();
    void failed(const char*);

  public:
    ~LaunchClient();
    bool start(const LaunchRequest&, const char* owned_root, const unsigned char*, std::size_t,
               unsigned short loader_port = 9021);
    LaunchClientState state() const;
    bool commit();
    void cancel();
    void stop();
    const char* error() const {
        return failure;
    }
};
} // namespace shelf
