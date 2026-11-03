// SPDX-License-Identifier: GPL-3.0-only
// Shared, bounded wire contract for an isShelf-owned console Home tile job.
// This codec performs no filesystem, network, or PS5 shell operation.
#pragma once

#include "console_tile_intent.hpp"
#include "console_tile_job_pin.hpp"
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace shelf::tile_job {

inline constexpr std::string_view request_header = "format=isshelf-console-tile-job/1\n";
inline constexpr std::string_view receipt_header = "format=isshelf-console-tile-result/1\n";
inline constexpr char job_parent_root[] = "/data/console-shelf";
inline constexpr char job_directory_name[] = "tile-jobs-v1";
inline constexpr char job_root[] = "/data/console-shelf/tile-jobs-v1";
inline constexpr std::size_t maximum_request_bytes = 192;
inline constexpr std::size_t maximum_receipt_bytes = 224;

enum class Action { inspect, upsert, remove };
enum class Result { absent, owned_source, upserted_source, retired_source, held };

// An emulator menu is valid only for the requested console. The app checks
// its discovered, installed route; a helper must check its own pinned route
// source. A global title allowlist by itself is insufficient.
using EmulatorRouteCheck = bool (*)(const void*, std::size_t, std::string_view);

struct Request {
    char nonce[33]{};
    Action action = Action::inspect;
    ConsoleTileIntent intent;
};
struct Receipt {
    char nonce[33]{};
    Action action = Action::inspect;
    ConsoleTileIntent intent;
    Result result = Result::held;
    char title_id[10]{};
};

inline bool nonce_valid(const char (&nonce)[33]) {
    if (nonce[32] != 0) return false;
    for (std::size_t i = 0; i < 32; ++i)
        if (!((nonce[i] >= '0' && nonce[i] <= '9') ||
              (nonce[i] >= 'a' && nonce[i] <= 'f'))) return false;
    return true;
}
inline std::string_view action_name(Action action) {
    switch (action) {
    case Action::inspect: return "inspect";
    case Action::upsert: return "upsert";
    case Action::remove: return "remove";
    }
    return {};
}
inline std::string_view result_name(Result result) {
    switch (result) {
    case Result::absent: return "absent";
    case Result::owned_source: return "owned_source";
    case Result::upserted_source: return "upserted_source";
    case Result::retired_source: return "retired_source";
    case Result::held: return "held";
    }
    return {};
}
inline bool title_id_valid(const char (&title)[10]) {
    if (std::memcmp(title, "PPSA", 4) != 0 || title[9] != 0) return false;
    for (std::size_t i = 4; i < 9; ++i)
        if (title[i] < '0' || title[i] > '9') return false;
    return std::strcmp(title, shelf_title_id) != 0;
}
inline bool intent_valid(const ConsoleTileIntent& intent,
                         EmulatorRouteCheck route_check = nullptr,
                         const void* route_context = nullptr) {
    if (!console_tile_creation_intent_valid(intent)) return false;
    if (intent.destination != ConsoleTileDestination::emulator) return true;
    return route_check && route_check(route_context, intent.system, intent.emulator_title);
}
inline bool request_valid(const Request& request,
                          EmulatorRouteCheck route_check = nullptr, const void* route_context = nullptr) {
    if (!nonce_valid(request.nonce) || request.intent.system >= system_count ||
        action_name(request.action).empty()) return false;
    if (request.action != Action::upsert)
        return request.intent.destination == ConsoleTileDestination::library &&
               !request.intent.emulator_title[0];
    return intent_valid(request.intent, route_check, route_context);
}
inline bool take_field(std::string_view& input, std::string_view key, std::string_view& value) {
    if (input.size() <= key.size() || input.substr(0, key.size()) != key ||
        input[key.size()] != '=') return false;
    input.remove_prefix(key.size() + 1);
    const auto end = input.find('\n');
    if (end == std::string_view::npos) return false;
    value = input.substr(0, end);
    input.remove_prefix(end + 1);
    return true;
}
inline bool parse_action(std::string_view name, Action& action) {
    if (name == "inspect") action = Action::inspect;
    else if (name == "upsert") action = Action::upsert;
    else if (name == "remove") action = Action::remove;
    else return false;
    return true;
}
inline bool parse_result(std::string_view name, Result& result) {
    if (name == "absent") result = Result::absent;
    else if (name == "owned_source") result = Result::owned_source;
    else if (name == "upserted_source") result = Result::upserted_source;
    else if (name == "retired_source") result = Result::retired_source;
    else if (name == "held") result = Result::held;
    else return false;
    return true;
}
inline bool catalog_system(std::string_view id, std::size_t& system) {
    for (std::size_t i = 0; i < system_count; ++i)
        if (catalog[i].id == id) { system = i; return true; }
    return false;
}
inline bool encode_request(const Request& request, char* output, std::size_t capacity,
                           EmulatorRouteCheck route_check = nullptr, const void* route_context = nullptr) {
    if (!output || !capacity || !request_valid(request, route_check, route_context)) return false;
    const auto action = action_name(request.action);
    const auto system = catalog[request.intent.system].id;
    const auto destination = request.action == Action::upsert
        ? console_tile_destination_name(request.intent.destination) : std::string_view("-");
    const char* emulator = request.action == Action::upsert &&
                           request.intent.destination == ConsoleTileDestination::emulator
        ? request.intent.emulator_title : "-";
    const int written = std::snprintf(output, capacity,
        "format=isshelf-console-tile-job/1\nnonce=%s\naction=%.*s\n"
        "system=%.*s\ndestination=%.*s\nemulator=%s\n", request.nonce,
        static_cast<int>(action.size()), action.data(),
        static_cast<int>(system.size()), system.data(),
        static_cast<int>(destination.size()), destination.data(), emulator);
    return written > 0 && static_cast<std::size_t>(written) < capacity &&
           static_cast<std::size_t>(written) <= maximum_request_bytes;
}
inline bool parse_request(std::string_view bytes, Request& output,
                          EmulatorRouteCheck route_check = nullptr, const void* route_context = nullptr) {
    if (bytes.size() > maximum_request_bytes || bytes.size() < request_header.size() ||
        bytes.substr(0, request_header.size()) != request_header) return false;
    bytes.remove_prefix(request_header.size());
    std::string_view nonce, action, system, destination, emulator;
    if (!take_field(bytes, "nonce", nonce) || !take_field(bytes, "action", action) ||
        !take_field(bytes, "system", system) || !take_field(bytes, "destination", destination) ||
        !take_field(bytes, "emulator", emulator) || !bytes.empty() || nonce.size() != 32)
        return false;
    Request parsed;
    std::memcpy(parsed.nonce, nonce.data(), nonce.size());
    if (!parse_action(action, parsed.action)) return false;
    if (parsed.action == Action::upsert) {
        if (!console_tile_fields(system, destination, emulator, parsed.intent)) return false;
    } else {
        if (destination != "-" || emulator != "-" ||
            !catalog_system(system, parsed.intent.system)) return false;
    }
    if (!request_valid(parsed, route_check, route_context)) return false;
    output = parsed;
    return true;
}
inline bool receipt_valid(const Receipt& receipt) {
    if (!nonce_valid(receipt.nonce) || receipt.intent.system >= system_count ||
        action_name(receipt.action).empty() || result_name(receipt.result).empty()) return false;
    const bool has_title = receipt.title_id[0] != 0;
    if (has_title && (!title_id_valid(receipt.title_id) ||
                      !owned_title_matches(receipt.intent.system, receipt.title_id))) return false;
    if (receipt.result == Result::held)
        return !has_title && receipt.intent.destination == ConsoleTileDestination::library &&
               !receipt.intent.emulator_title[0];
    if (receipt.result == Result::absent)
        return receipt.action == Action::inspect && !has_title &&
               receipt.intent.destination == ConsoleTileDestination::library &&
               !receipt.intent.emulator_title[0];
    if (receipt.result == Result::retired_source)
        return receipt.action == Action::remove && has_title &&
               receipt.intent.destination == ConsoleTileDestination::library &&
               !receipt.intent.emulator_title[0];
    if (receipt.result == Result::owned_source)
        return receipt.action == Action::inspect && has_title &&
               console_tile_intent_valid(receipt.intent);
    return receipt.result == Result::upserted_source && receipt.action == Action::upsert &&
           has_title && console_tile_intent_valid(receipt.intent);
}
inline bool encode_receipt(const Receipt& receipt, char* output, std::size_t capacity) {
    if (!output || !capacity || !receipt_valid(receipt)) return false;
    const auto action = action_name(receipt.action);
    const auto system = catalog[receipt.intent.system].id;
    const auto result = result_name(receipt.result);
    const bool has_route = receipt.result == Result::owned_source ||
                           receipt.result == Result::upserted_source;
    const auto destination = has_route
        ? console_tile_destination_name(receipt.intent.destination) : std::string_view("-");
    const char* emulator = has_route && receipt.intent.destination == ConsoleTileDestination::emulator
        ? receipt.intent.emulator_title : "-";
    const char* title = receipt.title_id[0] ? receipt.title_id : "-";
    const int written = std::snprintf(output, capacity,
        "format=isshelf-console-tile-result/1\nnonce=%s\naction=%.*s\n"
        "system=%.*s\nresult=%.*s\ntitle=%s\ndestination=%.*s\nemulator=%s\n",
        receipt.nonce, static_cast<int>(action.size()), action.data(),
        static_cast<int>(system.size()), system.data(),
        static_cast<int>(result.size()), result.data(), title,
        static_cast<int>(destination.size()), destination.data(), emulator);
    return written > 0 && static_cast<std::size_t>(written) < capacity &&
           static_cast<std::size_t>(written) <= maximum_receipt_bytes;
}
inline bool parse_receipt(std::string_view bytes, Receipt& output) {
    if (bytes.size() > maximum_receipt_bytes || bytes.size() < receipt_header.size() ||
        bytes.substr(0, receipt_header.size()) != receipt_header) return false;
    bytes.remove_prefix(receipt_header.size());
    std::string_view nonce, action, system, result, title, destination, emulator;
    if (!take_field(bytes, "nonce", nonce) || !take_field(bytes, "action", action) ||
        !take_field(bytes, "system", system) || !take_field(bytes, "result", result) ||
        !take_field(bytes, "title", title) || !take_field(bytes, "destination", destination) ||
        !take_field(bytes, "emulator", emulator) || !bytes.empty() || nonce.size() != 32)
        return false;
    Receipt parsed;
    std::memcpy(parsed.nonce, nonce.data(), nonce.size());
    if (!parse_action(action, parsed.action) || !parse_result(result, parsed.result) ||
        !catalog_system(system, parsed.intent.system)) return false;
    if (title != "-") {
        if (title.size() != 9) return false;
        std::memcpy(parsed.title_id, title.data(), title.size());
    }
    const bool route_expected = parsed.result == Result::owned_source ||
                                parsed.result == Result::upserted_source;
    if (route_expected != (destination != "-") ||
        (!route_expected && emulator != "-")) return false;
    if (route_expected) {
        if (!console_tile_fields(system, destination, emulator, parsed.intent)) return false;
    }
    if (!receipt_valid(parsed)) return false;
    output = parsed;
    return true;
}
inline bool receipt_matches_request(const Receipt& receipt, const Request& request,
                                    EmulatorRouteCheck route_check = nullptr,
                                    const void* route_context = nullptr) {
    if (!receipt_valid(receipt) || !request_valid(request, route_check, route_context) ||
        std::strcmp(receipt.nonce, request.nonce) != 0 ||
        receipt.action != request.action || receipt.intent.system != request.intent.system)
        return false;
    if ((receipt.result == Result::owned_source || receipt.result == Result::upserted_source) &&
        receipt.intent.destination == ConsoleTileDestination::emulator &&
        (!route_check || !route_check(route_context, receipt.intent.system,
                                       receipt.intent.emulator_title)))
        return false;
    if (receipt.result == Result::held) return true;
    if (request.action == Action::inspect)
        return receipt.result == Result::absent || receipt.result == Result::owned_source;
    if (request.action == Action::remove)
        return receipt.result == Result::retired_source;
    return receipt.result == Result::upserted_source &&
           console_tile_intents_equal(receipt.intent, request.intent);
}

// A source receipt and API registration never constitute TV acceptance.
enum class Phase {
    none, queued, sent_unverified, source_observed, registered_unconfirmed,
    inspected_absent, tv_confirmed, removal_pending, removal_api_absent,
    removed_confirmed, held
};
enum class Observation {
    request_created, send_attempted, api_registered,
    api_absent, owner_tile_visible, owner_tile_absent, conflict
};
inline bool advance(Action action, Phase& phase, Observation observation) {
    if (observation == Observation::conflict && phase != Phase::tv_confirmed &&
        phase != Phase::removed_confirmed) { phase = Phase::held; return true; }
    Phase next = phase;
    if (phase == Phase::none && observation == Observation::request_created)
        next = Phase::queued;
    else if (phase == Phase::queued && observation == Observation::send_attempted)
        next = Phase::sent_unverified;
    else if (phase == Phase::source_observed && observation == Observation::api_registered)
        next = Phase::registered_unconfirmed;
    else if (action == Action::upsert && phase == Phase::registered_unconfirmed &&
             observation == Observation::owner_tile_visible)
        next = Phase::tv_confirmed;
    else if (action == Action::remove && phase == Phase::removal_pending &&
             observation == Observation::api_absent)
        next = Phase::removal_api_absent;
    else if (action == Action::remove && phase == Phase::removal_api_absent &&
             observation == Observation::owner_tile_absent)
        next = Phase::removed_confirmed;
    else return false;
    phase = next;
    return true;
}
inline bool observe_receipt(const Request& request, const Receipt& receipt, Phase& phase,
                           EmulatorRouteCheck route_check = nullptr,
                           const void* route_context = nullptr) {
    if (phase != Phase::sent_unverified) return false;
    if (!receipt_matches_request(receipt, request, route_check, route_context)) {
        phase = Phase::held;
        return false;
    }
    if (receipt.result == Result::held) phase = Phase::held;
    else if (receipt.result == Result::absent) phase = Phase::inspected_absent;
    else if (receipt.result == Result::retired_source) phase = Phase::removal_pending;
    else phase = Phase::source_observed;
    return true;
}
} // namespace shelf::tile_job
