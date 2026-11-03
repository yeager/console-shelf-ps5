// SPDX-License-Identifier: GPL-3.0-only
// The generic shortcut writer helper has not been reviewed or bundled.
#pragma once

#include <cstddef>

#if defined(SHELF_CONSOLE_TILE_HELPER_TEST_PIN) && \
    (!defined(SHELF_CONSOLE_TILE_HELPER_HOST_TEST) || defined(SHELF_PS5))
#error "A synthetic helper pin is permitted only in a host test"
#endif

namespace shelf::tile_job::detail {

#ifdef SHELF_CONSOLE_TILE_HELPER_TEST_PIN
// "abc" is a SHA-256 test vector, never a PS5 helper asset.
inline constexpr std::size_t expected_generic_helper_bytes = 3;
inline constexpr char expected_generic_helper_sha256[] =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
#else
// A future source review must replace both constants with one exact writer ELF.
inline constexpr std::size_t expected_generic_helper_bytes = 0;
inline constexpr char expected_generic_helper_sha256[] = "";
#endif

} // namespace shelf::tile_job::detail
