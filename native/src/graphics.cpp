// SPDX-License-Identifier: GPL-3.0-only
#include "shelf/graphics.hpp"
#include "shelf/console_tile_startup.hpp"
#include "shelf/artwork.hpp"
#include "shelf/game_case.hpp"
#include "shelf/carousel.hpp"
#include "shelf/scene.generated.hpp"
#include "shelf/memory.hpp"
#include "shelf/movie.hpp"
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"
#include "stb/stb_truetype.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace shelf {
namespace {
constexpr int width = 1920, height = 1080;
constexpr SDL_Color white{246, 246, 255, 255}, muted{157, 168, 198, 255}, purple{203, 187, 255, 255},
    panel{22, 26, 28, 255};
SDL_Renderer* renderer = nullptr;
SDL_Texture* background = nullptr;
SDL_Texture* shelf_background = nullptr;
SDL_Texture* halo = nullptr;
std::array<SDL_Texture*, 2> settings_buttons{};
char root[2048]{};
char room_data[2048]{};
unsigned render_time = 0, tv_changed_at = 0;
std::size_t tv_selected = system_count;
struct Font {
    SDL_Texture* texture = nullptr;
    int size = 0;
    bool bold = false;
    stbtt_bakedchar chars[224]{};
};
std::array<Font, 32> fonts;
unsigned resource_errors = 0;
struct Art {
    SDL_Texture* texture = nullptr;
    char name[256]{};
    int w = 0, h = 0;
    int support_y = 0;
    bool monochrome = false;
    SDL_Rect bounds{};
    unsigned age = 0;
};
std::array<Art, 12> art;
unsigned art_age = 0;
struct CaseFrame {
    SDL_Texture* texture = nullptr;
    std::uint64_t key = 0;
    int w = 0, h = 0;
    bool spine = false;
    unsigned age = 0;
};
std::array<CaseFrame, 2> case_frames;
unsigned case_age = 0;
OrbitMotion orbit;
ShelfPageMotion shelf_motion;
BrowseResult shelf_list{};
bool shelf_visible = false;
bool room_visible = false;
bool tv_motion = false;
Movie tv_movie;
SDL_Texture* movie_texture = nullptr;
SDL_Texture* noise_texture = nullptr;
Uint32* noise_pixels = nullptr;
int noise_width = 0, noise_height = 0;
std::size_t movie_selected = system_count;
bool movie_demo = false;
bool movie_bundled = false;
void stop_movie() {
    tv_movie.close();
    if (movie_texture) SDL_DestroyTexture(movie_texture);
    movie_texture = nullptr; movie_selected = system_count; movie_bundled = false;
}
float room_scale() {
    return std::max(static_cast<float>(width) / room_scene::width, static_cast<float>(height) / room_scene::height);
}
int room_x(float x) {
    return static_cast<int>((width - room_scene::width * room_scale()) / 2 + x * room_scale() + .5f);
}
int room_y(float y) {
    return static_cast<int>((height - room_scene::height * room_scale()) / 2 + y * room_scale() + .5f);
}
bool orbit_visible = false;
BrowseResult orbit_list{};
const std::array<std::string_view, 8> sorts = {"Featured order",   "Name: A to Z",    "Name: Z to A",
                                               "Newest hardware",  "Oldest hardware", "Brand, then name",
                                               "Most games first", "Favorites first"};
const std::array<std::string_view, 5> filters = {"All systems", "Emulator detected", "Games detected",
                                                 "Built-in systems", "No launch route"};
const std::array<std::string_view, 4> icon_names = {"Gaming", "Classic mint", "Jade", "Vector (default)"};
const std::array<std::string_view, 4> icon_files = {"assets/shelf-icon.png", "assets/shelf-icon-v1.png",
                                                "assets/shelf-icon-premium-v2.png", "assets/branding/shelf-gaming-mark.png"};
void color(SDL_Color c) {
    SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
}
void rect(int x, int y, int w, int h, SDL_Color c) {
    color(c);
    SDL_Rect box{x, y, w, h};
    SDL_RenderFillRect(renderer, &box);
}
void line(int x1, int y1, int x2, int y2, SDL_Color c) {
    color(c);
    SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
}
void rounded(int x, int y, int w, int h, int radius, SDL_Color c) {
    radius = std::min(radius, std::min(w, h) / 2);
    rect(x, y + radius, w, h - 2 * radius, c);
    color(c);
    for (int row = 0; row < radius; ++row) {
        const float dy = static_cast<float>(radius - row) - .5f;
        const int inset = radius - static_cast<int>(std::sqrt(static_cast<float>(radius * radius) - dy * dy));
        SDL_RenderDrawLine(renderer, x + inset, y + row, x + w - inset - 1, y + row);
        SDL_RenderDrawLine(renderer, x + inset, y + h - row - 1, x + w - inset - 1, y + h - row - 1);
    }
}
void box(int x, int y, int w, int h, bool selected, SDL_Color fill = panel) {
    rounded(x, y, w, h, 20, selected ? purple : SDL_Color{53, 59, 83, 255});
    const int edge = selected ? 3 : 1;
    rounded(x + edge, y + edge, w - edge * 2, h - edge * 2, 20 - edge, fill);
}
void room_wall_panel() {
    // Match Settings: the backboard fits between the left light and the TV.
    rounded(160, 105, 1008, 277, 22, {4, 9, 8, 72});
    rounded(164, 103, 1000, 275, 20, {167, 153, 124, 62});
    rounded(165, 104, 998, 273, 19, {13, 23, 21, 239});
    line(188, 105, 1138, 105, {206, 207, 181, 40});
}
void room_control(int x, int y, int w, int h, bool selected) {
    rounded(x, y, w, h, 14, selected ? SDL_Color{162, 221, 194, 255} : SDL_Color{83, 100, 89, 255});
    const int edge = selected ? 3 : 1;
    rounded(x + edge, y + edge, w - edge * 2, h - edge * 2, 14 - edge, {20, 31, 27, 255});
}
void settings_button(bool selected) {
    auto*& texture = settings_buttons[selected ? 1 : 0];
    if (!texture) {
        // The same rounded cog outline as the browser UI. Supersampling and
        // premultiplied coverage avoid jagged corners on a television.
        constexpr std::array<SDL_FPoint, 24> cog{
            {{9.7f, 3},   {9.1f, 5.2f},   {7.3f, 6.2f},   {5.1f, 5.6f},   {2.9f, 9.4f},   {4.5f, 11},
             {4.5f, 13},  {2.9f, 14.6f},  {5.1f, 18.4f},  {7.3f, 17.8f},  {9.1f, 18.8f},  {9.7f, 21},
             {14.3f, 21}, {14.9f, 18.8f}, {16.7f, 17.8f}, {18.9f, 18.4f}, {21.1f, 14.6f}, {19.5f, 13},
             {19.5f, 11}, {21.1f, 9.4f},  {18.9f, 5.6f},  {16.7f, 6.2f},  {14.9f, 5.2f},  {14.3f, 3}}};
        std::array<Uint32, 64 * 64> pixels{};
        for (int py = 0; py < 64; ++py)
            for (int px = 0; px < 64; ++px) {
                unsigned sums[4]{};
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx) {
                        const float x = static_cast<float>(px) + (sx + .5f) / 4;
                        const float y = static_cast<float>(py) + (sy + .5f) / 4;
                        const float dx = std::abs(x - 32) - 13, dy = std::abs(y - 32) - 13;
                        const float ox = std::max(0.0f, dx), oy = std::max(0.0f, dy);
                        const float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(dx, dy), 0.0f) - 16;
                        SDL_Color sample{};
                        if (d <= 0) {
                            const float gradient = std::clamp(y / 64, 0.0f, 1.0f);
                            sample = {static_cast<Uint8>(29 - 9 * gradient), static_cast<Uint8>(32 - 9 * gradient),
                                      static_cast<Uint8>(52 - 11 * gradient), 255};
                            if (d >= -(selected ? 2.0f : 1.15f))
                                sample = selected ? purple : SDL_Color{65, 67, 92, 255};
                            const float gx = (x - 32) / 1.35f + 12, gy = (y - 32) / 1.35f + 12;
                            bool stroke =
                                std::abs(std::sqrt((gx - 12) * (gx - 12) + (gy - 12) * (gy - 12)) - 3.1f) <= .8f;
                            for (std::size_t i = 0; i < cog.size() && !stroke; ++i) {
                                const auto a = cog[i], b = cog[(i + 1) % cog.size()];
                                const float vx = b.x - a.x, vy = b.y - a.y;
                                const float t =
                                    std::clamp(((gx - a.x) * vx + (gy - a.y) * vy) / (vx * vx + vy * vy), 0.0f, 1.0f);
                                const float ex = gx - a.x - t * vx, ey = gy - a.y - t * vy;
                                stroke = ex * ex + ey * ey <= .64f;
                            }
                            if (stroke)
                                sample = selected ? SDL_Color{244, 237, 255, 255} : SDL_Color{198, 190, 223, 255};
                        } else if (selected && d < 3) {
                            const float falloff = 1 - d / 3;
                            sample = {203, 187, 255, static_cast<Uint8>(52 * falloff * falloff)};
                        }
                        sums[0] += sample.r * sample.a;
                        sums[1] += sample.g * sample.a;
                        sums[2] += sample.b * sample.a;
                        sums[3] += sample.a;
                    }
                const unsigned alpha = (sums[3] + 8) / 16;
                const unsigned red = sums[3] ? (sums[0] + sums[3] / 2) / sums[3] : 0;
                const unsigned green = sums[3] ? (sums[1] + sums[3] / 2) / sums[3] : 0;
                const unsigned blue = sums[3] ? (sums[2] + sums[3] / 2) / sums[3] : 0;
                pixels[static_cast<std::size_t>(py * 64 + px)] = red | (green << 8) | (blue << 16) | (alpha << 24);
            }
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, 64, 64);
        if (!texture || SDL_UpdateTexture(texture, nullptr, pixels.data(), 64 * 4) < 0) {
            ++resource_errors;
            return;
        }
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    }
    // Fit between the TV's right bezel and the room's right vertical light.
    const SDL_Rect bounds{1786, 30, 42, 42};
    SDL_RenderCopy(renderer, texture, nullptr, &bounds);
}
Font* font(int size, bool bold) {
    for (auto& entry : fonts)
        if (entry.texture && entry.size == size && entry.bold == bold)
            return &entry;
    Font* entry = nullptr;
    for (auto& candidate : fonts)
        if (!candidate.texture) {
            entry = &candidate;
            break;
        }
    if (!entry) {
        ++resource_errors;
        return nullptr;
    }
    char filename[2304];
    std::snprintf(filename, sizeof(filename), "%s/native/assets/fonts/Poppins-%s.ttf", root,
                  bold ? "SemiBold" : "Regular");
    FILE* file = std::fopen(filename, "rb");
    if (!file) {
        ++resource_errors;
        return nullptr;
    }
    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    std::rewind(file);
    if (length < 0 || length > 1024 * 1024) {
        std::fclose(file);
        ++resource_errors;
        return nullptr;
    }
    const int atlas = size <= 35 ? 512 : 1024;
    const auto atlas_pixels = static_cast<std::size_t>(atlas) * static_cast<std::size_t>(atlas);
    auto* bytes = static_cast<unsigned char*>(allocate(static_cast<std::size_t>(length)));
    auto* bitmap = static_cast<unsigned char*>(allocate_zero(atlas_pixels, 1));
    auto* pixels = static_cast<Uint32*>(allocate(atlas_pixels * 4));
    bool okay = bytes && bitmap && pixels;
    if (okay)
        okay = std::fread(bytes, 1, static_cast<std::size_t>(length), file) == static_cast<std::size_t>(length);
    std::fclose(file);
    if (okay)
        okay =
            stbtt_BakeFontBitmap(bytes, 0, static_cast<float>(size), bitmap, atlas, atlas, 32, 224, entry->chars) > 0;
    if (okay) {
        for (std::size_t i = 0; i < atlas_pixels; ++i)
            pixels[i] = (static_cast<Uint32>(bitmap[i]) << 24) | 0xffffffu;
        entry->texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, atlas, atlas);
        if (entry->texture) {
            SDL_UpdateTexture(entry->texture, nullptr, pixels, atlas * 4);
            SDL_SetTextureBlendMode(entry->texture, SDL_BLENDMODE_BLEND);
            entry->size = size;
            entry->bold = bold;
        }
    }
    release(bytes);
    release(bitmap);
    release(pixels);
    if (!entry->texture)
        ++resource_errors;
    return entry->texture ? entry : nullptr;
}
unsigned codepoint(std::string_view text, std::size_t& at) {
    const auto c = static_cast<unsigned char>(text[at++]);
    if (c < 128)
        return c;
    if ((c & 0xe0) == 0xc0 && at < text.size())
        return ((c & 31u) << 6) | (static_cast<unsigned char>(text[at++]) & 63u);
    if ((c & 0xf0) == 0xe0 && at + 1 < text.size()) {
        const unsigned a = static_cast<unsigned char>(text[at++]), b = static_cast<unsigned char>(text[at++]);
        return ((c & 15u) << 12) | ((a & 63u) << 6) | (b & 63u);
    }
    return '?';
}
int text_width(std::string_view text, int size, bool bold = false) {
    auto* f = font(size, bold);
    if (!f)
        return 0;
    float advance = 0;
    for (std::size_t at = 0; at < text.size();) {
        unsigned c = codepoint(text, at);
        if (c < 32 || c > 255)
            c = '?';
        advance += f->chars[c - 32].xadvance;
    }
    return static_cast<int>(advance);
}
void text(std::string_view value, int x, int y, int size, SDL_Color tint = white, bool bold = false, int limit = 1800) {
    auto* f = font(size, bold);
    if (!f)
        return;
    SDL_SetTextureColorMod(f->texture, tint.r, tint.g, tint.b);
    SDL_SetTextureAlphaMod(f->texture, tint.a);
    float cursor = static_cast<float>(x);
    for (std::size_t at = 0; at < value.size();) {
        unsigned c = codepoint(value, at);
        if (c < 32 || c > 255)
            c = '?';
        const auto& g = f->chars[c - 32];
        if (cursor + g.xadvance > static_cast<float>(x + limit))
            break;
        SDL_Rect source{g.x0, g.y0, g.x1 - g.x0, g.y1 - g.y0};
        SDL_Rect dest{static_cast<int>(cursor + g.xoff), y + size + static_cast<int>(g.yoff), source.w, source.h};
        SDL_RenderCopy(renderer, f->texture, &source, &dest);
        cursor += g.xadvance;
    }
}
void centered(std::string_view value, int x, int y, int size, SDL_Color tint = white, bool bold = false) {
    text(value, x - text_width(value, size, bold) / 2, y, size, tint, bold);
}
Art* image(std::string_view name, bool monochrome = false) {
    ++art_age;
    for (auto& entry : art)
        if (std::string_view(entry.name) == name && entry.monochrome == monochrome) {
            entry.age = art_age;
            return &entry;
        }
    auto* slot = &art[0];
    for (auto& candidate : art)
        if (candidate.age < slot->age)
            slot = &candidate;
    if (slot->texture)
        SDL_DestroyTexture(slot->texture);
    slot->texture = nullptr;
    std::snprintf(slot->name, sizeof(slot->name), "%.*s", static_cast<int>(name.size()), name.data());
    slot->age = art_age;
    slot->monochrome = monochrome;
    char filename[2304];
    const bool spine = name.size() == 23 && name.substr(0, 7) == "@spine/";
    bool cover = name.size() == 23 && (name.substr(0, 7) == "@cover/" || spine);
    if (cover) for (char c : name.substr(7)) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) cover = false;
    const bool custom = cover || name == "@room" || (name.size() > 7 && std::string_view(name.data(), 7) == "@intro/" &&
                         find_system(std::string_view(name.data() + 7, name.size() - 7)) < system_count);
    if (name == "@room") std::snprintf(filename, sizeof(filename), "%s/tv-image.png", room_data);
    else if (cover) std::snprintf(filename, sizeof(filename), "%s/covers/%s%s.png", room_data, slot->name + 7, spine ? ".spine" : "");
    else if (custom) std::snprintf(filename, sizeof(filename), "%s/intros/%s.png", room_data, slot->name + 7);
    else std::snprintf(filename, sizeof(filename), "%s/%s", root, slot->name);
    if (custom) {
        int iw = 0, ih = 0, ic = 0;
        const int limit = cover ? 960 : 2048;
        if (!room_data[0] || !stbi_info(filename, &iw, &ih, &ic) || iw < 1 || ih < 1 || iw > limit || ih > limit)
            return slot;
    }
    int channels = 0;
    unsigned char* data = stbi_load(filename, &slot->w, &slot->h, &channels, 4);
    if (data && slot->w > 0 && slot->h > 0 && slot->w <= 4096 && slot->h <= 4096) {
        int left = slot->w, top = slot->h, right = -1, bottom = -1;
        slot->support_y = 0;
        for (int y = 0; y < slot->h; ++y) {
            int solid = 0;
            for (int x = 0; x < slot->w; ++x) {
                const auto alpha = data[(static_cast<std::size_t>(y) * slot->w + x) * 4 + 3];
                if (alpha > 8) {
                    left = std::min(left, x);
                    top = std::min(top, y);
                    right = std::max(right, x);
                    bottom = std::max(bottom, y);
                }
                if (alpha >= 192)
                    ++solid;
            }
            if (solid >= 3)
                slot->support_y = y + 1;
        }
        if (!slot->support_y)
            slot->support_y = bottom >= 0 ? bottom + 1 : slot->h;
        if (right >= 0) {
            left = std::max(0, left - 12);
            top = std::max(0, top - 12);
            right = std::min(slot->w - 1, right + 12);
            bottom = std::min(slot->h - 1, bottom + 12);
            slot->bounds = {left, top, right - left + 1, bottom - top + 1};
        } else {
            slot->bounds = {0, 0, slot->w, slot->h};
        }
        if (monochrome)
            for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(slot->w) * slot->h; ++pixel) {
                auto* rgba = data + pixel * 4;
                const auto grey = static_cast<unsigned char>((54u * rgba[0] + 183u * rgba[1] + 19u * rgba[2]) / 256u);
                rgba[0] = rgba[1] = rgba[2] = grey;
            }
        slot->texture =
            SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, slot->w, slot->h);
        if (slot->texture) {
            SDL_UpdateTexture(slot->texture, nullptr, data, slot->w * 4);
            SDL_SetTextureBlendMode(slot->texture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(slot->texture, SDL_ScaleModeLinear);
        }
    }
    stbi_image_free(data);
    if (!slot->texture && !custom)
        ++resource_errors;
    return slot;
}
void picture(std::string_view name, int x, int y, int w, int h, Uint8 alpha = 255, bool monochrome = false) {
    auto* entry = image(name, monochrome);
    if (!entry || !entry->texture)
        return;
    const float scale = std::min(static_cast<float>(w) / static_cast<float>(entry->w),
                                 static_cast<float>(h) / static_cast<float>(entry->h));
    const int dw = static_cast<int>(static_cast<float>(entry->w) * scale),
              dh = static_cast<int>(static_cast<float>(entry->h) * scale);
    SDL_Rect dest{x + (w - dw) / 2, y + (h - dh) / 2, dw, dh};
    SDL_SetTextureAlphaMod(entry->texture, alpha);
    SDL_RenderCopy(renderer, entry->texture, nullptr, &dest);
}
struct CasePoint { float x, y, z; };
struct CaseView {
    float yaw, pitch, scale = 1, x = 0, y = 0;
    SDL_FPoint project(CasePoint p) const {
        const float rx = p.x * std::cos(yaw) + p.z * std::sin(yaw);
        const float rz = -p.x * std::sin(yaw) + p.z * std::cos(yaw);
        const float ry = p.y * std::cos(pitch) - rz * std::sin(pitch);
        const float depth = p.y * std::sin(pitch) + rz * std::cos(pitch);
        const float perspective = 4.f / (4.f - depth);
        return {x + rx * perspective * scale, y - ry * perspective * scale};
    }
};
void case_quad(const CaseView& view, const std::array<CasePoint, 4>& corners, SDL_Color tint,
               SDL_Texture* texture = nullptr, SDL_FRect uv = {0, 0, 1, 1}, int divisions = 1, int bottom_alpha = -1) {
    constexpr int maximum = 8;
    const int n = std::max(1, std::min(maximum, divisions)), stride = n + 1;
    SDL_Vertex vertices[(maximum + 1) * (maximum + 1)]{};
    int indices[maximum * maximum * 6];
    for (int row = 0; row <= n; ++row) for (int column = 0; column <= n; ++column) {
        const float u = static_cast<float>(column) / n, v = static_cast<float>(row) / n;
        CasePoint p;
        p.x = (corners[0].x * (1-u) + corners[1].x * u) * (1-v) + (corners[3].x * (1-u) + corners[2].x * u) * v;
        p.y = (corners[0].y * (1-u) + corners[1].y * u) * (1-v) + (corners[3].y * (1-u) + corners[2].y * u) * v;
        p.z = (corners[0].z * (1-u) + corners[1].z * u) * (1-v) + (corners[3].z * (1-u) + corners[2].z * u) * v;
        auto& vertex = vertices[row * stride + column];
        vertex.position = view.project(p);
        vertex.color = tint;
        if (bottom_alpha >= 0) vertex.color.a = static_cast<Uint8>(tint.a * (1-v) + bottom_alpha * v);
        vertex.tex_coord = {uv.x + uv.w * u, uv.y + uv.h * v};
    }
    int at = 0;
    for (int row = 0; row < n; ++row) for (int column = 0; column < n; ++column) {
        const int a = row * stride + column, b = a + 1, c = b + stride, d = a + stride;
        for (int index : {a,b,c,a,c,d}) indices[at++] = index;
    }
    if (texture) { SDL_SetTextureAlphaMod(texture, 255); SDL_SetTextureColorMod(texture, 255, 255, 255); }
    if (SDL_RenderGeometry(renderer, texture, vertices, stride * stride, indices, at) < 0) ++resource_errors;
}
void case_front(const CaseView& view, float left, float top, float right, float bottom, float z, SDL_Color tint,
                SDL_Texture* texture = nullptr, int divisions = 1) {
    case_quad(view, {{{left,top,z},{right,top,z},{right,bottom,z},{left,bottom,z}}}, tint, texture, {0,0,1,1}, divisions);
}
void plastic_case_shell(const CaseView& view, const CaseProfile& profile, float cw, float depth, Uint8 alpha) {
    // A rounded solid, with lighting derived from its surface normals. The
    // narrow curved shoulder catches light; the flat black plastic stays dark.
    constexpr int samples = 10;
    constexpr float radius = .007f;
    const int count = view.scale > 150 ? samples : 4;
    const float half[3] = {cw / 2, .5f, depth / 2};
    const auto coordinate = [&](int axis, int index) {
        const float edge = half[axis] - radius;
        if (index < count/2) return -edge - radius * std::cos(index * 1.57079633f / (count/2-1));
        return edge + radius * std::sin((index-count/2) * 1.57079633f / (count/2-1));
    };
    const int axes[3][3] = {{0,2,1},{1,0,2},{2,0,1}};
    const float signs[3] = {-1,1,1};
    for (int face = 0; face < 3; ++face) {
        SDL_Vertex vertices[samples*samples]{};
        int indices[(samples-1)*(samples-1)*6];
        const int normal_axis = axes[face][0], u_axis = axes[face][1], v_axis = axes[face][2];
        for (int row = 0; row < count; ++row) for (int column = 0; column < count; ++column) {
            float p[3]{};
            p[normal_axis] = half[normal_axis] * signs[face];
            p[u_axis] = coordinate(u_axis, column); p[v_axis] = coordinate(v_axis, row);
            float core[3], n[3];
            for (int a = 0; a < 3; ++a) {
                core[a] = std::max(-half[a]+radius, std::min(half[a]-radius, p[a]));
                n[a] = p[a] - core[a];
            }
            const float length = std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
            for (int a = 0; a < 3; ++a) { n[a] /= length; p[a] = core[a] + n[a]*radius; }
            const float nx = n[0]*std::cos(view.yaw)+n[2]*std::sin(view.yaw);
            const float rz = -n[0]*std::sin(view.yaw)+n[2]*std::cos(view.yaw);
            const float ny = n[1]*std::cos(view.pitch)-rz*std::sin(view.pitch);
            const float nz = n[1]*std::sin(view.pitch)+rz*std::cos(view.pitch);
            const float diffuse = std::max(0.f, -.345f*nx+.603f*ny+.719f*nz);
            float specular = std::max(0.f, -.176f*nx+.307f*ny+.936f*nz);
            for (int power = 0; power < 6; ++power) specular *= specular;
            float strip = std::min(1.f, std::max(0.f, -.2f*nx-.1f*ny+.975f*nz));
            for (int power = 0; power < 7; ++power) strip *= strip;
            const float gloss = specular * 120.f + strip * 34.f + std::max(0.f, ny)*2.f;
            const auto channel = [&](unsigned char base) { return static_cast<Uint8>(std::min(255.f, base*(.45f+.55f*diffuse)+gloss)); };
            auto& vertex = vertices[row*count+column];
            vertex.position = view.project({p[0],p[1],p[2]});
            vertex.color = {channel(profile.red),channel(profile.green),channel(profile.blue),alpha};
        }
        int at = 0;
        for (int row = 0; row < count-1; ++row) for (int column = 0; column < count-1; ++column) {
            const int a = row*count+column, b = a+1, c = b+count, d = a+count;
            for (int index : {a,b,c,a,c,d}) indices[at++] = index;
        }
        if (SDL_RenderGeometry(renderer,nullptr,vertices,count*count,indices,at) < 0) ++resource_errors;
    }
}
void case_spine_title(const CaseView& view, const LibraryGame& game, float left, float depth, Uint8 alpha) {
    auto* f = font(20, true);
    if (!f) return;
    std::string_view name(game.name);
    const auto region = name.find(" (");
    if (region != name.npos) name = std::string_view(name.data(), region);
    const float scale = std::min(.76f / std::max(1, text_width(name, 20, true)), depth * .68f / 20.f);
    const float atlas = f->size <= 35 ? 512.f : 1024.f;
    float cursor = 0;
    for (std::size_t at = 0; at < name.size();) {
        unsigned c = codepoint(name, at);
        if (c < 32 || c > 255) c = '?';
        const auto& g = f->chars[c - 32];
        const float y1 = .38f - (cursor + g.xoff) * scale;
        const float y2 = y1 - (g.x1 - g.x0) * scale;
        const float z1 = (g.yoff + 7) * scale, z2 = z1 + (g.y1 - g.y0) * scale;
        case_quad(view, {{{left,y1,z1},{left,y2,z1},{left,y2,z2},{left,y1,z2}}}, {231,232,225,alpha}, f->texture,
                  {g.x0 / atlas, g.y0 / atlas, (g.x1-g.x0) / atlas, (g.y1-g.y0) / atlas});
        cursor += g.xadvance;
    }
}
void draw_game_case(const LibraryGame& game, Art* entry, int x, int y, int w, int h, Uint8 alpha) {
    const auto profile = game_case_profile(game, static_cast<float>(entry->w) / entry->h);
    const bool large = h > 100, cardboard = profile.material == CaseMaterial::cardboard;
    const float cw = profile.width / profile.height, depth = profile.depth / profile.height;
    const float left = -cw / 2, right = cw / 2, front = depth / 2, rear = -front;
    CaseView view{cardboard ? .4f : .52f, .11f};
    float minx = 100, maxx = -100, miny = 100, maxy = -100;
    for (float px : {left, right}) for (float py : {-.5f, .5f}) for (float pz : {rear, front}) {
        const auto p = view.project({px, py, pz});
        minx = std::min(minx, p.x); maxx = std::max(maxx, p.x);
        miny = std::min(miny, p.y); maxy = std::max(maxy, p.y);
    }
    view.scale = std::min(w * .91f / (maxx-minx), h * (large ? .86f : .95f) / (maxy-miny));
    view.x = x + (w - (maxx-minx) * view.scale) / 2 - minx * view.scale;
    view.y = y + h * (large ? .88f : .98f) - maxy * view.scale;
    const auto floor = view.project({0,-.5f,0});
    const int shadow_width = static_cast<int>((maxx-minx) * view.scale * .57f);
    if (large) {
        const int radius = std::max(10, h / 36);
        for (int row = -radius; row <= radius; ++row) {
            const float fade = 1.f - static_cast<float>(row * row) / ((radius+1) * (radius+1));
            const int half = static_cast<int>(shadow_width * std::sqrt(std::max(0.f, fade)));
            line(static_cast<int>(floor.x) - half, static_cast<int>(floor.y) + row + radius / 2,
                 static_cast<int>(floor.x) + half, static_cast<int>(floor.y) + row + radius / 2,
                 {0,0,0,static_cast<Uint8>(40 * fade * alpha / 255)});
        }
        SDL_Rect clip{x,y,w,h}; SDL_RenderSetClipRect(renderer, &clip);
        case_quad(view, {{{left,-.51f,front},{right,-.51f,front},{right,-.67f,front},{left,-.67f,front}}},
                  {158,174,177,static_cast<Uint8>(80u * alpha / 255u)}, entry->texture, {0,1,1,-.16f}, 8, 0);
        SDL_RenderSetClipRect(renderer, nullptr);
    }
    const auto shade = [&](float factor) -> SDL_Color {
        return {static_cast<Uint8>(std::min(255.f, profile.red * factor)),
                static_cast<Uint8>(std::min(255.f, profile.green * factor)),
                static_cast<Uint8>(std::min(255.f, profile.blue * factor)),alpha};
    };
    const bool plastic = profile.material == CaseMaterial::plastic;
    const float shoulder = plastic ? .008f : 0;
    if (plastic) plastic_case_shell(view, profile, cw, depth, alpha);
    else {
        case_quad(view, {{{left,.5f,rear},{left,.5f,front},{left,-.5f,front},{left,-.5f,rear}}}, shade(.55f));
        case_quad(view, {{{left,.5f,rear},{right,.5f,rear},{right,.5f,front},{left,.5f,front}}}, shade(1.35f));
        case_front(view, left, .5f, right, -.5f, front, shade(.88f));
    }
    const std::array<CasePoint,4> spine{{{left,.5f-shoulder,rear+shoulder},{left,.5f-shoulder,front-shoulder},
                                       {left,-.5f+shoulder,front-shoulder},{left,-.5f+shoulder,rear+shoulder}}};
    if (game.artwork_spine_ready) {
        char spine_key[32]; spine_name(game, spine_key);
        auto* spine_art = image(spine_key);
        if (spine_art && spine_art->texture) case_quad(view, spine, {197,203,208,alpha}, spine_art->texture, {0,0,1,1}, large ? 8 : 1);
    } else if (large) case_spine_title(view, game, left-.0005f, depth, static_cast<Uint8>(190u * alpha / 255u));
    const float rim = cardboard ? .004f : plastic ? .009f : .014f;
    const float aspect = static_cast<float>(entry->w) / entry->h;
    const float art_height = std::min(1 - rim*2, (cw - rim*2) / aspect), art_width = art_height * aspect;
    case_front(view, -art_width/2, art_height/2, art_width/2, -art_height/2, front+.0005f,
               {255,255,255,alpha}, entry->texture, large ? 8 : 1);
    if (!plastic) {
        const Uint8 edge = static_cast<Uint8>((cardboard ? 43u : 130u) * alpha / 255u);
        case_front(view, left+.002f, .496f, left+rim*.72f, -.496f, front+.001f, {215,226,235,edge});
        case_front(view, left+.004f, .499f, right-.004f, .496f, front+.001f, {239,242,240,edge});
        case_front(view, right-.004f, .496f, right-.001f, -.496f, front+.001f, {16,21,24,alpha});
    }
}
void game_picture(const LibraryGame& game, int x, int y, int w, int h, Uint8 alpha = 255) {
    const auto fallback = catalog[game.system].image;
    if (!game.artwork_ready) { picture(fallback, x, y, w, h, alpha); return; }
    char name[32]; artwork_name(game, name);
    auto* entry = image(name);
    if (!entry || !entry->texture) { picture(fallback, x, y, w, h, alpha); return; }
    const auto profile = game_case_profile(game, static_cast<float>(entry->w) / entry->h);
    if (game.artwork_kind != ArtworkKind::front || profile.material == CaseMaterial::none) {
        // A supplied 3D case is complete artwork. Icons and screenshots retain
        // their original proportions until a real front cover is available.
        picture(name, x, y, w, h, alpha); return;
    }
    if (h > 100 && SDL_RenderTargetSupported(renderer)) {
        const auto key = game_favorite_key(game);
        ++case_age;
        CaseFrame* slot = nullptr;
        for (auto& frame : case_frames)
            if (frame.texture && frame.key == key && frame.w == w && frame.h == h && frame.spine == game.artwork_spine_ready)
                slot = &frame;
        if (!slot) {
            slot = &case_frames[0];
            for (auto& frame : case_frames) if (frame.age < slot->age) slot = &frame;
            if (slot->texture) SDL_DestroyTexture(slot->texture);
            slot->texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, w*2, h*2);
            if (slot->texture) {
                auto* target = SDL_GetRenderTarget(renderer);
                SDL_Rect viewport, clip;
                SDL_RenderGetViewport(renderer, &viewport); SDL_RenderGetClipRect(renderer, &clip);
                const bool clipped = SDL_RenderIsClipEnabled(renderer) == SDL_TRUE;
                if (SDL_SetRenderTarget(renderer, slot->texture) == 0) {
                    SDL_RenderSetClipRect(renderer, nullptr);
                    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0); SDL_RenderClear(renderer);
                    draw_game_case(game, entry, 0, 0, w*2, h*2, 255);
                    SDL_SetRenderTarget(renderer, target);
                    SDL_RenderSetViewport(renderer, &viewport); SDL_RenderSetClipRect(renderer, clipped ? &clip : nullptr);
                    SDL_SetTextureBlendMode(slot->texture, SDL_BLENDMODE_BLEND);
                    SDL_SetTextureScaleMode(slot->texture, SDL_ScaleModeLinear);
                    slot->key = key; slot->w = w; slot->h = h; slot->spine = game.artwork_spine_ready;
                } else {
                    SDL_DestroyTexture(slot->texture); slot->texture = nullptr;
                }
            }
        }
        if (slot->texture) {
            slot->age = case_age;
            SDL_SetTextureAlphaMod(slot->texture, alpha);
            SDL_Rect destination{x,y,w,h}; SDL_RenderCopy(renderer, slot->texture, nullptr, &destination);
            return;
        }
    }
    draw_game_case(game, entry, x, y, w, h, alpha);
}
void shelf_picture(std::string_view name, int cx, int base, int w, int h, bool selected, int floor_top, int floor_end,
                   bool monochrome, Uint8 alpha = 255, float display_scale = 1.f) {
    auto* entry = image(name, monochrome);
    if (!entry || !entry->texture)
        return;
    const auto& bounds = entry->bounds;
    const float fit = std::min(static_cast<float>(w) / bounds.w, static_cast<float>(h) / bounds.h) *
                      (selected ? 1.045f : 1.f) * display_scale;
    const int dw = static_cast<int>(bounds.w * fit), dh = static_cast<int>(bounds.h * fit);
    const int feet = static_cast<int>((entry->support_y - bounds.y) * fit);
    SDL_Rect dest{cx - dw / 2, base - feet, dw, dh};
    // Use the illustration's silhouette so both the rear console and its lower
    // controller cast a contact shadow, each at its own height on the wood.
    // Clipping to the horizontal surface keeps shadows off the wall/fascia.
    SDL_Rect floor{80, floor_top, 1760, floor_end - floor_top};
    SDL_RenderSetClipRect(renderer, &floor);
    SDL_SetTextureColorMod(entry->texture, 0, 0, 0);
    SDL_Rect shadow = dest;
    shadow.y += 7;
    SDL_SetTextureAlphaMod(entry->texture, static_cast<Uint8>(45u * alpha / 255u));
    SDL_RenderCopy(renderer, entry->texture, &bounds, &shadow);
    shadow.y -= 4;
    SDL_SetTextureAlphaMod(entry->texture, static_cast<Uint8>(105u * alpha / 255u));
    SDL_RenderCopy(renderer, entry->texture, &bounds, &shadow);
    SDL_RenderSetClipRect(renderer, nullptr);
    SDL_SetTextureColorMod(entry->texture, 255, 255, 255);
    // Solid hardware stays opaque; selection uses scale/light rather than
    // allowing the shelf's back edge to show through the console body.
    SDL_SetTextureAlphaMod(entry->texture, alpha);
    SDL_RenderCopy(renderer, entry->texture, &bounds, &dest);
}
void orbit_picture(std::string_view name, double cx, double cy, double w, double h, double yaw, Uint8 alpha, bool monochrome) {
    auto* entry = image(name, monochrome);
    if (!entry || !entry->texture)
        return;
    const auto& bounds = entry->bounds;
    const double fit = std::min(w / bounds.w, h / bounds.h);
    const double hw = bounds.w * fit / 2, hh = bounds.h * fit / 2;
    const double angle = yaw * 3.141592653589793 / 180;
    SDL_Vertex vertices[4]{};
    for (int i = 0; i < 4; ++i) {
        const double x = (i == 0 || i == 3) ? -hw : hw;
        const double y = i < 2 ? -hh : hh;
        const double projection = 1200 / (1200 + x * std::sin(angle));
        vertices[i].position = {static_cast<float>(cx + x * std::cos(angle) * projection),
                                static_cast<float>(cy + y * projection)};
        vertices[i].color = {255, 255, 255, alpha};
        vertices[i].tex_coord = {static_cast<float>(bounds.x + (i == 0 || i == 3 ? 0 : bounds.w)) / entry->w,
                                 static_cast<float>(bounds.y + (i < 2 ? 0 : bounds.h)) / entry->h};
    }
    const int indices[] = {0, 1, 2, 0, 2, 3};
    if (SDL_RenderGeometry(renderer, entry->texture, vertices, 4, indices, 6) < 0)
        ++resource_errors;
}
SDL_Color accent(const SystemDefinition& s) {
    if (s.brand == "Nintendo")
        return {190, 92, 150, 255};
    if (s.brand == "Sega")
        return {70, 140, 230, 255};
    if (s.brand == "PlayStation")
        return {131, 119, 234, 255};
    return {86, 180, 163, 255};
}
void glow(int x, int y, int w, int h, SDL_Color tint, Uint8 alpha = 100) {
    SDL_Rect dest{x, y, w, h};
    SDL_SetTextureColorMod(halo, tint.r, tint.g, tint.b);
    SDL_SetTextureAlphaMod(halo, alpha);
    SDL_RenderCopy(renderer, halo, nullptr, &dest);
}
void icon(int kind, int x, int y, SDL_Color tint = purple) {
    if (kind == 0) {
        for (int a = 0; a < 360; a += 8) {
            const double r = static_cast<double>(a) * 3.141592653589793 / 180;
            rect(x + static_cast<int>(std::cos(r) * 10), y + static_cast<int>(std::sin(r) * 10), 2, 2, tint);
        }
        line(x + 8, y + 8, x + 16, y + 16, tint);
    }
    if (kind == 1) {
        line(x - 13, y - 7, x + 13, y - 7, tint);
        line(x - 13, y + 7, x + 13, y + 7, tint);
        rounded(x - 7, y - 12, 9, 10, 4, tint);
        rounded(x + 2, y + 2, 9, 10, 4, tint);
    }
    if (kind == 2) {
        for (int a = 0; a < 4; ++a)
            rounded(x - 13 + (a % 2) * 15, y - 13 + (a / 2) * 15, 10, 10, 3, tint);
    }
    if (kind == 3) {
        // A continuous cog outline, not a sun/light-mode symbol.
        int previous_x = 0, previous_y = 0;
        for (int a = 0; a <= 32; ++a) {
            const double angle = static_cast<double>(a) * 3.141592653589793 / 16;
            const int radius = a % 4 == 0 || a % 4 == 3 ? 15 : 11;
            const int px = x + static_cast<int>(std::cos(angle) * radius);
            const int py = y + static_cast<int>(std::sin(angle) * radius);
            if (a)
                line(previous_x, previous_y, px, py, tint);
            previous_x = px;
            previous_y = py;
        }
        for (int a = 0; a < 360; a += 8) {
            const double r = static_cast<double>(a) * 3.141592653589793 / 180;
            rect(x + static_cast<int>(std::cos(r) * 5), y + static_cast<int>(std::sin(r) * 5), 2, 2, tint);
        }
    }
    if (kind == 4) {
        rounded(x - 5, y - 13, 10, 26, 3, tint);
        rounded(x - 15, y - 8, 6, 16, 2, tint);
        rounded(x + 9, y - 8, 6, 16, 2, tint);
    }
}
void switch_control(int x, int y, bool on) {
    rounded(x, y, 58, 30, 15, on ? purple : SDL_Color{62, 72, 100, 255});
    rounded(x + (on ? 31 : 4), y + 4, 22, 22, 11, on ? SDL_Color{38, 28, 65, 255} : muted);
}
void label(int x, int y, std::string_view heading, std::string_view value, bool selected, int w = 1032) {
    box(x, y, w, 86, selected);
    text(heading, x + 22, y + 12, 21, muted);
    text(value, x + 22, y + 39, 27, white, true, w - 80);
    text("<  >", x + w - 94, y + 29, 24, selected ? purple : muted);
}
void footer(std::string_view hint, int text_x = 80) {
    rect(0, 1015, width, 65, {9, 12, 26, 245});
    text(hint, text_x, 1032, 21, muted);
}
void splash(float opacity, unsigned time_ms, bool loading, float appearance = 1, AppIcon icon = AppIcon::vector) {
    const auto alpha = static_cast<Uint8>(std::clamp(opacity, 0.0f, 1.0f) * 255);
    rect(0, 0, width, height, {11, 13, 28, alpha});
    const auto ink = alpha;
    const int rise = static_cast<int>(12 * (1 - appearance));
    const int side = static_cast<int>(116 * (.92f + .08f * appearance));
    picture(icon_files[static_cast<std::size_t>(icon)], 960 - side / 2, 347 + rise, side, side, ink);
    centered("isShelf", 960, 505 + rise, 48, {241, 240, 251, ink}, true);
    centered(loading ? "Finding your emulators and games" : "Your systems, together", 960, 578 + rise, 23,
             {180, 174, 205, ink});
    if (loading) {
        for (unsigned dot = 0; dot < 3; ++dot) {
            const auto distance = (dot + 3 - time_ms / 340 % 3) % 3;
            rounded(931 + static_cast<int>(dot) * 24, 639, 9, 9, 4,
                    {194, 177, 255, static_cast<Uint8>(ink / (distance + 1))});
        }
        char hint[80];
        std::snprintf(hint, sizeof(hint), "Hold Circle for %u seconds to close", close_hold_ms / 1000);
        centered(hint, 960, 984, 19, {140, 140, 165, alpha});
    }
}
std::string_view availability(const UiState& state, std::size_t id) {
    const auto& a = state.inventory[id];
    if (state.library && !state.library->scan.complete && !state.library->scan.roots_read)
        return "Library connection unavailable";
    if (a.built_in)
        return "Built-in system";
    if (a.launch == LaunchCapability::direct_game)
        return "Direct game route";
    if (a.launch == LaunchCapability::emulator_menu)
        return "Opens emulator menu";
    if (a.emulator_count)
        return "Emulator detected";
    return "No emulator detected";
}
void tv_lines(std::string_view value, int x, int y, int w, int size, SDL_Color tint = white) {
    if (text_width(value, size, true) <= w) { centered(value, x + w / 2, y, size, tint, true); return; }
    std::size_t cut = value.size();
    while (cut && text_width(std::string_view(value.data(), cut), size, true) > w) --cut;
    const auto space = std::string_view(value.data(), cut).find_last_of(' ');
    if (space != std::string_view::npos && space > cut / 2) cut = space;
    text(std::string_view(value.data(), cut), x, y, size, tint, true, w);
    if (cut < value.size()) {
        if (value[cut] == ' ') ++cut;
        text(std::string_view(value.data() + cut, value.size() - cut), x, y + size + 7, size, tint, true, w);
    }
}
void tv_static(const SDL_Rect& target, bool still) {
    static unsigned last_phase = ~0u;
    const unsigned phase = still ? 0 : render_time / 50;
    const int w = std::min(960, std::max(1, target.w)), h = std::min(540, std::max(1, target.h));
    if (noise_width != w || noise_height != h) {
        if (noise_texture) SDL_DestroyTexture(noise_texture);
        release(noise_pixels);
        noise_pixels = static_cast<Uint32*>(allocate(static_cast<std::size_t>(w) * h * 4));
        noise_texture = noise_pixels ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h) : nullptr;
        noise_width = w; noise_height = h;
        last_phase = ~0u;
    }
    if (!noise_texture || !noise_pixels) return;
    if (phase != last_phase) {
        unsigned random = phase + 4711;
        for (int row = 0; row < h; ++row) {
            const int shade = ((static_cast<unsigned>(row) + phase * 2) % 216 < 24 ? 5 : 0) + (row % 2 ? 3 : 0);
            for (int col = 0; col < w; ++col) {
                random ^= random << 13; random ^= random >> 17; random ^= random << 5;
                const Uint32 level = 62 + ((random & 255) + ((random >> 8) & 255)) * 82 / 510 - shade;
                noise_pixels[row * w + col] = 0xff000000u | level | (level << 8) | (level << 16);
            }
        }
        SDL_UpdateTexture(noise_texture, nullptr, noise_pixels, w * 4);
        last_phase = phase;
    }
    SDL_RenderCopy(renderer, noise_texture, nullptr, &target);
}
void tv_intro_logo(const SDL_Rect& frame, AppIcon icon, Uint8 alpha = 255) {
    // The bundled 640x336 intros have a baked 28px logo at (584,30).
    // An opaque inset badge covers it and draws the current choice without
    // restarting playback or modifying the user's personal intro files.
    const float sx = frame.w / 640.f, sy = frame.h / 336.f;
    const int x = frame.x + static_cast<int>(574 * sx), y = frame.y + static_cast<int>(20 * sy);
    const int w = std::max(1, static_cast<int>(48 * sx)), h = std::max(1, static_cast<int>(48 * sy));
    const auto cover = static_cast<Uint8>(std::min(255, static_cast<int>(alpha) * 255 / 225));
    rounded(x, y, w, h, std::max(1, std::min(w, h) / 7), {4, 12, 15, cover});
    picture(icon_files[static_cast<std::size_t>(icon)], x + static_cast<int>(8 * sx), y + static_cast<int>(8 * sy),
            std::max(1, static_cast<int>(32 * sx)), std::max(1, static_cast<int>(32 * sy)), alpha);
}
bool tv_video(const SystemDefinition& system, std::size_t id, const SDL_Rect& target, bool still, AppIcon icon) {
    bool changed = false;
    if (movie_selected != id) {
        stop_movie(); movie_selected = id; movie_demo = false;
        char path[4096];
        std::snprintf(path, sizeof(path), "%s/intros/%.*s.mpg", room_data, static_cast<int>(system.id.size()), system.id.data());
        bool loaded = room_data[0] && tv_movie.open(path, render_time);
        if (!loaded) {
            std::snprintf(path, sizeof(path), "%s/assets/room/intros/%.*s.mpg", root, static_cast<int>(system.id.size()), system.id.data());
            loaded = tv_movie.open(path, render_time); movie_bundled = loaded;
        }
        if (!loaded) {
            std::snprintf(path, sizeof(path), "%s/assets/room/shelf-intro.mpg", root);
            loaded = tv_movie.open(path, render_time); movie_demo = true;
        }
        if (loaded) {
            movie_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
                                               tv_movie.width(), tv_movie.height());
            changed = true;
        }
    }
    if (!movie_texture) return false;
    changed = tv_movie.advance(render_time, still) || changed;
    if (changed) SDL_UpdateTexture(movie_texture, nullptr, tv_movie.pixels(), tv_movie.width() * 4);
    const float fit = std::min(static_cast<float>(target.w) / tv_movie.width(), static_cast<float>(target.h) / tv_movie.height());
    const int w = static_cast<int>(tv_movie.width() * fit), h = static_cast<int>(tv_movie.height() * fit);
    SDL_Rect fitted{target.x + (target.w - w) / 2, target.y + (target.h - h) / 2, w, h};
    SDL_RenderCopy(renderer, movie_texture, nullptr, &fitted);
    if (movie_bundled) tv_intro_logo(fitted, icon);
    tv_motion = !still && tv_movie.playing();
    if (movie_demo) {
        rect(target.x, target.y + target.h - 30, target.w, 30, {4, 12, 15, 220});
        centered("isShelf demo - add your own console intro", target.x + target.w / 2, target.y + target.h - 24, 16, muted);
    }
    return true;
}
void room_tv(const UiState& state) {
    const auto& p = state.settings.preferences;
    tv_motion = false;
    if (p.tv_mode == TvMode::off) { stop_movie(); return; }
    const int x = room_x(static_cast<float>(room_scene::tv_x)), y = room_y(static_cast<float>(room_scene::tv_y));
    const int w = static_cast<int>(room_scene::tv_width * room_scale()), h = static_cast<int>(room_scene::tv_height * room_scale());
    SDL_Rect clip{x, y, w, h};
    SDL_RenderSetClipRect(renderer, &clip);
    rect(x, y, w, h, {4, 12, 15, 238});
    const auto id = state.selected_system;
    const auto& system = catalog[id];
    const bool eligible = p.intro_all || state.inventory[id].built_in || state.inventory[id].emulator_count;
    const bool noise = tv_noise_for(p, state.inventory[id]);
    if (p.tv_mode != TvMode::video || !eligible || noise) stop_movie();
    if (tv_selected != id) { tv_selected = id; tv_changed_at = render_time; }
    const float reveal = p.tv_mode == TvMode::intro && !p.reduced_motion ?
                         std::min(1.f, static_cast<unsigned>(render_time - tv_changed_at) / 420.f) : 1;
    tv_motion = reveal < 1;
    const auto alpha = static_cast<Uint8>(225 * reveal);
    bool pictured = false;
    if (noise) { tv_static(clip, p.reduced_motion); tv_motion = !p.reduced_motion; pictured = true; }
    if (!noise && p.tv_mode == TvMode::video && eligible)
        pictured = tv_video(system, id, clip, p.reduced_motion || state.presentation.loading, p.icon);
    if (!noise && (p.tv_mode == TvMode::picture || p.tv_mode == TvMode::intro)) {
        char intro[96]; std::snprintf(intro, sizeof(intro), "@intro/%.*s", static_cast<int>(system.id.size()), system.id.data());
        bool bundled = false;
        auto* custom = (p.tv_mode == TvMode::picture || eligible) ? image(p.tv_mode == TvMode::picture ? std::string_view("@room") : std::string_view(intro)) : nullptr;
        if (p.tv_mode == TvMode::intro && eligible && (!custom || !custom->texture)) {
            std::snprintf(intro, sizeof(intro), "assets/room/intros/%.*s.png", static_cast<int>(system.id.size()), system.id.data());
            custom = image(intro); bundled = custom && custom->texture;
        }
        if (custom && custom->texture) {
            SDL_Rect source{0, 0, custom->w, custom->h};
            const float fit = std::max(static_cast<float>(w) / custom->w, static_cast<float>(h) / custom->h);
            source.w = static_cast<int>(w / fit); source.h = static_cast<int>(h / fit);
            source.x = (custom->w - source.w) / 2; source.y = (custom->h - source.h) / 2;
            SDL_SetTextureColorMod(custom->texture, 255, 255, 255);
            SDL_SetTextureAlphaMod(custom->texture, alpha);
            SDL_RenderCopy(renderer, custom->texture, &source, &clip);
            if (bundled) tv_intro_logo({x - static_cast<int>(source.x * fit), y - static_cast<int>(source.y * fit),
                                       static_cast<int>(custom->w * fit), static_cast<int>(custom->h * fit)}, p.icon, alpha);
            pictured = true;
        }
        if (p.tv_mode == TvMode::intro && !eligible) {
            picture(icon_files[static_cast<std::size_t>(p.icon)], x + w / 2 - 38, y + 75, 76, 76, 180);
            centered("Your play room", x + w / 2, y + 174, 25, {205, 224, 221, 200}, true);
            pictured = true;
        }
    }
    if (!pictured && (p.tv_mode == TvMode::console || p.tv_mode == TvMode::intro || p.tv_mode == TvMode::video)) {
        picture(system.image, x + w / 2 - 140, y + 21, 280, h - 97, alpha, use_monochrome(p, state.inventory[id], system.id));
        tv_lines(system.name, x + 32, y + h - 79, w - 64, 25, {233, 249, 246, alpha});
        if (p.tv_mode == TvMode::console) {
            char counts[96]; std::snprintf(counts, sizeof(counts), "%zu games  /  %zu emulator routes",
                                         state.inventory[id].game_count, state.inventory[id].emulator_count);
            centered(counts, x + w / 2, y + h - 39, 17, {166, 202, 197, alpha});
        } else centered(eligible ? "Add your own intro in Play room" : "Intro set to installed consoles only",
                        x + w / 2, y + h - 39, 17, {166, 202, 197, alpha});
    } else if (!pictured && (p.tv_mode == TvMode::picture || p.tv_mode == TvMode::message)) {
        picture(icon_files[static_cast<std::size_t>(p.icon)], x + w / 2 - 31, y + 33, 62, 62, 210);
        tv_lines(state.settings.tv_text, x + 42, y + 129, w - 84, 31, {230, 248, 243, 230});
        if (p.tv_mode == TvMode::picture)
            centered("Add tv-image.png to your isShelf data folder", x + w / 2, y + h - 38, 17, muted);
    } else if (!pictured && p.tv_mode == TvMode::clock) {
        const auto now = std::time(nullptr); const auto* local = std::localtime(&now);
        char clock[16] = "--:--", date[80]{};
        if (local) { std::strftime(clock, sizeof(clock), "%H:%M", local); std::strftime(date, sizeof(date), "%A, %d %B", local); }
        picture(icon_files[static_cast<std::size_t>(p.icon)], x + w / 2 - 27, y + 22, 54, 54, 190);
        centered(clock, x + w / 2, y + 88, 66, {229, 249, 244, 230}, true);
        centered(date, x + w / 2, y + 172, 20, {180, 212, 204, 230});
        tv_lines(state.settings.tv_text, x + 42, y + 222, w - 84, 18, {180, 212, 204, 200});
    } else if (!pictured && p.tv_mode == TvMode::favorites) {
        static const Library* previous_library = nullptr;
        static std::size_t previous_count = 0, count = 0;
        static std::array<std::uint64_t, 64> previous_keys{};
        static std::array<std::size_t, 64> games{};
        if (previous_library != state.library || previous_count != (state.library ? state.library->game_count : 0) ||
            previous_keys != state.settings.game_favorites) {
            previous_library = state.library; previous_count = state.library ? state.library->game_count : 0;
            previous_keys = state.settings.game_favorites; count = 0;
            for (std::size_t i = 0; state.library && i < state.library->game_count && count < games.size(); ++i) {
                const auto key = game_favorite_key(state.library->games[i]);
                if (std::find(previous_keys.begin(), previous_keys.end(), key) != previous_keys.end()) games[count++] = i;
            }
        }
        if (count) {
            const auto& game = state.library->games[games[(render_time / (tv_periods[p.tv_period] * 1000u)) % count]];
            game_picture(game, x + w / 2 - 115, y + 15, 230, 150, 215);
            tv_lines(game.name, x + 35, y + 176, w - 70, 25, {233, 249, 246, 230});
            centered("FROM YOUR FAVOURITES", x + w / 2, y + h - 31, 15, {161, 207, 195, 210}, true);
        } else {
            picture(icon_files[static_cast<std::size_t>(p.icon)], x + w / 2 - 30, y + 41, 60, 60, 200);
            centered("Your favourite games", x + w / 2, y + 136, 27, {233, 249, 246, 230}, true);
            centered("Press Triangle on a game in isShelf's library", x + w / 2, y + 193, 19, {180, 212, 204, 230});
        }
    }
    // Glass, a soft upper reflection and edge falloff keep content inside the photographed screen.
    for (int row = 0; row < 18; ++row) rect(x, y + row, w, 1, {202, 224, 219, static_cast<Uint8>(20 - row)});
    const SDL_Vertex reflection[] = {{{static_cast<float>(x), static_cast<float>(y)}, {221, 232, 228, 13}, {0, 0}},
                                    {{static_cast<float>(x + w * 3 / 5), static_cast<float>(y)}, {221, 232, 228, 5}, {0, 0}},
                                    {{static_cast<float>(x + w / 3), static_cast<float>(y + h)}, {221, 232, 228, 0}, {0, 0}},
                                    {{static_cast<float>(x), static_cast<float>(y + h)}, {221, 232, 228, 7}, {0, 0}}};
    const int triangles[] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(renderer, nullptr, reflection, 4, triangles, 6);
    line(x, y + h - 1, x + w, y + h - 1, {0, 0, 0, 120});
    SDL_RenderSetClipRect(renderer, nullptr);
}
void home(const UiState& state) {
    const auto shown = shown_systems(state);
    const auto& p = state.settings.preferences;
    const bool room = p.layout == Layout::shelf;
    const int header_x = room ? 188 : 80;
    if (room) room_wall_panel();
    text("ONE HOME FOR YOUR SYSTEMS", header_x, 119, 19, room ? SDL_Color{176, 215, 190, 255} : purple, true);
    if (room)
        text("Your console collection", header_x, 154, 50, white, true);
    else {
        text("What do you want", 80, 153, 59, white, true);
        text("to play today?", 80, 218, 59, purple, true);
    }
    const int tool_y = room ? 244 : 314, summary_y = room ? 316 : 391;
    if (room) room_control(header_x, tool_y, 760, 66, state.tools && state.tool == 0);
    else box(80, tool_y, 1560, 66, state.tools && state.tool == 0);
    icon(0, header_x + 31, tool_y + 32, room ? SDL_Color{185, 205, 193, 255} : muted);
    text(state.query[0] ? state.query : "Find a console, brand or core...", header_x + 63, tool_y + 16, 24,
         state.query[0] ? white : room ? SDL_Color{185, 205, 193, 255} : muted, false, room ? 670 : 1420);
    const int layout_x = room ? 968 : 1664;
    if (room) room_control(layout_x, tool_y, 170, 66, state.tools && state.tool == 1);
    else box(layout_x, tool_y, 180, 66, state.tools && state.tool == 1);
    icon(p.layout == Layout::grid ? 4 : 2, layout_x + 32, tool_y + 33);
    text(layout_labels[static_cast<std::size_t>(next_layout(p.layout))], layout_x + 58, tool_y + 17, 24, white, true);
    char summary[160];
    std::snprintf(summary, sizeof(summary), "%s  /  %.*s  /  %zu systems", layout_labels[static_cast<std::size_t>(p.layout)].data(),
                  static_cast<int>(sorts[static_cast<std::size_t>(p.sort)].size()),
                  sorts[static_cast<std::size_t>(p.sort)].data(), shown.count);
    text(summary, room ? 192 : 84, summary_y, 20, room ? SDL_Color{190, 205, 196, 255} : muted);
    if (!shown.count) {
        if (state.library_discovering) {
            centered("Finding your emulators and games", 960, 620, 42, white, true);
            centered("Your systems will appear here as soon as discovery finishes.", 960, 679, 26, muted);
        } else {
            centered("Your shelf is ready for your choices", 960, 620, 42, white, true);
            centered(p.layout == Layout::shelf && p.shelf_single ? "Choose up to 8 consoles in Settings > Consoles." :
                     p.scope == Scope::favorites ? "Add favourites with Triangle, or change the view in Settings." :
                     "Choose All consoles or enable systems in settings.", 960, 679, 26, muted);
        }
        footer("Cross  Choose consoles    Options  Settings    Circle  Back");
        return;
    }
    if (p.layout == Layout::grid) {
        const int base = (state.focus / 8) * 8;
        for (int slot = 0; slot < 8; ++slot) {
            const int at = base + slot;
            if (at >= static_cast<int>(shown.count))
                break;
            const auto id = shown.indices[static_cast<std::size_t>(at)];
            const auto& s = catalog[id];
            const int x = 80 + (slot % 4) * 442, y = 442 + (slot / 4) * 280;
            const bool active = !state.tools && at == state.focus;
            box(x, y, 416, 258, active, {23, 27, 47, 255});
            const bool monochrome = use_monochrome(p, state.inventory[id], s.id);
            glow(x + 30, y + 2, 350, 200, monochrome ? SDL_Color{128, 139, 154, 255} : accent(s), active ? 120 : 62);
            if (p.grid_look == GridLook::icons)
                picture(s.image, x + 20, y + 7, 376, 166, 255, monochrome);
            else {
                text(s.brand, x + 22, y + 25, 22, purple, true);
                text("CONSOLE", x + 22, y + 70, 38, muted, true);
            }
            text(s.name, x + 20, y + 173, 26, white, true, 374);
            text(availability(state, id), x + 20, y + 212, 19, muted, false, 374);
            if (p.favorite[id])
                text("*", x + 377, y + 12, 30, purple, true);
        }
    } else if (p.layout == Layout::shelf) {
        const int first = shelf_motion.page() * 8;
        const int offset = static_cast<int>(shelf_motion.offset());
        const auto opacity = static_cast<Uint8>(255 * shelf_motion.opacity());
        for (int tier = 0; tier < 2; ++tier) {
            if (first + tier * 4 >= static_cast<int>(shown.count))
                break;
            const auto& plane = room_scene::tiers[static_cast<std::size_t>(tier)];
            const int contact = room_y(static_cast<float>(plane.feet));
            const int art_height = static_cast<int>(plane.art_height * room_scale());
            const float column = static_cast<float>(room_scene::right - room_scene::left) / room_scene::columns;
            for (int col = 0; col < 4; ++col) {
                const int at = first + tier * 4 + col;
                if (at >= static_cast<int>(shown.count))
                    break;
                const auto id = shown.indices[static_cast<std::size_t>(at)];
                const auto& system = catalog[id];
                const int cx = room_x(room_scene::left + (col + .5f) * column);
                const bool active = !state.tools && at == state.focus;
                const int hardware_width = static_cast<int>(280 * system.room_scale);
                const int hardware_height = static_cast<int>(art_height * system.room_scale);
                if (active)
                    glow(cx + offset - hardware_width / 2, contact - hardware_height, hardware_width, hardware_height + 10, {125, 223, 211, 255},
                         static_cast<Uint8>(48u * opacity / 255u));
                const int shadow_width = static_cast<int>(222 * system.room_scale);
                glow(cx + offset - shadow_width / 2, contact - 11, shadow_width, 22, {0, 0, 0, 255}, static_cast<Uint8>(225u * opacity / 255u));
                shelf_picture(system.image, cx + offset, contact, static_cast<int>(column * room_scale() * .83f), art_height,
                              active, room_y(static_cast<float>(plane.wood_top)), room_y(static_cast<float>(plane.wood_end)),
                              use_monochrome(p, state.inventory[id], system.id), opacity, system.room_scale);
                if (active)
                    rounded(cx - 31, contact + 8, 62, 3, 1, {150, 235, 219, opacity});
                const int label_y = room_y(static_cast<float>(plane.label));
                centered(system.name, cx + 1, label_y + 2, 20, {0, 0, 0, static_cast<Uint8>(200u * opacity / 255u)}, true);
                centered(system.name, cx, label_y, 20, active ? SDL_Color{white.r, white.g, white.b, opacity} :
                         SDL_Color{219, 225, 238, opacity}, true);
                if (p.favorite[id])
                    text("*", cx + offset + 130, contact - art_height, 25, {purple.r, purple.g, purple.b, opacity}, true);
            }
        }
        char position[64];
        std::snprintf(position, sizeof(position), "%d-%d / %zu", first + 1,
                      std::min(first + 8, static_cast<int>(shown.count)), shown.count);
        text(availability(state, state.selected_system), 192, summary_y + 29, 19, {190, 205, 196, 255}, false, 500);
        text(position, 1008, summary_y, 19, {190, 205, 196, 255});
        char pages[96];
        std::snprintf(pages, sizeof(pages), "%s  %d / %d  %s", state.focus >= 8 ? "L1  <" : "",
                      state.focus / 8 + 1, (static_cast<int>(shown.count) + 7) / 8,
                      state.focus / 8 < (static_cast<int>(shown.count) - 1) / 8 ? ">  R1" : "");
        rounded(790, 919, 340, 52, 22, {12, 19, 20, 220});
        centered(pages, 960, 934, 22, white, true);
    } else {
        const auto& selected = catalog[state.selected_system];
        glow(610, 742, 700, 130, accent(selected), 65);
        // A quiet elliptical floor anchors the curved collection without cards.
        for (int degree = 0; degree < 360; degree += 3) {
            const double a = degree * 3.141592653589793 / 180;
            const double b = (degree + 3) * 3.141592653589793 / 180;
            line(960 + static_cast<int>(std::cos(a) * 770), 748 + static_cast<int>(std::sin(a) * 66),
                 960 + static_cast<int>(std::cos(b) * 770), 748 + static_cast<int>(std::sin(b) * 66),
                 {98, 101, 143, 19});
        }
        struct Slot {
            std::size_t id;
            double offset;
        };
        std::array<Slot, 9> slots{};
        std::size_t count = 0;
        for (std::size_t i = 0; i < shown.count; ++i) {
            const double offset =
                orbit_distance(static_cast<double>(i) - orbit.position, static_cast<double>(shown.count));
            if (std::abs(offset) < 4 && count < slots.size())
                slots[count++] = {shown.indices[i], offset};
        }
        std::sort(slots.begin(), slots.begin() + count,
                  [](const Slot& a, const Slot& b) { return std::abs(a.offset) > std::abs(b.offset); });
        SDL_Rect clip{80, 432, 1760, 420};
        SDL_RenderSetClipRect(renderer, &clip);
        for (std::size_t i = 0; i < count; ++i) {
            const auto pose = orbit_pose(slots[i].offset, 1760);
            const double projection = 1200 / (1200 - pose.z), scale = pose.scale * projection;
            orbit_picture(catalog[slots[i].id].image, 960 + pose.x * projection, 630 + pose.y * projection, 320 * scale,
                          310 * scale, pose.yaw, static_cast<Uint8>(255 * pose.opacity),
                          use_monochrome(p, state.inventory[slots[i].id], catalog[slots[i].id].id));
        }
        SDL_RenderSetClipRect(renderer, nullptr);
        centered(selected.brand, 960, 845, 18, muted);
        centered(selected.name, 960, 873, 38, white, true);
        centered(availability(state, state.selected_system), 960, 930, 21, muted);
        char position[48];
        std::snprintf(position, sizeof(position), "%d / %zu", state.focus + 1, shown.count);
        text(position, 1730, 391, 20, muted);
    }
    footer("D-pad / Stick  Browse    Cross  Open    Triangle  Search    Square  Layout    Options  Settings    "
           "Circle  Back");
}
void detail(const UiState& state) {
    const auto id = state.selected_system;
    const auto& s = catalog[id];
    text("YOUR SYSTEM COLLECTION", 80, 145, 20, purple, true);
    text(s.name, 80, 194, 58, white, true, 1100);
    text(s.brand, 84, 275, 27, muted);
    if (state.page == Page::library && state.inventory[id].game_count) {
        // Selected-game artwork occupies this side of the library below.
    } else if (state.settings.preferences.layout == Layout::shelf) {
        const auto& plane = room_scene::tiers[0];
        const int contact = room_y(static_cast<float>(plane.feet));
        shelf_picture(s.image, room_x(1320), contact, 310, static_cast<int>(plane.art_height * room_scale()), false,
                      room_y(static_cast<float>(plane.wood_top)), room_y(static_cast<float>(plane.wood_end)),
                       use_monochrome(state.settings.preferences, state.inventory[id], s.id), 255, s.room_scale);
    } else {
        glow(1190, 230, 640, 630, accent(s), 110);
        picture(s.image, 1160, 205, 640, 640, 255, use_monochrome(state.settings.preferences, state.inventory[id], s.id));
    }
    text(availability(state, id), 84, 366, 29, purple, true);
    char count[80];
    std::snprintf(count, sizeof(count), "%zu discovered games", state.inventory[id].game_count);
    if (state.page != Page::library)
        text(count, 84, 415, 24, muted);
    if (state.page == Page::library) {
        const auto* route = state.library ? preferred_route(*state.library, id, state.settings.preferred_emulator[id].data()) : nullptr;
        const auto games = state.library ? games_for_system(*state.library, id) : GameList{};
        LaunchRequest preview;
        const auto plan = state.library && state.game_focus >= 0 && static_cast<std::size_t>(state.game_focus) < games.count
                              ? plan_game(*state.library, games.indices[static_cast<std::size_t>(state.game_focus)],
                                          state.settings.preferred_emulator[id].data(), preview)
                              : LaunchPlan::unavailable;
        const auto caption = launch_caption(plan);
        const SDL_Color mode_color = plan == LaunchPlan::menu ? SDL_Color{239, 205, 151, 255}
                                                              : SDL_Color{159, 220, 194, 255};
        rounded(80, 400, 1000, 126, 18, {13, 23, 25, 248});
        rect(80, 420, 4, 86, mode_color);
        text(route ? route->name : state.inventory[id].built_in ? "PS5 system" : "Launch information",
             104, 412, 19, {207, 218, 213, 255}, false, 690);
        text(caption.badge, 842, 412, 17, mode_color, true, 220);
        text(caption.action, 104, 440, 29, white, true, 940);
        text(caption.expectation, 104, 482, 20, {215, 222, 219, 255}, false, 940);
        if (!state.library) {
            text("Your library", 84, 554, 38, white, true);
            text("Connect a library to see your games here.", 84, 615, 26, muted);
        } else {
            if (!games.count)
                text("No games found for this console yet.", 84, 558, 26, muted);
            const int start = state.game_focus / 8 * 8;
            for (int row = 0; row < 8 && static_cast<std::size_t>(start + row) < games.count; ++row) {
                const auto& game = state.library->games[games.indices[static_cast<std::size_t>(start + row)]];
                const int y = 546 + row * 52;
                box(80, y, 1000, 46, !state.tools && state.game_focus == start + row);
                game_picture(game, 91, y + 2, 40, 42);
                text(game.name, 147, y + 7, 23, white, !state.tools && state.game_focus == start + row, 850);
                const auto key = game_favorite_key(game);
                if (std::find(state.settings.game_favorites.begin(), state.settings.game_favorites.end(), key) != state.settings.game_favorites.end())
                    text("*", 1026, y + 9, 27, purple, true);
            }
            if (games.count) {
                char position[80];
                std::snprintf(position, sizeof(position), "%d / %zu", state.game_focus + 1, games.count);
                text(position, 84, 973, 19, muted);
                const auto selected_index = std::min(games.count - 1, static_cast<std::size_t>(std::max(0, state.game_focus)));
                const auto& selected = state.library->games[games.indices[selected_index]];
                rounded(1150, 254, 690, 730, 28, {10, 18, 21, 255});
                text("YOUR GAME COLLECTION", 1190, 281, 18, {170, 199, 190, 255}, true, 600);
                glow(1220, 390, 540, 450, accent(s), 46);
                game_picture(selected, 1190, 327, 610, 550);
                text(selected.name, 1190, 916, 25, white, true, 600);
                text(selected.artwork_ready ? "From your collection" : "Artwork not available yet",
                     1190, 953, 18, muted, false, 600);
            }
        }
        char actions[240];
        std::snprintf(actions, sizeof(actions), "Cross  %s    D-pad  Browse    Left / Right  Page    Triangle  Favourite    Circle  Back", caption.action);
        footer(actions);
        return;
    }
    const auto& route = state.settings.preferred_emulator[id];
    const auto* detected = state.library ? preferred_route(*state.library, id, route.data()) : nullptr;
    label(80, 515, "Preferred emulator",
          state.inventory[id].built_in ? std::string_view("PS5 system")
          : detected                   ? std::string_view(detected->name)
                                       : std::string_view("No emulator detected"),
          !state.tools && state.detail_focus == 0, 920);
    box(80, 631, 443, 82, !state.tools && state.detail_focus == 1);
    text("Open emulator library", 107, 652, 27,
         state.inventory[id].emulator_count ? white : muted, true);
    box(547, 631, 453, 82, !state.tools && state.detail_focus == 2);
    text("Show game library", 575, 652, 29, white, true);
    box(80, 750, 920, 82, !state.tools && state.detail_focus == 3);
    const bool tile_status_known = state.tile_observation_system == id;
    const bool tile_removing = state.tile_removing[id] || state.tile_removal_batch_ready;
    const bool tile_removal_handoff = state.tile_removal_batch_ready ||
                                      state.tile_removal_handoff[id];
    text(!console_tile_writes_enabled ? "Home shortcut opening options"
         : tile_removing && tile_removal_handoff ? "Apply queued Home Screen removals"
         : tile_removing ? "Finish PS5 Home Screen removal"
         : !tile_status_known ? "Manage PS5 Home Screen shortcut"
         : state.tile_installed[id] ? "Edit PS5 Home Screen shortcut" : "Add to PS5 Home Screen",
         107, 771, 27, white, true);
    text(!console_tile_writes_enabled ? "Save where this console's Home tile opens. Tiles are installed separately."
         : tile_removing && tile_removal_handoff
             ? "Close isShelf once so ShadowMount can apply every queued removal."
         : state.tile_removing[id] ? "Open to verify system removal and clean the recovery copy."
         : !tile_status_known ? "Check, add, update or remove this console's shortcut."
                                   : "Choose where this console's shortcut opens.",
         84, 859, 23, muted);
    footer("D-pad  Choose action    Cross  Select    Triangle  Favourite    Circle  Back    Options  Settings");
}
void console_tile(const UiState& state) {
    const auto id = state.selected_system;
    const auto& system = catalog[id];
    const bool isshelf_only = console_tile_isshelf_only(id);
    const bool removing = state.tile_removing[id] || state.tile_removal_batch_ready;
    const bool removal_handoff = state.tile_removal_batch_ready ||
                                 state.tile_removal_handoff[id];
    text("PS5 HOME SCREEN", 80, 145, 20, purple, true);
    text(system.name, 80, 194, 58, white, true, 1100);
    text(console_tile_writes_enabled && removing
             ? removal_handoff
                 ? "This shortcut is queued for removal."
                 : "This shortcut is being removed."
             : isshelf_only ? "This native shortcut always opens in isShelf."
             : !console_tile_writes_enabled ? "Where should this console shortcut open?"
                                            : "Where should this console shortcut open?",
         84, 287, 28, muted);
    glow(1230, 310, 540, 540, accent(system), 95);
    picture(system.image, 1200, 310, 600, 600);
    if (!console_tile_writes_enabled) {
        LaunchRequest menu_request;
        const bool emulator_found = state.launch_connected && state.library &&
                                    plan_emulator(*state.library, id,
                                                  state.settings.preferred_emulator[id].data(), menu_request) !=
                                        LaunchPlan::unavailable;
        const bool shared_retroarch = emulator_found &&
            !console_tile_emulator_menu_selects_system(menu_request.title);
        const bool emulator_available = emulator_found && !shared_retroarch;
        const std::array<const char*, 3> labels = {
            isshelf_only ? "Open in isShelf" : "isShelf game library", "Direct emulator menu", "isShelf home"};
        const std::array<const char*, 3> descriptions = {
            isshelf_only ? "Show this native console's games inside isShelf."
                         : "Show games from this console inside isShelf.",
            shared_retroarch ? "RetroArch cannot start with only this console selected; use isShelf library."
            : emulator_available ? "Open this console's preferred installed emulator."
                               : "Available when a supported emulator is installed.",
            "Show isShelf with this console selected."};
        const int destination_count = isshelf_only ? 1 : 3;
        for (int choice = 0; choice < destination_count; ++choice) {
            const int y = 364 + choice * 124;
            box(80, y, 1000, 104, state.tile_focus == choice);
            text(labels[static_cast<std::size_t>(choice)], 112, y + 15, 29,
                 choice == 1 && !emulator_available ? muted : white, true);
            text(descriptions[static_cast<std::size_t>(choice)], 112, y + 60, 20, muted);
        }
        // Keep the observation and explanation on an opaque card. The room
        // floor begins below this panel; bare text there loses contrast.
        const int status_y = isshelf_only ? 510 : 742;
        box(80, status_y, 1000, 189, false);
        const auto& saved = state.settings.shortcut_routes[id];
        const char* saved_name = saved.input != tile_route::Input::valid ? "No choice saved yet"
            : isshelf_only ? "Saved: Open in isShelf"
            : saved.preference.destination == ConsoleTileDestination::library ? "Saved: isShelf game library"
            : saved.preference.destination == ConsoleTileDestination::home ? "Saved: isShelf home"
            : "Saved: direct emulator menu";
        text(saved_name, 112, status_y + 15, 25, white, true);
        text("Cross saves a destination. This screen cannot add or remove a Home tile.",
              112, status_y + 55, 19, muted, false, 940);
        text("SHADOWMOUNT TITLE STATUS (READ ONLY)", 112, status_y + 87, 18, purple, true);
        const char* observed = state.tile_busy ? "Checking this title..."
            : state.tile_observation_system == id && state.tile_observation[0]
                ? state.tile_observation
            : state.tile_connected ? "Waiting for this title's status..."
                                   : "ShadowMount status unavailable.";
        text(observed, 112, status_y + 116, 19, muted, false, 940);
        const char* save_line = state.route_save_system == id &&
                                state.route_save_status == RouteSaveStatus::unavailable
            ? "Choice could not be confirmed. Reopen isShelf to check."
            : state.route_save_system == id && state.route_save_status == RouteSaveStatus::saved
                ? "Choice saved for this console's next Home tile launch."
            : state.route_save_system == id && state.route_save_status == RouteSaveStatus::pending
                ? "Saving this choice..."
                : "A Home tile can be added when shortcut installation is available.";
        text(save_line, 112, status_y + 147, 18, muted, false, 940);
        footer(isshelf_only ? "Cross  Open in isShelf    Circle  Back"
                            : "D-pad  Choose destination    Cross  Save choice    Circle  Back");
        return;
    }
    if (state.tile_emulator_batch_close_ready) {
        char summary[112];
        std::snprintf(summary, sizeof(summary),
                      "Added %zu and updated %zu console tiles.",
                      state.tile_emulator_batch_created,
                      state.tile_emulator_batch_updated);
        text("Standalone emulators open directly; RetroArch systems open their isShelf library.",
             84, 339, 22, muted, false, 1000);
        box(80, 427, 1000, 138, state.tile_focus == 0);
        text("Close isShelf and update PS5 Home", 110, 447, 27, white, true, 940);
        text(summary, 110, 503, 24, white, true);
        text("ShadowMount applies newly queued Home Screen tiles after isShelf closes.",
             84, 610, 21, muted, false, 1000);
        footer("Cross  Close and apply emulator tiles    Circle  Back");
        return;
    }
    if (removing) {
        if (removal_handoff) {
            if (state.tile_residual_sweep) {
                text("The post-close helper will check all 52 fixed isShelf registration slots.",
                     84, 339, 22, muted, false, 1000);
                box(80, 427, 1000, 138, state.tile_focus == 0);
                text("Close isShelf and clean PS5 Home", 110, 447, 27, white, true, 940);
                text("Only exact isShelf metadata and artwork matches will be removed.",
                     110, 503, 21, white, true, 930);
                text("ShadowMount applies each accepted removal after isShelf closes; absent slots are skipped.",
                     84, 610, 20, muted, false, 1000);
                footer("Cross  Close and verify residual tiles    Circle  Back");
                return;
            }
            char queued[96];
            if (state.tile_removal_count == 1)
                std::snprintf(queued, sizeof(queued), "1 isShelf tile is queued.");
            else
                std::snprintf(queued, sizeof(queued), "%zu isShelf tiles are queued.",
                              state.tile_removal_count);
            text("ShadowMount applies Home Screen removals only after isShelf closes.",
                 84, 339, 22, muted);
            box(80, 427, 1000, 138, state.tile_focus == 0);
            text("Close isShelf and update PS5 Home", 110, 447, 27, white, true, 940);
            text(queued, 110, 503, 24, white, true);
            text("Go back to queue more tiles, or press Cross once when you are finished.",
                 84, 610, 21, muted, false, 1000);
            footer("Cross  Close and apply queued removals    Circle  Back");
            return;
        }
        text("isShelf keeps recovery copies until ShadowMount confirms each tile is gone.",
             84, 339, 22, muted);
        box(80, 405, 1000, 90, state.tile_focus == 0);
        text("Verify this tile's removal", 110, 418, 27, white, true, 940);
        text("Finish and clean this console's recovery data", 110, 458, 19, white, true);
        box(80, 510, 1000, 90, state.tile_focus == 1);
        text("Finish all queued removals", 110, 523, 27, white, true, 940);
        text("Rebuild one batch, then close isShelf once so ShadowMount can apply it",
             110, 563, 19, white, true, 930);
        box(80, 615, 1000, 90, state.tile_focus == 2);
        text("Add or repair all emulator tiles", 110, 628, 27, white, true, 940);
        text("Finish confirmed removals, then restore every supported emulator shortcut",
             110, 668, 19, muted, false, 930);
        text("If a registration remains, its recovery copy is kept for a later retry.",
             84, 735, 21, muted, false, 1000);
        footer("D-pad  Choose action    Cross  Select    Circle  Back");
        return;
    }
    const auto* route = state.library
                            ? preferred_route(*state.library, id, state.settings.preferred_emulator[id].data())
                            : nullptr;
    LaunchRequest menu_request;
    const bool emulator_found = state.launch_connected && state.library &&
                                plan_emulator(*state.library, id,
                                              state.settings.preferred_emulator[id].data(), menu_request) !=
                                    LaunchPlan::unavailable;
    const bool shared_retroarch = emulator_found &&
        !console_tile_emulator_menu_selects_system(menu_request.title);
    const bool emulator_available = emulator_found && !shared_retroarch;
    const std::array<const char*, 3> labels = {
        isshelf_only ? "Open in isShelf" : "isShelf game library", "Direct emulator menu", "isShelf home"};
    const std::array<const char*, 3> descriptions = {
        isshelf_only ? "Show this native console's games inside isShelf."
                     : "Show games from this console inside isShelf.",
        shared_retroarch ? "RetroArch cannot start with only this console selected; use isShelf library."
        : emulator_available ? "Open this console's preferred installed emulator."
                           : "Available when a supported emulator is installed.",
        "Show isShelf with this console selected."};
    const int destination_count = isshelf_only ? 1 : 3;
    for (int choice = 0; choice < destination_count; ++choice) {
        const int y = 320 + choice * 96;
        box(80, y, 1000, 80, state.tile_focus == choice);
        text(labels[static_cast<std::size_t>(choice)], 112, y + 7, 25,
             state.tile_removing[id] || (choice == 1 && !emulator_available) ? muted : white, true);
        text(descriptions[static_cast<std::size_t>(choice)], 112, y + 43, 18, muted);
    }
    const bool installed = state.tile_installed[id];
    const int first_action_y = isshelf_only ? 440 : 608;
    if (installed) {
        box(80, first_action_y, 1000, 50, state.tile_focus == 3);
        text("Remove this tile", 112, first_action_y + 9, 23, white, true);
    }
    const int emulator_all_focus = installed ? 4 : 3;
    const int emulator_all_y = isshelf_only
        ? first_action_y + (installed ? 62 : 0)
        : installed ? 670 : 608;
    box(80, emulator_all_y, 1000, 70, state.tile_focus == emulator_all_focus);
    text(state.tile_batch_busy && state.pending_tile.action == ConsoleTileAction::upsert_emulators
             ? "Adding or repairing emulator tiles..."
             : isshelf_only ? "Add or repair all other emulator tiles"
                            : "Add or repair all emulator tiles",
         112, emulator_all_y + 7, 24, white, true);
    text(isshelf_only
             ? "Keeps this native tile in isShelf while repairing supported emulator shortcuts."
             : "Standalone emulators open directly; RetroArch systems open the matching isShelf library.",
         112, emulator_all_y + 39, 17, muted, false, 930);
    const int refresh_focus = installed ? 5 : 4;
    const int refresh_y = isshelf_only ? emulator_all_y + 82 : installed ? 752 : 690;
    box(80, refresh_y, 1000, 70, state.tile_focus == refresh_focus);
    text(state.tile_batch_busy && state.pending_tile.action == ConsoleTileAction::refresh_installed
             ? "Updating installed shortcuts..."
                               : "Update all installed shortcuts",
         112, refresh_y + 7, 24, white, true);
    text("Normalizes unsupported routes; re-registers stale artwork and refreshes the runtime.",
         112, refresh_y + 39, 17, muted, false, 930);
    const int remove_all_focus = installed ? 6 : 5;
    const int remove_all_y = isshelf_only ? refresh_y + 82 : installed ? 834 : 772;
    box(80, remove_all_y, 1000, 82, state.tile_focus == remove_all_focus);
    text(state.tile_batch_busy && state.pending_tile.action == ConsoleTileAction::remove_installed
             ? "Queueing all removals..."
             : "Remove all isShelf tiles",
         112, remove_all_y + 7, 24, white, true);
    text("Queues every owned tile, then closes isShelf once so ShadowMount can update Home.",
         112, remove_all_y + 43, 17, muted, false, 930);
    if (!isshelf_only && route && !emulator_available)
        text(shared_retroarch ? "RetroArch has no selected-system menu launch; use isShelf game library."
                              : "The direct emulator menu is currently unavailable.",
             84, 928, 17, muted);
    footer("D-pad  Choose action    Cross  Select    Circle  Back");
}
void shortcut_route(const UiState& state) {
    const auto id = state.shortcut_route_system;
    if (id >= system_count) {
        text("SHORTCUT UNAVAILABLE", 164, 145, 20, purple, true);
        text("Open the main isShelf tile instead.", 164, 194, 40, white, true);
        footer("Hold Circle  Close isShelf", 164);
        return;
    }
    const auto& system = catalog[id];
    const bool isshelf_only = console_tile_isshelf_only(id);
    text("OPEN FROM PS5 HOME", 164, 145, 20, purple, true);
    text(system.name, 164, 194, 58, white, true, 1000);
    text(isshelf_only ? "This native console tile opens only in isShelf."
                      : "Choose where this console's Home tile opens.",
         168, 287, 28, muted);
    glow(1230, 310, 540, 540, accent(system), 95);
    picture(system.image, 1200, 310, 600, 600);
    ConsoleTileIntent emulator;
    const bool emulator_available = console_tile_route_choice_intent(state, 1, emulator);
    LaunchRequest menu_request;
    const bool emulator_found = state.shortcut_route_ready && state.launch_connected && state.library &&
                                plan_emulator(*state.library, id,
                                              state.settings.preferred_emulator[id].data(), menu_request) !=
                                    LaunchPlan::unavailable;
    const bool shared_retroarch = emulator_found &&
        !console_tile_emulator_menu_selects_system(menu_request.title);
    const std::array<const char*, 3> labels = {
        isshelf_only ? "Open in isShelf" : "isShelf game library", "Direct emulator menu", "isShelf home"};
    const std::array<const char*, 3> descriptions = {
        state.shortcut_route_ready ? isshelf_only
                                         ? "Show this native console's games inside isShelf."
                                         : "Show games from this console inside isShelf."
                                   : "Available when the library scan completes.",
        shared_retroarch ? "RetroArch cannot start with only this console selected; use isShelf library."
        : emulator_available ? "Open this console's preferred installed emulator."
                           : "Available when a supported emulator is installed.",
        "Show isShelf with this console selected."};
    const int destination_count = isshelf_only ? 1 : 3;
    for (int choice = 0; choice < destination_count; ++choice) {
        const int y = 364 + choice * 124;
        box(164, y, 1000, 104, state.shortcut_route_focus == choice);
        const bool enabled = choice == 2 ||
                             (state.shortcut_route_ready && (choice != 1 || emulator_available));
        text(labels[static_cast<std::size_t>(choice)], 196, y + 15, 29,
             enabled ? white : muted, true);
        text(descriptions[static_cast<std::size_t>(choice)], 196, y + 60, 20, muted);
    }
    text(state.shortcut_route_ready ? "This choice is saved, then opens now."
                                    : isshelf_only
                                          ? "Library scan incomplete. Reopen with ftpsrv running."
                                          : "Library scan incomplete. Home can still be saved and opened.",
         168, isshelf_only ? 535 : 783, 22, muted, false, 996);
    footer(isshelf_only ? "Cross  Open in isShelf    Hold Circle  Close isShelf"
                        : "D-pad  Choose destination    Cross  Save and open    Hold Circle  Close isShelf", 164);
}
void settings(const UiState& state) {
    // Keep the room's left light and TV visible around the settings surface.
    constexpr int panel_x = 164, panel_width = 1000;
    constexpr int content_x = 206, content_width = 916;
    const auto setting_label = [&](int y, std::string_view heading, std::string_view value, bool selected) {
        label(content_x, y, heading, value, selected, content_width);
    };
    rect(0, 0, width, height, {4, 6, 16, 180});
    box(panel_x, 76, panel_width, 900, false, {20, 23, 41, 255});
    picture(icon_files[static_cast<std::size_t>(state.settings.preferences.icon)],
            panel_x + panel_width - 124, 126, 76, 76);
    text("MAKE IT YOURS", content_x, 108, 19, purple, true);
    text("isShelf settings", content_x, 139, 43, white, true);
    text("Changes save automatically", content_x + 2, 199, 22, muted);
    const std::array<std::string_view, 4> tabs = {"Appearance", "Sort & filter", "Consoles", "Play room"};
    for (int i = 0; i < 4; ++i) {
        text(tabs[static_cast<std::size_t>(i)], content_x + i * 228, 250, 25,
             static_cast<int>(state.tab) == i ? purple : muted, true);
        if (static_cast<int>(state.tab) == i)
            rounded(content_x + i * 228, 292, 190, 3, 1, purple);
    }
    line(panel_x, 304, panel_x + panel_width, 304, {49, 55, 78, 255});
    const auto& p = state.settings.preferences;
    if (state.tab == SettingsTab::appearance) {
        text("Your shelf, your style", content_x, 338, 34, white, true);
        text("Choose Grid, curved Swipe, or a lit display Shelf.", content_x, 388, 24, muted);
        setting_label(429, "Layout", layout_labels[static_cast<std::size_t>(p.layout)], state.settings_focus == 0);
        setting_label(519, "Grid look", p.layout != Layout::grid ? "Available in Grid layout" :
              p.grid_look == GridLook::icons ? "3D console icons" : "Classic cards",
              state.settings_focus == 1);
        setting_label(609, "isShelf logo", icon_names[static_cast<std::size_t>(p.icon)], state.settings_focus == 2);
        setting_label(699, "Missing emulator artwork", p.monochrome_missing ? "Black & white" : "Full colour",
              state.settings_focus == 3);
        setting_label(789, "Motion", p.reduced_motion ? "Reduced: instant page changes" : "Smooth console transitions",
              state.settings_focus == 4);
    } else if (state.tab == SettingsTab::browse) {
        text("Find your next favourite", content_x, 338, 34, white, true);
        text("Choose an order and narrow down your collection.", content_x, 388, 24, muted);
        setting_label(435, "Sort by", sorts[static_cast<std::size_t>(p.sort)], state.settings_focus == 0);
        setting_label(539, "Brand", p.brand ? brands[p.brand] : "All brands", state.settings_focus == 1);
        setting_label(643, "Availability", filters[static_cast<std::size_t>(p.filter)], state.settings_focus == 2);
        setting_label(747, "Shelf pages", p.shelf_single ? "One curated shelf: choose up to 8 consoles" :
              "Browse collection: 8 consoles per page", state.settings_focus == 3);
        text(p.shelf_single ? "Choose your 8 in Consoles. Grid and Swipe keep your full collection." :
              "L1 / R1 changes pages in Shelf. Your filters apply to every page.", content_x + 6, 855, 21, muted,
             false, content_width - 12);
    } else if (state.tab == SettingsTab::room) {
        text("Your room, your screen", content_x, 338, 34, white, true);
        text("TV content sits behind the glass. Shelf layout only.", content_x, 388, 24, muted);
        setting_label(421, "TV display", tv_labels[static_cast<std::size_t>(p.tv_mode)], state.settings_focus == 0);
        setting_label(505, "Personal message - Cross to edit", state.settings.tv_text, state.settings_focus == 1);
        char interval[64]; std::snprintf(interval, sizeof(interval), "Every %u seconds", tv_periods[p.tv_period]);
        setting_label(589, "Favourite game rotation", interval, state.settings_focus == 2);
        setting_label(673, "Console intros", p.intro_all ? "All consoles" : "Installed emulators and built-in systems", state.settings_focus == 3);
        setting_label(757, "TV static for missing content", noise_labels[static_cast<std::size_t>(p.tv_noise)], state.settings_focus == 4);
        text("Own intros: /data/console-shelf/intros/<console-id>.png or .mpg", content_x + 6, 854, 20, muted,
             false, content_width - 12);
        text("isShelf originals included. Your personal PNG or MPEG-1 clip takes priority.", content_x + 6, 882,
             19, muted, false, content_width - 12);
    } else {
        setting_label(330, "Show on my shelf", p.scope == Scope::all ? "All consoles" :
              p.scope == Scope::available ? "Emulator or games" : "Favourites only",
              state.settings_focus == 0);
        box(content_x, 432, content_width, 58, state.settings_focus == 1);
        icon(0, content_x + 30, 460, muted);
        text(state.console_query[0] ? state.console_query : "Find a console or brand...", content_x + 60, 446, 22,
             state.console_query[0] ? white : muted, false, content_width - 80);
        std::size_t enabled = 0;
        for (std::size_t i = 0; i < system_count; ++i)
            if (p.shelf_single ? p.shelf_selected[i] : !p.hidden[i]) ++enabled;
        char tally[80];
        std::snprintf(tally, sizeof(tally), "%zu of %zu %s", enabled, p.shelf_single ? shelf_capacity : system_count,
                      p.shelf_single ? "display spaces filled" : "enabled");
        text(tally, content_x + 4, 509, 20, muted);
        rounded(697, 500, 194, 42, 10, state.settings_focus == 2 ? purple : SDL_Color{29, 34, 57, 255});
        centered(p.shelf_single ? "Fill spaces" : state.console_query[0] ? "Show results" : "Show all", 794, 507, 20,
                 state.settings_focus == 2 ? panel : muted, true);
        rounded(906, 500, 216, 42, 10, state.settings_focus == 3 ? purple : SDL_Color{29, 34, 57, 255});
        centered(p.shelf_single ? "Clear selection" : state.console_query[0] ? "Hide results" : "Hide all", 1014, 507, 20,
                 state.settings_focus == 3 ? panel : muted, true);
        const auto picker = console_picker(state);
        const int selected = state.settings_focus - 4;
        const int start = std::max(0, selected - 3);
        SDL_Rect clip{content_x - 4, 555, content_width + 8, 321};
        SDL_RenderSetClipRect(renderer, &clip);
        for (int row = 0; row < 5; ++row) {
            const int at = start + row;
            if (at >= static_cast<int>(picker.count))
                break;
            const auto e = picker.entries[static_cast<std::size_t>(at)];
            const int y = 555 + row * 66;
            const bool active = at == selected;
            if (active)
                rounded(content_x, y, content_width, 62, 12, {58, 48, 88, 255});
            if (e.group) {
                text(brands[e.index], content_x + 20, y + 17, 23, purple, true);
                text("Cross: toggle brand", 907, y + 20, 18, muted);
            } else {
                const auto& s = catalog[e.index];
                const bool enabled_item = p.shelf_single ? p.shelf_selected[e.index] : !p.hidden[e.index];
                picture(s.image, content_x + 14, y + 3, 61, 55, enabled_item ? 255 : 100,
                        use_monochrome(p, state.inventory[e.index], s.id));
                text(s.name, content_x + 95, y + 17, 25, enabled_item ? white : muted, true, 730);
                switch_control(1050, y + 17, enabled_item);
            }
        }
        SDL_RenderSetClipRect(renderer, nullptr);
        if (!picker.count)
            centered("No consoles match this search", panel_x + panel_width / 2, 647, 27, muted);
    }
    rect(panel_x + 1, 913, panel_width - 2, 62, {15, 19, 35, 255});
    text("L1 / R1  Sections     D-pad  Choose     Cross / Left / Right  Change     Circle  Done",
         content_x, 929, 20, muted, false, content_width);
}
void keyboard(const UiState& state) {
    rect(0, 0, width, height, {4, 6, 16, 210});
    box(330, 215, 1260, 650, false, {20, 23, 41, 255});
    text(state.keyboard_for_tv ? "Your room message" : "Find a console", 380, 248, 38, white, true);
    box(380, 318, 1160, 66, false);
    text(state.keyboard_for_tv ? state.settings.tv_text : state.keyboard_for_consoles ? state.console_query : state.query,
         407, 333, 29, purple, true);
    for (int i = 0; i < 40; ++i) {
        const int x = 380 + (i % 10) * 117, y = 418 + (i / 10) * 78;
        const bool active = state.keyboard_focus == i;
        rounded(x, y, 105, 64, 12, active ? purple : SDL_Color{35, 40, 63, 255});
        const char key = keyboard_keys[static_cast<std::size_t>(i)];
        char label_text[8] = {key, 0};
        const char* displayed = key == '<'   ? "Del"
                                : key == '_' ? "Space"
                                : key == '!' ? "Clear"
                                : key == '^' ? "Done"
                                             : label_text;
        centered(displayed, x + 52, y + 15, key == '_' || key == '!' || key == '^' ? 19 : 26, active ? panel : white,
                 true);
    }
    text("Cross  Type     Square  Delete     Triangle  Clear     Options / Circle  Done", 384, 783, 22, muted);
}
} // namespace

