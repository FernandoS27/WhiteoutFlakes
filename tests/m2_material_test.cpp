// M2Material → GPU state, the other device-free half.
//
// Transcribed from CM2SceneRender::SetupMaterial @ 0x100f84a90 and the
// s_gxBlend / s_fogModeList tables dumped from a WoW 6.0.1 client.

#include "renderer/profiles/wow/m2_material.h"
#include "renderer/profiles/wow/m2_surface_table.h"

#include <whiteout/models/m2/m2.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace whiteout::flakes::renderer::profiles::wow;
namespace gfx = whiteout::flakes::gfx;
namespace core = whiteout::flakes::renderer::core;
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

    // ...but only one of them is honoured. SetupMaterial chooses between two
    // GxDSState presets on kM2NoDepthWrite alone — {3,7} and {1,7}, differing in
    // the write bit — and never reads kM2NoDepthTest. So the test stays on even
    // when both bits are set.
    const auto both = M2StateFor(M2Blend::Opaque, kM2NoDepthTest | kM2NoDepthWrite, 1.0f);
    CHECK(both.depth.depthTest);
    CHECK_FALSE(both.depth.depthWrite);
    CHECK(M2StateFor(M2Blend::Opaque, kM2NoDepthTest, 1.0f).depth.depthTest);

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

    // A mirrored actor flips which face is culled — SetupMaterial picks front
    // or back off the reverse-culling bit. Two-sided still wins over both.
    CHECK(M2StateFor(M2Blend::Opaque, 0, 1.0f, true).raster.cull == gfx::CullMode::Front);
    CHECK(M2StateFor(M2Blend::Opaque, kM2TwoSided, 1.0f, true).raster.cull == gfx::CullMode::None);
}

TEST_CASE("M2 modulate blends feed the combiner a constant", "[m2][material]") {
    const whiteout::Vector3f batch{0.25f, 0.5f, 0.75f};
    const whiteout::Vector3f geoset{0.5f, 0.5f, 0.5f};

    // Everything else is the batch colour track times the geoset's.
    const auto plain = M2CombinerInput(M2Blend::Alpha, batch, geoset);
    CHECK(plain.x == Approx(0.125f));
    CHECK(plain.y == Approx(0.25f));
    CHECK(plain.z == Approx(0.375f));

    // Mod and Mod2x ignore both and take SetupMaterial's emissive constant.
    const auto mod = M2CombinerInput(M2Blend::Mod, batch, geoset);
    CHECK(mod.x == Approx(1.0f));
    CHECK(mod.y == Approx(1.0f));
    CHECK(mod.z == Approx(1.0f));

    // 0.5 is the whole point: Mod2x's DstColor/SrcColor blend doubles, so 1.0
    // here renders the surface at twice the client's brightness.
    const auto mod2x = M2CombinerInput(M2Blend::Mod2x, batch, geoset);
    CHECK(mod2x.x == Approx(0.5f));
    CHECK(mod2x.y == Approx(0.5f));
    CHECK(mod2x.z == Approx(0.5f));
}

TEST_CASE("M2 pass assignment follows model alpha times element alpha",
          "[m2][material][classify]") {
    auto surf = [](M2Blend b) {
        M2Surface s;
        s.blend = b;
        return s;
    };

    // At full model alpha the blend mode decides, as it always did.
    CHECK(M2ClassifySurface(surf(M2Blend::Opaque), 1.0f, 1.0f).blend == core::BlendClass::Opaque);
    CHECK(M2ClassifySurface(surf(M2Blend::AlphaKey), 1.0f, 1.0f).blend ==
          core::BlendClass::AlphaKey);
    CHECK(M2ClassifySurface(surf(M2Blend::Alpha), 1.0f, 1.0f).blend ==
          core::BlendClass::Transparent);

    // A *fading* model demotes both opaque classes into the sorted transparent
    // set — this is the delta. BeginDraw multiplies model alpha and element
    // alpha into one scalar and tests that, so a solid model carrying a
    // half-faded batch demotes just the same.
    CHECK(M2ClassifySurface(surf(M2Blend::Opaque), 0.5f, 0.5f).blend ==
          core::BlendClass::Transparent);
    CHECK(M2ClassifySurface(surf(M2Blend::AlphaKey), 0.5f, 0.5f).blend ==
          core::BlendClass::Transparent);
    CHECK(M2ClassifySurface(surf(M2Blend::Opaque), 1.0f, 0.25f).blend ==
          core::BlendClass::Transparent);

    // The threshold is 0.99999, not 1.0 — a model one ulp shy of solid is still
    // solid to the client.
    CHECK(M2ClassifySurface(surf(M2Blend::Opaque), 0.999995f, 1.0f).blend ==
          core::BlendClass::Opaque);
    CHECK(M2ClassifySurface(surf(M2Blend::Opaque), 0.99f, 1.0f).blend ==
          core::BlendClass::Transparent);

    // Culled below 1e-4 of model alpha...
    CHECK_FALSE(M2ClassifySurface(surf(M2Blend::Alpha), 0.00001f, 1.0f).visible);
    CHECK(M2ClassifySurface(surf(M2Blend::Alpha), 0.001f, 1.0f).visible);
    // ...except BlendAdd, which is premultiplied and still adds light at zero.
    CHECK(M2ClassifySurface(surf(M2Blend::BlendAdd), 0.0f, 0.0f).visible);

    // Element alpha culls on the same threshold and by the same product — the
    // client has no separate "this batch never draws" gate, so a zeroed weight
    // or colour track has to reach here as alpha and nothing else.
    CHECK_FALSE(M2ClassifySurface(surf(M2Blend::Alpha), 1.0f, 0.00001f).visible);
    CHECK(M2ClassifySurface(surf(M2Blend::BlendAdd), 1.0f, 0.0f).visible);
}

