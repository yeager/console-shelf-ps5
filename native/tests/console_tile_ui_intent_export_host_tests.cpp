// SPDX-License-Identifier: GPL-3.0-only
// Host-only first half of the opt-in destination seam test. No service calls.
#include "shelf/ui.hpp"
#include "shelf/console_tile_write_gate.hpp"
#include "../../tools/console_badges/generic_tile_pack_native.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>

static_assert(shelf::console_tile_writes_enabled,
              "The host picker fixture needs the open host-only UI gate");
static_assert(shelf::system_count == 52);

namespace {

bool existing_pilot(std::size_t system) {
    return system == 2 || system == 3 || system == 4 || system == 11;
}

const char* one_direct_emulator(std::size_t system) {
    for (const auto& menu : isshelf_badge::generic_tile::menus)
        if (menu.system == system &&
            shelf::console_tile_emulator_menu_selects_system(menu.title_id))
            return menu.title_id;
    return nullptr;
}

bool has_shared_retroarch(std::size_t system) {
    for (const auto& menu : isshelf_badge::generic_tile::menus)
        if (menu.system == system &&
            !shelf::console_tile_emulator_menu_selects_system(menu.title_id))
            return true;
    return false;
}

void emit(std::size_t system, int focus, shelf::ConsoleTileDestination destination,
          const char* emulator_title, const shelf::Library* installed) {
    shelf::UiState ui{};
    ui.page = shelf::Page::console_tile;
    ui.selected_system = system;
    ui.tile_focus = focus;
    ui.tile_connected = true;
    ui.launch_connected = installed != nullptr;
    ui.library = installed;
    shelf::input(ui, shelf::Action::confirm);
    assert(ui.tile_requested &&
           ui.pending_tile.action == shelf::ConsoleTileAction::upsert &&
           ui.pending_tile.intent.system == system &&
           ui.pending_tile.intent.destination == destination &&
           std::strcmp(ui.pending_tile.intent.emulator_title,
                       emulator_title ? emulator_title : "") == 0);
    const auto name = shelf::console_tile_destination_name(destination);
    std::printf("%zu\t%.*s\t%s\n", system,
                static_cast<int>(name.size()), name.data(),
                emulator_title ? emulator_title : "-");
}

} // namespace

int main() {
    std::size_t new_titles = 0;
    std::size_t emulator_choices = 0;
    static shelf::Library installed{};
    for (std::size_t system = 0; system < shelf::system_count; ++system) {
        if (existing_pilot(system)) continue; // Four live pilots are separate.
        ++new_titles;
        emit(system, 0, shelf::ConsoleTileDestination::library, nullptr, nullptr);
        if (shelf::console_tile_isshelf_only(system)) {
            for (int forbidden : {1, 2}) {
                shelf::UiState ui{};
                ui.page = shelf::Page::console_tile;
                ui.selected_system = system;
                ui.tile_focus = forbidden;
                ui.tile_connected = true;
                shelf::input(ui, shelf::Action::confirm);
                assert(!ui.tile_requested && ui.tile_focus == 0);
            }
            continue;
        } else {
            emit(system, 2, shelf::ConsoleTileDestination::home, nullptr, nullptr);
        }
        const char* title = one_direct_emulator(system);
        if (!title) {
            shelf::UiState ui{};
            ui.page = shelf::Page::console_tile;
            ui.selected_system = system;
            ui.tile_focus = 1;
            ui.tile_connected = true;
            if (has_shared_retroarch(system)) {
                installed.route_count = 1;
                installed.routes[0] = {};
                installed.routes[0].system = system;
                std::snprintf(installed.routes[0].id,
                              sizeof(installed.routes[0].id), "shared-retroarch");
                std::snprintf(installed.routes[0].title_id,
                              sizeof(installed.routes[0].title_id), "PPSA99169");
                std::snprintf(installed.routes[0].folder,
                              sizeof(installed.routes[0].folder),
                              "/data/homebrew/PPSA99169");
                ui.launch_connected = true;
                ui.library = &installed;
            }
            shelf::input(ui, shelf::Action::confirm);
            assert(!ui.tile_requested &&
                   std::strstr(ui.notice, has_shared_retroarch(system)
                      ? "RetroArch needs a game" : "No supported emulator") != nullptr);
            continue;
        }
        installed.route_count = 1;
        installed.routes[0] = {};
        installed.routes[0].system = system;
        std::snprintf(installed.routes[0].id, sizeof(installed.routes[0].id),
                      "available-route");
        std::snprintf(installed.routes[0].title_id,
                      sizeof(installed.routes[0].title_id), "%s", title);
        std::snprintf(installed.routes[0].folder,
                      sizeof(installed.routes[0].folder),
                      "/data/homebrew/%s", title);
        emit(system, 1, shelf::ConsoleTileDestination::emulator, title, &installed);
        ++emulator_choices;
    }
    assert(new_titles == 48 && emulator_choices == 9);
    std::fprintf(stderr, "UI fixture: %zu new titles, %zu emulator choices\n",
                 new_titles, emulator_choices);
}
