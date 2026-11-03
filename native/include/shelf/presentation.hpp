// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cstdint>
namespace shelf {
inline constexpr std::uint32_t close_hold_ms = 3000;
inline constexpr std::uint32_t opening_entrance_ms = 360;
inline constexpr std::uint32_t opening_reveal_ms = 520;
inline constexpr std::uint32_t close_fade_ms = 240;
// Only the hardware layer moves. The room and wood are rendered separately.
class ShelfPageMotion {
    static constexpr float duration_ms = 260.f;
    static constexpr float handoff = .38f;
    std::uint32_t began = 0;
  public:
    int from = 0, target = 0, direction = 1;
    bool active = false;
    float progress = 1;
    void reset(int page) {
        from = target = page;
        active = false;
        progress = 1;
    }
    void select(int page, std::uint32_t now, bool snap) {
        if (snap) {
            reset(page);
            return;
        }
        if (page != target) {
            from = target;
            target = page;
            direction = target > from ? 1 : -1;
            began = now;
            active = true;
        }
        progress = active ? std::min(1.f, static_cast<std::uint32_t>(now - began) / duration_ms) : 1;
        if (progress >= 1)
            active = false;
    }
    bool outgoing() const { return active && progress < handoff; }
    int page() const { return outgoing() ? from : target; }
    float fraction() const {
        const float t = outgoing() ? progress / handoff : (progress - handoff) / (1.f - handoff);
        const float clamped = std::max(0.f, std::min(1.f, t));
        return clamped * clamped * (3 - 2 * clamped);
    }
    float opacity() const { return !active ? 1 : outgoing() ? 1 - fraction() : fraction(); }
    float offset() const { return !active ? 0 : direction * 130.f * (outgoing() ? -fraction() : 1 - fraction()); }
};
struct Presentation {
    bool loading = false, preparing = false, artwork = false;
    bool launching = false;
    float reveal = 1, appear = 1, hold = 0, close = 0, close_hint = 0;
};
class CloseHint {
    bool visible = false;
    std::uint32_t began = 0;

  public:
    void show(std::uint32_t now) {
        visible = true;
        began = now;
    }
    void dismiss() {
        visible = false;
    }
    bool update(std::uint32_t now, Presentation& view) {
        const bool changed = visible || view.close_hint > 0;
        const auto age = static_cast<std::uint32_t>(now - began);
        if (age >= 3000)
            visible = false;
        const float enter = std::min(1.f, age / 180.f);
        const float leave = std::min(1.f, (3000.f - std::min(3000.f, static_cast<float>(age))) / 350.f);
        view.close_hint = visible ? std::min(enter, leave) : 0;
        return changed;
    }
};
class OpeningGate {
    bool unknown = false, foreground = false, fallback = false, owner_input = false;
    std::uint32_t unknown_at = 0, visible_at = 0;

  public:
    void sample(std::uint32_t now, int status, unsigned flags) {
        if (status == 0) {
            unknown = fallback = false;
            const bool next = flags == 0;
            if (next && !foreground)
                visible_at = now;
            foreground = next;
        } else {
            foreground = false;
            if (!unknown) {
                unknown = true;
                fallback = false;
                unknown_at = now;
            }
            if (!fallback && static_cast<std::uint32_t>(now - unknown_at) >= 5000) {
                fallback = true;
                visible_at = now;
            }
        }
    }
    // A physical controller/key event delivered to isShelf is an explicit
    // owner escape from a stale visibility report. Keep the same short stable
    // interval as a normal foreground transition, then reveal the controls.
    // This is deliberately separate from the automatic status-error fallback:
    // remote requests and background work cannot trigger it.
    void allow_owner_input(std::uint32_t now) {
        if (!owner_input && !foreground && !fallback) {
            owner_input = true;
            visible_at = now;
        }
    }
    bool ready(std::uint32_t now) const {
        return (foreground || fallback || owner_input) &&
               static_cast<std::uint32_t>(now - visible_at) >= 120;
    }
    std::uint32_t age(std::uint32_t now) const {
        return foreground || fallback || owner_input
                   ? static_cast<std::uint32_t>(now - visible_at)
                   : 0;
    }
    bool used_fallback() const {
        return fallback;
    }
    bool used_owner_input() const {
        return owner_input;
    }
};
enum class LaunchVisibilityDecision { wait, commit, cancel };
class LaunchVisibilityGate {
    bool active = false, foreground = false;
    std::uint32_t began = 0, foreground_at = 0;

  public:
    void reset() {
        active = foreground = false;
        began = foreground_at = 0;
    }
    LaunchVisibilityDecision sample(std::uint32_t now, int status, unsigned flags) {
        if (!active) {
            active = true;
            began = now;
        }
        const bool next_foreground = status == 0 && flags == 0;
        if (next_foreground && !foreground)
            foreground_at = now;
        foreground = next_foreground;
        // One transient clear sample is not enough: keep the helper in its
        // ready state until isShelf has remained unobscured for 120 ms.
        if (foreground && static_cast<std::uint32_t>(now - foreground_at) >= 120)
            return LaunchVisibilityDecision::commit;
        // The helper independently checks that isShelf remains the running
        // big app. This app-side bound only gives short system overlays time
        // to clear; it never retries a launch or authorizes a background app.
        if (static_cast<std::uint32_t>(now - began) >= 5000)
            return LaunchVisibilityDecision::cancel;
        return LaunchVisibilityDecision::wait;
    }
};
class CloseGesture {
    bool down = false, committed = false;
    std::int32_t owner = -1;
    std::uint32_t pressed_at = 0, committed_at = 0;

  public:
    void begin_close(std::uint32_t now) {
        if (!committed) {
            committed = true;
            committed_at = now;
            down = false;
        }
    }
    void press(std::int32_t device, std::uint32_t now) {
        if (down || committed)
            return;
        down = true;
        owner = device;
        pressed_at = now;
    }
    bool release(std::int32_t device, std::uint32_t now) {
        if (!down || owner != device || committed)
            return false;
        if (static_cast<std::uint32_t>(now - pressed_at) >= close_hold_ms) {
            committed = true;
            committed_at = now;
            return false;
        }
        down = false;
        return static_cast<std::uint32_t>(now - pressed_at) < 500;
    }
    void cancel(std::int32_t device = -1) {
        if (!committed && (device == -1 || owner == device))
            down = false;
    }
    void update(std::uint32_t now, Presentation& view) {
        const auto elapsed = static_cast<std::uint32_t>(now - pressed_at);
        if (down && !committed && elapsed >= close_hold_ms) {
            committed = true;
            committed_at = now;
        }
        view.hold = down ? std::min(1.0f, elapsed / static_cast<float>(close_hold_ms)) : 0;
        const float t = committed ? std::min(1.0f, (now - committed_at) / static_cast<float>(close_fade_ms)) : 0;
        view.close = t * t * (3 - 2 * t);
    }
    bool closing() const {
        return committed;
    }
    bool active() const {
        return down || committed;
    }
    bool finished(std::uint32_t now) const {
        return committed && static_cast<std::uint32_t>(now - committed_at) >= close_fade_ms;
    }
};
} // namespace shelf
