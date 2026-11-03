// SPDX-License-Identifier: GPL-3.0-only
// Consume the exact reviewed offline tile/menu catalog used by the helper.
#pragma once

#include "catalog.generated.hpp"
#include "../../../tools/console_badges/generic_tile_catalog.generated.hpp"
#include <cstddef>
#include <cstring>
#include <string_view>

namespace shelf::tile_job {
inline bool catalog_pin_matches(std::size_t system) {
    return system < system_count && system < isshelf_badge::generic_tile::tile_count &&
           catalog[system].id == isshelf_badge::generic_tile::tiles[system].system;
}
inline const char* owned_title_for(std::size_t system) {
    return catalog_pin_matches(system) ? isshelf_badge::generic_tile::tiles[system].title_id : nullptr;
}
inline bool owned_title_matches(std::size_t system, const char* title) {
    const char* expected = owned_title_for(system);
    return expected && title && std::strcmp(expected, title) == 0;
}
inline bool owned_title_known(std::string_view title) {
    for (std::size_t system = 0; system < system_count; ++system)
        if (catalog_pin_matches(system) && title == isshelf_badge::generic_tile::tiles[system].title_id)
            return true;
    return false;
}
inline bool pinned_emulator_menu(std::size_t system, std::string_view title) {
    return catalog_pin_matches(system) &&
           isshelf_badge::generic_tile::menu_pinned(system, title);
}
} // namespace shelf::tile_job
