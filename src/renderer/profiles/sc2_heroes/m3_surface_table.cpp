#include "m3_surface_table.h"

#include "io/m3/m3_model_adapter.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

namespace whiteout::flakes::renderer::profiles::sc2_heroes {

namespace {

using ::whiteout::m3::BlendMode;
using ::whiteout::m3::ColorChannelSelect;
using ::whiteout::m3::MaterialType;
using ::whiteout::m3::Model;
using ::whiteout::m3::SpecularMode;
using ::whiteout::m3::StandardMaterial;
using ::whiteout::m3::TextureLayer;
using ::whiteout::m3::TextureLayerFlag;
using ::whiteout::m3::UVMappingMode;
using io::M3LayerSlot;

static_assert(kM3LayerCount == static_cast<u32>(M3LayerSlot::Count));
static_assert(kM3LayerNormal == static_cast<u32>(M3LayerSlot::Normal));
static_assert(kM3LayerEnvironment == static_cast<u32>(M3LayerSlot::Environment));

/// MATM index → standard material, through at most one composite hop.
/// Composite sections reference MATM entries themselves; the highest
/// bind-pose weight whose entry is Standard wins. No recursion — a composite
/// naming another composite resolves to nothing, like every other type.
const StandardMaterial* ResolveStandard(const Model& model, u32 matmIndex) {
    if (matmIndex >= model.materialMaps.size())
        return nullptr;
    const auto& map = model.materialMaps[matmIndex];
    if (map.materialType == MaterialType::Standard) {
        return map.materialIndex < model.standardMaterials.size()
                   ? &model.standardMaterials[map.materialIndex]
                   : nullptr;
    }
    if (map.materialType == MaterialType::Composite &&
        map.materialIndex < model.compositeMaterials.size()) {
        const StandardMaterial* best = nullptr;
        f32 bestWeight = -1.0f;
        for (const auto& section : model.compositeMaterials[map.materialIndex].sections) {
            if (section.materialIndex >= model.materialMaps.size())
                continue;
            const auto& inner = model.materialMaps[section.materialIndex];
            if (inner.materialType != MaterialType::Standard ||
                inner.materialIndex >= model.standardMaterials.size())
                continue;
            const f32 w = section.mapMultiplier.initValue;
            if (w > bestWeight) {
                bestWeight = w;
                best = &model.standardMaterials[inner.materialIndex];
            }
        }
        return best;
    }
    return nullptr;
}

/// Bind-pose tint. Two unauthored-sentinel guards, both of which otherwise
/// paint models black: an all-zero ColorBGRA and a zero rgbMultiply are what
/// an exporter writes for a field nobody touched, not "multiply by nothing".
Vector4f LayerTint(const TextureLayer& layer, f32 extraMul) {
    const auto c = layer.color.initValue;
    f32 r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f, a = c.a / 255.0f;
    if (c.r == 0 && c.g == 0 && c.b == 0 && c.a == 0) {
        r = g = b = a = 1.0f;
    }
    f32 mul = layer.rgbMultiply.initValue;
    if (mul <= 0.0f)
        mul = 1.0f;
    mul *= extraMul;
    return {r * mul, g * mul, b * mul, a};
}

u8 ResolveUvSource(const TextureLayer& layer) {
    // The v1 subset: explicit set 0 or 1. Planar modes fall back to set 0
    // rather than sampling garbage; the envio modes carry no UV at all and
    // the environment slot reads a direction instead.
    return layer.uvMapping == UVMappingMode::ExplicitUV1 ? u8{1} : u8{0};
}

u32 LayerWrapFlags(const TextureLayer& layer) {
    const u32 f = static_cast<u32>(layer.flags);
    return ((f & static_cast<u32>(TextureLayerFlag::UVWrapX)) ? 0x1u : 0u) |
           ((f & static_cast<u32>(TextureLayerFlag::UVWrapY)) ? 0x2u : 0u);
}

} // namespace

std::unique_ptr<M3SurfaceTable> BuildM3SurfaceTable(const Model& model,
                                                    std::span<const std::size_t> emittedRegions) {
    auto table = std::make_unique<M3SurfaceTable>();
    if (model.divisions.empty())
        return table;
    const auto& div = model.divisions[0];

    // The adapter's canonical texture order, keyed the way it dedupes.
    const std::vector<io::M3TextureRef> textures = io::CollectM3Textures(model);
    std::unordered_map<std::string, i32> textureIndex;
    textureIndex.reserve(textures.size());
    for (std::size_t i = 0; i < textures.size(); ++i) {
        std::string key = textures[i].path;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        textureIndex.emplace(std::move(key), static_cast<i32>(i));
    }

    auto& surfaces = table->Surfaces();
    surfaces.resize(emittedRegions.size());
    for (std::size_t g = 0; g < emittedRegions.size(); ++g) {
        M3Surface& s = surfaces[g];
        if (emittedRegions[g] >= div.regions.size())
            continue;
        const auto& region = div.regions[emittedRegions[g]];

        if (region.getVersion() >= 5 && region.uvScale > 0.0f) {
            // The stored scale is in SNORM space: a stock v5 region carries
            // 16.0, and 16/32767 is the same 1/2048 older regions imply. This
            // table keeps uvMultiply per RAW i16 unit (the shader folds the
            // 32767 back in), so convert.
            s.uvMultiply = region.uvScale / 32767.0f;
            s.uvOffset = region.uvOffset;
        }

        // The region's first batch, the same pick GetMeshes stamps into
        // MeshData::materialId.
        const StandardMaterial* mat = nullptr;
        for (const auto& batch : div.batches) {
            if (batch.regionIndex == emittedRegions[g]) {
                mat = ResolveStandard(model, batch.materialIndex);
                break;
            }
        }
        if (!mat)
            continue;

        s.valid = true;
        s.blendMode = mat->blendMode;
        s.materialFlags = static_cast<u32>(mat->flags);
        s.priority = mat->priority;
        s.specularExponent = mat->specularExponent > 0.0f ? mat->specularExponent : 20.0f;
        if (mat->alphaTestThreshold > 0) {
            s.alphaTestThreshold =
                std::min(static_cast<f32>(mat->alphaTestThreshold) / 255.0f, 1.0f);
        }

        f32 hdrSpec = mat->hdrSpecularMultiplier;
        if (hdrSpec <= 0.0f)
            hdrSpec = 1.0f;
        f32 hdrEmis = mat->hdrEmissiveMultiplier;
        if (hdrEmis <= 0.0f)
            hdrEmis = 1.0f;
        s.emissiveMultiplier = hdrEmis;

        // FakeEnergyConservingSpec (psmaterial.fx:203): dim the material
        // specular by a polynomial fit of the relative highlight area, so high
        // exponents keep their energy and broad ones dim. The engine sets the
        // axis to !(flags & SimulateRoughness) (CMaterial_ApplyForDraw,
        // 0x1028bc1a0) and no shipped corpus material carries the flag, so
        // retail effectively always dims — the 2-5x hdrSpecularMultiplier
        // values are authored against it.
        //
        // Retail feeds it the SPECULARITY, which a gloss layer varies per
        // pixel (psmainshading.fx:154 -> MaterialSpecularity). Constant
        // exponent, constant dim: fold it into the spec tint. Gloss layer, and
        // the shader has to pay it per pixel instead — `dimPerPixel` is that
        // hand-off.
        const bool energyDim =
            (static_cast<u32>(mat->flags) &
             static_cast<u32>(::whiteout::m3::MaterialFlag::SimulateRoughness)) == 0;
        const TextureLayer* gloss = io::M3LayerForSlot(*mat, M3LayerSlot::Gloss);
        s.dimPerPixel = energyDim && gloss && io::M3LayerActive(*gloss);
        if (energyDim && !s.dimPerPixel) {
            const f32 p = std::clamp(s.specularExponent, 1.0f, 512.0f);
            const f32 dim =
                std::clamp(-0.000004444f * p * p + 0.004333f * p + 0.0020834f, 0.0f, 1.0f);
            hdrSpec *= dim;
        }

        // p_vEnvioConstantDiffSpec.x. Only MAT_ v20 carries the three
        // hdrEnvironment* fields, and the parser leaves them zero below that —
        // measured, 717 of 744 shipped env materials read 0 there and the 27
        // v20 ones read exactly 1. Taking the field at face value would
        // multiply almost every reflection to black, so pre-v20 means "no
        // constant", not "constant zero". The diffuse and specular
        // multipliers are zero on all 228 v20 materials measured, so
        // b_iEnvioMultipliers' lighting-modulated branches are dead in
        // shipped content and this flat term is the whole of it.
        f32 envConstant = 1.0f;
        if (mat->getVersion() >= 20 && mat->hdrEnvironmentConstant > 0.0f)
            envConstant = mat->hdrEnvironmentConstant;

        for (u32 slot = 0; slot < kM3LayerCount; ++slot) {
            M3Layer& out = s.layers[slot];
            const TextureLayer* layer =
                io::M3LayerForSlot(*mat, static_cast<M3LayerSlot>(slot));
            if (!layer || !io::M3LayerActive(*layer))
                continue;
            out.mode = io::M3LayerHasTexture(*layer) ? u8{1} : u8{2};
            if (out.mode == 1) {
                std::string key = io::M3CleanPath(layer->texturePath);
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (auto it = textureIndex.find(key); it != textureIndex.end())
                    out.textureId = it->second;
            }
            out.uvSource = ResolveUvSource(*layer);
            out.channels = static_cast<u8>(layer->colorType);
            out.wrapFlags = LayerWrapFlags(*layer);

            f32 extraMul = 1.0f;
            switch (static_cast<M3LayerSlot>(slot)) {
            case M3LayerSlot::Diffuse:
                // TEAMCOLOR_DIFFUSE, gated the way the shader is
                // (psmateriallayer.fx:213: ChannelSelect != RGB): the diffuse
                // alpha is the team mask.
                if (layer->colorType != ColorChannelSelect::RGB)
                    out.teamColorMode = 1;
                break;
            case M3LayerSlot::Decal:
                out.blendOp = static_cast<u8>(mat->layerBlendMode);
                break;
            case M3LayerSlot::Specular:
                extraMul = hdrSpec;
                if (mat->specularMode == SpecularMode::AlphaOnly)
                    out.channels = static_cast<u8>(ColorChannelSelect::Alpha);
                break;
            case M3LayerSlot::Emissive:
                out.blendOp = static_cast<u8>(mat->emissiveBlendMode1);
                break;
            case M3LayerSlot::Emissive2:
                out.blendOp = static_cast<u8>(mat->emissiveBlendMode2);
                break;
            case M3LayerSlot::AlphaMask:
            case M3LayerSlot::AlphaMask2:
                // A mask samples its alpha unless the author picked a channel.
                if (layer->colorType == ColorChannelSelect::RGB)
                    out.channels = static_cast<u8>(ColorChannelSelect::Alpha);
                break;
            case M3LayerSlot::Environment:
                // ApplyEnv's op is the material's layer blend, the same field
                // the decal reads (psmaterial.fx:332).
                out.blendOp = static_cast<u8>(mat->layerBlendMode);
                extraMul = envConstant;
                s.envReflect = layer->uvMapping == UVMappingMode::ReflectCubicEnvio ||
                               layer->uvMapping == UVMappingMode::ReflectSphericalEnvio;
                break;
            default:
                break;
            }
            out.tint = LayerTint(*layer, extraMul);
            out.add = layer->rgbAdd.initValue * extraMul;
            out.invert =
                (static_cast<u32>(layer->flags) & static_cast<u32>(TextureLayerFlag::ColorInvert))
                    ? u8{1}
                    : u8{0};
            out.clampColor =
                (static_cast<u32>(layer->flags) & static_cast<u32>(TextureLayerFlag::ColorClamp))
                    ? u8{1}
                    : u8{0};
        }
    }
    return table;
}

core::SurfaceClass M3ClassifySurface(const M3Surface& surface) {
    core::SurfaceClass c;
    c.visible = surface.valid;
    if (surface.blendMode == BlendMode::Opaque) {
        c.blend = surface.alphaTestThreshold > 0.0f ? core::BlendClass::AlphaKey
                                                    : core::BlendClass::Opaque;
    } else {
        c.blend = core::BlendClass::Transparent;
    }
    return c;
}

} // namespace whiteout::flakes::renderer::profiles::sc2_heroes
