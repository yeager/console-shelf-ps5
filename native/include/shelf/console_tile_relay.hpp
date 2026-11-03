// SPDX-License-Identifier: GPL-3.0-only
// Cooperative handoff for isShelf-owned PS5 Home shortcuts. The foreground
// tile closes before the relay makes one bounded launch request for isShelf.
#pragma once

#include "console_tile_intent.hpp"
#include "cjson/cJSON.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace shelf {

inline constexpr char console_tile_relay_target[] = "PPSA99541";
inline constexpr unsigned console_tile_relay_first_title = 99700;
inline constexpr unsigned console_tile_relay_last_title = 99899;
inline constexpr unsigned console_tile_relay_reserved_title = 99764;
inline constexpr unsigned console_tile_relay_idle_observations = 5;
inline constexpr unsigned console_tile_relay_max_observations = 120;
inline constexpr unsigned console_tile_relay_observation_ms = 250;
inline constexpr unsigned console_tile_relay_release_max_observations = 160;
inline constexpr unsigned console_tile_relay_release_observation_ms = 250;
// ShadowMount beta11 protects a just-closed title while its lifecycle handoff
// settles. Its public API explicitly treats EBUSY as a calm-retry condition,
// so make one second unmount request only after five continuous idle seconds.
inline constexpr unsigned console_tile_relay_unmount_retry_observation = 21;
// If that one retry is still busy, pace the read-only release gate to one
// request per second instead of occupying ShadowMount's single HTTP worker.
inline constexpr unsigned console_tile_relay_info_observation_interval = 4;
inline constexpr std::uint32_t console_tile_relay_magic = 0x32525349u; // "ISR2"
inline constexpr std::uint32_t console_tile_relay_wire_version = 1;
inline constexpr std::size_t console_tile_relay_request_size = 160;
inline constexpr std::size_t console_tile_relay_reply_size = 16;
inline constexpr std::size_t console_tile_relay_origin_offset = 8;
inline constexpr std::size_t console_tile_relay_origin_capacity = 16;
inline constexpr std::size_t console_tile_relay_route_offset = 24;
inline constexpr std::size_t console_tile_relay_route_capacity =
    console_tile_relay_request_size - console_tile_relay_route_offset;

inline bool console_tile_relay_title_valid(std::string_view title);

enum class ConsoleTileRelayReply : std::uint32_t {
    queued = 0,
    invalid = 1,
    foreground_mismatch = 2,
    service_unavailable = 3,
};

enum class ConsoleTileReleaseProbe {
    invalid,
    pending,
    ready,
};

enum class ConsoleTileUnmountProbe {
    invalid,
    pending,
    ready,
};

inline bool console_tile_relay_should_retry_unmount(
    unsigned release_probe, unsigned attempts, bool retry_pending) {
    return retry_pending && attempts == 1 &&
           release_probe >= console_tile_relay_unmount_retry_observation;
}

inline bool console_tile_relay_should_poll_info(unsigned release_probe,
                                                unsigned last_info_probe) {
    return last_info_probe == 0 ||
           release_probe - last_info_probe >=
               console_tile_relay_info_observation_interval;
}

inline const cJSON* console_tile_relay_exact_field(const cJSON* object,
                                                   const char* name) {
    if (!cJSON_IsObject(object) || !name) return nullptr;
    const cJSON* found = nullptr;
    for (const cJSON* child = object->child; child; child = child->next) {
        if (child->string && std::strcmp(child->string, name) == 0) {
            if (found) return nullptr;
            found = child;
        }
    }
    return found;
}

