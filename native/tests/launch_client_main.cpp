// SPDX-License-Identifier: GPL-3.0-only
#define SDL_MAIN_HANDLED
#include "shelf/launch_client.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
int main(int argc, char** argv) {
    if (argc != 4 || SDL_Init(0) != 0)
        return 1;
    const char* mode = argv[3];
    unsigned char packet[64]{};
    std::memcpy(packet,
                "\x7f"
                "ELF\x02\x01\x01\x00",
                8);
    if (std::strcmp(mode, "invalid") == 0)
        packet[0] = 0;
    if (std::strcmp(mode, "probe") == 0) {
        SDL_atomic_t cancelled{0};
        const bool sent = shelf::send_probe_elf_once(
            packet, sizeof(packet), cancelled, static_cast<unsigned short>(std::strtoul(argv[2], nullptr, 10)));
        std::printf("{\"passed\":%s,\"helperExecuted\":false}\n", sent ? "true" : "false");
        SDL_Quit();
        return sent ? 0 : 1;
    }
    shelf::LaunchRequest request;
    std::strcpy(request.id, "0123456789abcdef0123456789abcdef");
    std::strcpy(request.title, "PPSA99203");
    request.issued = static_cast<std::uint64_t>(std::time(nullptr));
    int result = 1;
    {
        shelf::LaunchClient client;
        const bool began = client.start(request, argv[1], packet, sizeof(packet),
                                        static_cast<unsigned short>(std::strtoul(argv[2], nullptr, 10)));
        const auto started = SDL_GetTicks();
        bool cancelled = false;
        while (began && SDL_GetTicks() - started < 6500) {
            const auto state = client.state();
            if (std::strcmp(mode, "late-ready-cancel") == 0 && !cancelled && SDL_GetTicks() - started >= 100) {
                client.cancel();
                cancelled = true;
            }
            if (cancelled && SDL_GetTicks() - started >= 700) {
                result = client.commit() ? 1 : 0;
                break;
            }
            if (state == shelf::LaunchClientState::ready) {
                if (std::strcmp(mode, "cancel-ready") == 0) {
                    client.cancel();
                    result = client.commit() ? 1 : 0;
                } else
                    result = client.commit() ? 0 : 1;
                break;
            }
            if (!cancelled && state == shelf::LaunchClientState::failed) {
                result = std::strcmp(mode, "success") == 0 || std::strcmp(mode, "stale") == 0 ||
                                 std::strcmp(mode, "terminal-unconfirmed") == 0
                             ? 1
                             : 0;
                break;
            }
            SDL_Delay(5);
        }
        if (!began && std::strcmp(mode, "invalid") == 0 && client.state() == shelf::LaunchClientState::failed)
            result = 0;
        client.stop();
        std::printf("{\"passed\":%s,\"state\":%d,\"helperExecuted\":false}\n", result == 0 ? "true" : "false",
                    static_cast<int>(client.state()));
    }
    SDL_Quit();
    return result;
}
