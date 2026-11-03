// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "library.hpp"
#include <cstring>

namespace shelf {
enum class CaseMaterial { jewel, plastic, cardboard, none };
struct CaseProfile {
    // Relative physical proportions, independent of the source image dimensions.
    float width, height, depth;
    CaseMaterial material;
    unsigned char red, green, blue;
};
inline CaseProfile game_case_profile(const LibraryGame& game, float art_aspect) {
    const auto id = catalog[game.system].id;
    const bool japan = std::strstr(game.name, "(Japan") != nullptr;
    const bool pal = std::strstr(game.name, "Europe") || std::strstr(game.name, "Australia") ||
                     std::strncmp(game.title_id, "SLES", 4) == 0 || std::strncmp(game.title_id, "SCES", 4) == 0;
    if (id == "ps1") return {142, 125, pal ? 17.f : 10.f, CaseMaterial::jewel, 64, 69, 73};
    if (id == "ps2") return {135, 190, 14, CaseMaterial::plastic, 4, 5, 7};
    if (id == "ps3") return {135, 171, 14, CaseMaterial::plastic, 117, 123, 129};
    if (id == "ps4" || id == "ps5") return {135, 171, 14, CaseMaterial::plastic, 24, 75, 156};
    if (id == "psp") return {105, 177, 14, CaseMaterial::plastic, 151, 155, 160};
    if (id == "n64") return japan || art_aspect < .9f
        ? CaseProfile{143, 204, 27, CaseMaterial::cardboard, 51, 52, 48}
        : CaseProfile{195, 137, 30, CaseMaterial::cardboard, 47, 49, 44};
    if (id == "snes") return japan || art_aspect < .9f
        ? CaseProfile{106, 190, 28, CaseMaterial::cardboard, 67, 69, 67}
        : CaseProfile{195, 135, 30, CaseMaterial::cardboard, 48, 49, 53};
    if (id == "nes" || id == "fds") return {137, 192, 25, CaseMaterial::cardboard, 46, 47, 49};
    if (id == "gb" || id == "gbc" || id == "gba") {
        if (japan && art_aspect > 1.2f) return {144, 89, 25, CaseMaterial::cardboard, 61, 64, 64};
        if (japan && art_aspect < .9f) return {98, 125, 25, CaseMaterial::cardboard, 61, 64, 64};
        return {128, 128, 25, CaseMaterial::cardboard, 61, 64, 64};
    }
    if (id == "ds" || id == "3ds") return {135, 122, 15, CaseMaterial::plastic,
        static_cast<unsigned char>(id == "3ds" ? 191 : 35),
        static_cast<unsigned char>(id == "3ds" ? 193 : 37),
        static_cast<unsigned char>(id == "3ds" ? 196 : 40)};
    if (id == "switch" || id == "switch2") return {104, 170, 10, CaseMaterial::plastic,
        static_cast<unsigned char>(id == "switch2" ? 163 : 135),
        static_cast<unsigned char>(id == "switch2" ? 39 : 140),
        static_cast<unsigned char>(id == "switch2" ? 44 : 146)};
    if (id == "gamecube" || id == "wii" || id == "wiiu") return {135, 190, 14, CaseMaterial::plastic,
        static_cast<unsigned char>(id == "wii" ? 199 : 28),
        static_cast<unsigned char>(id == "wii" ? 201 : id == "wiiu" ? 116 : 30),
        static_cast<unsigned char>(id == "wii" ? 198 : id == "wiiu" ? 166 : 34)};
    if (id == "genesis" || id == "mastersystem") return {135, 190, 25, CaseMaterial::plastic, 28, 29, 30};
    if (id == "saturn" || id == "segacd") return japan
        ? CaseProfile{142, 125, 10, CaseMaterial::jewel, 60, 65, 69}
        : CaseProfile{145, 210, 24, CaseMaterial::jewel, 47, 51, 55};
    if (id == "dreamcast" || id == "pcengine" || id == "supergrafx" || id == "neogeocd" || id == "pcfx")
        return {142, 125, id == "dreamcast" && pal ? 16.f : 10.f, CaseMaterial::jewel, 63, 70, 79};
    if (id == "threedo") return {145, 210, 24, CaseMaterial::cardboard, 39, 41, 42};
    if (id == "atari2600" || id == "atari5200" || id == "atari7800" || id == "atarijaguar")
        return {140, 190, 27, CaseMaterial::cardboard, 43, 43, 45};
    if (id == "arcadefbneo" || id == "arcademame" || id == "naomi" || id == "atomiswave")
        return {art_aspect, 1, .02f, CaseMaterial::none, 34, 37, 39};
    // Original packaging varied for computers and several handhelds. The actual
    // front scan supplies its proportions instead of imposing a DVD case.
    if (art_aspect < .35f || art_aspect > 2.5f)
        return {art_aspect, 1, .02f, CaseMaterial::none, 34, 37, 39};
    return {art_aspect, 1, .12f, CaseMaterial::cardboard, 44, 47, 49};
}
} // namespace shelf
