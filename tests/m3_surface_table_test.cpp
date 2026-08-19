// ============================================================================
// M3SurfaceTable + the adapter's canonical texture set (device-free).
//
// Synthetic models throughout, m3_anim_builders' argument: the behaviours
// worth pinning — the trailing-NUL path trap, case-insensitive dedupe, the
// per-layer emissive slots carrying their own blend modes, the composite
// dominant-section pick, the unauthored-tint sentinels, the team-mask gate,
// the energy-conserving spec dim — need materials keyed against each other in
// ways no single shipped model exercises in isolation.
// ============================================================================

#include "io/m3/m3_model_adapter.h"
#include "renderer/profiles/sc2_heroes/m3_surface_table.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace whiteout;
using namespace whiteout::flakes;
using namespace whiteout::flakes::renderer::profiles::sc2_heroes;
namespace wio = whiteout::flakes::io;

namespace {

m3::TextureLayer TexLayer(std::string path, m3::UVMappingMode uv = m3::UVMappingMode::ExplicitUV0,
                          m3::ColorChannelSelect ch = m3::ColorChannelSelect::RGB) {
    m3::TextureLayer l;
    l.texturePath = std::move(path);
    l.uvMapping = uv;
    l.colorType = ch;
    l.color.initValue = {255, 255, 255, 255}; // BGRA white
    l.rgbMultiply.initValue = 1.0f;
    return l;
}

/// A one-region, one-batch division whose batch names MATM entry @p matm.
m3::MeshDivision Division(u16 matm) {
    m3::MeshDivision div;
    m3::Region region;
    region.firstVertex = 0;
    region.vertexCount = 3;
    region.firstIndex = 0;
    region.indexCount = 3;
    div.regions.push_back(region);
    m3::Batch batch;
    batch.regionIndex = 0;
    batch.materialIndex = matm;
    div.batches.push_back(batch);
    return div;
}

m3::MaterialMap Matm(m3::MaterialType type, u32 index) {
    m3::MaterialMap m;
    m.materialType = type;
    m.materialIndex = index;
    return m;
}

constexpr std::size_t kOneRegion[] = {0};

} // namespace

TEST_CASE("m3_surface_table: canonical texture set dedupes case-insensitively") {
    m3::Model model;
    m3::StandardMaterial a;
    a.diffuseLayer = TexLayer("Assets/Textures/Marine_Diff.dds");
    a.normalLayer = TexLayer("Assets/Textures/Marine_Norm.dds");
    m3::StandardMaterial b;
    // Same diffuse, different case — one entry, first-seen spelling.
    b.diffuseLayer = TexLayer("assets/textures/MARINE_diff.dds");
    b.specularLayer = TexLayer("Assets/Textures/Marine_Spec.dds");
    model.standardMaterials = {a, b};

    const auto refs = wio::CollectM3Textures(model);
    REQUIRE(refs.size() == 3);
    CHECK(refs[0].path == "Assets/Textures/Marine_Diff.dds");
    CHECK(refs[1].path == "Assets/Textures/Marine_Norm.dds");
    CHECK(refs[2].path == "Assets/Textures/Marine_Spec.dds");
}

TEST_CASE("m3_surface_table: the Ref<CHAR> trailing NUL never reaches a key") {
    // The parser keeps the terminator: size() is one past the text. A raw
    // copy would make the asset key silently miss CASC forever.
    std::string withNul = "Assets/Textures/Zergling_Diff.dds";
    withNul.push_back('\0');

    CHECK(wio::M3CleanPath(withNul) == "Assets/Textures/Zergling_Diff.dds");

    m3::Model model;
    m3::StandardMaterial mat;
    mat.diffuseLayer = TexLayer(withNul);
    model.standardMaterials = {mat};
    const auto refs = wio::CollectM3Textures(model);
    REQUIRE(refs.size() == 1);
    CHECK(refs[0].path == "Assets/Textures/Zergling_Diff.dds");
    CHECK(refs[0].path.find('\0') == std::string::npos);
}

TEST_CASE("m3_surface_table: wrap flags come from the layer") {
    m3::Model model;
    m3::StandardMaterial mat;
    auto layer = TexLayer("a.dds");
    layer.flags = static_cast<m3::TextureLayerFlag>(
        static_cast<u32>(m3::TextureLayerFlag::UVWrapX) |
        static_cast<u32>(m3::TextureLayerFlag::UVWrapY));
    mat.diffuseLayer = layer;
    model.standardMaterials = {mat};
    const auto refs = wio::CollectM3Textures(model);
    REQUIRE(refs.size() == 1);
    CHECK(refs[0].wrapFlags == 0x3u);
}

