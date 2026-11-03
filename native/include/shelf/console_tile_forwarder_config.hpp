// SPDX-License-Identifier: GPL-3.0-only
// Exact three-file identity for the cooperative console-tile program.
#pragma once

#include "console_tile_relay.hpp"
#include "cjson/cJSON.h"
#include <cstddef>
#include <cstring>

namespace shelf {

inline constexpr char console_tile_forwarder_runtime[] =
    "isshelf-cooperative-forwarder/3";
inline constexpr char console_tile_forwarder_source[] =
    "https://github.com/heydemoura/ps5-forwarder-format";
inline constexpr char console_tile_forwarder_commit[] =
    "22e8762ad7dbd01e18bd8c0e95f86461fdf8aaf4";

inline const char* console_tile_forwarder_string(const cJSON* object,
                                                 const char* name) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

inline bool console_tile_forwarder_exact_fields(
    const cJSON* object, const char* const* allowed, std::size_t count) {
    bool seen[16]{};
    if (!object || count > sizeof(seen) / sizeof(seen[0])) return false;
    for (const cJSON* child = object->child; child; child = child->next) {
        std::size_t matched = count;
        for (std::size_t i = 0; i < count; ++i)
            if (child->string && std::strcmp(child->string, allowed[i]) == 0) {
                matched = i;
                break;
            }
        if (matched == count || seen[matched]) return false;
        seen[matched] = true;
    }
    for (std::size_t i = 0; i < count; ++i)
        if (!seen[i]) return false;
    return true;
}

inline bool parse_console_tile_forwarder_config(
    const cJSON* owner, const cJSON* config, const cJSON* param,
    char (&origin)[10], char (&route)[console_tile_message_capacity + 1]) {
    constexpr const char* owner_fields[] = {
        "format", "system", "titleId", "target", "forwarderSource",
        "forwarderCommit", "runtime"};
    constexpr const char* config_fields[] = {
        "format", "target", "args", "creator"};
    if (!cJSON_IsObject(owner) || !cJSON_IsObject(config) ||
        !cJSON_IsObject(param) ||
        !console_tile_forwarder_exact_fields(
            owner, owner_fields, sizeof(owner_fields) / sizeof(owner_fields[0])) ||
        !console_tile_forwarder_exact_fields(
            config, config_fields, sizeof(config_fields) / sizeof(config_fields[0])))
        return false;
    const char* title = console_tile_forwarder_string(owner, "titleId");
    const char* system = console_tile_forwarder_string(owner, "system");
    const auto* args = cJSON_GetObjectItemCaseSensitive(config, "args");
    const auto* argument = cJSON_IsArray(args) && cJSON_GetArraySize(args) == 1
                               ? cJSON_GetArrayItem(args, 0)
                               : nullptr;
    const char* creator = console_tile_forwarder_string(config, "creator");
    if (std::strcmp(console_tile_forwarder_string(owner, "format"),
                    "isshelf-console-tile/1") != 0 ||
        std::strcmp(console_tile_forwarder_string(owner, "target"),
                    console_tile_relay_target) != 0 ||
        std::strcmp(console_tile_forwarder_string(owner, "forwarderSource"),
                    console_tile_forwarder_source) != 0 ||
        std::strcmp(console_tile_forwarder_string(owner, "forwarderCommit"),
                    console_tile_forwarder_commit) != 0 ||
        std::strcmp(console_tile_forwarder_string(owner, "runtime"),
                    console_tile_forwarder_runtime) != 0 ||
        std::strcmp(console_tile_forwarder_string(param, "titleId"), title) != 0 ||
        !console_tile_relay_title_valid(title) || !system[0] ||
        std::strcmp(console_tile_forwarder_string(config, "format"),
                    "ps5-forwarder/1") != 0 ||
        std::strcmp(console_tile_forwarder_string(config, "target"),
                    console_tile_relay_target) != 0 ||
        (std::strcmp(creator, "isShelf") != 0 &&
         std::strcmp(creator, "isShelf console tiles") != 0) ||
        !cJSON_IsString(argument) || !argument->valuestring)
        return false;
    const std::size_t route_size = std::strlen(argument->valuestring);
    ConsoleTileIntent intent;
    if (route_size > console_tile_message_capacity ||
        !parse_console_tile_arg({argument->valuestring, route_size}, intent) ||
        intent.system >= system_count || catalog[intent.system].id != system)
        return false;
    std::memcpy(origin, title, 9);
    origin[9] = 0;
    std::memcpy(route, argument->valuestring, route_size + 1);
    return true;
}

} // namespace shelf
