// ============================================================================
// Which textures the M3 and MDX exports write.
//
// An export writes the textures its written model reads -- a Reforged footman
// went out with 43 files for 22 references, the rest being the source maps the
// bake had already turned into StarCraft II ones -- and, when a Warcraft III
// export asks, names War3 (Mod)'s copy of a texture instead of writing one.
// Hand-built models and pixels, so a G0 test.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "export_texture_set.h"

#include <whiteout/textures/texture.h>

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

using namespace whiteout::flakes;
namespace m3 = ::whiteout::m3;
namespace mdx = ::whiteout::mdx;
namespace tx = ::whiteout::textures;

namespace {

m3::TextureLayer Textured(std::string path) {
    m3::TextureLayer layer;
    layer.texturePath = std::move(path);
    return layer;
}

mdx::Track<u32> Keys(std::vector<u32> ids) {
    mdx::Track<u32> track;
    track.isUsed = true;
    track.keyCount = ids.size();
    for (std::size_t i = 0; i < ids.size(); ++i)
        track.timestamps.push_back(static_cast<u32>(i * 100));
    track.keys_data = std::move(ids);
    return track;
}

mdx::Texture Named(std::string fileName) {
    mdx::Texture texture;
    texture.fileName = std::move(fileName);
    return texture;
}

/// An RGBA8 picture with structure in every channel, from @p texel(x, y).
template <typename Texel>
tx::Texture Picture(u32 width, u32 height, Texel texel) {
    tx::Texture texture = tx::Texture::create2D(tx::PixelFormat::RGBA8, width, height, 1);
    std::span<u8> out = texture.mipData(0);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const std::array<u8, 4> rgba = texel(x, y);
            std::copy(rgba.begin(), rgba.end(), out.begin() + (y * width + x) * 4);
        }
    }
    return texture;
}

/// Smooth in every channel over 64 texels, so a block average keeps it.
std::array<u8, 4> Swirl(u32 x, u32 y) {
    return {static_cast<u8>(x * 4), static_cast<u8>(y * 4), static_cast<u8>(x * y / 16),
            static_cast<u8>(64 + x + y)};
}

} // namespace

TEST_CASE("an m3's named textures are every layer of every material kind",
          "[export][textures]") {
    m3::Model model;
    m3::StandardMaterial standard;
    standard.diffuseLayer = Textured("Assets/Textures/Body.dds");
    // The renderer binds eleven slots; the file carries eighteen.
    standard.heightLayer = Textured("Assets\\Textures\\Height.DDS");
    standard.alphaLayer2 = m3::TextureLayer{}; // a colour-only carrier names nothing
    model.standardMaterials.push_back(standard);
    m3::DisplacementMaterial displacement;
    displacement.normalMap = Textured("Assets/Textures/Ripple.dds");
    model.displacementMaterials.push_back(displacement);
    m3::LensFlare flare;
    flare.flareMap = Textured("Assets/Textures/Flare.dds");
    model.lensFlareMaterials.push_back(flare);
    m3::DataDrivenMaterial madd;
    madd.texturePaths = {"Assets/Textures/Madd.dds", ""};
    model.dataDrivenMaterials.push_back(madd);

    const std::unordered_set<std::string> named = M3TexturePathsNamed(model);
    CHECK(named == std::unordered_set<std::string>{
                       "assets/textures/body.dds", "assets/textures/height.dds",
                       "assets/textures/ripple.dds", "assets/textures/flare.dds",
                       "assets/textures/madd.dds"});
}

TEST_CASE("renaming an m3 texture repoints every layer that names it", "[export][textures]") {
    m3::Model model;
    m3::StandardMaterial body;
    body.diffuseLayer = Textured("Assets/Textures/Grunt.dds");
    body.alphaLayer1 = Textured("assets\\textures\\GRUNT.dds");
    body.normalLayer = Textured("Assets/Textures/Grunt_norm.dds");
    model.standardMaterials.push_back(body);

    CHECK(RenameM3TexturePath(model, "Assets/Textures/Grunt.dds",
                              "Assets/Textures/war3_Grunt.dds") == 2);
    const m3::StandardMaterial& out = model.standardMaterials.front();
    CHECK(out.diffuseLayer->texturePath == "Assets/Textures/war3_Grunt.dds");
    CHECK(out.alphaLayer1->texturePath == "Assets/Textures/war3_Grunt.dds");
    CHECK(out.normalLayer->texturePath == "Assets/Textures/Grunt_norm.dds");
}

