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

#include <cstring>
#include <vector>

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
/// The MATM index `Division(0)` names, as the adapter would report it.
constexpr u32 kMatm0[] = {0};

// crc32 of the fragment and property names a MADD record keys on. Only the
// handful these tests need — the full table lives in WhiteoutLib.
constexpr u32 kFragDiffuse = 0x611cefedu;
constexpr u32 kFragEnvio = 0xb48618d1u;
constexpr u32 kFragLightingDeferred = 0x28828f0cu;
constexpr u32 kFragFog = 0xbae5abf3u;
constexpr u32 kFragRailTex = 0x22bd4618u;
constexpr u32 kPropTexDiffuse = 0xc6443019u;
constexpr u32 kPropTexEnvironmentMap = 0x3b7d7ea1u;
constexpr u32 kPropEnvioControl = 0x6ce1012fu;
constexpr u32 kPropTex = 0xd2a03992u;

struct MaddProperty {
    u32 hash;
    std::vector<u8> value;
};
struct MaddGroup {
    u32 hash;
    std::vector<MaddProperty> properties;
};

/// A `MADD` property blob: the two-level dictionary `decodeProperties` reads,
/// built here because there is no writer for it and a data-driven material is
/// nothing without one. Both levels store parallel arrays behind absolute
/// offsets, and the decoder rejects the record outright if any of them is off
/// by a byte, so the offsets are computed rather than assumed.
std::vector<u8> MaddBlob(const std::vector<MaddGroup>& groups) {
    std::vector<u8> blob;
    auto put = [&blob](const void* bytes, std::size_t n) {
        const auto* p = static_cast<const u8*>(bytes);
        blob.insert(blob.end(), p, p + n);
    };
    auto put32 = [&put](u32 v) { put(&v, sizeof v); };
    auto put16 = [&put](u16 v) { put(&v, sizeof v); };
    auto put64 = [&put](u64 v) { put(&v, sizeof v); };
    auto patch64 = [&blob](std::size_t at, u64 v) { std::memcpy(blob.data() + at, &v, sizeof v); };

    const u32 n = static_cast<u32>(groups.size());
    put32(n);
    put64(20);                    // the key array follows the 20-byte header
    put64(20ull + 4ull * n);      // then the group offsets
    for (const auto& g : groups)
        put32(g.hash);
    const std::size_t groupOffsets = blob.size();
    for (u32 i = 0; i < n; ++i)
        put64(0);

    for (u32 i = 0; i < n; ++i) {
        const auto& g = groups[i];
        patch64(groupOffsets + 8ull * i, blob.size());
        const u32 count = static_cast<u32>(g.properties.size());
        if (count == 0) {
            // A fragment with no properties — the decoder demands all four
            // arrays be null rather than empty.
            put32(0);
            for (int j = 0; j < 4; ++j)
                put64(0);
            continue;
        }
        const u64 at = blob.size();
        const u64 hashes = at + 36, sizes = hashes + 4ull * count;
        const u64 types = sizes + 2ull * count, values = types + count;
        put32(count);
        put64(hashes);
        put64(sizes);
        put64(types);
        put64(values);
        for (const auto& prop : g.properties)
            put32(prop.hash);
        for (const auto& prop : g.properties)
            put16(static_cast<u16>(prop.value.size()));
        for (u32 j = 0; j < count; ++j)
            blob.push_back(1); // live slot
        const std::size_t valueOffsets = blob.size();
        for (u32 j = 0; j < count; ++j)
            put64(0);
        for (u32 j = 0; j < count; ++j) {
            patch64(valueOffsets + 8ull * j, blob.size());
            put(g.properties[j].value.data(), g.properties[j].value.size());
        }
    }
    return blob;
}

/// `{u32 index into texturePaths, u32 source}` — how a Tex* property points.
std::vector<u8> MaddTex(u32 index) {
    std::vector<u8> v(8, 0);
    std::memcpy(v.data(), &index, sizeof index);
    return v;
}

std::vector<u8> MaddU32(u32 value) {
    std::vector<u8> v(4);
    std::memcpy(v.data(), &value, sizeof value);
    return v;
}

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

    const auto table = BuildM3SurfaceTable(model, kOneRegion, kMatm0);
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

    const auto table = BuildM3SurfaceTable(model, kOneRegion, kMatm0);
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

