// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "ui.hpp"
#include <cstdint>
#include <cstdlib>

namespace shelf {
// One physical gesture owns repeat until released. Axis hysteresis keeps a
// slightly shaky diagonal from alternating between rows and columns.
class NavigationInput {
    enum class Source { none, pad, stick } source = Source::none;
    Action held = Action::up;
    std::int32_t owner = -1, stick_owner = -1;
    int x = 0, y = 0;
    bool stick_blocked = false;
    std::uint32_t began = 0, next = 0;
    static constexpr int engage = 15000, release = 9000;
    static constexpr std::uint32_t initial_repeat_ms = 220;
    static constexpr std::uint32_t accelerated_after_ms = 700;
    static constexpr std::uint32_t repeat_ms = 85;
    static constexpr std::uint32_t accelerated_repeat_ms = 60;
    bool start(Action direction, Source kind, std::int32_t device, std::uint32_t now, Action& output) {
        source = kind;
        held = output = direction;
        owner = device;
        began = now;
        next = now + initial_repeat_ms;
        return true;
    }

  public:
    static bool directional(Action action) {
        return action == Action::up || action == Action::down || action == Action::left || action == Action::right;
    }
    bool button(Action direction, bool pressed, std::int32_t device, std::uint32_t now, Action& output) {
        if (!directional(direction))
            return false;
        if (pressed) {
            if (source == Source::pad && owner == device && held == direction)
                return false;
            return start(direction, Source::pad, device, now, output);
        }
        if (source == Source::pad && owner == device && held == direction)
            source = Source::none;
        return false;
    }
    bool axis(bool horizontal, int value, std::int32_t device, std::uint32_t now, Action& output) {
        if (stick_owner != device) {
            if (source != Source::none || std::abs(value) < engage)
                return false;
            stick_owner = device;
            x = y = 0;
        }
        (horizontal ? x : y) = value;
        const int ax = std::abs(x), ay = std::abs(y);
        if (ax <= release && ay <= release) {
            stick_blocked = false;
            if (source == Source::stick)
                source = Source::none;
            return false;
        }
        if (stick_blocked || source == Source::pad)
            return false;
        if (source != Source::stick && ax < engage && ay < engage)
            return false;
        bool use_x = ax >= ay;
        if (source == Source::stick) {
            const bool was_x = held == Action::left || held == Action::right;
            const int current = was_x ? ax : ay, other = was_x ? ay : ax;
            use_x = current > release && other * 4 <= current * 5 ? was_x : use_x;
        }
        const auto direction = use_x ? (x > 0 ? Action::right : Action::left) : (y > 0 ? Action::down : Action::up);
        if (source == Source::stick && direction == held)
            return false;
        return start(direction, Source::stick, device, now, output);
    }
    bool tick(std::uint32_t now, Action& output) {
        if (source == Source::none || static_cast<std::int32_t>(now - next) < 0)
            return false;
        output = held;
        // Accelerate a sustained hold, without queuing a burst after a slow frame.
        next = now + (static_cast<std::uint32_t>(now - began) >= accelerated_after_ms
                          ? accelerated_repeat_ms : repeat_ms);
        return true;
    }
    void cancel() {
        source = Source::none;
        stick_blocked = std::abs(x) > release || std::abs(y) > release;
    }
    void disconnected(std::int32_t device) {
        if (owner == device)
            source = Source::none;
        if (stick_owner == device) {
            stick_owner = -1;
            x = y = 0;
            stick_blocked = false;
        }
    }
};
} // namespace shelf