TEST_CASE("an mdx texture nothing reads is dropped and the rest renumbered",
          "[export][textures]") {
    mdx::Model model;
    for (const char* name : {"Specular.dds", "Unused.dds", "Diffuse.dds", "Spare.dds", "Frame.dds"})
        model.textures.push_back(Named(name));

    mdx::Material material;
    mdx::Layer flipbook;
    flipbook.textureId = 2;
    flipbook.textureIdTracks = Keys({2, 4});
    material.layers.push_back(flipbook);
    mdx::Layer reforged;
    reforged.textureId = 2;
    mdx::Layer::SubTexture sub;
    sub.textureId = 4;
    reforged.subTextures.push_back(sub);
    material.layers.push_back(reforged);
    model.materials.push_back(material);
    mdx::ParticleEmitter2 emitter;
    emitter.textureId = 99; // names nothing, before and after
    model.particleEmitters2.push_back(emitter);

    const std::vector<bool> used = MdxTexturesUsed(model);
    CHECK(used == std::vector<bool>{false, false, true, false, true});

    PruneMdxTextures(model, used);
    REQUIRE(model.textures.size() == 2);
    CHECK(model.textures[0].fileName == "Diffuse.dds");
    CHECK(model.textures[1].fileName == "Frame.dds");
    const mdx::Layer& a = model.materials[0].layers[0];
    CHECK(a.textureId == 0);
    CHECK(std::vector<u32>(a.textureIdTracks.keys().begin(), a.textureIdTracks.keys().end()) ==
          std::vector<u32>{0, 1});
    const mdx::Layer& b = model.materials[0].layers[1];
    CHECK(b.textureId == 0);
    CHECK(b.subTextures[0].textureId == 1);
    CHECK(model.particleEmitters2[0].textureId == 99);
    CHECK(MdxTexturesUsed(model) == std::vector<bool>{true, true});
}

TEST_CASE("War3 (Mod) names a texture war3_ and its stem", "[export][textures]") {
    CHECK(War3ModTextureName("Textures\\Grunt.blp") == "war3_Grunt.dds");
    CHECK(War3ModTextureName("ReplaceableTextures/TeamColor/TeamColor00.blp") ==
          "war3_TeamColor00.dds");
    CHECK(War3ModTextureName("Units\\Orc.v2\\gutz") == "war3_gutz.dds");
    CHECK(War3ModTextureName("Textures\\").empty());
}

TEST_CASE("the same picture is recognised across a scale, a different one is not",
          "[export][textures]") {
    const tx::Texture ours = Picture(64, 64, Swirl);
    // What a re-encode at half the size leaves: the block average, off by a few.
    const tx::Texture copy = Picture(32, 32, [&](u32 x, u32 y) {
        std::array<u8, 4> rgba{};
        for (int c = 0; c < 4; ++c) {
            u32 sum = 0;
            for (u32 dy = 0; dy < 2; ++dy)
                for (u32 dx = 0; dx < 2; ++dx)
                    sum += Swirl(x * 2 + dx, y * 2 + dy)[c];
            rgba[c] = static_cast<u8>(std::min<u32>(255, sum / 4 + (x + y) % 3));
        }
        return rgba;
    });
    CHECK(SameTexturePicture(ours, copy));
    CHECK(SameTexturePicture(copy, ours));

    SECTION("another picture of the same shape") {
        const tx::Texture other = Picture(32, 32, [](u32 x, u32 y) { return Swirl(31 - y, x); });
        CHECK_FALSE(SameTexturePicture(ours, other));
    }
    SECTION("the same colour under another alpha") {
        const tx::Texture recut = Picture(32, 32, [&](u32 x, u32 y) {
            std::array<u8, 4> rgba = Swirl(x * 2, y * 2);
            rgba[3] = static_cast<u8>(255 - rgba[3]);
            return rgba;
        });
        CHECK_FALSE(SameTexturePicture(ours, recut));
    }
    SECTION("flat fills compare by their level") {
        const auto fill = [](u8 r, u8 g, u8 b) {
            return Picture(16, 16, [=](u32, u32) { return std::array<u8, 4>{r, g, b, 255}; });
        };
        CHECK(SameTexturePicture(fill(10, 10, 10), fill(12, 11, 10)));
        // Inside the colour-error cap, so only the level says no.
        CHECK_FALSE(SameTexturePicture(fill(10, 10, 10), fill(30, 10, 10)));
    }
    SECTION("a small feature somewhere else") {
        // Eight of 255 off on average, inside the cap; only correlation sees it.
        const auto spot = [](u32 left, u32 top) {
            return Picture(32, 32, [=](u32 x, u32 y) {
                const bool in = x >= left && x < left + 4 && y >= top && y < top + 4;
                const u8 v = in ? 255 : 0;
                return std::array<u8, 4>{v, v, v, 255};
            });
        };
        CHECK(SameTexturePicture(spot(2, 2), spot(2, 2)));
        CHECK_FALSE(SameTexturePicture(spot(2, 2), spot(24, 24)));
    }
    SECTION("a shape that no whole scale reaches") {
        CHECK_FALSE(SameTexturePicture(ours, Picture(64, 32, Swirl)));
        CHECK_FALSE(SameTexturePicture(ours, Picture(48, 48, Swirl)));
    }
}
