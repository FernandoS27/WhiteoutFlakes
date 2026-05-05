#include "viewcube_atlas.h"
#include <cstring>

namespace WhiteoutDex {
namespace {

constexpr i32 kCellW = 64;
constexpr i32 kCellH = 64;
constexpr i32 kFaces = 6;
constexpr i32 kAtlasW = kCellW * kFaces;
constexpr i32 kAtlasH = kCellH;

struct RGBA { u8 r, g, b, a; };

struct Glyph { u8 rows[7]; };


constexpr Glyph G_F = {{0x0F, 0x01, 0x01, 0x0F, 0x01, 0x01, 0x01}};
constexpr Glyph G_R = {{0x0F, 0x11, 0x11, 0x0F, 0x05, 0x09, 0x11}};
constexpr Glyph G_O = {{0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}};
constexpr Glyph G_N = {{0x11, 0x13, 0x15, 0x19, 0x11, 0x11, 0x11}};
constexpr Glyph G_T = {{0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}};
constexpr Glyph G_B = {{0x0F, 0x11, 0x11, 0x0F, 0x11, 0x11, 0x0F}};
constexpr Glyph G_A = {{0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}};
constexpr Glyph G_C = {{0x1E, 0x01, 0x01, 0x01, 0x01, 0x01, 0x1E}};
constexpr Glyph G_K = {{0x11, 0x09, 0x05, 0x03, 0x05, 0x09, 0x11}};
constexpr Glyph G_L = {{0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x1F}};
constexpr Glyph G_E = {{0x1F, 0x01, 0x01, 0x0F, 0x01, 0x01, 0x1F}};
constexpr Glyph G_I = {{0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}};
constexpr Glyph G_G = {{0x1E, 0x01, 0x01, 0x19, 0x11, 0x11, 0x1E}};
constexpr Glyph G_H = {{0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}};
constexpr Glyph G_P = {{0x0F, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x01}};

struct Label { const Glyph* glyphs[5]; i32 count; };

constexpr Label kLabels[kFaces] = {
    {{&G_F, &G_R, &G_O, &G_N, &G_T},      5},
    {{&G_B, &G_A, &G_C, &G_K, nullptr},   4},
    {{&G_L, &G_E, &G_F, &G_T, nullptr},   4},
    {{&G_R, &G_I, &G_G, &G_H, &G_T},      5},
    {{&G_T, &G_O, &G_P, nullptr, nullptr}, 3},
    {{&G_B, &G_O, &G_T, nullptr, nullptr}, 3},
};

constexpr RGBA kFaceBg[kFaces] = {
    {100, 140, 190, 230},
    {190, 120, 100, 230},
    {100, 180, 120, 230},
    {190, 170, 100, 230},
    {160, 160, 180, 230},
    {140, 130, 120, 230},
};

constexpr RGBA kBorder = {60,  60,  60,  230};
constexpr RGBA kText   = {240, 240, 240, 230};

inline void SetPixel(u8* pixels, i32 x, i32 y, RGBA c) {
    if (x < 0 || y < 0 || x >= kAtlasW || y >= kAtlasH) return;
    u8* p = pixels + (y * kAtlasW + x) * 4;
    p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = c.a;
}

void DrawGlyph2x(u8* pixels, const Glyph& g, i32 ox, i32 oy, RGBA c) {
    for (i32 row = 0; row < 7; ++row) {
        u8 bits = g.rows[row];
        for (i32 col = 0; col < 5; ++col) {
            if (!(bits & (1u << col))) continue;
            i32 x = ox + col * 2;
            i32 y = oy + row * 2;
            SetPixel(pixels, x,     y,     c);
            SetPixel(pixels, x + 1, y,     c);
            SetPixel(pixels, x,     y + 1, c);
            SetPixel(pixels, x + 1, y + 1, c);
        }
    }
}

void FillCell(u8* pixels, i32 cellX, RGBA bg) {
    for (i32 y = 0; y < kCellH; ++y) {
        for (i32 x = 0; x < kCellW; ++x) {
            SetPixel(pixels, cellX + x, y, bg);
        }
    }

    for (i32 x = 0; x < kCellW; ++x) {
        SetPixel(pixels, cellX + x, 0,           kBorder);
        SetPixel(pixels, cellX + x, kCellH - 1,  kBorder);
    }
    for (i32 y = 0; y < kCellH; ++y) {
        SetPixel(pixels, cellX,              y, kBorder);
        SetPixel(pixels, cellX + kCellW - 1, y, kBorder);
    }
}

}

std::vector<u8> GenerateViewCubeAtlas(i32& outW, i32& outH) {
    outW = kAtlasW;
    outH = kAtlasH;
    std::vector<u8> pixels(kAtlasW * kAtlasH * 4);

    constexpr i32 kGlyphW = 10;
    constexpr i32 kGlyphH = 14;
    constexpr i32 kGap    = 2;

    for (i32 face = 0; face < kFaces; ++face) {
        i32 cellX = face * kCellW;
        FillCell(pixels.data(), cellX, kFaceBg[face]);

        const Label& label = kLabels[face];
        i32 textW = label.count * kGlyphW + (label.count - 1) * kGap;
        i32 ox = cellX + (kCellW - textW) / 2;
        i32 oy = (kCellH - kGlyphH) / 2;

        for (i32 i = 0; i < label.count; ++i) {
            DrawGlyph2x(pixels.data(), *label.glyphs[i],
                        ox + i * (kGlyphW + kGap), oy, kText);
        }
    }

    return pixels;
}

}
