// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/ui_test_preview.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
void check(bool value, const char* why) {
    if (!value) {
        std::fprintf(stderr, "FAIL UI test preview: %s\n", why);
        std::exit(1);
    }
}
} // namespace

void test_ui_test_preview() {
    using namespace shelf::ui_test_preview;
    constexpr int pitch = source_width * 4 + 32;
    std::vector<unsigned char> source(static_cast<std::size_t>(pitch) * source_height, 0xee);
    std::vector<unsigned char> destination(rgb_bytes, 0);
    for (int y = 0; y < height; ++y) {
        auto* row = source.data() + static_cast<std::size_t>(y * 2) * pitch;
        for (int x = 0; x < width; ++x) {
            auto* pixel = row + static_cast<std::size_t>(x * 2 * 4);
            pixel[0] = static_cast<unsigned char>(x & 0xff);
            pixel[1] = static_cast<unsigned char>(y & 0xff);
            pixel[2] = static_cast<unsigned char>((x + y) & 0xff);
            pixel[3] = 0x7f;
            // The other three pixels must not influence the single-sample preview.
            pixel[4] = pixel[5] = pixel[6] = pixel[7] = 0xdd;
            auto* lower = pixel + pitch;
            lower[0] = lower[1] = lower[2] = lower[3] = 0xcc;
            lower[4] = lower[5] = lower[6] = lower[7] = 0xbb;
        }
    }
    check(downsample_abgr_2x(source.data(), pitch, destination.data(), destination.size()),
          "valid padded ABGR surface is accepted");
    for (const auto [x, y] : {std::pair{0, 0}, std::pair{319, 271}, std::pair{959, 539}}) {
        const auto at = (static_cast<std::size_t>(y) * width + x) * 3;
        check(destination[at] == static_cast<unsigned char>(x & 0xff) &&
                  destination[at + 1] == static_cast<unsigned char>(y & 0xff) &&
                  destination[at + 2] == static_cast<unsigned char>((x + y) & 0xff),
              "preview selects the exact top-left RGB sample and ignores alpha and neighbours");
    }
    check(!downsample_abgr_2x(nullptr, pitch, destination.data(), destination.size()) &&
              !downsample_abgr_2x(source.data(), source_width * 4 - 1,
                                   destination.data(), destination.size()) &&
              !downsample_abgr_2x(source.data(), pitch, destination.data(), destination.size() - 1),
          "null, short-pitch and short-destination inputs are rejected");
    std::puts("PASS: bounded low-cost private preview downsample and input guards.");
}
