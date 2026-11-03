// SPDX-License-Identifier: GPL-3.0-only
// Host-only composition of the picker state and reviewed source writer.
#pragma once

#ifdef SHELF_PS5
#error "No live PS5 CREATE binding or registration worker is qualified"
#endif

#include "generic_app_tile_source.hpp"
#include "generic_tile_create_state.hpp"

namespace shelf::generic_tile_create {

struct HostSourceRun {
    Begin begin = Begin::no_route;
    generic_app_tile_source::Result writer;
};

inline HostSourceRun run_host_source_once(CatalogState& state, std::size_t system,
                                          const char (&nonce)[33],
                                          const Admission& evidence,
                                          const char* root, const char* pack_path,
                                          tile_job::EmulatorRouteCheck installed_menu = nullptr,
                                          const void* library = nullptr) {
    HostSourceRun result;
    result.begin = state.begin_source(system, nonce, evidence, installed_menu, library);
    if (result.begin != Begin::started) return result;
    const auto* selected = state.get(system);
    if (!selected) return result; // Bounded system was already checked.
    result.writer = generic_app_tile_source::run_once(root, pack_path, nonce,
                                                       selected->route);
    state.finish_source(system, result.writer.complete,
                        result.writer.receipt_bytes, result.writer.receipt_fnv);
    return result;
}

} // namespace shelf::generic_tile_create
