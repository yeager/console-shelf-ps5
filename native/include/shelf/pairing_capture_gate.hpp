// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "ui.hpp"

namespace shelf {

// No frame or state serialization may be produced while a pairing code or
// entered symbols are visible. The page is reserved and default-off today.
inline constexpr bool pairing_capture_allowed(Page page) {
    return page != Page::pairing_code;
}

template <typename Writer>
bool pairing_capture_if_allowed(Page page, Writer&& writer) {
    return pairing_capture_allowed(page) && writer();
}

} // namespace shelf
