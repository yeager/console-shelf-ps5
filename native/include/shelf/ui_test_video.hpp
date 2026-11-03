// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>

namespace shelf {
// Optional private preview. The event thread supplies an already rendered
// 1920 x 1080 ABGR8888 software surface. Only its downsampled RGB copy reaches
// the encoder worker; that worker never calls SDL rendering functions.
class UiTestVideo {
    struct Worker;
    Worker* worker_ = nullptr;

  public:
    UiTestVideo() = default;
    UiTestVideo(const UiTestVideo&) = delete;
    UiTestVideo& operator=(const UiTestVideo&) = delete;
    ~UiTestVideo();
    bool start(const char* data_root, const char* session32, std::uint64_t ui_revision);
    void stop();
    bool active() const;
    bool due(std::uint32_t now) const;
    // A busy worker drops this frame rather than queueing another allocation.
    bool submit(const void* pixels, int pitch, std::uint64_t ui_revision, std::uint32_t now);
};
} // namespace shelf