TEST_CASE("M2 depth-writing transparent batches ask for a twin", "[m2][material][classify]") {
    auto surf = [](M2Blend b, whiteout::flakes::u16 flags) {
        M2Surface s;
        s.blend = b;
        s.materialFlags = flags;
        return s;
    };

    // Transparent and still writing depth: drawn twice, and hoisted.
    const auto writes = M2ClassifySurface(surf(M2Blend::Alpha, 0), 1.0f, 1.0f);
    CHECK(writes.blend == core::BlendClass::Transparent);
    CHECK(writes.needsDepthTwin);

    // Opting out of depth writing is what turns the pair back into one draw.
    CHECK_FALSE(M2ClassifySurface(surf(M2Blend::Alpha, kM2NoDepthWrite), 1.0f, 1.0f).needsDepthTwin);

    // Opaque batches are not in the transparent list at all, so no twin.
    CHECK_FALSE(M2ClassifySurface(surf(M2Blend::Opaque, 0), 1.0f, 1.0f).needsDepthTwin);
    CHECK_FALSE(M2ClassifySurface(surf(M2Blend::AlphaKey, 0), 1.0f, 1.0f).needsDepthTwin);

    // ...but a fading model demotes them, and then they do get one.
    CHECK(M2ClassifySurface(surf(M2Blend::Opaque, 0), 0.5f, 1.0f).needsDepthTwin);
}

TEST_CASE("M2 raw blend values outside the enum stay in range", "[m2][material]") {
    CHECK(M2BlendFromRaw(0) == M2Blend::Opaque);
    CHECK(M2BlendFromRaw(7) == M2Blend::BlendAdd);
    // Shipped data does not carry these, but a malformed material should draw
    // something rather than index off the end of the tables.
    CHECK(M2BlendFromRaw(8) == M2Blend::Opaque);
    CHECK(M2BlendFromRaw(0xFFFF) == M2Blend::Opaque);
}

// ---------------------------------------------------------------------------
// Element alpha, as CM2Scene::BeginDraw @ 0x100f793b0 forms it:
//
//     alpha = model.alpha
//           * colors[batch.colorIndex].alpha
//           * (batch.textureCount && !(batch.flags & 0x40)
//                  ? weights[weightCombos[batch.textureWeightComboIndex]] : 1)
//
// The weight lookup has no `+ unit` — units above the first reach the shader as
// the per-unit float4 and never touch visibility.
// ---------------------------------------------------------------------------

namespace {

// One three-unit batch over two weight tracks, wired the way `earthspiritsmall`
// is: combos {0, 0, 1}, weight 0 solid and weight 1 a constant zero.
whiteout::m2::Model TwoWeightModel(whiteout::u8 batchFlags) {
    namespace wm2 = whiteout::m2;
    wm2::Model m;
    m.materials.push_back({.flags = 0, .blendingMode = 0});

    wm2::TextureWeight solid;
    solid.weight.values = {{32767}};
    wm2::TextureWeight zero;
    zero.weight.values = {{0}};
    m.textureWeights = {solid, zero};
    m.textureWeightCombos = {0, 0, 1};
    m.textureCombos = {0, 0, 0};
    m.textures.resize(1);

    wm2::Batch b;
    b.flags = batchFlags;
    b.textureCount = 3;
    b.colorIndex = -1;
    m.skinProfiles.emplace_back().batches.push_back(b);
    return m;
}

} // namespace

TEST_CASE("M2 element alpha reads only the first unit's weight", "[m2][material][classify]") {
    const auto model = TwoWeightModel(0);
    const auto table = BuildM2SurfaceTable(model, 0);
    const M2Surface* s = table->Surface(0);
    REQUIRE(s);

    // Unit 2's weight is zero and unit 0's is not, so the batch draws. Reading
    // the wrong unit here emptied `earthspiritsmall` — all seven of its batches
    // share this combo layout.
    CHECK(s->elementAlpha == Approx(1.0f));
    CHECK(s->unitWeights[2] == Approx(0.0f));
    CHECK(M2ClassifySurface(*s, 1.0f, s->elementAlpha).visible);
}

TEST_CASE("M2 batch flag 0x40 drops the weight from element alpha",
          "[m2][material][classify]") {
    namespace wm2 = whiteout::m2;
    auto model = TwoWeightModel(0);
    // Point unit 0 at the zero track, so the flag is the only thing that can
    // decide whether the batch draws.
    model.textureWeightCombos = {1, 0, 0};

    // The table has to outlive the pointer into it: `BuildM2SurfaceTable`
    // returns a `unique_ptr`, so calling `Surface(0)` on the temporary hands
    // back a dangling pointer and the reads below are freed memory.
    const auto offTable = BuildM2SurfaceTable(model, 0);
    const M2Surface* off = offTable->Surface(0);
    REQUIRE(off);
    CHECK(off->elementAlpha == Approx(0.0f));
    CHECK_FALSE(off->ignoreWeights);
    CHECK_FALSE(M2ClassifySurface(*off, 1.0f, off->elementAlpha).visible);

    model.skinProfiles[0].batches[0].flags = 0x40;
    const auto table = BuildM2SurfaceTable(model, 0);
    const M2Surface* on = table->Surface(0);
    REQUIRE(on);
    CHECK(on->ignoreWeights);
    CHECK(on->elementAlpha == Approx(1.0f));
    CHECK(M2ClassifySurface(*on, 1.0f, on->elementAlpha).visible);
    // Still handed to the shader — the flag moves the weight, it does not
    // discard it.
    CHECK(on->unitWeights[0] == Approx(0.0f));
}
