// SPDX-License-Identifier: GPL-3.0-only
// Private-build startup arm only. The nonce binds a reviewed one-shot opener
// receipt to this launch; it is not authentication for the FTP test bridge.
#pragma once

#include "console_tile_intent.hpp"
#include "json.hpp"
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace shelf {
enum class UiTestRemoteArmInput { absent, valid, invalid };

inline UiTestRemoteArmInput parse_ui_test_remote_arm(int argc, char* const argv[],
                                                    ConsoleTileInput tile_input, bool tile_marker,
                                                    char (&nonce)[33]) {
    nonce[0] = 0;
    if (argc < 0 || argc > 64 || (argc > 0 && !argv))
        return UiTestRemoteArmInput::invalid;
    constexpr char stem[] = "--isshelf-ui-test-arm";
    constexpr char prefix[] = "--isshelf-ui-test-arm=";
    constexpr std::size_t length = sizeof(prefix) - 1 + 32;
    bool seen = false;
    int arm_index = -1;
    char candidate[33]{};
    for (int i = 0; i < argc; ++i) {
        if (!argv[i])
            return UiTestRemoteArmInput::invalid;
        if (std::strncmp(argv[i], stem, sizeof(stem) - 1) != 0)
            continue;
        if (seen || std::strncmp(argv[i], prefix, sizeof(prefix) - 1) != 0)
            return UiTestRemoteArmInput::invalid;
        // Stop at the first NUL; a fixed-length memchr could read beyond a
        // shorter argv allocation before noticing its terminator.
        std::size_t used = 0;
        while (used <= length && argv[i][used])
            ++used;
        if (used != length)
            return UiTestRemoteArmInput::invalid;
        const char* token = argv[i] + sizeof(prefix) - 1;
        bool nonzero = false;
        for (unsigned n = 0; n < 32; ++n) {
            const char ch = token[n];
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
                return UiTestRemoteArmInput::invalid;
            nonzero |= ch != '0';
        }
        if (!nonzero)
            return UiTestRemoteArmInput::invalid;
        std::memcpy(candidate, token, 32);
        candidate[32] = 0;
        seen = true;
        arm_index = i;
    }
    if (!seen)
        return UiTestRemoteArmInput::absent;
    // This PS5's CRT supplies an empty argv[0] before the one launch argument.
    // Keep the earlier one-argument and ordinary executable-name forms exact.
    bool ordinary_executable = false;
    if ((argc == 2 || argc == 3) && argv[0] && argv[0][0] && argv[0][0] != '-') {
        std::size_t name_size = 0;
        while (name_size < 256 && argv[0][name_size] &&
               static_cast<unsigned char>(argv[0][name_size]) >= 32 &&
               static_cast<unsigned char>(argv[0][name_size]) <= 126)
            ++name_size;
        ordinary_executable = name_size > 0 && name_size < 256 && argv[0][name_size] == 0;
    }
    const bool empty_ps5_argv0 = (argc == 2 || argc == 3) && argv[0] && argv[0][0] == 0;
    const bool ordinary_main = tile_input == ConsoleTileInput::absent && !tile_marker &&
        ((argc == 1 && arm_index == 0) ||
         (argc == 2 && arm_index == 1 && (ordinary_executable || empty_ps5_argv0)));
    // A separately reviewed fixed one-shot opener can test the PS2 chooser
    // without a physical L3+R3 press. It launches the main title with exactly
    // the static PS2 Home hint and this nonce arm, in that order. Neither an
    // installed forwarder nor a caller-supplied console/title can use it.
    constexpr char ps2_home[] = "--isshelf-console-tile=ps2,home,-";
    const bool ps2_pair = tile_input == ConsoleTileInput::valid && !tile_marker &&
        ((argc == 2 && arm_index == 0 && argv[1] && std::strcmp(argv[1], ps2_home) == 0) ||
         (argc == 3 && arm_index == 1 && (ordinary_executable || empty_ps5_argv0) &&
          argv[2] && std::strcmp(argv[2], ps2_home) == 0));
    if (!ordinary_main && !ps2_pair)
        return UiTestRemoteArmInput::invalid;
    std::memcpy(nonce, candidate, sizeof(candidate));
    return UiTestRemoteArmInput::valid;
}

inline bool ui_test_main_title_param(const char* path) {
    if (!path)
        return false;
    FILE* file = std::fopen(path, "rb");
    if (!file)
        return false;
    char bytes[8193];
    const auto size = std::fread(bytes, 1, sizeof(bytes), file);
    const bool complete = std::feof(file) != 0 && std::ferror(file) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!complete || !closed || !size || size > 8192)
        return false;
    JsonDocument document;
    if (!document.parse(bytes, size) || !cJSON_IsObject(document.root))
        return false;
    const char* title = json_string(document.root, "titleId");
    const char* content = json_string(document.root, "contentId");
    return title && content && std::strcmp(title, shelf_title_id) == 0 &&
           std::strcmp(content, "UP9000-PPSA99541_00-CONSOLESHELF0001") == 0;
}
} // namespace shelf