// ShadowMount returns HTTP 409 while a launch/exit owns its runtime state.
// Once the exact outgoing title is reported as unmounted, its launch layers
// have been released and the relay may make its single destination request.
inline ConsoleTileReleaseProbe parse_console_tile_release_probe(
    unsigned http_status, std::string_view body, std::string_view origin) {
    if (!console_tile_relay_title_valid(origin) || body.empty() ||
        body.size() > 32768)
        return ConsoleTileReleaseProbe::invalid;
    if (http_status == 409)
        return ConsoleTileReleaseProbe::pending;
    if (http_status != 200)
        return ConsoleTileReleaseProbe::invalid;
    const char* end = nullptr;
    cJSON* root = cJSON_ParseWithLengthOpts(
        body.data(), body.size(), &end, 0);
    if (!root) return ConsoleTileReleaseProbe::invalid;
    bool trailing = false;
    for (const char* at = end; at && at < body.data() + body.size(); ++at)
        if (*at != ' ' && *at != '\t' && *at != '\r' && *at != '\n')
            trailing = true;
    const cJSON* status = console_tile_relay_exact_field(root, "status");
    const cJSON* title = console_tile_relay_exact_field(root, "title_id");
    const cJSON* mounted = console_tile_relay_exact_field(root, "mounted");
    const bool exact = !trailing && cJSON_IsNumber(status) &&
                       status->valuedouble == 0.0 &&
                       cJSON_IsString(title) && title->valuestring &&
                       origin == title->valuestring && cJSON_IsBool(mounted);
    const auto result = !exact ? ConsoleTileReleaseProbe::invalid
                              : cJSON_IsFalse(mounted)
                                    ? ConsoleTileReleaseProbe::ready
                                    : ConsoleTileReleaseProbe::pending;
    cJSON_Delete(root);
    return result;
}

// The public ShadowMount unmount endpoint is a safe acceleration after Home
// has been observed for a full stable second.  A successful reply proves the
// exact outgoing title's runtime layers are already released.  Busy remains a
// normal pending state; every other reply falls back to the read-only info
// gate instead of authorizing a launch.
inline ConsoleTileUnmountProbe parse_console_tile_unmount_probe(
    unsigned http_status, std::string_view body, std::string_view origin) {
    if (!console_tile_relay_title_valid(origin) || body.empty() ||
        body.size() > 32768)
        return ConsoleTileUnmountProbe::invalid;
    const char* end = nullptr;
    cJSON* root = cJSON_ParseWithLengthOpts(
        body.data(), body.size(), &end, 0);
    if (!root) return ConsoleTileUnmountProbe::invalid;
    bool trailing = false;
    for (const char* at = end; at && at < body.data() + body.size(); ++at)
        if (*at != ' ' && *at != '\t' && *at != '\r' && *at != '\n')
            trailing = true;
    const cJSON* status = console_tile_relay_exact_field(root, "status");
    const cJSON* title = console_tile_relay_exact_field(root, "title_id");
    const cJSON* mounted = console_tile_relay_exact_field(root, "mounted");
    ConsoleTileUnmountProbe result = ConsoleTileUnmountProbe::invalid;
    if (!trailing && cJSON_IsNumber(status)) {
        if (http_status == 409 && status->valuedouble == 16.0) {
            result = ConsoleTileUnmountProbe::pending;
        } else if (http_status == 200 && status->valuedouble == 0.0 &&
                   cJSON_IsString(title) && title->valuestring &&
                   origin == title->valuestring && cJSON_IsFalse(mounted)) {
            result = ConsoleTileUnmountProbe::ready;
        }
    }
    cJSON_Delete(root);
    return result;
}

inline void console_tile_relay_put_u32(unsigned char* output, std::uint32_t value) {
    output[0] = static_cast<unsigned char>(value);
    output[1] = static_cast<unsigned char>(value >> 8);
    output[2] = static_cast<unsigned char>(value >> 16);
    output[3] = static_cast<unsigned char>(value >> 24);
}

inline std::uint32_t console_tile_relay_get_u32(const unsigned char* input) {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8) |
           (static_cast<std::uint32_t>(input[2]) << 16) |
           (static_cast<std::uint32_t>(input[3]) << 24);
}

inline bool console_tile_relay_title_valid(std::string_view title) {
    if (title.size() != 9 || title.substr(0, 4) != "PPSA") return false;
    unsigned number = 0;
    for (std::size_t i = 4; i < title.size(); ++i) {
        if (title[i] < '0' || title[i] > '9') return false;
        number = number * 10u + static_cast<unsigned>(title[i] - '0');
    }
    return number >= console_tile_relay_first_title &&
           number <= console_tile_relay_last_title &&
           number != console_tile_relay_reserved_title;
}

inline bool console_tile_relay_route_valid(std::string_view route) {
    ConsoleTileIntent intent;
    return parse_console_tile_arg(route, intent) &&
           console_tile_intent_valid(intent);
}
struct ConsoleTileRelayLaunchPlan {
    char title[10]{};
    bool forward_route = false;
    char argument[console_tile_message_capacity + 1]{};
};

