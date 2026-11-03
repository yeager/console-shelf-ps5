// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace shelf {

// Residual cleanup is deliberately narrower than the ordinary owned-title
// protocol. It can address only the preferred PPSA99700..PPSA99751 slots,
// whose catalog order is fixed in catalog.generated.hpp.
inline bool tile_residual_title_index(std::string_view title,
                                      std::size_t& system) {
    if (title.size() != 9 || title.substr(0, 4) != "PPSA")
        return false;
    unsigned number = 0;
    for (std::size_t index = 4; index < title.size(); ++index) {
        const char value = title[index];
        if (value < '0' || value > '9')
            return false;
        number = number * 10 + static_cast<unsigned>(value - '0');
    }
    if (number < 99700 || number >= 99752)
        return false;
    system = static_cast<std::size_t>(number - 99700);
    return true;
}

inline bool tile_residual_text_once(std::string_view text,
                                    std::string_view token) {
    const auto at = text.find(token);
    return at != text.npos && text.find(token, at + 1) == text.npos;
}

// The two platform metadata copies must first be byte-identical. This parser
// then binds that compact JSON to the fixed title/content/concept identity
// written by isShelf. It is intentionally stricter than general JSON and
// rejects whitespace rewrites, duplicate identity fields and control bytes.
inline bool tile_residual_param_identity(std::string_view text,
                                         std::string_view title) {
    std::size_t system = 0;
    if (!tile_residual_title_index(title, system) || text.size() < 128 ||
        text.size() > 4096 || text.front() != '{' || text.back() != '\n' ||
        text[text.size() - 2] != '}')
        return false;
    for (std::size_t index = 0; index + 1 < text.size(); ++index)
        if (static_cast<unsigned char>(text[index]) < 0x20)
            return false;

    char title_field[32], content_field[80], concept_field[32];
    const int title_used = std::snprintf(
        title_field, sizeof(title_field), "\"titleId\":\"%.*s\"",
        static_cast<int>(title.size()), title.data());
    const int content_used = std::snprintf(
        content_field, sizeof(content_field),
        "\"contentId\":\"UP9000-%.*s_00-PS5FORWARDER0000\"",
        static_cast<int>(title.size()), title.data());
    const unsigned number = 99700u + static_cast<unsigned>(system);
    const int concept_used = std::snprintf(
        concept_field, sizeof(concept_field), "\"conceptId\":\"%u\"",
        number);
    if (title_used <= 0 || static_cast<std::size_t>(title_used) >= sizeof(title_field) ||
        content_used <= 0 || static_cast<std::size_t>(content_used) >= sizeof(content_field) ||
        concept_used <= 0 || static_cast<std::size_t>(concept_used) >= sizeof(concept_field))
        return false;
    return tile_residual_text_once(text, title_field) &&
           tile_residual_text_once(text, content_field) &&
           tile_residual_text_once(text, concept_field) &&
           tile_residual_text_once(text, "\"contentBadgeType\":1") &&
           tile_residual_text_once(text, "\"applicationDrmType\":\"free\"");
}

} // namespace shelf
