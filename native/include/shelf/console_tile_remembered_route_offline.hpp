// SPDX-License-Identifier: GPL-3.0-only
// Offline candidate: remembered Home shortcut routes are untrusted UI data.
// No production caller, filesystem writer, title operation, or PS5 gate.
#pragma once
#include "console_tile_settings_route_codec.hpp"
#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace shelf::tile_route::remembered_offline {
using Table = std::array<settings_codec::Slot, system_count>;
using DurableWrite = bool (*)(void*, std::string_view);
using Readback = bool (*)(void*, std::string&);
using Apply = bool (*)(void*, const ConsoleTileIntent&);

enum class Result { invalid, stale_emulator, encode_failed, write_failed,
                    readback_uncertain, saved_apply_failed, saved_and_applied };

// `base` must be canonical schema-1 settings output with no route lines.
// This never rewrites arbitrary existing text or treats a route as authority.
inline bool append_routes(std::string_view base, const Table& routes,
                          std::string& output) {
    const auto before = settings_codec::scan_routes(base);
    if (!before.file_valid || base.empty() || base.back() != '\n') return false;
    for (const auto& slot : before.routes)
        if (slot.input != Input::absent) return false;
    std::string candidate(base);
    for (std::size_t i = 0; i < system_count; ++i) {
        const auto& slot = routes[i];
        if (slot.input == Input::absent) continue;
        if (slot.input != Input::valid || slot.preference.system != i) return false;
        char line[settings_codec::maximum_settings_line + 1]{};
        if (!settings_codec::encode_line(slot.preference, line, sizeof(line))) return false;
        candidate += line;
        if (candidate.size() > settings_codec::maximum_settings_bytes) return false;
    }
    const auto check = settings_codec::scan_routes(candidate);
    if (!check.file_valid) return false;
    for (std::size_t i = 0; i < system_count; ++i) {
        if (check.routes[i].input != routes[i].input) return false;
        if (routes[i].input == Input::valid) {
            const auto& expected = routes[i].preference;
            const auto& actual = check.routes[i].preference;
            if (actual.system != expected.system || actual.destination != expected.destination ||
                std::strcmp(actual.emulator_title, expected.emulator_title) != 0) return false;
        }
    }
    output = std::move(candidate);
    return true;
}

// A chooser selection is saved and read back before Apply may launch an
// emulator or leave isShelf. `write` must durably save the complete settings
// image; this pure candidate does not provide or authorize that writer.
inline Result remember_then_apply(std::string_view canonical_base,
                                  const Table& current,
                                  const ConsoleTileIntent& selected,
                                  InstalledMenu installed, void* installed_context,
                                  DurableWrite write, Readback readback,
                                  Apply apply, void* io_context) {
    if (!console_tile_intent_valid(selected) || !write || !readback || !apply)
        return Result::invalid;
    Preference preference;
    preference.system = selected.system;
    preference.destination = selected.destination;
    std::memcpy(preference.emulator_title, selected.emulator_title,
                sizeof(preference.emulator_title));
    const auto decision = tile_route::resolve(Source{selected.system}, Input::valid,
                                              preference, installed, installed_context);
    if (decision.resolution == Resolution::stale_emulator) return Result::stale_emulator;
    if (decision.resolution != Resolution::ready) return Result::invalid;
    Table next = current;
    next[selected.system] = {Input::valid, preference};
    std::string candidate;
    if (!append_routes(canonical_base, next, candidate)) return Result::encode_failed;
    if (!write(io_context, candidate)) return Result::write_failed;
    std::string observed;
    if (!readback(io_context, observed) || observed != candidate)
        return Result::readback_uncertain;
    const auto checked = settings_codec::scan_routes(observed);
    if (!checked.file_valid || checked.routes[selected.system].input != Input::valid)
        return Result::readback_uncertain;
    const auto again = tile_route::resolve(Source{selected.system}, Input::valid,
                                           checked.routes[selected.system].preference,
                                           installed, installed_context);
    if (again.resolution != Resolution::ready) return Result::stale_emulator;
    return apply(io_context, again.intent) ? Result::saved_and_applied
                                           : Result::saved_apply_failed;
}
} // namespace shelf::tile_route::remembered_offline
