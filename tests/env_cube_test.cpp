// ============================================================================
// BuildCubeFromSphereMap — the projection StarCraft II's Spherical envio
// mappings are read through.
//
// Device-free and fixture-free: the source image is generated so that every
// texel encodes its own coordinates, which turns "did the projection put the
// right texel on the right face" into an arithmetic identity rather than an
// eyeball. The identity being pinned is psuvmapping.fx GenSphericalEnvio:
//
//     uv = lookup.xz * (0.5, -0.5) + 0.5
//
// read backwards. It matters that this is exact and not a fit — the whole
// reason the flat maps are projected into cubes at all is so the shader has
// one lookup, and a projection that only approximately inverts retail's would
// make the 12% of shipped env layers that are spherical quietly wrong.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "renderer/assets/env_cube_texture.h"

#include <cmath>
#include <vector>

using namespace whiteout::flakes;
using whiteout::flakes::renderer::assets::BuildCubeFromSphereMap;
using whiteout::flakes::renderer::assets::SphereMapCubeFaceSize;

namespace {

/// A source whose red channel encodes u and green encodes v, so a fetched
/// texel names the coordinate it was fetched from. Blue is constant so the
/// mip filter has something invariant to preserve.
std::vector<u8> CoordinateRamp(int w, int h) {
    std::vector<u8> px(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            u8* p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
            p[0] = static_cast<u8>((x * 255) / (w - 1));
            p[1] = static_cast<u8>((y * 255) / (h - 1));
            p[2] = 77;
            p[3] = 255;
        }
    }
    return px;
}

/// Byte offset of face @p face's mip 0 in the packed cube. Layer-major,
/// mip-minor, which is the order IGFXDevice::CreateTexture walks.
std::size_t FaceBase(int face, int size, int mips) {
    std::size_t perFace = 0;
    for (int m = 0; m < mips; ++m) {
        const std::size_t s = static_cast<std::size_t>(std::max(1, size >> m));
        perFace += s * s * 4;
    }
    return perFace * static_cast<std::size_t>(face);
}

const u8* Texel(const std::vector<u8>& cube, int face, int size, int mips, int x, int y) {
    return &cube[FaceBase(face, size, mips) + (static_cast<std::size_t>(y) * size + x) * 4];
}

} // namespace

TEST_CASE("a sphere map projects into six cube faces with a full mip chain", "[envcube]") {
    const auto src = CoordinateRamp(64, 64);
    std::vector<u8> cube;
    i32 mips = 0;
    REQUIRE(BuildCubeFromSphereMap(src, 64, 64, 32, cube, mips));

    // 32 -> 1 is six halvings, so six levels plus the base.
    CHECK(mips == 6);

    std::size_t expect = 0;
    for (i32 m = 0; m < mips; ++m) {
        const std::size_t s = static_cast<std::size_t>(std::max(1, 32 >> m));
        expect += s * s * 4;
    }
    CHECK(cube.size() == expect * 6);
}

TEST_CASE("each face centre lands on retail's spherical lookup", "[envcube]") {
    // The identity: a face centre looks along that face's axis, and
    // GenSphericalEnvio maps a direction to (x*0.5+0.5, -z*0.5+0.5) — using x
    // and z ONLY, which is what makes the inverse well defined for every
    // direction rather than only for a hemisphere.
    constexpr int kSize = 32;
    const auto src = CoordinateRamp(64, 64);
    std::vector<u8> cube;
    i32 mips = 0;
    REQUIRE(BuildCubeFromSphereMap(src, 64, 64, kSize, cube, mips));

    struct Case {
        int face;
        float dx, dz; // the face's outward axis, x and z components
    };
    // DDS/D3D face order: +X, -X, +Y, -Y, +Z, -Z.
    const Case cases[] = {
        {0, 1.0f, 0.0f},  {1, -1.0f, 0.0f}, {2, 0.0f, 0.0f},
        {3, 0.0f, 0.0f},  {4, 0.0f, 1.0f},  {5, 0.0f, -1.0f},
    };
    const int c = kSize / 2; // the texel straddling the centre

    for (const auto& k : cases) {
        const float u = k.dx * 0.5f + 0.5f;
        const float v = k.dz * -0.5f + 0.5f;
        const u8* got = Texel(cube, k.face, kSize, mips, c, c);
        INFO("face " << k.face);
        // The ramp encodes the coordinate, so the fetched red/green ARE the uv
        // the projection asked for. Two levels of slack: the centre texel is
        // half a texel off the exact axis, and the fetch is bilinear.
        CHECK(std::abs(static_cast<int>(got[0]) - static_cast<int>(u * 255.0f)) <= 12);
        CHECK(std::abs(static_cast<int>(got[1]) - static_cast<int>(v * 255.0f)) <= 12);
        CHECK(static_cast<int>(got[2]) == 77);
    }
}

