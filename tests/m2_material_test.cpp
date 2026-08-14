// M2Material → GPU state, the other device-free half.
//
// Transcribed from CM2SceneRender::SetupMaterial @ 0x100f84a90 and the
// s_gxBlend / s_fogModeList tables dumped from a WoW 6.0.1 client.

#include "renderer/profiles/wow/m2_material.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace whiteout::flakes::renderer::profiles::wow;
namespace gfx = whiteout::flakes::gfx;
using Catch::Approx;

TEST_CASE("M2 blend modes map to the EGxBlend factors", "[m2][material]") {
    // The two that disable blending outright.
    CHECK_FALSE(M2BlendDesc(M2Blend::Opaque).enable);
    CHECK_FALSE(M2BlendDesc(M2Blend::AlphaKey).enable);

    const auto alpha = M2BlendDesc(M2Blend::Alpha);
    CHECK(alpha.enable);
    CHECK(alpha.srcColor == gfx::BlendFactor::SrcAlpha);
    CHECK(alpha.dstColor == gfx::BlendFactor::InvSrcAlpha);

    // NoAlphaAdd is One/One; Add is SrcAlpha/One. Conflating them is the
    // classic M2BLEND bug — s_gxBlend sends 3 to EGxBlend 10, not 3.
    const auto noAlphaAdd = M2BlendDesc(M2Blend::NoAlphaAdd);
    CHECK(noAlphaAdd.srcColor == gfx::BlendFactor::One);
    CHECK(noAlphaAdd.dstColor == gfx::BlendFactor::One);
    const auto add = M2BlendDesc(M2Blend::Add);
    CHECK(add.srcColor == gfx::BlendFactor::SrcAlpha);
    CHECK(add.dstColor == gfx::BlendFactor::One);

    const auto mod = M2BlendDesc(M2Blend::Mod);
    CHECK(mod.srcColor == gfx::BlendFactor::DstColor);
    CHECK(mod.dstColor == gfx::BlendFactor::Zero);
    const auto mod2x = M2BlendDesc(M2Blend::Mod2x);
    CHECK(mod2x.srcColor == gfx::BlendFactor::DstColor);
    CHECK(mod2x.dstColor == gfx::BlendFactor::SrcColor);

    // BlendAdd is premultiplied "over".
    const auto blendAdd = M2BlendDesc(M2Blend::BlendAdd);
    CHECK(blendAdd.srcColor == gfx::BlendFactor::One);
    CHECK(blendAdd.dstColor == gfx::BlendFactor::InvSrcAlpha);

    // Additive modes must leave destination alpha alone.
    CHECK(add.srcAlpha == gfx::BlendFactor::Zero);
    CHECK(add.dstAlpha == gfx::BlendFactor::One);
    CHECK(noAlphaAdd.srcAlpha == gfx::BlendFactor::Zero);
    CHECK(noAlphaAdd.dstAlpha == gfx::BlendFactor::One);
}

TEST_CASE("M2 alpha reference follows the blend mode", "[m2][material]") {
    // Only alpha-key scales with element alpha; that multiply is what stops a
    // fading model from punching holes in itself.
    CHECK(M2AlphaRef(M2Blend::AlphaKey, 1.0f) == Approx(128.0f / 255.0f));
    CHECK(M2AlphaRef(M2Blend::AlphaKey, 0.5f) == Approx(64.0f / 255.0f));
    CHECK(M2AlphaRef(M2Blend::AlphaKey, 0.0f) == Approx(0.0f));

    // Opaque and BlendAdd test nothing at all.
    CHECK(M2AlphaRef(M2Blend::Opaque, 1.0f) == Approx(0.0f));
    CHECK(M2AlphaRef(M2Blend::BlendAdd, 1.0f) == Approx(0.0f));

    // Everything else is the constant, unaffected by element alpha.
    for (auto m : {M2Blend::Alpha, M2Blend::NoAlphaAdd, M2Blend::Add, M2Blend::Mod,
                   M2Blend::Mod2x}) {
        CHECK(M2AlphaRef(m, 1.0f) == Approx(1.0f / 255.0f));
        CHECK(M2AlphaRef(m, 0.25f) == Approx(1.0f / 255.0f));
    }
}

