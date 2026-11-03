// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cmath>
namespace shelf {
inline double orbit_wrap(double value, double count) {
    return count > 0 ? std::fmod(std::fmod(value, count) + count, count) : 0;
}
inline double orbit_distance(double value, double count) {
    return count > 0 ? orbit_wrap(value + count / 2, count) - count / 2 : 0;
}
struct OrbitPose {
    double x, y, z, scale, yaw, opacity;
    bool visible;
};
inline OrbitPose orbit_pose(double offset, double width) {
    const double depth = std::abs(offset), angle = std::min(depth, 4.0) * 3.141592653589793 / 7.5;
    const double direction = offset < 0 ? -1 : offset > 0 ? 1 : 0;
    return {direction * width * (width < 600 ? .88 : .53) * std::sin(angle),
            -72 * (1 - std::cos(angle)),
            72 - 480 * (1 - std::cos(angle)),
            std::max(.30, 1.18 - depth * .28),
            -direction * std::min(depth * 9, 25.0),
            std::max(0.0, 1 - depth * .19) * std::clamp(4 - depth, 0.0, 1.0),
            depth < 4};
}
struct OrbitMotion {
    static constexpr double duration_ms = 260.0;
    double position = 0, target = 0, from = 0, start = 0;
    bool active = false;
    void update(double time) {
        if (!active)
            return;
        const double progress = std::clamp((time - start) / duration_ms, 0.0, 1.0);
        const double eased = 1 - std::pow(1 - progress, 3);
        position = from + (target - from) * eased;
        if (progress == 1) {
            position = target;
            active = false;
        }
    }
    void select(double index, double count, double time, bool immediate) {
        update(time);
        const double next = position + orbit_distance(index - position, count);
        immediate = immediate || std::abs(next - position) > 3;
        if (std::abs(next - target) < .0001 && !immediate && (active || std::abs(next - position) < .0001))
            return;
        from = position;
        target = next;
        start = time;
        active = !immediate && std::abs(target - position) > .0001;
        if (!active)
            position = target;
    }
};
} // namespace shelf
