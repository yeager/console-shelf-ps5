// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/console_tile_forwarder_config.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
void config_check(bool value, const char* reason) {
    if (!value) {
        std::fprintf(stderr, "FAIL console tile forwarder config: %s\n", reason);
        std::exit(1);
    }
}
struct Json {
    cJSON* value;
    explicit Json(const char* text) : value(cJSON_Parse(text)) {}
    ~Json() { cJSON_Delete(value); }
};
bool parse(const char* owner_text, const char* config_text, const char* param_text) {
    Json owner(owner_text), config(config_text), param(param_text);
    char origin[10], route[shelf::console_tile_message_capacity + 1];
    return shelf::parse_console_tile_forwarder_config(
        owner.value, config.value, param.value, origin, route);
}

const char* preferred_menu_title(std::size_t system) {
    const auto id = shelf::catalog[system].id;
    if (id == "ps3") return "PPSA99303";
    if (id == "ps2") return "PPSA99203";
    if (id == "ps1") return "PPSA99248";
    if (id == "switch") return "PPSA99008";
    if (id == "wiiu") return "PPSA99360";
    return shelf::catalog[system].suggested_core.empty() ? nullptr : "PPSA99169";
}
}

void test_console_tile_forwarder_config() {
    constexpr char owner[] =
        R"({"format":"isshelf-console-tile/1","system":"threedo","titleId":"PPSA99745","target":"PPSA99541","forwarderSource":"https://github.com/heydemoura/ps5-forwarder-format","forwarderCommit":"22e8762ad7dbd01e18bd8c0e95f86461fdf8aaf4","runtime":"isshelf-cooperative-forwarder/3"})";
    constexpr char config[] =
        R"({"format":"ps5-forwarder/1","target":"PPSA99541","args":["--isshelf-console-tile=threedo,library,-"],"creator":"isShelf"})";
    constexpr char param[] = R"({"titleId":"PPSA99745","other":"allowed"})";
    config_check(parse(owner, config, param),
                 "the exact owned runtime, title and route are accepted");
    config_check(!parse(owner, config,
                        R"({"titleId":"PPSA99744"})"),
                 "param and ownership title IDs must match");
    config_check(!parse(owner,
                        R"({"format":"ps5-forwarder/1","target":"PPSA99541","args":["--isshelf-console-tile=ps2,library,-"],"creator":"isShelf"})",
                        param),
                 "the route system must match the owner system");
    config_check(!parse(owner,
                        R"({"format":"ps5-forwarder/1","target":"PPSA99999","args":["--isshelf-console-tile=threedo,library,-"],"creator":"isShelf"})",
                        param),
                 "the target is fixed to isShelf");
    config_check(!parse(
                     R"({"format":"isshelf-console-tile/1","system":"threedo","titleId":"PPSA99745","target":"PPSA99541","forwarderSource":"x","forwarderCommit":"y","runtime":"old"})",
                     config, param),
                 "a legacy or unknown runtime cannot execute the new program");
    config_check(!parse(
                     R"({"format":"isshelf-console-tile/1","system":"threedo","titleId":"PPSA99745","target":"PPSA99541","forwarderSource":"x","forwarderCommit":"22e8762ad7dbd01e18bd8c0e95f86461fdf8aaf4","runtime":"isshelf-cooperative-forwarder/3"})",
                     config, param),
                 "an unrecognized ownership source cannot execute the new program");
    config_check(!parse(
                     R"({"format":"isshelf-console-tile/1","system":"threedo","titleId":"PPSA99745","target":"PPSA99541","forwarderSource":"https://github.com/heydemoura/ps5-forwarder-format","forwarderCommit":"wrong","runtime":"isshelf-cooperative-forwarder/3"})",
                     config, param),
                 "an unrecognized ownership commit cannot execute the new program");
    config_check(!parse(
                     R"({"format":"isshelf-console-tile/1","system":"threedo","titleId":"PPSA99745","target":"PPSA99541","forwarderSource":"x","forwarderCommit":"y","runtime":"isshelf-cooperative-forwarder/3","extra":true})",
                     config, param),
                 "unexpected owner fields fail closed");
    config_check(!parse(owner,
                        R"({"format":"ps5-forwarder/1","target":"PPSA99541","target":"PPSA99541","args":["--isshelf-console-tile=threedo,library,-"],"creator":"isShelf"})",
                        param),
                 "duplicate security-sensitive fields fail closed");

    std::size_t emulator_configs = 0;
    for (std::size_t system = 0; system < shelf::system_count; ++system) {
        const char* title = shelf::tile_job::owned_title_for(system);
        config_check(title != nullptr, "every catalog system has an owned title pin");
        const std::string owner_text =
            "{\"format\":\"isshelf-console-tile/1\",\"system\":\"" +
            std::string(shelf::catalog[system].id) +
            "\",\"titleId\":\"" + title +
            "\",\"target\":\"PPSA99541\",\"forwarderSource\":\"https://github.com/heydemoura/ps5-forwarder-format\",\"forwarderCommit\":\"22e8762ad7dbd01e18bd8c0e95f86461fdf8aaf4\",\"runtime\":\"isshelf-cooperative-forwarder/3\"}";
        const std::string param_text = "{\"titleId\":\"" +
                                       std::string(title) + "\"}";
        const std::string library_config =
            "{\"format\":\"ps5-forwarder/1\",\"target\":\"PPSA99541\",\"args\":[\"--isshelf-console-tile=" +
            std::string(shelf::catalog[system].id) +
            ",library,-\"],\"creator\":\"isShelf\"}";
        config_check(parse(owner_text.c_str(), library_config.c_str(),
                           param_text.c_str()),
                     "every catalog owner accepts its exact Library route");
        const char* emulator = preferred_menu_title(system);
        if (!emulator) continue;
        const std::string emulator_config =
            "{\"format\":\"ps5-forwarder/1\",\"target\":\"PPSA99541\",\"args\":[\"--isshelf-console-tile=" +
            std::string(shelf::catalog[system].id) + ",emulator," + emulator +
            "\"],\"creator\":\"isShelf\"}";
        config_check(parse(owner_text.c_str(), emulator_config.c_str(),
                           param_text.c_str()),
                     "every supported owner accepts its pinned emulator route");
        ++emulator_configs;
    }
    config_check(emulator_configs == shelf::system_count - 3,
                 "all 49 emulator-capable owner configurations parse exactly");
    std::puts("PASS: cooperative tile config binds owner, runtime, title, target and route.");
}