TEST_CASE("m3_surface_table: every composite section gets its own surface") {
    m3::Model model;
    m3::StandardMaterial glow;
    glow.diffuseLayer = TexLayer("glow.dds");
    m3::StandardMaterial skin;
    skin.diffuseLayer = TexLayer("skin.dds");
    model.standardMaterials = {glow, skin};

    m3::CompositeMaterial comp;
    m3::CompositeSection s0;
    s0.materialIndex = 0; // MATM index of `glow`
    m3::CompositeSection s1;
    s1.materialIndex = 1; // MATM index of `skin`
    comp.sections = {s0, s1};
    model.compositeMaterials = {comp};

    model.materialMaps = {Matm(m3::MaterialType::Standard, 0),
                          Matm(m3::MaterialType::Standard, 1),
                          Matm(m3::MaterialType::Composite, 0)};
    model.divisions = {Division(2)}; // the batch names the composite

    // What BuildEmittedRegions hands over for that batch: the one region twice,
    // once per section, in section order.
    constexpr std::size_t kRegions[] = {0, 0};
    constexpr u32 kMaterials[] = {0, 1};
    const auto table = BuildM3SurfaceTable(model, kRegions, kMaterials);
    REQUIRE(table->Count() == 2);
    REQUIRE(table->Surface(0) != nullptr);
    REQUIRE(table->Surface(1) != nullptr);
    CHECK(table->Surface(0)->valid);
    CHECK(table->Surface(1)->valid);
    // Canonical texture order is first-seen: glow.dds 0, skin.dds 1.
    CHECK(table->Surface(0)->layers[0].textureId == 0);
    CHECK(table->Surface(1)->layers[0].textureId == 1);
}

TEST_CASE("m3_surface_table: a data-driven material is restored as a standard one") {
    m3::Model model;
    m3::DataDrivenMaterial madd;
    madd.materialName = "hero";
    madd.texturePaths = {"body_diff.dds", "reflect.dds"};
    madd.unknown124 = static_cast<u32>(m3::BlendMode::Opaque);
    madd.propertyBlob = MaddBlob({
        {kFragDiffuse, {{kPropTexDiffuse, MaddTex(0)}}},
        {kFragEnvio,
         {{kPropTexEnvironmentMap, MaddTex(1)},
          {kPropEnvioControl, MaddU32(static_cast<u32>(m3::LayerBlendOp::Add))}}},
        {kFragLightingDeferred, {}},
        {kFragFog, {}},
    });
    model.dataDrivenMaterials = {madd};
    model.materialMaps = {Matm(m3::MaterialType::DataDriven, 0)};
    model.divisions = {Division(0)};

    const wio::M3DataDrivenResult report = wio::M3RestoreDataDrivenMaterials(model);
    CHECK(report.restored == 1);
    CHECK(report.approximated == 0);
    CHECK(report.refused == 0);

    // The map is repointed, so nothing downstream needs a MADD path.
    REQUIRE(model.standardMaterials.size() == 1);
    CHECK(model.materialMaps[0].materialType == m3::MaterialType::Standard);
    CHECK(model.materialMaps[0].materialIndex == 0);
    CHECK(model.standardMaterials[0].name == "hero");
    // Mod is the field's default, and the op the record names is what keeps an
    // unmasked reflection from multiplying the surface to black.
    CHECK(model.standardMaterials[0].layerBlendMode == m3::LayerBlendOp::Add);
    // The record names no address mode for either layer, and clamp is not what
    // that means: a clamped layer whose UVs leave [0,1] samples one edge column
    // for the whole surface.
    REQUIRE(model.standardMaterials[0].diffuseLayer.has_value());
    CHECK(hasFlag(model.standardMaterials[0].diffuseLayer->flags, m3::TextureLayerFlag::UVWrapX));
    CHECK(hasFlag(model.standardMaterials[0].diffuseLayer->flags, m3::TextureLayerFlag::UVWrapY));

    const auto refs = wio::CollectM3Textures(model);
    REQUIRE(refs.size() == 2);
    CHECK(refs[0].path == "body_diff.dds");
    CHECK(refs[0].wrapFlags == 0x3u);
    CHECK(refs[1].cube);

    const auto table = BuildM3SurfaceTable(model, kOneRegion, kMatm0);
    const M3Surface* s = table->Surface(0);
    REQUIRE(s != nullptr);
    CHECK(s->valid);
    CHECK(s->blendMode == m3::BlendMode::Opaque);
    CHECK(s->layers[0].textureId == 0);
    CHECK(s->layers[kM3LayerEnvironment].textureId == 1);
    CHECK(s->layers[kM3LayerEnvironment].blendOp == static_cast<u8>(m3::LayerBlendOp::Add));
}

