// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/ui.hpp"
#include "shelf/console_tile_job_routes.hpp"
#include "shelf/console_tile_startup.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace shelf {
bool begin_console_tile_uninstall_handoff(UiState& state, const char* title_id) {
    if (!title_id || !std::memchr(title_id, 0, sizeof(state.pending_launch.title)) ||
        !launch_tile_title_valid(title_id))
        return false;
    LaunchRequest request;
    request.mode = LaunchMode::tile_uninstall;
    std::snprintf(request.title, sizeof(request.title), "%s", title_id);
    state.pending_launch = request;
    state.launch_requested = true;
    std::snprintf(state.notice, sizeof(state.notice),
                  "Closing isShelf to finish removing this Home Screen shortcut...");
    return true;
}
bool queue_console_tile_removal(UiState& state, const char* title_id) {
    if (!title_id || !std::memchr(title_id, 0, sizeof(state.pending_launch.title)) ||
        !launch_tile_title_valid(title_id))
        return false;
    const std::string_view incoming(title_id);
    std::string_view queued(state.tile_removal_titles);
    while (!queued.empty()) {
        const auto comma = queued.find(',');
        const auto length = comma == queued.npos ? queued.size() : comma;
        if (queued.substr(0, length) == incoming)
            return true;
        if (comma == queued.npos)
            break;
        queued.remove_prefix(comma + 1);
    }
    if (state.tile_removal_count >= system_count)
        return false;
    const auto used = std::strlen(state.tile_removal_titles);
    const auto separator = used ? 1u : 0u;
    if (used + separator + incoming.size() >= sizeof(state.tile_removal_titles))
        return false;
    if (separator)
        state.tile_removal_titles[used] = ',';
    std::memcpy(state.tile_removal_titles + used + separator,
                incoming.data(), incoming.size() + 1);
    ++state.tile_removal_count;
    return true;
}
bool queue_console_tile_removals(UiState& state, const char* title_ids,
                                 std::size_t capacity) {
    if (!title_ids || !capacity || capacity > sizeof(state.tile_removal_titles))
        return false;
    const auto* end = static_cast<const char*>(std::memchr(title_ids, 0, capacity));
    if (!end)
        return false;
    const std::string_view list(title_ids, static_cast<std::size_t>(end - title_ids));
    const auto comma = list.find(',');
    const auto first_size = comma == list.npos ? list.size() : comma;
    const std::string_view first(list.data(), first_size);
    const std::string_view remaining = comma == list.npos
        ? std::string_view{}
        : std::string_view(list.data() + comma + 1, list.size() - comma - 1);
    if (!launch_tile_refresh_titles_valid(first, remaining))
        return false;
    char previous[sizeof(state.tile_removal_titles)];
    std::memcpy(previous, state.tile_removal_titles, sizeof(previous));
    const auto previous_count = state.tile_removal_count;
    std::string_view titles = list;
    while (!titles.empty()) {
        const auto next = titles.find(',');
        const auto length = next == titles.npos ? titles.size() : next;
        char title[10]{};
        if (length >= sizeof(title))
            return false;
        std::memcpy(title, titles.data(), length);
        if (!queue_console_tile_removal(state, title)) {
            state.tile_removal_count = previous_count;
            std::memcpy(state.tile_removal_titles, previous, sizeof(previous));
            return false;
        }
        if (next == titles.npos)
            titles = {};
        else
            titles.remove_prefix(next + 1);
    }
    return true;
}
void clear_console_tile_removal_state(UiState& state) {
    state.tile_installed.fill(false);
    state.tile_removing.fill(false);
    state.tile_removal_handoff.fill(false);
    state.tile_removal_batch_ready = false;
    state.tile_residual_sweep = false;
    state.tile_removal_count = 0;
    std::memset(state.tile_removal_titles, 0,
                sizeof(state.tile_removal_titles));
    if (state.page == Page::console_tile)
        state.tile_focus = 0;
}
bool begin_console_tile_removal_handoff(UiState& state) {
    const std::string_view list(state.tile_removal_titles);
    const auto comma = list.find(',');
    const auto first_size = comma == list.npos ? list.size() : comma;
    const std::string_view first(list.data(), first_size);
    const std::string_view remaining = comma == list.npos
        ? std::string_view{}
        : std::string_view(list.data() + comma + 1, list.size() - comma - 1);
    std::size_t count = 0;
    const bool valid_titles = state.tile_residual_sweep
        ? launch_tile_residual_titles_valid(first, remaining, &count)
        : launch_tile_refresh_titles_valid(first, remaining, &count);
    if (!state.tile_removal_count ||
        !valid_titles ||
        count != state.tile_removal_count ||
        first.size() >= sizeof(state.pending_launch.title) ||
        remaining.size() >= sizeof(state.pending_launch.rom))
        return false;
    LaunchRequest request;
    request.mode = state.tile_residual_sweep
        ? LaunchMode::tile_residual : LaunchMode::tile_remove;
    std::memcpy(request.title, first.data(), first.size());
    request.title[first.size()] = 0;
    if (!remaining.empty()) {
        std::memcpy(request.rom, remaining.data(), remaining.size());
        request.rom[remaining.size()] = 0;
    }
    state.pending_launch = request;
    state.launch_requested = true;
    if (state.tile_residual_sweep)
        std::snprintf(state.notice, sizeof(state.notice),
                      "Closing isShelf so exact residual registrations can be checked and removed...");
    else if (count == 1)
        std::snprintf(state.notice, sizeof(state.notice),
                      "Closing isShelf so ShadowMount can remove this Home Screen tile...");
    else
        std::snprintf(state.notice, sizeof(state.notice),
                      "Closing isShelf so ShadowMount can remove %zu Home Screen tiles...",
                      count);
    return true;
}
bool begin_console_tile_refresh_handoff(UiState& state, const char* title_ids,
                                        std::size_t capacity) {
    if (!title_ids || !capacity || capacity > 521)
        return false;
    const auto* end = static_cast<const char*>(std::memchr(title_ids, 0, capacity));
    if (!end)
        return false;
    const std::string_view list(title_ids,
                                static_cast<std::size_t>(end - title_ids));
    const auto comma = list.find(',');
    const auto first_size = comma == list.npos ? list.size() : comma;
    const std::string_view first(list.data(), first_size);
    const std::string_view remaining = comma == list.npos
        ? std::string_view{}
        : std::string_view(list.data() + comma + 1, list.size() - comma - 1);
    if (!launch_tile_refresh_titles_valid(first, remaining) ||
        first.size() >= sizeof(state.pending_launch.title) ||
        remaining.size() >= sizeof(state.pending_launch.rom))
        return false;
    LaunchRequest request;
    request.mode = LaunchMode::tile_refresh;
    std::memcpy(request.title, first.data(), first.size());
    request.title[first.size()] = 0;
    if (!remaining.empty()) {
        std::memcpy(request.rom, remaining.data(), remaining.size());
        request.rom[remaining.size()] = 0;
    }
    state.pending_launch = request;
    state.launch_requested = true;
    std::snprintf(state.notice, sizeof(state.notice),
                  "Closing isShelf to refresh the installed Home Screen artwork...");
    return true;
}
namespace {
int clamp(int value, int maximum) {
    return std::max(0, std::min(value, maximum));
}
BrowseResult picker_matches(const UiState& state) {
    return browse(Preferences{}, Inventory{}, {state.console_query, {}});
}
void normalize_home(UiState& state) {
    const auto shown = shown_systems(state);
    if (!shown.count) {
        state.focus = 0;
        state.tools = true;
        state.tool = 2;
        return;
    }
    state.focus = clamp(state.focus, static_cast<int>(shown.count) - 1);
    if (state.page == Page::home || (state.page == Page::settings && state.return_page == Page::home) ||
        (state.page == Page::keyboard && state.keyboard_return == Page::home))
        state.selected_system = shown.indices[static_cast<std::size_t>(state.focus)];
}
void open_settings(UiState& state, SettingsTab tab) {
    if (state.page != Page::settings)
        state.return_page = state.page;
    state.page = Page::settings;
    state.tab = tab;
    state.settings_focus = 0;
}
void open_keyboard(UiState& state, bool consoles, bool tv = false) {
    state.keyboard_return = state.page;
    state.keyboard_for_consoles = consoles;
    state.keyboard_for_tv = tv;
    state.page = Page::keyboard;
    state.keyboard_focus = 0;
}
void changed(UiState& state) {
    state.changed = true;
    normalize_home(state);
}
bool picker_enabled(const Preferences& p, std::size_t id) {
    return p.shelf_single ? p.shelf_selected[id] : !p.hidden[id];
}
void picker_set(UiState& state, std::size_t id, bool enabled) {
    auto& p = state.settings.preferences;
    if (!p.shelf_single) {
        p.hidden[id] = !enabled;
        return;
    }
    if (enabled && !p.shelf_selected[id] &&
        static_cast<std::size_t>(std::count(p.shelf_selected.begin(), p.shelf_selected.end(), true)) >= shelf_capacity) {
        std::snprintf(state.notice, sizeof(state.notice), "All 8 spaces are filled. Remove a console before adding another.");
        return;
    }
    p.shelf_selected[id] = enabled;
}
void cycle(UiState& state, int direction) {
    auto& p = state.settings.preferences;
    if (state.tab == SettingsTab::appearance) {
        if (state.settings_focus == 0)
            p.layout = next_layout(p.layout, direction);
        if (state.settings_focus == 1 && p.layout == Layout::grid)
            p.grid_look = p.grid_look == GridLook::icons ? GridLook::cards : GridLook::icons;
        if (state.settings_focus == 2)
            p.icon = static_cast<AppIcon>((static_cast<int>(p.icon) + direction + static_cast<int>(AppIcon::count)) %
                                          static_cast<int>(AppIcon::count));
        if (state.settings_focus == 3)
            p.monochrome_missing = !p.monochrome_missing;
        if (state.settings_focus == 4)
            p.reduced_motion = !p.reduced_motion;
        changed(state);
    } else if (state.tab == SettingsTab::browse) {
        if (state.settings_focus == 0)
            p.sort = static_cast<Sort>((static_cast<int>(p.sort) + direction + 8) % 8);
        if (state.settings_focus == 1)
            p.brand =
                static_cast<std::size_t>((static_cast<int>(p.brand) + direction + static_cast<int>(brands.size())) %
                                         static_cast<int>(brands.size()));
        if (state.settings_focus == 2)
            p.filter = static_cast<SystemFilter>((static_cast<int>(p.filter) + direction + 5) % 5);
        if (state.settings_focus == 3)
            p.shelf_single = !p.shelf_single;
        changed(state);
    } else if (state.tab == SettingsTab::room) {
        if (state.settings_focus == 0)
            p.tv_mode = static_cast<TvMode>((static_cast<int>(p.tv_mode) + direction + 8) % 8);
        if (state.settings_focus == 1) { open_keyboard(state, false, true); return; }
        if (state.settings_focus == 2)
            p.tv_period = static_cast<std::size_t>((static_cast<int>(p.tv_period) + direction + 4) % 4);
        if (state.settings_focus == 3) p.intro_all = !p.intro_all;
        if (state.settings_focus == 4) p.tv_noise = static_cast<TvNoise>((static_cast<int>(p.tv_noise) + direction + 4) % 4);
        changed(state);
    } else {
        if (state.settings_focus == 0) {
            p.scope = static_cast<Scope>((static_cast<int>(p.scope) + direction + 3) % 3);
            changed(state);
            return;
        }
        if (state.settings_focus == 1) {
            open_keyboard(state, true);
            return;
        }
        const auto matches = picker_matches(state);
        if (state.settings_focus == 2 || state.settings_focus == 3) {
            for (std::size_t i = 0; i < matches.count; ++i)
                picker_set(state, matches.indices[i], state.settings_focus == 2);
        } else {
            const auto entries = console_picker(state);
            const auto at = static_cast<std::size_t>(state.settings_focus - 4);
            if (at >= entries.count)
                return;
            const auto entry = entries.entries[at];
            if (entry.group) {
                bool all = true;
                for (std::size_t i = 0; i < matches.count; ++i)
                    if (catalog[matches.indices[i]].brand == brands[entry.index] && !picker_enabled(p, matches.indices[i]))
                        all = false;
                for (std::size_t i = 0; i < matches.count; ++i)
                    if (catalog[matches.indices[i]].brand == brands[entry.index])
                        picker_set(state, matches.indices[i], !all);
            } else
                picker_set(state, entry.index, !picker_enabled(p, entry.index));
        }
        changed(state);
    }
}
void erase_query(UiState& state) {
    char* query = state.keyboard_for_tv ? state.settings.tv_text : state.keyboard_for_consoles ? state.console_query : state.query;
    auto size = std::strlen(query);
    if (size) {
        --size;
        while (size && (static_cast<unsigned char>(query[size]) & 0xc0) == 0x80)
            --size;
        query[size] = 0;
    }
    if (state.keyboard_for_tv) state.changed = true;
    else normalize_home(state);
}
} // namespace