SDL_Texture* build_room_background(const char* image_name) {
    auto* pixels = static_cast<Uint32*>(allocate(static_cast<std::size_t>(width) * height * 4));
    if (!pixels)
        return nullptr;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const float dx = static_cast<float>(x - 1700) / 1000, dy = static_cast<float>(y + 80) / 850;
            const float glow_amount = std::max(0.f, 1.f - dx * dx - dy * dy);
            const auto r = static_cast<Uint32>(11 + glow_amount * 17), g = static_cast<Uint32>(13 + glow_amount * 8),
                       b = static_cast<Uint32>(28 + glow_amount * 28);
            pixels[y * width + x] = 0xff000000u | (b << 16) | (g << 8) | r;
        }
    // Bake each complete room once; no per-tier image panels or per-frame resize.
    char gallery_path[2304];
    std::snprintf(gallery_path, sizeof(gallery_path), "%s/%s", root, image_name);
    int gw = 0, gh = 0, channels = 0;
    unsigned char* gallery = stbi_load(gallery_path, &gw, &gh, &channels, 4);
    if (gallery && gw > 0 && gh > 0 && gw <= 4096 && gh <= 4096) {
        const float fit = std::max(static_cast<float>(width) / gw, static_cast<float>(height) / gh);
        const float ox = (gw - width / fit) / 2, oy = (gh - height / fit) / 2;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const float sx = std::clamp(ox + (x + .5f) / fit - .5f, 0.f, static_cast<float>(gw - 1));
                const float sy = std::clamp(oy + (y + .5f) / fit - .5f, 0.f, static_cast<float>(gh - 1));
                const int ix = static_cast<int>(sx), iy = static_cast<int>(sy);
                const int nx = std::min(ix + 1, gw - 1), ny = std::min(iy + 1, gh - 1);
                const float fx = sx - ix, fy = sy - iy;
                const float visibility = .88f - .12f * static_cast<float>(y) / height;
                Uint32 output = 0xff000000u;
                for (int channel = 0; channel < 3; ++channel) {
                    const auto sample = [&](int px, int py) {
                        return gallery[(static_cast<std::size_t>(py) * gw + px) * 4 + channel];
                    };
                    const float top = sample(ix, iy) * (1 - fx) + sample(nx, iy) * fx;
                    const float bottom = sample(ix, ny) * (1 - fx) + sample(nx, ny) * fx;
                    const auto base = (pixels[y * width + x] >> (channel * 8)) & 255;
                    const auto value = static_cast<Uint32>((top * (1 - fy) + bottom * fy) * visibility +
                                                           base * (1 - visibility));
                    output |= value << (channel * 8);
                }
                pixels[y * width + x] = output;
            }
    } else
        ++resource_errors;
    stbi_image_free(gallery);
    auto* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, width, height);
    if (texture)
        SDL_UpdateTexture(texture, nullptr, pixels, width * 4);
    release(pixels);
    return texture;
}
bool graphics_init(SDL_Renderer* target, const char* asset_root, const char* data_root) {
    renderer = target;
    if (std::strlen(asset_root) >= sizeof(root))
        return false;
    std::strcpy(root, asset_root);
    if (data_root && std::strlen(data_root) >= sizeof(room_data)) return false;
    std::snprintf(room_data, sizeof(room_data), "%s", data_root ? data_root : "");
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    background = build_room_background(room_scene::base_image);
    shelf_background = build_room_background(room_scene::shelf_image);
    Uint32 halo_pixels[256 * 256];
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) {
            const float dx = static_cast<float>(x - 128) / 128, dy = static_cast<float>(y - 128) / 128;
            const auto a = static_cast<Uint32>(std::max(0.f, 1.f - dx * dx - dy * dy) * 200);
            halo_pixels[y * 256 + x] = (a << 24) | 0xffffffu;
        }
    halo = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, 256, 256);
    if (halo) {
        SDL_UpdateTexture(halo, nullptr, halo_pixels, 256 * 4);
        SDL_SetTextureBlendMode(halo, SDL_BLENDMODE_BLEND);
    }
    return background && shelf_background && halo && font(24, false) && font(27, true);
}
void graphics_render(const UiState& state, unsigned time_ms, bool animate) {
    render_time = time_ms;
    if (state.presentation.artwork) {
        SDL_SetRenderDrawColor(renderer, 11, 13, 28, 255);
        SDL_RenderClear(renderer);
        splash(1, 0, false);
        SDL_RenderPresent(renderer);
        return;
    }
    const bool show_orbit = (!state.presentation.loading || state.presentation.preparing) && state.page == Page::home &&
                            state.settings.preferences.layout == Layout::swipe;
    if (show_orbit) {
        const auto list = shown_systems(state);
        if (!orbit_visible || list.count != orbit_list.count || list.indices != orbit_list.indices) {
            orbit = {};
            orbit.position = orbit.target = state.focus;
            orbit_list = list;
        }
        orbit.select(state.focus, static_cast<double>(list.count), time_ms, !animate || state.settings.preferences.reduced_motion);
        orbit.update(time_ms);
    } else
        orbit.active = false;
    orbit_visible = show_orbit;
    const bool show_shelf = state.page == Page::home && state.settings.preferences.layout == Layout::shelf;
    if (show_shelf) {
        const auto list = shown_systems(state);
        if (!shelf_visible || list.count != shelf_list.count || list.indices != shelf_list.indices)
            shelf_motion.reset(state.focus / 8);
        shelf_motion.select(state.focus / 8, time_ms, !animate || state.settings.preferences.reduced_motion);
        shelf_list = list;
    } else shelf_motion.active = false;
    shelf_visible = show_shelf;
    room_visible = state.settings.preferences.layout == Layout::shelf;
    if (!room_visible) { stop_movie(); tv_motion = false; }
    SDL_SetRenderDrawColor(renderer, 11, 13, 28, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, room_visible ? shelf_background : background, nullptr, nullptr);
    if (!state.presentation.loading || state.presentation.preparing) {
        if (room_visible) room_tv(state);
        const bool settings_surface = state.page == Page::settings ||
                                      (state.page == Page::keyboard && state.keyboard_for_consoles);
        if (!settings_surface) {
            const bool constrained_header = room_visible || state.page == Page::shortcut_route;
            const int brand_x = constrained_header ? 164 : 80;
            picture(icon_files[static_cast<std::size_t>(state.settings.preferences.icon)], brand_x, 30, 50, 50);
            text("isShelf", brand_x + 67, 36, 29, white, true);
            if (state.ui_test_active)
                text("UI TEST CONTROL / L3 + R3 TO STOP", 1260, 42, 18, purple, true);
            else {
#ifdef SHELF_PS5
                text("PS5 / DEVELOPMENT BUILD", 1388, 42, 18, muted, true);
#else
                text("NATIVE DEVELOPMENT BUILD / PC", 1388, 42, 18, muted, true);
#endif
            }
            if (state.page != Page::shortcut_route)
                settings_button(state.tools && state.tool == 2);
            line(brand_x, 99, constrained_header ? 1164 : 1840, 99, {43, 48, 69, 255});
        }
        if (state.page == Page::home)
            home(state);
        else if (state.page == Page::details || state.page == Page::library)
            detail(state);
        else if (state.page == Page::console_tile)
            console_tile(state);
        else if (state.page == Page::shortcut_route)
            shortcut_route(state);
        else if (state.page == Page::settings)
            settings(state);
        else if (state.page == Page::keyboard) {
            if (state.keyboard_for_consoles)
                settings(state);
            keyboard(state);
        }
        if (state.notice[0] && state.presentation.hold <= 0) {
            rounded(300, 931, 1320, 64, 16, {57, 46, 84, 255});
            centered(state.notice, 960, 948, 24, white);
        }
    }
    if (state.presentation.loading)
        splash(1, time_ms, true, state.presentation.appear, state.settings.preferences.icon);
    else if (state.presentation.reveal < 1)
        splash(1 - state.presentation.reveal, time_ms, false, 1, state.settings.preferences.icon);
    if (state.presentation.hold > 0 && state.presentation.close == 0) {
        rounded(660, 909, 600, 88, 18, {30, 30, 48, 245});
        centered("Hold Circle to close isShelf", 960, 924, 24, white);
        rounded(707, 969, 506, 5, 2, {64, 62, 83, 255});
        rounded(707, 969, std::max(1, static_cast<int>(506 * state.presentation.hold)), 5, 2, purple);
    } else if (state.presentation.close_hint > 0 && !state.presentation.loading && state.presentation.close == 0) {
        const float opacity = state.presentation.close_hint;
        rounded(660, 909, 600, 88, 18, {30, 30, 48, static_cast<Uint8>(245 * opacity)});
        centered("Hold Circle to close isShelf", 960, 924, 24, {white.r, white.g, white.b, static_cast<Uint8>(255 * opacity)});
        centered("Keep holding for 3 seconds", 960, 961, 18,
                 {muted.r, muted.g, muted.b, static_cast<Uint8>(255 * opacity)});
    }
    if (state.presentation.close > 0) {
        const auto alpha = static_cast<Uint8>(state.presentation.close * 255);
        rect(0, 0, width, height, {9, 12, 26, alpha});
        picture(icon_files[static_cast<std::size_t>(state.settings.preferences.icon)], 916, 413, 88, 88, alpha);
        centered(state.presentation.launching ? "Opening your selection" : "Closing isShelf", 960, 552, 29,
                 {241, 240, 251, alpha});
    }
    SDL_RenderPresent(renderer);
}
bool graphics_animating() {
    return (orbit_visible && orbit.active) || (shelf_visible && shelf_motion.active) || (room_visible && tv_motion);
}
bool graphics_capture(const char* path) {
    auto* pixels = static_cast<Uint32*>(allocate(static_cast<std::size_t>(width) * height * 4));
    if (!pixels)
        return false;
    const bool okay = SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ABGR8888, pixels, width * 4) == 0 &&
                      stbi_write_png(path, width, height, 4, pixels, width * 4) != 0;
    release(pixels);
    return okay;
}
unsigned graphics_errors() {
    return resource_errors;
}
void graphics_shutdown() {
    stop_movie();
    for (auto& frame : case_frames) {
        if (frame.texture) SDL_DestroyTexture(frame.texture);
        frame = CaseFrame{};
    }
    if (noise_texture) SDL_DestroyTexture(noise_texture);
    noise_texture = nullptr;
    release(noise_pixels); noise_pixels = nullptr; noise_width = noise_height = 0;
    for (auto*& texture : settings_buttons) {
        if (texture)
            SDL_DestroyTexture(texture);
        texture = nullptr;
    }
    for (auto& f : fonts) {
        if (f.texture)
            SDL_DestroyTexture(f.texture);
        f = Font{};
    }
    for (auto& entry : art) {
        if (entry.texture)
            SDL_DestroyTexture(entry.texture);
        entry = Art{};
    }
    if (background)
        SDL_DestroyTexture(background);
    if (shelf_background)
        SDL_DestroyTexture(shelf_background);
    if (halo)
        SDL_DestroyTexture(halo);
    background = nullptr;
    shelf_background = nullptr;
    halo = nullptr;
    renderer = nullptr;
}
} // namespace shelf
