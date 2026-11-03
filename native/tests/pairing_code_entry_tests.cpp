// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/pairing_code_entry.hpp"
#include "shelf/pairing_capture_gate.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL pairing entry: %s\n", message);
        std::exit(1);
    }
}
void enter_zero(shelf::PairingCodeEntry& entry, std::uint64_t now, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
        check(entry.apply(shelf::PairingCodeEntry::Command::accept, now) ==
                  shelf::PairingCodeEntry::Result::updated, "bounded zero entry");
}
} // namespace

void test_pairing_code_entry() {
    using Entry = shelf::PairingCodeEntry;
    using Command = Entry::Command;
    using Result = Entry::Result;
    static_assert(!std::is_copy_constructible<Entry>::value && !std::is_copy_assignable<Entry>::value,
                  "entry must not duplicate secret symbols");
    Entry entry;
    check(!entry.active(1) && entry.apply(Command::accept, 1) == Result::inactive,
          "default-off entry");
    check(!entry.begin(1, 0) && !entry.begin(1, Entry::max_lifetime_ms + 1),
          "zero and excessive lifetimes refused");
    check(!entry.begin(std::numeric_limits<std::uint64_t>::max() - 2, 3),
          "deadline overflow refused");
    check(entry.begin(1000, 500), "bounded entry begins");
    check(entry.apply(Command::left, 1001) == Result::unchanged &&
              entry.apply(Command::up, 1001) == Result::unchanged,
          "grid edges do not wrap");
    check(entry.apply(Command::right, 1002) == Result::updated && entry.selection(1002) == 1 &&
              entry.apply(Command::left, 1003) == Result::updated && entry.selection(1003) == 0,
          "controller grid navigation");
    enter_zero(entry, 1004, 19);
    check(entry.size(1004) == 19 && entry.symbol(1004, 18) == '0' &&
              entry.symbol(1004, 19) == '\0', "only entered symbols readable");
    check(entry.apply(Command::right, 1005) == Result::updated &&
              entry.apply(Command::accept, 1005) == Result::invalid && entry.size(1005) == 19,
          "noncanonical final padding rejected before entry");
    check(entry.apply(Command::left, 1006) == Result::updated &&
              entry.apply(Command::down, 1006) == Result::updated &&
              entry.apply(Command::down, 1006) == Result::updated && entry.selection(1006) == 16,
          "G selected with controller movement");
    check(entry.apply(Command::accept, 1007) == Result::complete && entry.size(1007) == 20 &&
              entry.apply(Command::accept, 1007) == Result::complete && entry.size(1007) == 20,
          "entry never exceeds twenty symbols");
    shelf::managed_tile_pairing_client::Secret secret{};
    check(entry.take_secret(1008, secret) && secret[11] == 1 &&
              entry.size(1008) == 0 && !entry.active(1008),
          "canonical code decodes once and clears entry");
    for (std::size_t i = 0; i < 11; ++i)
        check(secret[i] == 0, "zero prefix decoded");
    shelf::managed_tile_pairing_client::clear(secret);
    const auto* raw = reinterpret_cast<const unsigned char*>(&entry);
    for (std::size_t i = 0; i < sizeof(entry); ++i)
        check(raw[i] != static_cast<unsigned char>('G'), "consumed code symbol zeroized");

    check(entry.begin(2000, 100), "second session begins");
    enter_zero(entry, 2001, 2);
    check(entry.apply(Command::erase, 2002) == Result::updated && entry.size(2002) == 1 &&
              entry.apply(Command::clear, 2003) == Result::updated && entry.size(2003) == 0,
          "erase and clear remove symbols");
    enter_zero(entry, 2004, 1);
    check(entry.apply(Command::cancel, 2005) == Result::cancelled &&
              entry.symbol(2005, 0) == '\0' && !entry.active(2005), "cancel zeroizes");
    check(entry.begin(3000, 100), "expiry session begins");
    enter_zero(entry, 3001, 1);
    check(entry.active(3099) && !entry.active(3100) && entry.symbol(3100, 0) == '\0',
          "exact expiry clears symbols");
    check(entry.begin(4000, 100), "clock session begins");
    enter_zero(entry, 4001, 1);
    check(entry.apply(Command::accept, 4000) == Result::expired && !entry.active(4001),
          "clock regression fails closed");
    check(entry.begin(5000, 100), "partial entry session begins");
    enter_zero(entry, 5001, 1);
    secret.fill(0xff);
    check(!entry.take_secret(5002, secret) && !entry.active(5002),
          "partial code cannot be transferred");
    for (auto byte : secret)
        check(byte == 0, "failed transfer clears output");

    int writes = 0;
    const auto write = [&] { ++writes; return true; };
    check(!shelf::pairing_capture_if_allowed(shelf::Page::pairing_code, write) && writes == 0,
          "sensitive view cannot invoke capture callback");
    check(shelf::pairing_capture_if_allowed(shelf::Page::home, write) && writes == 1,
          "ordinary view retains capture callback");
    shelf::UiState state;
    state.page = shelf::Page::pairing_code;
    shelf::input(state, shelf::Action::settings);
    check(state.page == shelf::Page::pairing_code && !state.changed,
          "reserved view cannot fall through to Settings");
    std::puts("PASS: default-off controller pairing entry, expiry, zeroization and capture gate.");
}