TEST_CASE("m3_surface_table: a restored material keeps the record's blend mode") {
    // MAT_.blendMode survives the engine's forward conversion in the record
    // field WhiteoutLib still calls `unknown124`; the two values the shipped
    // corpus holds past the enum blend rather than paint an FX quad opaque.
    const std::pair<u32, m3::BlendMode> cases[] = {
        {0, m3::BlendMode::Opaque},   {1, m3::BlendMode::AlphaBlend},
        {2, m3::BlendMode::Add},      {3, m3::BlendMode::AlphaAdd},
        {5, m3::BlendMode::Mod2x},    {7, m3::BlendMode::AlphaBlend},
    };
    for (const auto& [stored, expected] : cases) {
        m3::Model model;
        m3::DataDrivenMaterial madd;
        madd.texturePaths = {"body_diff.dds"};
        madd.unknown124 = stored;
        madd.propertyBlob = MaddBlob({{kFragDiffuse, {{kPropTexDiffuse, MaddTex(0)}}}});
        model.dataDrivenMaterials = {madd};
        model.materialMaps = {Matm(m3::MaterialType::DataDriven, 0)};

        CHECK(wio::M3RestoreDataDrivenMaterials(model).restored == 1);
        REQUIRE(model.standardMaterials.size() == 1);
        CHECK(model.standardMaterials[0].blendMode == expected);
    }
}

TEST_CASE("m3_surface_table: a shader-graph material is approximated, not refused") {
    m3::Model model;
    m3::DataDrivenMaterial madd;
    madd.materialName = "graph";
    madd.texturePaths = {"hero_diff.dds", "hero_spec.dds", "hero_reflection.dds", "hero_emis.dds"};
    // A Rail* node is the graph vocabulary, which never had a StandardMaterial
    // form; the blob stores the nodes but not the edges, so the filename is the
    // only signal left for what each texture is.
    madd.propertyBlob = MaddBlob({{kFragRailTex,
                                   {{kPropTex, MaddTex(0)},
                                    {kPropTex, MaddTex(1)},
                                    {kPropTex, MaddTex(2)},
                                    {kPropTex, MaddTex(3)}}}});
    model.dataDrivenMaterials = {madd};
    model.materialMaps = {Matm(m3::MaterialType::DataDriven, 0)};
    model.divisions = {Division(0)};

    const wio::M3DataDrivenResult report = wio::M3RestoreDataDrivenMaterials(model);
    CHECK(report.restored == 0);
    CHECK(report.approximated == 1);
    REQUIRE(model.standardMaterials.size() == 1);
    CHECK(model.materialMaps[0].materialType == m3::MaterialType::Standard);

    // The ops a graph never names. Mod is the field default for both, and it is
    // the wrong one twice over: it darkens the emissive layer instead of
    // lighting with it, and washes the surface out with an unmasked cubemap.
    const m3::StandardMaterial& mat = model.standardMaterials[0];
    CHECK(mat.emissiveBlendMode1 == m3::LayerBlendOp::Add);
    REQUIRE(mat.environmentMaskLayer.has_value());
    CHECK(mat.environmentMaskLayer->texturePath == "hero_spec.dds");

    const auto table = BuildM3SurfaceTable(model, kOneRegion, kMatm0);
    const M3Surface* s = table->Surface(0);
    REQUIRE(s != nullptr);
    CHECK(s->valid);
    CHECK(s->layers[0].textureId == 0);
    // The mask reuses the specular texture rather than claiming its own slot.
    CHECK(s->layers[kM3LayerEnvironment + 1].textureId == s->layers[2].textureId);
}

TEST_CASE("m3_surface_table: a record with no standard form keeps its data-driven map") {
    m3::Model model;
    m3::DataDrivenMaterial madd;
    madd.materialName = "graph";
    // A graph with nothing to sample: no role to infer, so there is no likeness
    // to build and inventing one would be worse than drawing nothing.
    madd.propertyBlob = MaddBlob({{kFragRailTex, {}}});
    model.dataDrivenMaterials = {madd};
    model.materialMaps = {Matm(m3::MaterialType::DataDriven, 0)};
    model.divisions = {Division(0)};

    const wio::M3DataDrivenResult report = wio::M3RestoreDataDrivenMaterials(model);
    CHECK(report.refused == 1);
    CHECK(model.standardMaterials.empty());
    CHECK(model.materialMaps[0].materialType == m3::MaterialType::DataDriven);

    const auto table = BuildM3SurfaceTable(model, kOneRegion, kMatm0);
    const M3Surface* s = table->Surface(0);
    REQUIRE(s != nullptr);
    CHECK_FALSE(s->valid);
}

TEST_CASE("m3_surface_table: two maps naming one record restore it once") {
    m3::Model model;
    m3::DataDrivenMaterial madd;
    madd.texturePaths = {"body_diff.dds"};
    madd.propertyBlob = MaddBlob({{kFragDiffuse, {{kPropTexDiffuse, MaddTex(0)}}}});
    model.dataDrivenMaterials = {madd};
    model.materialMaps = {Matm(m3::MaterialType::DataDriven, 0),
                          Matm(m3::MaterialType::DataDriven, 0)};

    CHECK(wio::M3RestoreDataDrivenMaterials(model).restored == 1);
    CHECK(model.standardMaterials.size() == 1);
    CHECK(model.materialMaps[0].materialIndex == 0);
    CHECK(model.materialMaps[1].materialIndex == 0);
}

