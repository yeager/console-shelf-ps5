// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>

namespace shelf::ui_test_preview {
constexpr int source_width = 1920;
constexpr int source_height = 1080;
constexpr int width = 960;
constexpr int height = 540;
constexpr std::size_t rgb_bytes = static_cast<std::size_t>(width) * height * 3;

// The private viewer is a low-rate control preview, not a video recorder. A
// single bounded sample from each 2 x 2 ABGR cell keeps its text readable while
// avoiding the old four-pixel average on the event/render thread. The JPEG
// worker still runs asynchronously after this fixed-size RGB copy completes.
inline bool downsample_abgr_2x(const void* pixels, int pitch, unsigned char* rgb,
                              std::size_t capacity) {
    if (!pixels || !rgb || capacity < rgb_bytes ||
        pitch < source_width * 4 || pitch > source_width * 4 + 4096)
        return false;
    const auto* source = static_cast<const unsigned char*>(pixels);
    auto* destination = rgb;
    for (int y = 0; y < height; ++y) {
        const auto* row = source + static_cast<std::size_t>(y * 2) *
                                      static_cast<std::size_t>(pitch);
        for (int x = 0; x < width; ++x) {
            const auto* pixel = row + static_cast<std::size_t>(x * 2 * 4);
            *destination++ = pixel[0];
            *destination++ = pixel[1];
            *destination++ = pixel[2];
        }
    }
    return true;
}
} // namespace shelf::ui_test_preview
