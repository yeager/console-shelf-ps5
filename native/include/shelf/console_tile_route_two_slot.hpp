// SPDX-License-Identifier: GPL-3.0-only
// Offline-only fixed-size two-slot route record codec. No filesystem I/O.
#pragma once
#include "console_tile_settings_route_codec.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

namespace shelf::tile_route::two_slot {
inline constexpr std::size_t slot_bytes = 8192;
using Bytes = std::array<char, slot_bytes>;
using Routes = std::array<settings_codec::Slot, system_count>;

struct Record {
    bool valid = false;
    std::uint64_t generation = 0;
    Routes routes{};
};
enum class SlotState { blank, valid, damaged };
struct InspectedSlot {
    SlotState state = SlotState::damaged;
    Record record{};
};
enum class Winner { none, first, second, ambiguous };
struct Selection {
    Winner winner = Winner::none;
    Record record{};
    SlotState first_state = SlotState::damaged;
    SlotState second_state = SlotState::damaged;
};

namespace detail {
inline std::uint64_t checksum(const char* data, std::size_t size) {
    // Detect torn/partial writes; this is deliberately not authentication.
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}
inline bool append(Bytes& output, std::size_t& used, std::string_view item) {
    if (item.size() >= output.size() - used) return false;
    std::memcpy(output.data() + used, item.data(), item.size());
    used += item.size();
    return true;
}
inline bool take_line(std::string_view& input, std::string_view& line) {
    const auto end = input.find('\n');
    if (end == input.npos) return false;
    line = input.substr(0, end);
    input.remove_prefix(end + 1);
    return true;
}
inline bool parse_hex16(std::string_view input, std::uint64_t& output) {
    if (input.size() != 16) return false;
    std::uint64_t value = 0;
    for (const char c : input) {
        const unsigned digit = c >= '0' && c <= '9' ? static_cast<unsigned>(c - '0') :
                               c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10) : 16u;
        if (digit == 16u) return false;
        value = (value << 4) | digit;
    }
    output = value;
    return true;
}
} // namespace detail

inline bool encode(std::uint64_t generation, const Routes& routes, Bytes& output) {
    if (!generation) return false;
    Bytes candidate{};
    std::size_t used = 0;
    if (!detail::append(candidate, used, "ISSHELF-ROUTE-SLOT-1\n")) return false;
    char line[160]{};
    int n = std::snprintf(line, sizeof(line), "generation=%016llx\n",
                          static_cast<unsigned long long>(generation));
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(line) ||
        !detail::append(candidate, used, std::string_view(line, static_cast<std::size_t>(n))) ||
        !detail::append(candidate, used, "systems=52\n")) return false;
    static_assert(system_count == 52, "Slot record binds the reviewed catalog size");
    for (std::size_t i = 0; i < system_count; ++i) {
        const auto& slot = routes[i];
        if (slot.input == Input::absent) {
            const auto id = catalog[i].id;
            n = std::snprintf(line, sizeof(line), "route.%.*s=-\n",
                              static_cast<int>(id.size()), id.data());
            if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(line)) return false;
        } else if (slot.input == Input::valid && slot.preference.system == i &&
                   settings_codec::encode_line(slot.preference, line, sizeof(line))) {
            n = static_cast<int>(std::strlen(line));
        } else return false;
        if (!detail::append(candidate, used, std::string_view(line, static_cast<std::size_t>(n))))
            return false;
    }
    const auto hash = detail::checksum(candidate.data(), used);
    n = std::snprintf(line, sizeof(line), "checksum=%016llx\n",
                      static_cast<unsigned long long>(hash));
    if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(line) ||
        !detail::append(candidate, used, std::string_view(line, static_cast<std::size_t>(n))))
        return false;
    output = candidate;
    return true;
}

