// SPDX-License-Identifier: GPL-3.0-only
// Production-default host fixture: no generic helper pin exists yet.
#include "shelf/console_tile_job_bridge.hpp"
#include "shelf/console_tile_write_gate.hpp"
#include <array>
#include <cassert>

static_assert(shelf::tile_job::detail::expected_generic_helper_bytes == 0,
              "The generic PS5 helper must remain absent");
#ifdef SHELF_PS5
static_assert(!shelf::console_tile_writes_enabled,
              "The PS5 write gate must remain closed");
#endif

int main() {
    using shelf::tile_job::PinnedHelper;
    using shelf::tile_job::detail::exact_helper_sha256;
    constexpr char abc_sha[] =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    const unsigned char abc[] = {'a', 'b', 'c'};
    const unsigned char changed[] = {'a', 'b', 'd'};
    assert(exact_helper_sha256(abc, sizeof(abc), 3, abc_sha));
    assert(!exact_helper_sha256(changed, sizeof(changed), 3, abc_sha));
    assert(!exact_helper_sha256(abc, sizeof(abc), 2, abc_sha));
    assert(!exact_helper_sha256(nullptr, sizeof(abc), 3, abc_sha));
    assert(!exact_helper_sha256(abc, sizeof(abc), 3, "bad"));
    std::array<unsigned char, 1000> many{};
    many.fill('a');
    assert(exact_helper_sha256(
        many.data(), many.size(), many.size(),
        "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"));
    assert((!PinnedHelper{abc, sizeof(abc)}.available()));
    assert((!PinnedHelper{changed, sizeof(changed)}.available()));
    assert((!PinnedHelper{abc, 1}.available()));
    assert((!PinnedHelper{}.available()));
    return 0;
}
