// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <string_view>

namespace shelf {
// Keep the stored value stable: it is part of the on-disk settings format.
enum class Language { english, swedish, count };

inline constexpr std::array<std::string_view, 2> language_labels = {"English", "Svenska"};

// Returns a UI string in the requested language. English source text is the
// fallback, which keeps new strings usable until a catalogue is completed.
std::string_view localize(Language language, std::string_view source);
} // namespace shelf