TEST_CASE("m3_surface_table: layer slots resolve, including the fallbacks") {
    m3::Model model;
    m3::StandardMaterial mat;
    mat.blendMode = m3::BlendMode::Opaque;
    mat.specularExponent = 64.0f;
    mat.hdrEmissiveMultiplier = 2.0f;
    mat.hdrSpecularMultiplier = 1.0f;
    mat.specularMode = m3::SpecularMode::AlphaOnly;
    mat.emissiveBlendMode2 = m3::LayerBlendOp::Add;

    // RGBA diffuse: the alpha is the team mask, so the slot carries
    // TEAMCOLOR_DIFFUSE.
    mat.diffuseLayer =
        TexLayer("d.dds", m3::UVMappingMode::ExplicitUV0, m3::ColorChannelSelect::RGBA);
    mat.specularLayer = TexLayer("s.dds");
    // Emissive layer 1 exists but is INACTIVE (no path, no solid flag) — its
    // slot stays off; layer 2 lands in ITS OWN slot with emissiveBlendMode2.
    mat.emissiveLayer1 = m3::TextureLayer{};
    mat.emissiveLayer2 = TexLayer("e2.dds");
    mat.normalLayer = TexLayer("n.dds", m3::UVMappingMode::ExplicitUV1);
    // Alpha mask authored with the default RGB channel select — the table
    // rewrites it to Alpha, because a mask samples its alpha.
    mat.alphaLayer1 = TexLayer("m.dds");

    model.standardMaterials = {mat};
    model.materialMaps = {Matm(m3::MaterialType::Standard, 0)};
    model.divisions = {Division(0)};

    const auto table = BuildM3SurfaceTable(model, kOneRegion);
    REQUIRE(table->Count() == 1);
    const M3Surface* s = table->Surface(0);
    REQUIRE(s != nullptr);
    CHECK(s->valid);
    CHECK(s->specularExponent == 64.0f);
    // hdrEmissiveMultiplier is a surface field now — the shader scales the
    // additive emissive SUM with it (team adds included), not the layer tint.
    CHECK_THAT(s->emissiveMultiplier, Catch::Matchers::WithinAbs(2.0f, 1e-5f));

    const auto& diffuse = s->layers[0];
    CHECK(diffuse.mode == 1);
    CHECK(diffuse.textureId == 0);
    CHECK(diffuse.uvSource == 0);
    CHECK(diffuse.teamColorMode == 1);

    const auto& spec = s->layers[2];
    CHECK(spec.mode == 1);
    // SpecularMode::AlphaOnly folds into the channel select.
    CHECK(spec.channels == static_cast<u8>(m3::ColorChannelSelect::Alpha));
    // The energy-conserving dim (retail's FakeEnergyConservingSpec, on for
    // every material without SimulateRoughness) folds into the spec tint:
    // p=64 -> -4.444e-6*p^2 + 4.333e-3*p + 2.0834e-3.
    CHECK_THAT(spec.tint.x, Catch::Matchers::WithinAbs(0.26120f, 1e-4f));

    const auto& emissive1 = s->layers[3];
    CHECK(emissive1.mode == 0);

    const auto& emissive2 = s->layers[4];
    CHECK(emissive2.mode == 1);
    CHECK(emissive2.blendOp == static_cast<u8>(m3::LayerBlendOp::Add));
    CHECK_THAT(emissive2.tint.x, Catch::Matchers::WithinAbs(1.0f, 1e-5f));

    const auto& normal = s->layers[5];
    CHECK(normal.mode == 1);
    CHECK(normal.uvSource == 1);

    const auto& mask = s->layers[6];
    CHECK(mask.mode == 1);
    CHECK(mask.channels == static_cast<u8>(m3::ColorChannelSelect::Alpha));

    const auto& decal = s->layers[1];
    CHECK(decal.mode == 0);
}

TEST_CASE("m3_surface_table: solid-colour layers and the unauthored sentinels") {
    m3::Model model;
    m3::StandardMaterial mat;
    // The parser always fills these from the file; a synthetic material has
    // to say so or the tint checks below multiply by stack garbage.
    mat.hdrEmissiveMultiplier = 1.0f;
    mat.hdrSpecularMultiplier = 1.0f;
    // A solid-colour emissive: Color flag, no texture.
    m3::TextureLayer solid;
    solid.flags = m3::TextureLayerFlag::Color;
    solid.color.initValue = {0, 128, 255, 255}; // BGRA: red 255, green 128, blue 0
    solid.rgbMultiply.initValue = 1.0f;
    mat.emissiveLayer1 = solid;
    // A diffuse whose colour was never authored (all-zero BGRA + zero
    // multiplier) — both sentinels must read as white, not black.
    m3::TextureLayer unauthored;
    unauthored.texturePath = "d.dds";
    mat.diffuseLayer = unauthored;

    model.standardMaterials = {mat};
    model.materialMaps = {Matm(m3::MaterialType::Standard, 0)};
    model.divisions = {Division(0)};

    const auto table = BuildM3SurfaceTable(model, kOneRegion);
    const M3Surface* s = table->Surface(0);
    REQUIRE(s != nullptr);

    const auto& emissive = s->layers[3];
    CHECK(emissive.mode == 2);
    CHECK(emissive.textureId == -1);
    CHECK_THAT(emissive.tint.x, Catch::Matchers::WithinAbs(1.0f, 1e-5f));         // r
    CHECK_THAT(emissive.tint.y, Catch::Matchers::WithinAbs(128.0f / 255.0f, 1e-5f)); // g
    CHECK_THAT(emissive.tint.z, Catch::Matchers::WithinAbs(0.0f, 1e-5f));         // b

    const auto& diffuse = s->layers[0];
    CHECK(diffuse.mode == 1);
    CHECK_THAT(diffuse.tint.x, Catch::Matchers::WithinAbs(1.0f, 1e-5f));
    CHECK_THAT(diffuse.tint.w, Catch::Matchers::WithinAbs(1.0f, 1e-5f));
}