// Standalone emulator destinations are bounded by the generated system/menu
// pin table and launch directly. A legacy RetroArch console route is
// normalized to that console's isShelf library because the installed official
// RetroArch forwarder cannot select a core menu without game content. Library
// and Home routes also go through isShelf because they need its UI and system.
inline bool console_tile_relay_launch_plan(
    std::string_view route, ConsoleTileRelayLaunchPlan& output) {
    ConsoleTileIntent intent;
    if (!parse_console_tile_arg(route, intent) ||
        !console_tile_intent_valid(intent))
        return false;
    output = {};
    if (intent.destination == ConsoleTileDestination::emulator &&
        console_tile_emulator_menu_selects_system(intent.emulator_title)) {
        std::memcpy(output.title, intent.emulator_title, sizeof(output.title));
        output.forward_route = false;
    } else {
        std::memcpy(output.title, console_tile_relay_target,
                    sizeof(console_tile_relay_target));
        output.forward_route = true;
        ConsoleTileIntent normalized;
        if (!normalize_console_tile_intent(intent, normalized) ||
            !encode_console_tile_arg(normalized, output.argument,
                                     sizeof(output.argument)))
            return false;
    }
    return !output.forward_route
               ? launch_title_valid(output.title) &&
                     launch_menu_known(output.title) && !output.forward_route
               : std::strcmp(output.title, console_tile_relay_target) == 0 &&
                     output.forward_route && output.argument[0];
}

inline bool encode_console_tile_relay_request(
    std::string_view origin, std::string_view route, unsigned char* output,
    std::size_t capacity) {
    if (!output || capacity != console_tile_relay_request_size ||
        !console_tile_relay_title_valid(origin) ||
        !console_tile_relay_route_valid(route) ||
        origin.size() + 1 > console_tile_relay_origin_capacity ||
        route.size() + 1 > console_tile_relay_route_capacity)
        return false;
    std::memset(output, 0, capacity);
    console_tile_relay_put_u32(output, console_tile_relay_magic);
    console_tile_relay_put_u32(output + 4, console_tile_relay_wire_version);
    std::memcpy(output + console_tile_relay_origin_offset, origin.data(), origin.size());
    std::memcpy(output + console_tile_relay_route_offset, route.data(), route.size());
    return true;
}

inline bool decode_console_tile_relay_request(
    const unsigned char* input, std::size_t size, char (&origin)[10],
    char (&route)[console_tile_message_capacity + 1]) {
    if (!input || size != console_tile_relay_request_size ||
        console_tile_relay_get_u32(input) != console_tile_relay_magic ||
        console_tile_relay_get_u32(input + 4) != console_tile_relay_wire_version)
        return false;
    const auto* origin_field = reinterpret_cast<const char*>(
        input + console_tile_relay_origin_offset);
    const auto* route_field = reinterpret_cast<const char*>(
        input + console_tile_relay_route_offset);
    const void* origin_end = std::memchr(origin_field, 0,
                                         console_tile_relay_origin_capacity);
    const void* route_end = std::memchr(route_field, 0,
                                        console_tile_relay_route_capacity);
    if (!origin_end || !route_end) return false;
    const auto origin_size = static_cast<std::size_t>(
        static_cast<const char*>(origin_end) - origin_field);
    const auto route_size = static_cast<std::size_t>(
        static_cast<const char*>(route_end) - route_field);
    if (origin_size != 9 || route_size > console_tile_message_capacity ||
        !console_tile_relay_title_valid({origin_field, origin_size}) ||
        !console_tile_relay_route_valid({route_field, route_size}))
        return false;
    for (const unsigned char* at = input + console_tile_relay_origin_offset + origin_size + 1;
         at < input + console_tile_relay_route_offset; ++at)
        if (*at != 0) return false;
    for (const unsigned char* at = input + console_tile_relay_route_offset + route_size + 1;
         at < input + size; ++at)
        if (*at != 0) return false;
    std::memcpy(origin, origin_field, 9);
    origin[9] = 0;
    std::memcpy(route, route_field, route_size);
    route[route_size] = 0;
    return true;
}