BrowseResult shown_systems(const UiState& state) {
    auto shown = browse(state.settings.preferences, state.inventory, {state.query, {}});
    if (state.tile_home_override < system_count && shown.count < system_count &&
        std::find(shown.indices.begin(), shown.indices.begin() + shown.count, state.tile_home_override) ==
            shown.indices.begin() + shown.count) {
        for (std::size_t i = shown.count; i > 0; --i)
            shown.indices[i] = shown.indices[i - 1];
        shown.indices[0] = state.tile_home_override;
        ++shown.count;
    }
    return shown;
}
PickerList console_picker(const UiState& state) {
    PickerList list;
    const auto matches = picker_matches(state);
    for (std::size_t brand = 1; brand < brands.size(); ++brand) {
        bool added = false;
        for (std::size_t i = 0; i < matches.count; ++i) {
            const auto index = matches.indices[i];
            if (catalog[index].brand != brands[brand])
                continue;
            if (!added) {
                list.entries[list.count++] = {true, brand};
                added = true;
            }
            list.entries[list.count++] = {false, index};
        }
    }
    return list;
}

void text_input(UiState& state, const char* utf8) {
    if (state.page != Page::keyboard)
        return;
    char* query = state.keyboard_for_tv ? state.settings.tv_text : state.keyboard_for_consoles ? state.console_query : state.query;
    const auto size = std::strlen(query), extra = std::strlen(utf8);
    if (size + extra >= (state.keyboard_for_tv ? sizeof(state.settings.tv_text) : sizeof(state.query)))
        return;
    for (std::size_t i = 0; i < extra; ++i)
        if (static_cast<unsigned char>(utf8[i]) < 32)
            return;
    std::memcpy(query + size, utf8, extra + 1);
    if (state.keyboard_for_tv) state.changed = true;
    else normalize_home(state);
}