TEST_CASE("M2 fog mode neutralises rather than disables", "[m2][material]") {
    // s_fogModeList = {1, 1, 1, 2, 2, 3, 4, 2}, indexed by M2BLEND.
    CHECK(M2FogModeFor(M2Blend::Opaque) == M2FogMode::FogColor);
    CHECK(M2FogModeFor(M2Blend::AlphaKey) == M2FogMode::FogColor);
    CHECK(M2FogModeFor(M2Blend::Alpha) == M2FogMode::FogColor);
    CHECK(M2FogModeFor(M2Blend::NoAlphaAdd) == M2FogMode::Black);
    CHECK(M2FogModeFor(M2Blend::Add) == M2FogMode::Black);
    CHECK(M2FogModeFor(M2Blend::Mod) == M2FogMode::White);
    CHECK(M2FogModeFor(M2Blend::Mod2x) == M2FogMode::HalfWhite);
    CHECK(M2FogModeFor(M2Blend::BlendAdd) == M2FogMode::Black);

    // Material flag 0x02 wins over all of it.
    CHECK(M2StateFor(M2Blend::Mod, kM2Unfogged, 1.0f).fog == M2FogMode::Disabled);
    CHECK(M2StateFor(M2Blend::Opaque, kM2Unfogged, 1.0f).fog == M2FogMode::Disabled);
}

TEST_CASE("M2 lighting is off for the modulate blends", "[m2][material]") {
    CHECK(M2LightingEnabled(M2Blend::Opaque, 0));
    CHECK(M2LightingEnabled(M2Blend::Add, 0));
    CHECK_FALSE(M2LightingEnabled(M2Blend::Mod, 0));
    CHECK_FALSE(M2LightingEnabled(M2Blend::Mod2x, 0));
    // Flag 0x01 disables it regardless.
    CHECK_FALSE(M2LightingEnabled(M2Blend::Opaque, kM2Unlit));
}

TEST_CASE("M2 depth and cull flags keep their polarity", "[m2][material]") {
    // The depth bits DISABLE, despite the wiki naming them "depthTest" and
    // "depthWrite". Empirically: every opaque-blend material in the corpus has
    // flags == 0, and an opaque creature body cannot draw with depth off. Read
    // as enables, the most common material in the game renders as soup.
    const auto none = M2StateFor(M2Blend::Opaque, 0, 1.0f);
    CHECK(none.depth.depthTest);
    CHECK(none.depth.depthWrite);
    CHECK(none.depth.depthCompare == gfx::CompareOp::LessEqual);

    const auto neither = M2StateFor(M2Blend::Opaque, kM2NoDepthTest | kM2NoDepthWrite, 1.0f);
    CHECK_FALSE(neither.depth.depthTest);
    CHECK_FALSE(neither.depth.depthWrite);

    // 0x10 alone — the second most common value, and what an alpha-key layer
    // that must not occlude the layers behind it carries.
    const auto testOnly = M2StateFor(M2Blend::Alpha, kM2NoDepthWrite, 1.0f);
    CHECK(testOnly.depth.depthTest);
    CHECK_FALSE(testOnly.depth.depthWrite);

    // Two-sided means no culling; the default is back-face culling with a
    // counter-clockwise front face.
    CHECK(M2StateFor(M2Blend::Opaque, 0, 1.0f).raster.cull == gfx::CullMode::Back);
    CHECK(M2StateFor(M2Blend::Opaque, kM2TwoSided, 1.0f).raster.cull == gfx::CullMode::None);
    CHECK(M2StateFor(M2Blend::Opaque, 0, 1.0f).raster.frontCCW);
}

TEST_CASE("M2 raw blend values outside the enum stay in range", "[m2][material]") {
    CHECK(M2BlendFromRaw(0) == M2Blend::Opaque);
    CHECK(M2BlendFromRaw(7) == M2Blend::BlendAdd);
    // Shipped data does not carry these, but a malformed material should draw
    // something rather than index off the end of the tables.
    CHECK(M2BlendFromRaw(8) == M2Blend::Opaque);
    CHECK(M2BlendFromRaw(0xFFFF) == M2Blend::Opaque);
}