TEST_CASE("m3_surface_table: non-standard material types stay invalid") {
    m3::Model model;
    model.displacementMaterials.emplace_back();
    model.materialMaps = {Matm(m3::MaterialType::Displacement, 0)};
    model.divisions = {Division(0)};

    const auto table = BuildM3SurfaceTable(model, kOneRegion, kMatm0);
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
        const auto table = BuildM3SurfaceTable(model, kOneRegion, kMatm0);
        const M3Surface* s = table->Surface(0);
        REQUIRE(s != nullptr);
        CHECK_THAT(s->uvMultiply, Catch::Matchers::WithinAbs(1.0f / 2048.0f, 1e-9f));
        CHECK(s->uvOffset == 0.0f);
    }
    model.divisions[0].regions[0].setVersion(5);
    {
        const auto table = BuildM3SurfaceTable(model, kOneRegion, kMatm0);
        const M3Surface* s = table->Surface(0);
        REQUIRE(s != nullptr);
        CHECK_THAT(s->uvMultiply, Catch::Matchers::WithinAbs(16.0f / 32767.0f, 1e-9f));
        CHECK_THAT(s->uvOffset, Catch::Matchers::WithinAbs(0.5f, 1e-9f));
    }
}

TEST_CASE("m3_surface_table: the environment layer and its mask are separate slots") {
    m3::Model model;
    m3::StandardMaterial mat;
    mat.diffuseLayer = TexLayer("d.dds");
    mat.environmentLayer = TexLayer("reflect.dds", m3::UVMappingMode::ReflectCubicEnvio);
    mat.environmentMaskLayer = TexLayer("mask.dds");
    // ApplyEnv reads the material's layer blend, the same field the decal does.
    mat.layerBlendMode = m3::LayerBlendOp::Add;
    model.standardMaterials = {mat};
    model.materialMaps = {Matm(m3::MaterialType::Standard, 0)};
    model.divisions = {Division(0)};

    const auto table = BuildM3SurfaceTable(model, kOneRegion, kMatm0);
    const M3Surface* s = table->Surface(0);
    REQUIRE(s != nullptr);

    const M3Layer& env = s->layers[static_cast<u32>(wio::M3LayerSlot::Environment)];
    const M3Layer& envMask = s->layers[static_cast<u32>(wio::M3LayerSlot::EnvironmentMask)];
    CHECK(env.mode == 1);
    CHECK(envMask.mode == 1);
    // Distinct textures: retail multiplies the mask into the env layer's own
    // alpha rather than choosing between them, so one slot cannot serve both.
    CHECK(env.textureId != envMask.textureId);
    CHECK(env.blendOp == static_cast<u8>(m3::LayerBlendOp::Add));
    CHECK(s->envReflect);

    // The cube goes to the renderer marked as one — a cube view cannot answer
    // a 2D binding, so the request has to travel with the reference.
    const auto texs = wio::CollectM3Textures(model);
    bool sawCube = false, sawFlat = false;
    for (const auto& t : texs) {
        if (t.path == "reflect.dds") { sawCube = t.cube; }
        if (t.path == "mask.dds") { sawFlat = !t.cube; }
    }
    CHECK(sawCube);
    CHECK(sawFlat);
}

TEST_CASE("m3_surface_table: only the Reflect envio mappings reflect") {
    // psuvmapping.fx GenCubicEnvio takes a `reflect` flag: the plain
    // Cubic/Spherical mappings look the cube up along the normal instead. 34
    // of the 744 shipped env layers do that.
    m3::Model model;
    m3::StandardMaterial mat;
    mat.environmentLayer = TexLayer("reflect.dds", m3::UVMappingMode::CubicEnvio);
    model.standardMaterials = {mat};
    model.materialMaps = {Matm(m3::MaterialType::Standard, 0)};
    model.divisions = {Division(0)};
    CHECK_FALSE(BuildM3SurfaceTable(model, kOneRegion, kMatm0)->Surface(0)->envReflect);

    model.standardMaterials[0].environmentLayer->uvMapping =
        m3::UVMappingMode::ReflectSphericalEnvio;
    CHECK(BuildM3SurfaceTable(model, kOneRegion, kMatm0)->Surface(0)->envReflect);
}