void input(UiState& state, Action action) {
    // Reserved for a future explicit pairing flow. Until it is wired, ordinary
    // navigation cannot route this sensitive view into Settings or search.
    if (state.page == Page::pairing_code)
        return;
    state.notice[0] = 0;
    state.close_hint_requested = false;
    if (state.page == Page::keyboard) {
        if (action == Action::back || action == Action::settings) {
            state.page = state.keyboard_return;
            return;
        }
        if (action == Action::up)
            state.keyboard_focus = clamp(state.keyboard_focus - 10, 39);
        if (action == Action::down)
            state.keyboard_focus = clamp(state.keyboard_focus + 10, 39);
        if (action == Action::left)
            state.keyboard_focus = clamp(state.keyboard_focus - 1, 39);
        if (action == Action::right)
            state.keyboard_focus = clamp(state.keyboard_focus + 1, 39);
        if (action == Action::erase)
            erase_query(state);
        if (action == Action::favorite) {
            (state.keyboard_for_tv ? state.settings.tv_text : state.keyboard_for_consoles ? state.console_query : state.query)[0] = 0;
            if (state.keyboard_for_tv) state.changed = true;
            else normalize_home(state);
        }
        if (action == Action::confirm) {
            const auto key = keyboard_keys[static_cast<std::size_t>(state.keyboard_focus)];
            if (key == '^')
                state.page = state.keyboard_return;
            else if (key == '<')
                erase_query(state);
            else if (key == '!') {
                (state.keyboard_for_tv ? state.settings.tv_text : state.keyboard_for_consoles ? state.console_query : state.query)[0] = 0;
                if (state.keyboard_for_tv) state.changed = true;
                else normalize_home(state);
            } else {
                const char text[] = {key == '_' ? ' ' : key, 0};
                text_input(state, text);
            }
        }
        return;
    }
    if (state.page == Page::shortcut_route) {
        if (state.route_save_busy)
            return;
        const bool isshelf_only = console_tile_isshelf_only(state.shortcut_route_system);
        if (isshelf_only && (action == Action::up || action == Action::left ||
                             action == Action::down || action == Action::right))
            state.shortcut_route_focus = 0;
        else if (action == Action::up || action == Action::left)
            state.shortcut_route_focus = clamp(state.shortcut_route_focus - 1, 2);
        else if (action == Action::down || action == Action::right)
            state.shortcut_route_focus = clamp(state.shortcut_route_focus + 1, 2);
        else if (action == Action::confirm) {
            ConsoleTileIntent selected;
            if (!console_tile_route_choice_intent(state, state.shortcut_route_focus, selected)) {
                std::snprintf(state.notice, sizeof(state.notice), "%s",
                              isshelf_only
                                  ? "PS5 and PS4 shortcuts open in isShelf after the library scan completes."
                              : !state.shortcut_route_ready && state.shortcut_route_focus != 2
                                  ? "Library scan incomplete. Choose Home or reopen with ftpsrv running."
                              : state.shortcut_route_focus == 1 ? "No supported emulator menu is installed for this console."
                                                                : "That shortcut destination is unavailable.");
                return;
            }
            state.pending_route_save = selected;
            state.route_save_requested = true;
            state.route_save_busy = true;
            state.route_save_apply_after = true;
            state.route_save_system = selected.system;
            state.route_save_status = RouteSaveStatus::pending;
            std::snprintf(state.notice, sizeof(state.notice),
                          "Saving this console's Home shortcut choice...");
        } else if (action == Action::back || action == Action::settings) {
            std::snprintf(state.notice, sizeof(state.notice),
                          "Choose where to open, or hold Circle to close isShelf.");
        }
        return;
    }
    if (action == Action::settings) {
        if (state.page == Page::settings)
            state.page = state.return_page;
        else
            open_settings(state, state.tab);
        return;
    }
    if (action == Action::layout && state.page == Page::home) {
        auto& layout = state.settings.preferences.layout;
        layout = next_layout(layout);
        changed(state);
        return;
    }
    if (action == Action::filters) {
        open_settings(state, SettingsTab::browse);
        return;
    }
    if (action == Action::consoles) {
        open_settings(state, SettingsTab::consoles);
        return;
    }
    if (state.page == Page::settings) {
        if (action == Action::back) {
            state.page = state.return_page;
            return;
        }
        if (action == Action::previous_tab || action == Action::next_tab) {
            state.tab =
                static_cast<SettingsTab>((static_cast<int>(state.tab) + (action == Action::next_tab ? 1 : -1) + 4) % 4);
            state.settings_focus = 0;
            return;
        }
        const int max_focus =
            state.tab == SettingsTab::consoles ? 3 + static_cast<int>(console_picker(state).count) :
            state.tab == SettingsTab::appearance || state.tab == SettingsTab::room ? 4 : 3;
        if (action == Action::up)
            state.settings_focus = clamp(state.settings_focus - 1, max_focus);
        if (action == Action::down)
            state.settings_focus = clamp(state.settings_focus + 1, max_focus);
        if (action == Action::left || action == Action::right || action == Action::confirm)
            cycle(state, action == Action::left ? -1 : 1);
        return;
    }
    if (state.page == Page::console_tile) {
        if (action == Action::back) {
            if (console_tile_writes_enabled)
                state.tile_create.cancel_selection(state.selected_system);
            state.page = Page::details;
            return;
        }
        const bool removing = console_tile_writes_enabled &&
            (state.tile_removing[state.selected_system] || state.tile_removal_batch_ready);
        const bool removal_handoff = state.tile_removal_batch_ready ||
                                     state.tile_removal_handoff[state.selected_system];
        const bool installed = console_tile_writes_enabled && state.tile_installed[state.selected_system];
        const bool emulator_close = console_tile_writes_enabled &&
                                    state.tile_emulator_batch_close_ready;
        const bool isshelf_only_picker = console_tile_isshelf_only(state.selected_system) &&
                                         !emulator_close && !removing && !removal_handoff;
        const int last = !console_tile_writes_enabled ? 2
                         : emulator_close || removal_handoff ? 0
                         : removing ? 2 : installed ? 6 : 5;
        if (isshelf_only_picker && (action == Action::up || action == Action::left))
            state.tile_focus = !console_tile_writes_enabled || state.tile_focus <= 3
                ? 0 : state.tile_focus - 1;
        else if (action == Action::up || action == Action::left)
            state.tile_focus = clamp(state.tile_focus - 1, last);
        if (isshelf_only_picker && (action == Action::down || action == Action::right))
            state.tile_focus = !console_tile_writes_enabled
                ? 0 : state.tile_focus < 3 ? 3 : clamp(state.tile_focus + 1, last);
        else if (action == Action::down || action == Action::right)
            state.tile_focus = clamp(state.tile_focus + 1, last);
        if (action != Action::confirm)
            return;
        if (isshelf_only_picker && (state.tile_focus == 1 || state.tile_focus == 2)) {
            state.tile_focus = 0;
            std::snprintf(state.notice, sizeof(state.notice),
                          "PS5 and PS4 shortcuts always open in isShelf.");
            return;
        }
        if (!console_tile_writes_enabled) {
            if (state.route_save_busy)
                return;
            ConsoleTileIntent choice;
            choice.system = state.selected_system;
            if (state.tile_focus == 0)
                choice.destination = ConsoleTileDestination::library;
            else if (state.tile_focus == 1) {
                choice.destination = ConsoleTileDestination::emulator;
                LaunchRequest launch;
                if (!state.launch_connected || !state.library ||
                    plan_emulator(*state.library, state.selected_system,
                                  state.settings.preferred_emulator[state.selected_system].data(), launch) ==
                        LaunchPlan::unavailable) {
                    std::snprintf(state.notice, sizeof(state.notice),
                                  "No supported emulator menu is installed for this console.");
                    return;
                }
                if (!console_tile_emulator_menu_selects_system(launch.title)) {
                    std::snprintf(state.notice, sizeof(state.notice),
                                  "RetroArch needs a game to select this console. Choose isShelf game library.");
                    return;
                }
                std::snprintf(choice.emulator_title, sizeof(choice.emulator_title), "%s", launch.title);
            } else
                choice.destination = ConsoleTileDestination::home;
            if (!tile_job::intent_valid(choice, tile_job::installed_emulator_route, state.library)) {
                std::snprintf(state.notice, sizeof(state.notice),
                              "The selected shortcut route no longer matches this console.");
                return;
            }
            state.pending_route_save = choice;
            state.route_save_requested = true;
            state.route_save_busy = true;
            state.route_save_apply_after = false;
            state.route_save_system = state.selected_system;
            state.route_save_status = RouteSaveStatus::pending;
            std::snprintf(state.notice, sizeof(state.notice),
                          "Saving this console's Home shortcut choice...");
            return;
        }
        if (state.tile_busy) {
            std::snprintf(state.notice, sizeof(state.notice), "%s",
                          state.tile_batch_busy
                              ? state.pending_tile.action == ConsoleTileAction::remove_installed
                                  ? "Queueing all isShelf Home Screen tiles for removal..."
                              : state.pending_tile.action == ConsoleTileAction::upsert_emulators
                                  ? "Adding or repairing all supported emulator tiles..."
                                  : "Updating all installed PS5 Home Screen shortcuts..."
                              : removing
                              ? removal_handoff
                                  ? "Preparing one post-close ShadowMount update..."
                                  : "Checking PS5 registration before finishing removal..."
                              : "Checking this PS5 Home Screen shortcut...");
            return;
        }
        if (emulator_close) {
            state.exit_requested = true;
            std::snprintf(state.notice, sizeof(state.notice),
                          "Closing isShelf so ShadowMount can update PS5 Home...");
            return;
        }
        if (!state.tile_connected) {
            std::snprintf(state.notice, sizeof(state.notice), "PS5 Home Screen shortcut installation is unavailable in this build.");
            return;
        }
        ConsoleTileRequest request;
        request.intent.system = state.selected_system;
        if (removing && state.tile_focus == 2) {
            if (!state.launch_connected || !state.library ||
                !state.library->scan.complete) {
                std::snprintf(state.notice, sizeof(state.notice),
                              "Finish the emulator scan before adding or repairing all emulator tiles.");
                return;
            }
            request.action = ConsoleTileAction::upsert_emulators;
        } else if (removing && state.tile_focus == 1) {
            request.action = ConsoleTileAction::remove_installed;
        } else if (removing) {
            if (removal_handoff) {
                if (!begin_console_tile_removal_handoff(state))
                    std::snprintf(state.notice, sizeof(state.notice),
                                  "Queued removals could not be prepared. Reopen isShelf and try again.");
                return;
            }
            request.action = ConsoleTileAction::confirm_removed;
            state.pending_tile = request;
            state.tile_requested = true;
            std::snprintf(state.notice, sizeof(state.notice),
                          "Checking PS5 registration before finishing removal...");
            return;
        } else {
            const int emulator_all_focus = installed ? 4 : 3;
            const int refresh_focus = installed ? 5 : 4;
            const int remove_all_focus = installed ? 6 : 5;
            if (state.tile_focus == remove_all_focus) {
                request.action = ConsoleTileAction::remove_installed;
            } else if (state.tile_focus == refresh_focus) {
                request.action = ConsoleTileAction::refresh_installed;
            } else if (state.tile_focus == emulator_all_focus) {
                if (!state.launch_connected || !state.library ||
                    !state.library->scan.complete) {
                    std::snprintf(state.notice, sizeof(state.notice),
                                  "Finish the emulator scan before adding or repairing all emulator tiles.");
                    return;
                }
                request.action = ConsoleTileAction::upsert_emulators;
            } else if (state.tile_focus == 3) {
                if (!installed)
                    return;
                request.action = ConsoleTileAction::remove;
            } else if (state.tile_focus == 0) {
                request.intent.destination = ConsoleTileDestination::library;
            } else if (state.tile_focus == 1) {
                request.intent.destination = ConsoleTileDestination::emulator;
                LaunchRequest launch;
                if (!state.launch_connected || !state.library ||
                    plan_emulator(*state.library, state.selected_system,
                                  state.settings.preferred_emulator[state.selected_system].data(), launch) ==
                        LaunchPlan::unavailable) {
                    std::snprintf(state.notice, sizeof(state.notice), "No supported emulator menu is installed for this console.");
                    return;
                }
                if (!console_tile_emulator_menu_selects_system(launch.title)) {
                    std::snprintf(state.notice, sizeof(state.notice),
                                  "RetroArch needs a game to select this console. Choose isShelf game library.");
                    return;
                }
                std::snprintf(request.intent.emulator_title, sizeof(request.intent.emulator_title), "%s", launch.title);
            } else {
                request.intent.destination = ConsoleTileDestination::home;
            }
        }
        if (request.action != ConsoleTileAction::upsert_emulators &&
            request.action != ConsoleTileAction::refresh_installed &&
            request.action != ConsoleTileAction::remove_installed &&
            !tile_job::intent_valid(request.intent, tile_job::installed_emulator_route,
                                    state.library)) {
            std::snprintf(state.notice, sizeof(state.notice),
                          "The selected shortcut route no longer matches this console.");
            return;
        }
        state.pending_tile = request;
        state.tile_requested = true;
        if (request.action == ConsoleTileAction::upsert_emulators) {
            state.tile_emulator_batch_close_ready = false;
            state.tile_emulator_batch_created = 0;
            state.tile_emulator_batch_updated = 0;
        }
        std::snprintf(state.notice, sizeof(state.notice), "%s",
                      request.action == ConsoleTileAction::upsert_emulators
                          ? "Adding or repairing every supported emulator tile..."
                      : request.action == ConsoleTileAction::refresh_installed
                          ? "Updating all installed PS5 Home Screen shortcuts..."
                      : request.action == ConsoleTileAction::remove_installed
                          ? "Queueing every isShelf Home Screen tile for removal..."
                      : request.action == ConsoleTileAction::remove
                          ? "Queueing this Home Screen tile for removal..."
                          : "Preparing PS5 Home Screen shortcut...");
        return;
    }
    if (state.page == Page::details) {
        if (action == Action::back) {
            state.page = Page::home;
            return;
        }
        if (state.tools) {
            if (action == Action::down) {
                state.tools = false;
                state.detail_focus = 0;
            }
            if (action == Action::confirm)
                input(state, Action::settings);
            return;
        }
        if (action == Action::up && state.detail_focus == 0) {
            state.tools = true;
            state.tool = 2;
            return;
        }
        if (state.detail_focus == 0 && state.library &&
            (action == Action::left || action == Action::right || action == Action::confirm)) {
            const auto* selected = preferred_route(*state.library, state.selected_system,
                                                   state.settings.preferred_emulator[state.selected_system].data());
            std::array<std::size_t, max_routes> routes{};
            int count = 0, current = 0;
            for (std::size_t i = 0; i < state.library->route_count; ++i)
                if (state.library->routes[i].system == state.selected_system) {
                    if (&state.library->routes[i] == selected)
                        current = count;
                    routes[static_cast<std::size_t>(count++)] = i;
                }
            if (count) {
                current = (current + count + (action == Action::left ? -1 : 1)) % count;
                std::snprintf(state.settings.preferred_emulator[state.selected_system].data(), 48, "%s",
                              state.library->routes[routes[static_cast<std::size_t>(current)]].id);
                state.changed = true;
            }
            return;
        }
        // The two launch actions share a row. Vertical input follows the
        // displayed rows; horizontal input moves between those two actions.
        if (action == Action::up) {
            if (state.detail_focus == 3) state.detail_focus = 1;
            else if (state.detail_focus == 1 || state.detail_focus == 2) state.detail_focus = 0;
        } else if (action == Action::down) {
            if (state.detail_focus == 0) state.detail_focus = 1;
            else if (state.detail_focus == 1 || state.detail_focus == 2) state.detail_focus = 3;
        } else if (action == Action::left && state.detail_focus == 2)
            state.detail_focus = 1;
        else if (action == Action::right && state.detail_focus == 1)
            state.detail_focus = 2;
        if (action == Action::favorite) {
            state.settings.preferences.favorite[state.selected_system] =
                !state.settings.preferences.favorite[state.selected_system];
            state.changed = true;
        }
        if (action == Action::confirm) {
            if (state.detail_focus == 3) {
                state.page = Page::console_tile;
                LaunchRequest emulator;
                state.tile_focus = !console_tile_isshelf_only(state.selected_system) &&
                    state.launch_connected && state.library &&
                    plan_emulator(*state.library, state.selected_system,
                                  state.settings.preferred_emulator[state.selected_system].data(),
                                  emulator) != LaunchPlan::unavailable ? 1 : 0;
                state.route_save_status = RouteSaveStatus::none;
                state.notice[0] = '\0';
            } else if (state.detail_focus == 2) {
                state.page = Page::library;
                state.game_focus = 0;
            } else if (state.detail_focus == 1 && state.launch_connected && state.library &&
                       plan_emulator(*state.library, state.selected_system,
                                     state.settings.preferred_emulator[state.selected_system].data(),
                                     state.pending_launch) != LaunchPlan::unavailable) {
                state.launch_requested = true;
                std::snprintf(state.notice, sizeof(state.notice), "Preparing the emulator...");
            } else
                std::snprintf(state.notice, sizeof(state.notice), "%s",
                              state.inventory[state.selected_system].emulator_count
                                  ? "Launch adapter is not connected in this development build."
                                  : "No compatible emulator has been detected yet.");
        }
        return;
    }
    if (state.page == Page::library) {
        if (action == Action::back)
            state.page = Page::details;
        if (state.tools) {
            if (action == Action::down) {
                state.tools = false;
                state.game_focus = 0;
            }
            if (action == Action::confirm)
                input(state, Action::settings);
            return;
        }
        if (action == Action::up && state.game_focus == 0) {
            state.tools = true;
            state.tool = 2;
            return;
        }
        if (state.library) {
            const auto games = games_for_system(*state.library, state.selected_system);
            const int maximum = games.count ? static_cast<int>(games.count) - 1 : 0;
            if (action == Action::up)
                state.game_focus = clamp(state.game_focus - 1, maximum);
            if (action == Action::down)
                state.game_focus = clamp(state.game_focus + 1, maximum);
            if (action == Action::left)
                state.game_focus = clamp(state.game_focus - 8, maximum);
            if (action == Action::right)
                state.game_focus = clamp(state.game_focus + 8, maximum);
            if (action == Action::favorite && games.count) {
                const auto key = game_favorite_key(state.library->games[games.indices[static_cast<std::size_t>(state.game_focus)]]);
                auto& favorites = state.settings.game_favorites;
                auto slot = std::find(favorites.begin(), favorites.end(), key);
                if (slot != favorites.end()) *slot = 0;
                else {
                    slot = std::find(favorites.begin(), favorites.end(), 0);
                    if (slot != favorites.end()) *slot = key;
                    else std::snprintf(state.notice, sizeof(state.notice), "Your 64 favourite game spaces are full.");
                }
                state.changed = true;
            }
            if (action == Action::confirm && games.count) {
                const auto plan =
                    state.launch_connected
                        ? plan_game(*state.library, games.indices[static_cast<std::size_t>(state.game_focus)],
                                    state.settings.preferred_emulator[state.selected_system].data(),
                                    state.pending_launch)
                        : LaunchPlan::unavailable;
                if (plan != LaunchPlan::unavailable) {
                    state.launch_requested = true;
                    std::snprintf(state.notice, sizeof(state.notice), "%s",
                                  plan == LaunchPlan::menu
                                      ? "This emulator opens its own library. Choose the game there."
                                      : "Preparing your game...");
                } else
                    std::snprintf(state.notice, sizeof(state.notice),
                                  "A direct launch route is not available for this game yet.");
            }
        }
        return;
    }
    if (action == Action::back) {
        state.close_hint_requested = true;
        return;
    }
    if (action == Action::search) {
        open_keyboard(state, false);
        return;
    }
    auto shown = shown_systems(state);
    if (state.settings.preferences.layout == Layout::shelf && shown.count &&
        (action == Action::previous_tab || action == Action::next_tab)) {
        const int size = static_cast<int>(shelf_capacity);
        const int last_page = (static_cast<int>(shown.count) - 1) / size;
        const int page = state.focus / size;
        const int next = clamp(page + (action == Action::next_tab ? 1 : -1), last_page);
        if (next != page) {
            state.focus = std::min(next * size + state.focus % size, static_cast<int>(shown.count) - 1);
            state.tools = false;
            normalize_home(state);
        }
        return;
    }
    if (state.tools || !shown.count) {
        state.tools = true;
        if (action == Action::up)
            state.tool = 2;
        if (action == Action::left && state.tool == 1)
            state.tool = 0;
        if (action == Action::right && state.tool == 0)
            state.tool = 1;
        if (action == Action::down && state.tool == 2)
            state.tool = 1;
        else if (action == Action::down && shown.count) {
            state.tools = false;
            normalize_home(state);
        }
        if (action == Action::confirm) {
            if (state.tool == 0)
                open_keyboard(state, false);
            else if (state.tool == 1)
                input(state, Action::layout);
            else
                open_settings(state, !shown.count ? SettingsTab::consoles : state.tab);
        }
        return;
    }
    const int stride = state.settings.preferences.layout == Layout::swipe ? 1 : 4;
    const bool swipe = state.settings.preferences.layout == Layout::swipe;
    const bool room = state.settings.preferences.layout == Layout::shelf;
    const int page_start = room ? state.focus / 8 * 8 : 0;
    const int local_focus = state.focus - page_start;
    if (action == Action::up && (swipe || local_focus < stride)) {
        state.tools = true;
        state.tool = swipe || local_focus < 3 ? 0 : 1;
    } else if (action == Action::up)
        state.focus -= stride;
    if (action == Action::down && !swipe)
        state.focus = room && local_focus >= stride ? state.focus :
                      clamp(state.focus + stride, std::min(room ? page_start + 7 : static_cast<int>(shown.count) - 1,
                                                          static_cast<int>(shown.count) - 1));
    if (action == Action::left)
        state.focus = swipe ? (state.focus + static_cast<int>(shown.count) - 1) % static_cast<int>(shown.count)
                            : std::max(state.focus - state.focus % stride, state.focus - 1);
    if (action == Action::right)
        state.focus = swipe ? (state.focus + 1) % static_cast<int>(shown.count)
                            : std::min({state.focus + 1, state.focus - state.focus % stride + stride - 1,
                                        static_cast<int>(shown.count) - 1});
    normalize_home(state);
    if (action == Action::confirm) {
        state.page = Page::details;
        state.detail_focus = state.inventory[state.selected_system].built_in ? 2 : 1;
    }
    if (action == Action::favorite) {
        const auto selected = state.selected_system;
        state.settings.preferences.favorite[selected] = !state.settings.preferences.favorite[selected];
        state.changed = true;
        shown = shown_systems(state);
        for (std::size_t i = 0; i < shown.count; ++i)
            if (shown.indices[i] == selected)
                state.focus = static_cast<int>(i);
        normalize_home(state);
    }
}
} // namespace shelf