inline void encode_console_tile_relay_reply(ConsoleTileRelayReply status,
                                            std::int32_t code,
                                            unsigned char (&output)[console_tile_relay_reply_size]) {
    std::memset(output, 0, sizeof(output));
    console_tile_relay_put_u32(output, console_tile_relay_magic);
    console_tile_relay_put_u32(output + 4, console_tile_relay_wire_version);
    console_tile_relay_put_u32(output + 8, static_cast<std::uint32_t>(status));
    console_tile_relay_put_u32(output + 12, static_cast<std::uint32_t>(code));
}

inline bool decode_console_tile_relay_reply(
    const unsigned char* input, std::size_t size, ConsoleTileRelayReply& status,
    std::int32_t& code) {
    if (!input || size != console_tile_relay_reply_size ||
        console_tile_relay_get_u32(input) != console_tile_relay_magic ||
        console_tile_relay_get_u32(input + 4) != console_tile_relay_wire_version)
        return false;
    const auto raw = console_tile_relay_get_u32(input + 8);
    if (raw > static_cast<std::uint32_t>(ConsoleTileRelayReply::service_unavailable))
        return false;
    status = static_cast<ConsoleTileRelayReply>(raw);
    code = static_cast<std::int32_t>(console_tile_relay_get_u32(input + 12));
    return true;
}

enum class ConsoleTileRelayResult {
    invalid,
    foreground_changed,
    close_timeout,
    release_unavailable,
    release_timeout,
    launch_refused,
    launch_requested,
};

struct ConsoleTileRelayOps {
    void* context = nullptr;
    // 0: no running big app; 1: title filled; -1: unknown/error.
    int (*current)(void*, char (&title)[10]) = nullptr;
    void (*wait)(void*, unsigned milliseconds) = nullptr;
    // 0: outgoing runtime is still owned; 1: released; -1: invalid reply.
    int (*released)(void*, const char* origin) = nullptr;
    // One launch of the already validated plan. No retry.
    int (*launch)(void*, const char* route) = nullptr;
};

// The caller must observe `origin` in the foreground before acknowledging the
// tile's request. Once acknowledged, this function permits only that same tile
// or an idle shell until Home has been stable for a full second.
inline ConsoleTileRelayResult cooperative_console_tile_relay(
    const char* origin, const char* route, const ConsoleTileRelayOps& ops) {
    if (!origin || !route || !ops.current || !ops.wait || !ops.released ||
        !ops.launch ||
        !console_tile_relay_title_valid(origin) ||
        !console_tile_relay_route_valid(route))
        return ConsoleTileRelayResult::invalid;

    unsigned idle = 0;
    for (unsigned observation = 0;
         observation < console_tile_relay_max_observations; ++observation) {
        char title[10]{};
        const int current = ops.current(ops.context, title);
        if (current < 0)
            return ConsoleTileRelayResult::foreground_changed;
        if (current == 1) {
            if (std::strcmp(title, origin) != 0)
                return ConsoleTileRelayResult::foreground_changed;
            idle = 0;
        } else {
            ++idle;
            if (idle >= console_tile_relay_idle_observations) break;
        }
        if (observation + 1 < console_tile_relay_max_observations)
            ops.wait(ops.context, console_tile_relay_observation_ms);
    }
    if (idle < console_tile_relay_idle_observations)
        return ConsoleTileRelayResult::close_timeout;

    for (unsigned observation = 0;
         observation < console_tile_relay_release_max_observations;
         ++observation) {
        char title[10]{};
        const int active = ops.current(ops.context, title);
        if (active < 0 || (active == 1 && std::strcmp(title, origin) != 0))
            return ConsoleTileRelayResult::foreground_changed;
        const int released = ops.released(ops.context, origin);
        if (released < 0)
            return ConsoleTileRelayResult::release_unavailable;
        if (released > 0)
            return ops.launch(ops.context, route) >= 0
                       ? ConsoleTileRelayResult::launch_requested
                       : ConsoleTileRelayResult::launch_refused;
        if (observation + 1 < console_tile_relay_release_max_observations)
            ops.wait(ops.context,
                     console_tile_relay_release_observation_ms);
    }
    return ConsoleTileRelayResult::release_timeout;
}

} // namespace shelf