TEST_CASE("m3_surface_table: hdrEnvironmentConstant is only read at MAT_ v20") {
    // The three hdrEnvironment* fields exist from v20; below that the parser
    // leaves them zero. Measured: 717 of 744 shipped env materials read 0
    // there, so taking the field at face value would multiply almost every
    // reflection in the corpus to black.
    m3::Model model;
    m3::StandardMaterial mat;
    mat.environmentLayer = TexLayer("reflect.dds", m3::UVMappingMode::ReflectCubicEnvio);
    mat.hdrEnvironmentConstant = 0.0f;
    model.standardMaterials = {mat};
    model.materialMaps = {Matm(m3::MaterialType::Standard, 0)};
    model.divisions = {Division(0)};

    const auto envSlot = static_cast<u32>(wio::M3LayerSlot::Environment);
    {
        // Version unset: no constant, so the tint keeps the layer's own value.
        const auto table = BuildM3SurfaceTable(model, kOneRegion, kMatm0);
        CHECK_THAT(table->Surface(0)->layers[envSlot].tint.x,
                   Catch::Matchers::WithinAbs(1.0f, 1e-6f));
    }
    model.standardMaterials[0].setVersion(20);
    model.standardMaterials[0].hdrEnvironmentConstant = 2.0f;
    {
        const auto table = BuildM3SurfaceTable(model, kOneRegion, kMatm0);
        CHECK_THAT(table->Surface(0)->layers[envSlot].tint.x,
                   Catch::Matchers::WithinAbs(2.0f, 1e-6f));
    }
}

TEST_CASE("m3_surface_table: rgbAdd rides the same extra multiplier the tint does") {
    // psmateriallayer.fx: `cResult.rgba = cResult.rgba * multiply + add`. The
    // add half was missing, and it is not cosmetic — the golden Adept's
    // environment layer is authored (x1.5, +0.4) under a Mod op, so dropping
    // the +0.4 multiplied the whole model down to bronze. Whatever extra
    // multiplier the tint folds in has to reach the add too, or the pair stops
    // reading as `(texel * multiply + add) * extra`.
    m3::Model model;
    m3::StandardMaterial mat;
    mat.environmentLayer = TexLayer("reflect.dds", m3::UVMappingMode::ReflectCubicEnvio);
    mat.environmentLayer->rgbMultiply.initValue = 1.5f;
    mat.environmentLayer->rgbAdd.initValue = 0.4f;
    mat.environmentLayer->flags = static_cast<m3::TextureLayerFlag>(
        static_cast<u32>(m3::TextureLayerFlag::ColorClamp) |
        static_cast<u32>(m3::TextureLayerFlag::ColorInvert));
    mat.setVersion(20);
    mat.hdrEnvironmentConstant = 2.0f;
    model.standardMaterials = {mat};
    model.materialMaps = {Matm(m3::MaterialType::Standard, 0)};
    model.divisions = {Division(0)};

    const auto table = BuildM3SurfaceTable(model, kOneRegion, kMatm0);
    const M3Layer& env =
        table->Surface(0)->layers[static_cast<u32>(wio::M3LayerSlot::Environment)];
    CHECK_THAT(env.tint.x, Catch::Matchers::WithinAbs(1.5f * 2.0f, 1e-6f));
    CHECK_THAT(env.add, Catch::Matchers::WithinAbs(0.4f * 2.0f, 1e-6f));
    CHECK(env.invert == 1);
    CHECK(env.clampColor == 1);
}

TEST_CASE("m3_surface_table: the normal slot declares itself linear") {
    // The layer slot is the authority on colour space, not the filename. Over
    // the 51469-model SC2 + Heroes corpus 16299 of 71792 normal-map references
    // (22.7%) carry a name `DetermineImageUsage` reads as colour, and an sRGB
    // view on a DXT5nm map gamma-decodes green (y) while leaving alpha (x)
    // alone — every normal tilts along the bitangent, whose sign flips at a
    // mirrored-UV seam, so half the model lights and half goes dark.
    m3::Model model;
    m3::StandardMaterial mat;
    mat.diffuseLayer = TexLayer("Assets/Textures/Marine_Diffuse_Blood.dds");
    // A shipped spelling the suffix list does NOT match, which is the point.
    mat.normalLayer = TexLayer("Assets/Textures/Marine_Normal_Blood.dds");
    mat.specularLayer = TexLayer("Assets/Textures/Marine_Specular_Blood.dds");
    model.standardMaterials = {mat};
    model.materialMaps = {Matm(m3::MaterialType::Standard, 0)};
    model.divisions = {Division(0)};

    const auto texs = wio::CollectM3Textures(model);
    REQUIRE(texs.size() == 3);
    for (const auto& t : texs) {
        const bool isNormal = t.path.find("_Normal_") != std::string::npos;
        CHECK(t.linear == isNormal);
    }
}

