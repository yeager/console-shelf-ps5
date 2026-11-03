// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace shelf {
// The existing UI picker stays read-only in normal PS5 builds. An explicit
// private qualification build may admit the owned-source manager without
// changing the public/default policy.
#if (defined(SHELF_PS5) && !defined(SHELF_PS5_OWNED_TILE_WRITES)) || \
    defined(SHELF_TILE_WRITE_QUARANTINE_TEST)
inline constexpr bool console_tile_writes_enabled = false;
#else
inline constexpr bool console_tile_writes_enabled = true;
#endif
} // namespace shelf