TEST_CASE("m3_surface_table: composite resolves to its dominant standard section") {
    m3::Model model;
    m3::StandardMaterial weak;
    weak.diffuseLayer = TexLayer("weak.dds");
    m3::StandardMaterial dominant;
    dominant.diffuseLayer = TexLayer("dominant.dds");
    model.standardMaterials = {weak, dominant};

    m3::CompositeMaterial comp;
    m3::CompositeSection s0;
    s0.materialIndex = 0; // MATM index of `weak`
    s0.mapMultiplier.initValue = 0.25f;
    m3::CompositeSection s1;
    s1.materialIndex = 1; // MATM index of `dominant`
    s1.mapMultiplier.initValue = 0.75f;
    comp.sections = {s0, s1};
    model.compositeMaterials = {comp};

    model.materialMaps = {Matm(m3::MaterialType::Standard, 0),
                          Matm(m3::MaterialType::Standard, 1),
                          Matm(m3::MaterialType::Composite, 0)};
    model.divisions = {Division(2)}; // the batch names the composite

    const auto table = BuildM3SurfaceTable(model, kOneRegion);
    const M3Surface* s = table->Surface(0);
    REQUIRE(s != nullptr);
    CHECK(s->valid);
    // dominant.dds is texture index 1 in the canonical order (weak first).
    CHECK(s->layers[0].textureId == 1);
}

TEST_CASE("m3_surface_table: non-standard material types stay invalid") {
    m3::Model model;
    model.displacementMaterials.emplace_back();
    model.materialMaps = {Matm(m3::MaterialType::Displacement, 0)};
    model.divisions = {Division(0)};

    const auto table = BuildM3SurfaceTable(model, kOneRegion);
    const M3Surface* s = table->Surface(0);
    REQUIRE(s != nullptr);
    CHECK_FALSE(s->valid);
    CHECK_FALSE(M3ClassifySurface(*s).visible);
}

TEST_CASE("m3_surface_table: classification") {
    M3Surface s;
    s.valid = true;

    s.blendMode = m3::BlendMode::Opaque;
    CHECK(M3ClassifySurface(s).blend == renderer::core::BlendClass::Opaque);
    CHECK(M3ClassifySurface(s).visible);

    s.alphaTestThreshold = 0.25f;
    CHECK(M3ClassifySurface(s).blend == renderer::core::BlendClass::AlphaKey);

    s.alphaTestThreshold = 0.0f;
    s.blendMode = m3::BlendMode::AlphaBlend;
    CHECK(M3ClassifySurface(s).blend == renderer::core::BlendClass::Transparent);
    s.blendMode = m3::BlendMode::Add;
    CHECK(M3ClassifySurface(s).blend == renderer::core::BlendClass::Transparent);
    s.blendMode = m3::BlendMode::Mod;
    CHECK(M3ClassifySurface(s).blend == renderer::core::BlendClass::Transparent);
}

TEST_CASE("m3_surface_table: REGN v5 carries the UV transform, older implies 1/2048") {
    m3::Model model;
    m3::StandardMaterial mat;
    mat.diffuseLayer = TexLayer("d.dds");
    model.standardMaterials = {mat};
    model.materialMaps = {Matm(m3::MaterialType::Standard, 0)};

    m3::MeshDivision div = Division(0);
    // The stored v5 scale is SNORM-space: retail regions carry 16.0, which is
    // the same 1/2048-per-raw-unit older regions imply. The table converts.
    div.regions[0].uvScale = 16.0f;
    div.regions[0].uvOffset = 0.5f;
    // Version unset (-1): the parser never read uvScale, so the defaults win.
    model.divisions = {div};
    {
        const auto table = BuildM3SurfaceTable(model, kOneRegion);
        const M3Surface* s = table->Surface(0);
        REQUIRE(s != nullptr);
        CHECK_THAT(s->uvMultiply, Catch::Matchers::WithinAbs(1.0f / 2048.0f, 1e-9f));
        CHECK(s->uvOffset == 0.0f);
    }
    model.divisions[0].regions[0].setVersion(5);
    {
        const auto table = BuildM3SurfaceTable(model, kOneRegion);
        const M3Surface* s = table->Surface(0);
        REQUIRE(s != nullptr);
        CHECK_THAT(s->uvMultiply, Catch::Matchers::WithinAbs(16.0f / 32767.0f, 1e-9f));
        CHECK_THAT(s->uvOffset, Catch::Matchers::WithinAbs(0.5f, 1e-9f));
    }
}