TEST_CASE("m3_surface_table: one file bound both ways is two textures") {
    // Same reasoning as the cube flag above: linearity belongs to the binding,
    // so a path wanted as colour on one material and as a normal map on
    // another cannot collapse to one slot — one view cannot answer both.
    m3::Model model;
    m3::StandardMaterial colour;
    colour.diffuseLayer = TexLayer("shared.dds");
    m3::StandardMaterial data;
    data.normalLayer = TexLayer("shared.dds");
    model.standardMaterials = {colour, data};
    model.materialMaps = {Matm(m3::MaterialType::Standard, 0),
                          Matm(m3::MaterialType::Standard, 1)};
    model.divisions = {Division(0)};

    const auto texs = wio::CollectM3Textures(model);
    REQUIRE(texs.size() == 2);
    CHECK(texs[0].linear != texs[1].linear);
}

TEST_CASE("m3_surface_table: the layer UV transform is a TRS about the UV centre") {
    // SC2 (sub_102ABBDE0) rotates and scales about the texture centre (0.5,0.5)
    // with the offset applied in source space: M = T(+0.5)·S·R·T(-(0.5+offset)).
    // psmateriallayer.fx feeds the 2x4 (u, v, 0, 1) and keeps .xy, so only these
    // two rows exist and only uvAngle.z can reach the output.
    f32 r0[4], r1[4];

    wio::M3ComposeUvTransform({0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, r0, r1);
    CHECK(r0[0] == 1.0f);
    CHECK(r0[1] == 0.0f);
    CHECK(r0[3] == 0.0f);
    CHECK(r1[0] == 0.0f);
    CHECK(r1[1] == 1.0f);
    CHECK(r1[3] == 0.0f);

    // Tiling repeats about the centre: uv 0.5 is the fixed point, so uv 1 lands
    // on 2 (translation column = -1), not 3.
    wio::M3ComposeUvTransform({0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {3.0f, 1.0f}, r0, r1);
    CHECK(r0[0] == 3.0f);
    CHECK(r0[3] == -1.0f);
    CHECK(r1[1] == 1.0f);

    // Offset lives in source space, ahead of the linear half, so it reaches the
    // translation column negated.
    wio::M3ComposeUvTransform({0.25f, -0.5f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, r0, r1);
    CHECK(r0[3] == -0.25f);
    CHECK(r1[3] == 0.5f);

    // A quarter turn in the UV plane, radians — the value shipped content
    // clusters on. About the centre, (u, v) = (1, 0) comes out (0, 0) and the
    // centre is fixed.
    constexpr f32 kHalfPi = 1.57079633f;
    wio::M3ComposeUvTransform({0.0f, 0.0f}, {0.0f, 0.0f, -kHalfPi}, {1.0f, 1.0f}, r0, r1);
    const f32 u = r0[0] * 1.0f + r0[1] * 0.0f + r0[3];
    const f32 v = r1[0] * 1.0f + r1[1] * 0.0f + r1[3];
    CHECK_THAT(u, Catch::Matchers::WithinAbs(0.0, 1e-6));
    CHECK_THAT(v, Catch::Matchers::WithinAbs(0.0, 1e-6));
    const f32 cu = r0[0] * 0.5f + r0[1] * 0.5f + r0[3];
    const f32 cv = r1[0] * 0.5f + r1[1] * 0.5f + r1[3];
    CHECK_THAT(cu, Catch::Matchers::WithinAbs(0.5, 1e-6));
    CHECK_THAT(cv, Catch::Matchers::WithinAbs(0.5, 1e-6));

    // The two rotations that are NOT in the plane cannot reach the result.
    f32 x0[4], x1[4];
    wio::M3ComposeUvTransform({0.0f, 0.0f}, {kHalfPi, kHalfPi, 0.0f}, {1.0f, 1.0f}, x0, x1);
    CHECK(x0[0] == 1.0f);
    CHECK(x1[1] == 1.0f);
}

TEST_CASE("m3_surface_table: every resolved layer names its UV-transform slot") {
    m3::Model model;
    m3::StandardMaterial mat;
    mat.diffuseLayer = TexLayer("diff.dds");
    mat.emissiveLayer1 = TexLayer("glow.dds");
    m3::StandardMaterial unused;
    unused.diffuseLayer = TexLayer("other.dds");
    // Two materials so the id is measurably keyed on the material index and
    // not just on the slot.
    model.standardMaterials = {unused, mat};
    model.materialMaps = {Matm(m3::MaterialType::Standard, 1)};
    model.divisions = {Division(0)};

    const auto table = BuildM3SurfaceTable(model, kOneRegion, kMatm0);
    const M3Surface* s = table->Surface(0);
    REQUIRE(s != nullptr);
    REQUIRE(s->valid);
    // Material 1, and the ordinals M3LayerSlot gives Diffuse and Emissive.
    CHECK(s->layers[0].uvTransformId == static_cast<i32>(kM3LayerCount) + 0);
    CHECK(s->layers[3].uvTransformId == static_cast<i32>(kM3LayerCount) + 3);
    // A slot with no layer names nothing, so the shader keeps the identity.
    CHECK(s->layers[1].uvTransformId == -1);
    CHECK(s->layers[kM3LayerEnvironment].uvTransformId == -1);
}

TEST_CASE("m3_surface_table: fresnel constants come off the layer") {
    m3::Model model;
    m3::StandardMaterial mat;
    mat.diffuseLayer = TexLayer("diff.dds");
    // The shipped Golden Adept's reflection: standard mode, and a min/max
    // that is an OUTPUT range — the shader wants it as (bias, scale).
    mat.diffuseLayer->fresnelMode = m3::FresnelMode::Standard;
    mat.diffuseLayer->fresnelExponent = 1.5f;
    mat.diffuseLayer->fresnelMin = 0.6f;
    mat.diffuseLayer->fresnelMax = 3.4f;
    mat.emissiveLayer1 = TexLayer("glow.dds");
    mat.emissiveLayer1->fresnelMode = m3::FresnelMode::Inverted;
    mat.emissiveLayer1->fresnelExponent = 2.0f;
    mat.emissiveLayer1->fresnelMin = 0.0f;
    mat.emissiveLayer1->fresnelMax = 1.0f;
    // The transform half, which only 1662 of 26463 shipped fresnel layers ask
    // for and which is inert without its flag.
    mat.emissiveLayer1->fresnelMask = {1.0f, 1.0f, 0.0f};
    mat.emissiveLayer1->fresnelTranslation = {0.0f, 0.0f, 0.25f};
    mat.specularLayer = TexLayer("spec.dds");
    mat.specularLayer->fresnelMode = m3::FresnelMode::Standard;
    mat.specularLayer->fresnelMask = {0.5f, 0.5f, 0.5f};
    mat.specularLayer->flags = m3::TextureLayerFlag::FresnelTransform |
                               m3::TextureLayerFlag::FresnelNormalize;

    model.standardMaterials = {mat};
    model.materialMaps = {Matm(m3::MaterialType::Standard, 0)};
    model.divisions = {Division(0)};

    const auto table = BuildM3SurfaceTable(model, kOneRegion, kMatm0);
    const M3Surface* s = table->Surface(0);
    REQUIRE(s != nullptr);

    const M3Layer& diffuse = s->layers[0];
    CHECK(diffuse.fresnelMode == 1);
    CHECK(diffuse.fresnelExponentBiasScale.x == 1.5f);
    CHECK(diffuse.fresnelExponentBiasScale.y == 0.6f);
    CHECK_THAT(diffuse.fresnelExponentBiasScale.z, Catch::Matchers::WithinAbs(2.8, 1e-6));
    // No transform flag: the mask and translation stay inert whatever the
    // record put in them.
    CHECK(diffuse.fresnelFlags == 0);

    const M3Layer& emissive = s->layers[3];
    CHECK(emissive.fresnelMode == 2);
    CHECK(emissive.fresnelFlags == 0);
    CHECK(emissive.fresnelMask.z == 1.0f);
    CHECK(emissive.fresnelTranslation.z == 0.0f);

    const M3Layer& specular = s->layers[2];
    CHECK(specular.fresnelFlags == 0x3);
    CHECK(specular.fresnelMask.x == 0.5f);

    // A layer with no fresnel leaves the neutral term, so the shader's one
    // compare per layer is all a non-fresnel material ever costs.
    m3::Model plain;
    m3::StandardMaterial pm;
    pm.diffuseLayer = TexLayer("diff.dds");
    plain.standardMaterials = {pm};
    plain.materialMaps = {Matm(m3::MaterialType::Standard, 0)};
    plain.divisions = {Division(0)};
    const auto plainTable = BuildM3SurfaceTable(plain, kOneRegion, kMatm0);
    CHECK(plainTable->Surface(0)->layers[0].fresnelMode == 0);
}

TEST_CASE("m3_surface_table: the evaluator emits a palette entry only for a layer that moves") {
    // The other half of the UV transform: the surface table names a slot and
    // the source fills it, keyed by io::M3UvTransformId so the two never have
    // to agree on anything but the model.
    m3::Model model;
    m3::Bone bone;
    bone.parentIndex = 0xFFFFu;
    bone.scale.initValue = {1.0f, 1.0f, 1.0f};
    bone.rotation.initValue = {0.0f, 0.0f, 0.0f, 1.0f};
    model.bones = {bone};

    m3::StandardMaterial mat;
    mat.diffuseLayer = TexLayer("diff.dds");
    mat.diffuseLayer->uvTiling.initValue = {3.0f, 1.0f};
    mat.diffuseLayer->uvOffset.initValue = {0.25f, 0.0f};
    // Left at the identity: no track drives it and its bind pose is neutral,
    // so it must NOT reach the palette.
    mat.specularLayer = TexLayer("spec.dds");
    mat.specularLayer->uvTiling.initValue = {1.0f, 1.0f};
    model.standardMaterials = {mat};
    model.materialMaps = {Matm(m3::MaterialType::Standard, 0)};
    model.divisions = {Division(0)};

    wio::M3ModelAdapter adapter(std::move(model));
    const auto fs = adapter.Evaluate(::whiteout::flakes::PoseRequest{});

    REQUIRE(fs.texAnimMatrices.size() == 1);
    const auto& e = fs.texAnimMatrices[0];
    CHECK(e.textureAnimId == wio::M3UvTransformId(0, wio::M3LayerSlot::Diffuse));
    CHECK(e.row0[0] == 3.0f);
    // Centre-pivot TRS: 3x tiling puts uv 1 at 2, and the source-space +0.25
    // offset reaches the translation column as 3·(-(0.5+0.25)) + 0.5 = -1.75.
    CHECK(e.row0[3] == -1.75f);
    CHECK(e.row1[1] == 1.0f);
    CHECK(e.row1[3] == 0.0f);
}

TEST_CASE("a PAR_'s flipbook comes from the first slot the material fills",
          "[m3_surface][sc2_particle]") {
    // `b_iUVMapping[slot]` is per texture SLOT and retail builds one UV per
    // slot in the vertex shader; a `renderer::Vertex` has ONE UV set, so a
    // single slot has to answer. It cannot be slot 0 unconditionally — 1223 of
    // 5037 corpus emitters have no active diffuse layer and draw out of the
    // emissive one, and reading the empty slot called every one of them
    // "no flipbook".
    const auto build = [](std::optional<m3::TextureLayer> diffuse,
                          std::optional<m3::TextureLayer> emissive) {
        m3::Model model;
        m3::StandardMaterial mat;
        mat.diffuseLayer = std::move(diffuse);
        mat.emissiveLayer1 = std::move(emissive);
        model.standardMaterials = {mat};
        model.materialMaps = {Matm(m3::MaterialType::Standard, 0)};
        model.divisions = {Division(0)};

        m3::ParticleEmitter par;
        par.materialIndex = 0;
        model.particleEmitters = {par};
        return model;
    };
    const auto flipbookOf = [](const m3::Model& model) {
        const auto table = BuildM3SurfaceTable(model, kOneRegion, kMatm0);
        REQUIRE(table->ParticleSurfaceBase() >= 0);
        const M3Surface* s = table->Surface(static_cast<u32>(table->ParticleSurfaceBase()));
        REQUIRE(s != nullptr);
        REQUIRE(s->valid);
        return M3ParticleFlipbookUv(*s);
    };

    const auto flip = [](const char* p) {
        return TexLayer(p, m3::UVMappingMode::ParticleFlipbook);
    };
    const auto plain = [](const char* p) {
        return TexLayer(p, m3::UVMappingMode::ExplicitUV0);
    };

    // The ordinary case, and the one slot 0 already answered.
    CHECK(flipbookOf(build(flip("sheet.dds"), std::nullopt)));
    CHECK_FALSE(flipbookOf(build(plain("one.dds"), std::nullopt)));

    // The 1223: no diffuse at all, the look drawn out of the emissive slot.
    // Reading slot 0 here reports the default of a layer nothing wrote.
    CHECK(flipbookOf(build(std::nullopt, flip("sheet.dds"))));
    CHECK_FALSE(flipbookOf(build(std::nullopt, plain("one.dds"))));

    // A diffuse that is PRESENT but unused is the same blindness — an
    // `M3LayerActive` layer is one with a texture or an authored colour, and
    // this has neither, so the emissive still answers.
    m3::TextureLayer empty;
    CHECK(flipbookOf(build(empty, flip("sheet.dds"))));

    // And where the two disagree, the dominant layer wins rather than the
    // last one read: 1057 corpus emitters are this shape and no single baked
    // UV set can serve them.
    CHECK_FALSE(flipbookOf(build(plain("base.dds"), flip("sheet.dds"))));
    CHECK(flipbookOf(build(flip("sheet.dds"), plain("glow.dds"))));

    // Nothing filled: false, not the uninitialised read of slot 0.
    m3::Model bare = build(std::nullopt, std::nullopt);
    const auto table = BuildM3SurfaceTable(bare, kOneRegion, kMatm0);
    const M3Surface* s = table->Surface(static_cast<u32>(table->ParticleSurfaceBase()));
    CHECK_FALSE(M3ParticleFlipbookUv(*s));
}
