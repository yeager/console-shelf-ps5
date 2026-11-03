// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/console_tile_route_two_slot.hpp"
#include "shelf/console_tile_write_gate.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
void check(bool okay, const char* reason) {
    if (!okay) {
        std::fprintf(stderr, "FAIL two-slot route codec: %s\n", reason);
        std::exit(1);
    }
}
std::string_view view(const shelf::tile_route::two_slot::Bytes& bytes) {
    return {bytes.data(), bytes.size()};
}
void rewrite_checksum(shelf::tile_route::two_slot::Bytes& bytes) {
    const std::string body(bytes.data());
    const auto at = body.find("checksum=");
    check(at != std::string::npos, "test record contains checksum");
    const auto hash = shelf::tile_route::two_slot::detail::checksum(bytes.data(), at);
    char hex[17]{};
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
    std::memcpy(bytes.data() + at + 9, hex, 16);
}
bool no_menu(void*, std::size_t, std::string_view) { return false; }
bool ps2_menu(void*, std::size_t system, std::string_view title) {
    return system == shelf::tile_route::system_for("ps2") && title == "PPSA99203";
}
} // namespace

int main() {
    using namespace shelf;
    using namespace shelf::tile_route;
    using namespace shelf::tile_route::two_slot;
#if defined(SHELF_PS5) || defined(SHELF_TILE_WRITE_QUARANTINE_TEST)
    static_assert(!console_tile_writes_enabled, "Record codec cannot enable tile writes");
#endif
    static_assert(system_count == 52, "Test binds all 52 catalog consoles");
    Routes routes;
    for (std::size_t i = 0; i < system_count; ++i) {
        routes[i].input = Input::valid;
        routes[i].preference = {i, i & 1 ? ConsoleTileDestination::home :
                                           ConsoleTileDestination::library, {}};
    }
    Bytes first{};
    check(encode(1, routes, first), "52 route choices fit one fixed slot");
    const auto one = decode(view(first));
    check(one.valid && one.generation == 1, "generation one round-trips");
    check(inspect(view(first)).state == SlotState::valid,
          "complete encoded slot is classified valid");
    for (std::size_t i = 0; i < system_count; ++i)
        check(one.routes[i].input == Input::valid &&
                  one.routes[i].preference.system == i &&
                  one.routes[i].preference.destination == routes[i].preference.destination,
              "every route remains bound to its catalog ID");
    check(std::find(first.begin(), first.end(), '\0') != first.end(),
          "record has canonical zero padding");

    const auto ps2 = system_for("ps2");
    routes[ps2].preference.destination = ConsoleTileDestination::emulator;
    std::memcpy(routes[ps2].preference.emulator_title, "PPSA99203", 10);
    routes[system_for("ps3")].input = Input::absent;
    routes[system_for("ps3")].preference = Preference{};
    Bytes second{};
    check(encode(2, routes, second), "newer record with installed emulator encodes");
    auto choice = select(view(first), view(second));
    check(choice.winner == Winner::second && choice.record.generation == 2 &&
              choice.record.routes[ps2].input == Input::valid &&
              choice.record.routes[system_for("ps3")].input == Input::absent,
          "newer valid slot wins and absent route stays absent");
    const auto stale = resolve({ps2}, choice.record.routes[ps2].input,
                               choice.record.routes[ps2].preference, no_menu, nullptr);
    const auto ready = resolve({ps2}, choice.record.routes[ps2].input,
                               choice.record.routes[ps2].preference, ps2_menu, nullptr);
    check(stale.resolution == Resolution::stale_emulator &&
              ready.resolution == Resolution::ready &&
              ready.intent.destination == ConsoleTileDestination::emulator,
          "record never bypasses installed emulator check");

    Bytes broken = second;
    broken[50] ^= 1;
    const auto recovered_first = select(view(first), view(broken));
    check(inspect(view(broken)).state == SlotState::damaged &&
              recovered_first.winner == Winner::first &&
              recovered_first.record.generation == 1,
          "corrupt newer slot falls back to last valid slot");
    std::uint64_t next = 777;
    check(!next_generation(recovered_first, next) && next == 777,
          "recovered read does not erase a damaged peer");
    const auto recovered_second = select(view(broken), view(first));
    check(recovered_second.winner == Winner::second &&
              recovered_second.first_state == SlotState::damaged &&
              !next_generation(recovered_second, next),
          "damaged first slot also permits read but blocks advancement");
    Bytes torn = first;
    std::copy_n(second.begin(), 200, torn.begin());
    const auto recovered_torn = select(view(first), view(torn));
    check(inspect(view(torn)).state == SlotState::damaged &&
              recovered_torn.winner == Winner::first &&
              !next_generation(recovered_torn, next),
          "partial in-place overwrite cannot supersede last valid slot");
    Bytes empty{};
    const auto virgin = select(view(empty), view(empty));
    check(inspect(view(empty)).state == SlotState::blank &&
              !decode(view(empty)).valid && virgin.winner == Winner::none &&
              virgin.first_state == SlotState::blank &&
              virgin.second_state == SlotState::blank &&
              !next_generation(virgin, next),
          "only two exact all-zero slots are virgin; ordinary advancement refuses them");
    check(!initial_generation_after_preprovisioning(virgin, false, next) &&
              next == 777 &&
              initial_generation_after_preprovisioning(virgin, true, next) &&
              next == 1,
          "blank pair may initialize only after separate preprovisioning verification");
    const auto valid_blank = select(view(first), view(empty));
    const auto blank_valid = select(view(empty), view(first));
    check(valid_blank.winner == Winner::first &&
              valid_blank.second_state == SlotState::blank &&
              next_generation(valid_blank, next) && next == 2 &&
              blank_valid.winner == Winner::second &&
              blank_valid.first_state == SlotState::blank &&
              next_generation(blank_valid, next) && next == 2 &&
              !initial_generation_after_preprovisioning(valid_blank, true, next),
          "valid plus exact blank may advance into blank peer, but cannot reinitialize");
    const auto damaged_blank = select(view(broken), view(empty));
    const auto damaged_damaged = select(view(broken), view(torn));
    check(damaged_blank.winner == Winner::none &&
              damaged_blank.first_state == SlotState::damaged &&
              !next_generation(damaged_blank, next) &&
              !initial_generation_after_preprovisioning(damaged_blank, true, next) &&
              damaged_damaged.winner == Winner::none &&
              damaged_damaged.first_state == SlotState::damaged &&
              damaged_damaged.second_state == SlotState::damaged &&
              !next_generation(damaged_damaged, next) &&
              !initial_generation_after_preprovisioning(damaged_damaged, true, next),
          "damaged slots never masquerade as virgin or permit advancement");
    check(select(view(first), view(first)).winner == Winner::first,
          "identical generation and bytes are not ambiguous");
    Bytes same_generation{};
    check(encode(1, routes, same_generation) &&
          select(view(first), view(same_generation)).winner == Winner::ambiguous,
          "conflicting equal generations require a choice");

    Bytes malformed = first;
    std::string body(malformed.data());
    auto at = body.find("route.ps2=home");
    check(at != std::string::npos, "test finds PS2 route");
    std::memcpy(malformed.data() + at + 10, "nope", 4);
    rewrite_checksum(malformed);
    check(!decode(view(malformed)).valid,
          "checksum-correct malformed route still fails catalog validation");
    malformed = first;
    body.assign(malformed.data());
    at = body.find("route.ps2=");
    check(at != std::string::npos, "test finds PS2 identity");
    std::memcpy(malformed.data() + at + 6, "zzz", 3);
    rewrite_checksum(malformed);
    check(!decode(view(malformed)).valid,
          "checksum-correct unknown console identity is rejected");
    malformed = first;
    malformed.back() = 'x';
    check(inspect(view(malformed)).state == SlotState::damaged,
          "nonzero padding is damaged");
    const std::string overlong(slot_bytes + 1, '\0');
    check(inspect(std::string_view(first.data(), first.size() - 1)).state ==
              SlotState::damaged &&
              inspect(std::string_view(empty.data(), empty.size() - 1)).state ==
              SlotState::damaged &&
              inspect(std::string_view()).state == SlotState::damaged &&
              inspect(overlong).state == SlotState::damaged,
          "every wrong length is damaged, including zero-only reads");
    check(next_generation(choice, next) && next == 3,
          "next slot gets a strictly increasing generation");
    check(!next_generation(select(view(first), view(same_generation)), next),
          "ambiguous sequence cannot be advanced");
    Bytes maximum{};
    check(encode(UINT64_MAX, routes, maximum) &&
          !next_generation(select(view(maximum), view(empty)), next),
          "generation overflow is refused");
    routes[ps2].input = Input::invalid;
    check(!encode(3, routes, malformed), "invalid route cannot be stored");
    std::puts("PASS: 52-console route slots classify blank/valid/damaged, recover reads, and refuse unsafe advancement");
}
