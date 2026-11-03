// SPDX-License-Identifier: GPL-3.0-only
// App-side proof that a selected emulator menu belongs to this console.
#pragma once

#include "console_tile_job.hpp"
#include "launch_plan.hpp"

namespace shelf::tile_job {
inline bool installed_emulator_route(const void* context, std::size_t system,
                                     std::string_view title) {
    const auto* library = static_cast<const Library*>(context);
    if (!library || system >= system_count || library->route_count > max_routes ||
        !launch_menu_known(title) || !pinned_emulator_menu(system, title) ||
        !console_tile_emulator_menu_selects_system(title)) return false;
    for (std::size_t i = 0; i < library->route_count; ++i) {
        const auto& route = library->routes[i];
        if (route.system == system && launch_bounded(route.title_id) &&
            title == route.title_id && launch_route_folder(route))
            return true;
    }
    return false;
}

// Completed historical jobs retain the route checked when created. An
// emulator may be uninstalled later; that cannot invalidate its saved source
// receipt or block a different console's first request. New requests still
// use installed_emulator_route above.
inline bool pinned_historical_emulator_route(const void*, std::size_t system,
                                             std::string_view title) {
    return system < system_count && launch_menu_known(title) &&
           pinned_emulator_menu(system, title) &&
           console_tile_emulator_menu_selects_system(title);
}
} // namespace shelf::tile_job