inline Record decode(std::string_view fixed_bytes) {
    Record result;
    if (fixed_bytes.size() != slot_bytes) return result;
    const auto end = fixed_bytes.find('\0');
    if (end == fixed_bytes.npos || end == 0) return result;
    for (std::size_t i = end; i < fixed_bytes.size(); ++i)
        if (fixed_bytes[i] != '\0') return result;
    auto body = fixed_bytes.substr(0, end);
    std::string_view line;
    if (!detail::take_line(body, line) || line != "ISSHELF-ROUTE-SLOT-1" ||
        !detail::take_line(body, line) || line.substr(0, 11) != "generation=" ||
        !detail::parse_hex16(line.substr(11), result.generation) || !result.generation ||
        !detail::take_line(body, line) || line != "systems=52") return Record{};
    for (std::size_t i = 0; i < system_count; ++i) {
        if (!detail::take_line(body, line)) return Record{};
        const auto id = catalog[i].id;
        if (line.size() < 7 + id.size() || line.substr(0, 6) != "route." ||
            line.substr(6, id.size()) != id || line[6 + id.size()] != '=') return Record{};
        const auto value = line.substr(7 + id.size());
        if (value == "-") continue;
        Preference parsed;
        if (!settings_codec::parse_value(i, value, parsed)) return Record{};
        result.routes[i].input = Input::valid;
        result.routes[i].preference = parsed;
    }
    const std::size_t prefix_bytes = static_cast<std::size_t>(body.data() - fixed_bytes.data());
    if (!detail::take_line(body, line) || !body.empty() ||
        line.substr(0, 9) != "checksum=") return Record{};
    std::uint64_t stored = 0;
    if (!detail::parse_hex16(line.substr(9), stored) ||
        stored != detail::checksum(fixed_bytes.data(), prefix_bytes)) return Record{};
    result.valid = true;
    return result;
}

inline InspectedSlot inspect(std::string_view fixed_bytes) {
    // A wrong-length read is damaged even if every byte received was zero.
    if (fixed_bytes.size() != slot_bytes) return {};
    bool all_zero = true;
    for (const char byte : fixed_bytes) {
        if (byte != '\0') { all_zero = false; break; }
    }
    if (all_zero) return {SlotState::blank, {}};
    const Record record = decode(fixed_bytes);
    return record.valid ? InspectedSlot{SlotState::valid, record} : InspectedSlot{};
}

inline Selection select(std::string_view first, std::string_view second) {
    const InspectedSlot a = inspect(first), b = inspect(second);
    Selection result;
    result.first_state = a.state;
    result.second_state = b.state;
    if (a.state != SlotState::valid && b.state != SlotState::valid) return result;
    if (a.state == SlotState::valid && b.state != SlotState::valid) {
        result.winner = Winner::first;
        result.record = a.record;
    } else if (a.state != SlotState::valid && b.state == SlotState::valid) {
        result.winner = Winner::second;
        result.record = b.record;
    } else if (a.record.generation > b.record.generation) {
        result.winner = Winner::first;
        result.record = a.record;
    } else if (b.record.generation > a.record.generation) {
        result.winner = Winner::second;
        result.record = b.record;
    } else if (first == second) {
        result.winner = Winner::first;
        result.record = a.record;
    } else result.winner = Winner::ambiguous;
    return result;
}

inline bool next_generation(const Selection& selected, std::uint64_t& output) {
    // Reading can recover from a damaged peer, but writing may not erase it.
    if (selected.first_state == SlotState::damaged ||
        selected.second_state == SlotState::damaged ||
        selected.winner == Winner::none || selected.winner == Winner::ambiguous)
        return false;
    if ((selected.winner == Winner::first &&
         selected.first_state != SlotState::valid) ||
        (selected.winner == Winner::second &&
         selected.second_state != SlotState::valid)) return false;
    // A valid + exactly blank pair may advance into the blank slot, provided a
    // future adapter independently verifies both fixed preprovisioned files.
    if (!selected.record.valid ||
        selected.record.generation == std::numeric_limits<std::uint64_t>::max()) return false;
    output = selected.record.generation + 1;
    return true;
}

inline bool initial_generation_after_preprovisioning(
    const Selection& selected, bool verified_preprovisioned_pair,
    std::uint64_t& output) {
    // This pure planner does not verify file identity or perform any I/O.
    if (!verified_preprovisioned_pair || selected.winner != Winner::none ||
        selected.first_state != SlotState::blank ||
        selected.second_state != SlotState::blank) return false;
    output = 1;
    return true;
}
} // namespace shelf::tile_route::two_slot