TEST_CASE("the poles collapse to the sphere map's centre", "[envcube]") {
    // +Y and -Y look along an axis the mapping ignores entirely, so both face
    // centres must read the SAME texel — the middle of the map. This is the
    // property that makes the projection lossless in the direction that
    // matters and many-to-one in the other, and it is also why these maps
    // read soft near the horizon in the game.
    constexpr int kSize = 32;
    const auto src = CoordinateRamp(64, 64);
    std::vector<u8> cube;
    i32 mips = 0;
    REQUIRE(BuildCubeFromSphereMap(src, 64, 64, kSize, cube, mips));

    const u8* up = Texel(cube, 2, kSize, mips, kSize / 2, kSize / 2);
    const u8* down = Texel(cube, 3, kSize, mips, kSize / 2, kSize / 2);
    // Both sit at the map's centre, u = v = 0.5. Not bit-equal: a face's
    // centre TEXEL is half a texel off the exact axis, and +Y and -Y carry
    // that offset with opposite z signs, so they straddle the centre rather
    // than landing on it. The claim is the collapse, not the tie.
    CHECK(std::abs(static_cast<int>(up[0]) - 127) <= 8);
    CHECK(std::abs(static_cast<int>(up[1]) - 127) <= 8);
    CHECK(std::abs(static_cast<int>(down[0]) - 127) <= 8);
    CHECK(std::abs(static_cast<int>(down[1]) - 127) <= 8);
    // u comes from d.x, which both faces carry with the same sign.
    CHECK(static_cast<int>(up[0]) == static_cast<int>(down[0]));
}

TEST_CASE("a face size is chosen from the source and capped", "[envcube]") {
    // The longer edge, rounded DOWN to a power of two — a cube equator spans
    // 4N texels against the sphere map's ~pi*W, so matching W is the closer of
    // the two neighbouring powers rather than an arbitrary pick.
    CHECK(SphereMapCubeFaceSize(256, 256) == 256);
    CHECK(SphereMapCubeFaceSize(512, 512) == 512);
    CHECK(SphereMapCubeFaceSize(300, 300) == 256);
    // Capped, because the faces are uncompressed RGBA8.
    CHECK(SphereMapCubeFaceSize(1024, 1024) == 512);
    // Non-square takes the longer edge; a tiny source still gets a floor.
    CHECK(SphereMapCubeFaceSize(256, 64) == 256);
    CHECK(SphereMapCubeFaceSize(8, 8) == 16);
}

TEST_CASE("a degenerate source is refused rather than half-built", "[envcube]") {
    std::vector<u8> cube;
    i32 mips = 0;
    const auto src = CoordinateRamp(8, 8);
    CHECK_FALSE(BuildCubeFromSphereMap(src, 0, 8, 16, cube, mips));
    CHECK_FALSE(BuildCubeFromSphereMap(src, 8, 8, 0, cube, mips));
    // Fewer bytes than the declared size: the caller mis-described the image.
    CHECK_FALSE(BuildCubeFromSphereMap(std::span<const u8>(src).subspan(0, 16), 8, 8, 16, cube, mips));
}
