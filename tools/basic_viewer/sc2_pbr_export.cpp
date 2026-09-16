// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "sc2_pbr_export.h"

#include "renderer/model/model_source_utils.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/content_ref.h"
#include "whiteout/flakes/util/texture_image_usage.h"

#include <whiteout/models/wem/materials/native.h>
#include <whiteout/models/wem/retarget.h>
#include <whiteout/textures/pbr_bake.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes {

namespace {

namespace nat = ::whiteout::models::wem::native;

/// An `.m3` string is a fixed-width field, so a path arrives with its NULs.
std::string Trim(const std::string& value) {
    std::size_t end = value.size();
    while (end > 0 && (value[end - 1] == '\0' || value[end - 1] == ' ')) {
        --end;
    }
    return value.substr(0, end);
}

std::string Lower(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

const nat::M3TextureLayer* LayerOf(const std::optional<nat::M3TextureLayer>& layer) {
    if (!layer.has_value() || Trim(layer->texturePath).empty()) {
        return nullptr;
    }
    return &*layer;
}

/// Which component of a layer StarCraft II actually reads.
///
/// `SelectChannels` (psmateriallayer.fx, and `m3_standard.slang` after it)
/// splats one channel across all four for the single-channel modes, so a mask
/// authored in green is read from green and not from red. `RGB` forces alpha to
/// 1, which for the team ops means *the whole surface* — that is the one case
/// with no channel to name, so it comes back absent and the caller uses its
/// constant.
std::optional<tx::Channel> AlphaChannelFor(nat::M3ColorChannelSelect select) {
    switch (select) {
    case nat::M3ColorChannelSelect::Red:
        return tx::Channel::R;
    case nat::M3ColorChannelSelect::Green:
        return tx::Channel::G;
    case nat::M3ColorChannelSelect::Blue:
        return tx::Channel::B;
    case nat::M3ColorChannelSelect::Alpha:
    case nat::M3ColorChannelSelect::RGBA:
        return tx::Channel::A;
    case nat::M3ColorChannelSelect::RGB:
        break;
    }
    return std::nullopt;
}

/// The scalar component of a *mask* layer — gloss, occlusion, the environment
/// mask. Unlike the team mask above, a mask authored as a colour is greyscale
/// and red is as good as any of the three.
tx::Channel ScalarChannelFor(nat::M3ColorChannelSelect select) {
    return AlphaChannelFor(select).value_or(tx::Channel::R);
}

/// Whether the specular map's samples are decoded before the bake reads them.
///
/// Yes, and not as a modelling choice: `DetermineImageUsage` gives a
/// `..._spec.dds` no data suffix, so `ApplyTextureSrgbPolicy` promotes it, and
/// the HD render path this export is compared against samples it through an
/// sRGB view. The number the game's own shader multiplies is the decoded one,
/// so that is the number a reflectance has to be derived from.
constexpr bool kSpecularIsDisplayReferred = true;

/// The exponent the engine substitutes for an unauthored zero
/// (`m3_surface_table.cpp`, and 35.2% of measured materials need it).
constexpr f32 kDefaultSpecularExponent = 20.0f;

/// `MaterialFlag::SimulateRoughness` — StarCraft II's own PBR styling (the
/// StarTools guide: the spec map doubles as the envio mask, the gloss rides
/// its alpha as a perceptual roughness), on 1,801 StarCraft II and 4,972
/// Heroes materials. Its absence is what turns `FakeEnergyConservingSpec` on.
constexpr u32 kSimulateRoughness = 0x800u;

/// The mean linear luminance of Reforged's stock environment panorama
/// (`_hd.w3mod/replaceabletextures/environmentmap.dds`, weighted by
/// cos(latitude)) — the probe every exported material reflects, `exportPbr`
/// naming it in slot 5 unconditionally. A StarCraft II envio layer adds (or
/// multiplies by) `cube * tint * mask` in radiance and Reforged adds
/// `probe * F0`, so the F0 that keeps a reflection as bright as it was is
/// the mask times the ratio of the two environments' brightness. Measured
/// 2026-09-09: StarCraft II's PBR cube averages 0.075, `Reflection_Silver`
/// 0.076, Heroes' shared reflection 0.025 — the placeholder rate of 1 that
/// stood here under-reflected by an order of magnitude.
constexpr f32 kReforgedProbeLuminance = 0.0875f;

/// What lobe width a mip-0 env-map lookup reads as once it is a probe
/// reflection. Sharp, but not a mirror — the source cubes are small and soft.
constexpr f32 kEnvRoughnessCap = 0.30f;

/// The mean linear RGB of a texture's top mip — the flat colour a map
/// collapses to when its target has no texture to give it.
void MeanLinearRgb(const tx::Texture& texture, f32 out[3]) {
    out[0] = out[1] = out[2] = 1.0f;
    const tx::Texture rgba = texture.copyAsFormat(tx::PixelFormat::RGBA8);
    const std::span<const u8> px = rgba.mipData(0);
    if (px.size() < 4) {
        return;
    }
    auto toLinear = [](f32 v) {
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    f64 sum[3] = {0.0, 0.0, 0.0};
    for (std::size_t i = 0; i < px.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            sum[c] += toLinear(px[i + static_cast<std::size_t>(c)] / 255.0f);
        }
    }
    const f64 count = static_cast<f64>(px.size() / 4);
    for (int c = 0; c < 3; ++c) {
        out[c] = static_cast<f32>(sum[c] / count);
    }
}

/// The mean linear luminance of a texture's top mip — what an env map is
/// "worth" when its per-direction detail has to collapse into one scalar.
f32 MeanLinearLuminance(const tx::Texture& texture) {
    const tx::Texture rgba = texture.copyAsFormat(tx::PixelFormat::RGBA8);
    const std::span<const u8> px = rgba.mipData(0);
    if (px.size() < 4) {
        return 0.0f;
    }
    auto toLinear = [](f32 v) {
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    f64 sum = 0.0;
    const std::size_t count = px.size() / 4;
    for (std::size_t i = 0; i < px.size(); i += 4) {
        sum += 0.2126 * toLinear(px[i] / 255.0f) + 0.7152 * toLinear(px[i + 1] / 255.0f) +
               0.0722 * toLinear(px[i + 2] / 255.0f);
    }
    return static_cast<f32>(sum / static_cast<f64>(count));
}

/// M3's layer blend vocabulary onto the bake's decal fold.
std::optional<tx::pbr::DecalOp> DecalOpFor(nat::M3LayerBlendOp op) {
    switch (op) {
    case nat::M3LayerBlendOp::Mod:
        return tx::pbr::DecalOp::Mod;
    case nat::M3LayerBlendOp::Mod2x:
        return tx::pbr::DecalOp::Mod2x;
    case nat::M3LayerBlendOp::Add:
        return tx::pbr::DecalOp::AddScaled;
    case nat::M3LayerBlendOp::AddNoAlpha:
        return tx::pbr::DecalOp::Add;
    case nat::M3LayerBlendOp::Lerp:
        return tx::pbr::DecalOp::Lerp;
    default:
        // The two TeamColor ops are masks, not colour.
        return std::nullopt;
    }
}

bool IsTeamOp(nat::M3LayerBlendOp op) {
    return op == nat::M3LayerBlendOp::TeamColorEmissiveAdd ||
           op == nat::M3LayerBlendOp::TeamColorDiffuseAdd;
}

/// The `M3Standard` behind a material, whether the file held one or a `MADD`
/// the loader restores one from.
const nat::M3Standard* StandardOf(const wem::Material& material) {
    const auto* m3 = std::get_if<nat::M3Material>(&material.Native());
    if (m3 == nullptr) {
        return nullptr;
    }
    if (const auto* standard = std::get_if<nat::M3Standard>(&m3->body)) {
        return standard;
    }
    return m3->restoredStandard.has_value() ? &*m3->restoredStandard : nullptr;
}

/// Resolves an `.m3` texture path to the document index the importer gave it,
/// and decodes it once. Both halves are cached: a StarCraft II model shares one
/// normal map across every material of a unit, and a BCn decode of a 2048 map
/// is not free.
class TextureCache {
public:
    TextureCache(wem::Document& document, io::IContentProvider* provider)
        : document_(document), provider_(provider) {
        for (u32 i = 0; i < static_cast<u32>(document.textures.size()); ++i) {
            byPath_.emplace(Lower(Trim(PathOf(document.textures[i]))), i);
        }
    }

    /// The `Document::textures` index for @p path, or `kInvalidIndex`.
    u32 IndexOf(const std::string& path) const {
        const auto it = byPath_.find(Lower(Trim(path)));
        return it == byPath_.end() ? wem::kInvalidIndex : it->second;
    }

    /// The decoded texture behind @p path, or null. Failures are cached too, so
    /// a missing file is looked for once.
    const tx::Texture* Decode(const std::string& path) {
        const std::string key = Lower(Trim(path));
        if (key.empty() || provider_ == nullptr) {
            return nullptr;
        }
        const auto cached = decoded_.find(key);
        if (cached != decoded_.end()) {
            return cached->second.has_value() ? &*cached->second : nullptr;
        }

        std::optional<tx::Texture> texture;
        std::string extension;
        std::optional<std::vector<u8>> bytes =
            provider_->ReadFile(ContentRef::FromPath(Trim(path)), &extension);
        if (bytes && !bytes->empty()) {
            extension = Lower(extension);
            if (extension.empty()) {
                extension = renderer::model::SniffTextureExtension(
                    std::span<const u8>(bytes->data(), bytes->size()));
            }
            texture = renderer::model::DispatchTextureParser(
                extension, [&](auto& parser) { return parser.parse(std::span<const u8>(*bytes)); });
        }
        const auto inserted = decoded_.emplace(key, std::move(texture)).first;
        return inserted->second.has_value() ? &*inserted->second : nullptr;
    }

    /// Adds a texture the source never had and returns its document index.
    u32 Intern(const std::string& path) {
        const u32 existing = IndexOf(path);
        if (existing != wem::kInvalidIndex) {
            return existing;
        }
        wem::TextureRef ref;
        ref.key = wem::TexturePath{path};
        ref.path = path;
        ref.declaredSpace = wem::ColorSpace::Linear;
        document_.textures.push_back(std::move(ref));
        const u32 index = static_cast<u32>(document_.textures.size() - 1);
        byPath_.emplace(Lower(path), index);
        return index;
    }

private:
    static std::string PathOf(const wem::TextureRef& ref) {
        if (const auto* path = std::get_if<wem::TexturePath>(&ref.key)) {
            return path->value;
        }
        return ref.path;
    }

    wem::Document& document_;
    io::IContentProvider* provider_ = nullptr;
    std::unordered_map<std::string, u32> byPath_;
    std::unordered_map<std::string, std::optional<tx::Texture>> decoded_;
};

/// `Assets/Textures/Marine_Specular_Blood.dds` -> `.../Marine_Blood_orm.dds`,
/// which is the shape Reforged's own content uses (`..._Diffuse` beside
/// `..._ORM`) and therefore what a modder opening the folder expects. A known
/// role suffix is replaced rather than appended so the name does not read
/// `_specular_orm`.
std::string OrmNameFor(const std::string& sourcePath, const std::string& materialName,
                       u32 materialIndex) {
    std::string base = Trim(sourcePath);
    if (base.empty()) {
        base = materialName.empty() ? ("material" + std::to_string(materialIndex)) : materialName;
        // A material NAME is not a file name: `15 - Default` and `t Glow A`
        // are shipped spellings.
        for (char& c : base) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
                c = '_';
            }
        }
    }
    const std::size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > base.find_last_of("/\\") + 1) {
        base = base.substr(0, dot);
    }
    // Longest first: `_specular` before `_spec`, or the cut leaves `_ular`.
    static const char* kRoles[] = {"_specular", "_diffuse",  "_emissive", "_normal",
                                   "_emiss",    "_gloss",    "_spec",     "_diff",
                                   "_norms",    "_norm",     "_nrm",      "_nom"};
    const std::string lowered = Lower(base);
    for (const char* role : kRoles) {
        const std::size_t at = lowered.rfind(role);
        if (at != std::string::npos) {
            // Cut the role out wherever it sits, not only off the end:
            // `Marine_Specular_Blood` is the shipped spelling and
            // `Marine_Specular_Blood_orm` names the wrong map twice.
            base.erase(at, std::strlen(role));
            break;
        }
    }
    return base + "_orm.dds";
}

/// `.../Overlord_Diffuse.dds` -> `.../Overlord_Diffuse_cut.dds`.
///
/// A coverage-composed base colour cannot go back into the diffuse's own file:
/// StarCraft II shares one diffuse across materials with DIFFERENT coverage --
/// an Overlord's opaque Body and its blended Sacs read the same texture, and
/// the Sacs' mask is the specular map's alpha -- so the composed map is a new
/// texture and only this material's slot points at it.
std::string CutoutNameFor(const std::string& sourcePath, u32 materialIndex) {
    std::string base = Trim(sourcePath);
    const std::size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > base.find_last_of("/\\") + 1) {
        base = base.substr(0, dot);
    }
    if (base.empty()) {
        base = "material" + std::to_string(materialIndex);
    }
    return base + "_cut.dds";
}

/// The bind-pose tint of a layer, which is what the source's shader multiplies
/// its sample by. `LayerTint` in `m3_surface_table.cpp` says the same thing from
/// the renderer's side, including both unauthored-sentinel guards: an all-zero
/// `ColorBGRA` and a zero `rgbMultiply` are what an exporter writes for a field
/// nobody touched, not "multiply by nothing".
void LayerTintInto(const nat::M3TextureLayer& layer, f32 extraMul, f32 out[3]) {
    const auto c = layer.color.initValue;
    f32 rgb[3] = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f};
    if (c.r == 0 && c.g == 0 && c.b == 0 && c.a == 0) {
        rgb[0] = rgb[1] = rgb[2] = 1.0f;
    }
    f32 mul = layer.rgbMultiply.initValue;
    if (mul <= 0.0f) {
        mul = 1.0f;
    }
    mul *= extraMul;
    for (int i = 0; i < 3; ++i) {
        out[i] = rgb[i] * mul;
    }
}

/// An alpha-mask layer is active with no texture when it is a solid colour
/// (flag `Color`), which `LayerOf`'s path test misses — 3,466 shipped masks.
const nat::M3TextureLayer* MaskLayerOf(const std::optional<nat::M3TextureLayer>& layer) {
    if (!layer.has_value()) {
        return nullptr;
    }
    if (hasFlag(layer->flags, nat::M3TextureLayerFlag::Color)) {
        return &*layer;
    }
    return LayerOf(layer);
}

/// The coverage term one alpha-mask layer states, as the engine computes its
/// alpha (`ComputeLayerColorInternal`): channel select, `* alphaFactor`,
/// invert, `* rgbMultiply + rgbAdd`. The unauthored-zero guards are
/// `LayerTintInto`'s: a zero multiplier is a field nobody touched, not
/// "multiply by nothing".
tx::pbr::ScalarInput CoverageOf(const nat::M3TextureLayer& layer, TextureCache& cache,
                                const wem::ElementRef& where, wem::Diagnostics& out) {
    tx::pbr::ScalarInput mask;
    mask.constant = 1.0f;
    if (hasFlag(layer.flags, nat::M3TextureLayerFlag::Color)) {
        // A solid-colour layer's alpha is its constant colour's, with the
        // all-zero unauthored guard.
        const auto c = layer.color.initValue;
        const bool authored = !(c.r == 0 && c.g == 0 && c.b == 0 && c.a == 0);
        mask.constant = authored ? static_cast<f32>(c.a) / 255.0f : 1.0f;
    } else if (const std::optional<tx::Channel> channel = AlphaChannelFor(layer.colorType)) {
        mask.texture = cache.Decode(layer.texturePath);
        mask.channel = *channel;
        if (mask.texture == nullptr) {
            out.warn(wem::DiagCode::TextureUnresolved,
                     "the alpha mask '" + Trim(layer.texturePath) +
                         "' could not be decoded; the surface exports opaque",
                     where, wem::ProfileId::Wc3Reforged);
        }
    }
    // An RGB select forces the alpha to 1 (`SelectChannels`) — and the sweep
    // found zero shipped masks that use it — so the fall-through constant is
    // exactly what the engine reads.
    const f32 alphaFactor = layer.mapAlpha.initValue;
    mask.scale = alphaFactor > 0.0f ? alphaFactor : 1.0f;
    mask.invert = hasFlag(layer.flags, nat::M3TextureLayerFlag::ColorInvert);
    const f32 multiply = layer.rgbMultiply.initValue;
    mask.postScale = multiply > 0.0f ? multiply : 1.0f;
    mask.bias = layer.rgbAdd.initValue;
    return mask;
}

/// The envio mask as `ApplyEnv` multiplies it: `cMaskValue.rgb` under Add
/// and Mod — the decoded RGB's luminance for an RGB select, RGB times alpha
/// for RGBA, one channel splatted otherwise — and `.a` under Lerp, where an
/// RGB select is a constant 1. The texture's own colour-space policy decides
/// the decode, the way the renderer binds it (a `_spec` map is sRGB).
tx::pbr::ScalarInput EnvMaskOf(const nat::M3TextureLayer& layer, nat::M3LayerBlendOp op,
                               TextureCache& cache, const wem::ElementRef& where,
                               wem::Diagnostics& out) {
    tx::pbr::ScalarInput mask;
    mask.constant = 1.0f;
    const bool byAlpha = op == nat::M3LayerBlendOp::Lerp;
    if (hasFlag(layer.flags, nat::M3TextureLayerFlag::Color)) {
        const auto c = layer.color.initValue;
        if (!(c.r == 0 && c.g == 0 && c.b == 0 && c.a == 0)) {
            mask.constant = byAlpha ? static_cast<f32>(c.a) / 255.0f
                                    : (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) / 255.0f;
        }
    } else {
        mask.texture = cache.Decode(layer.texturePath);
        if (mask.texture == nullptr) {
            out.warn(wem::DiagCode::TextureUnresolved,
                     "the envio mask '" + Trim(layer.texturePath) +
                         "' could not be decoded; the reflection covers the surface",
                     where, wem::ProfileId::Wc3Reforged);
        }
        mask.srgb = !io::IsLinearImageUsage(io::DetermineImageUsage(Trim(layer.texturePath)));
        switch (layer.colorType) {
        case nat::M3ColorChannelSelect::RGB:
            if (byAlpha) {
                mask.texture = nullptr;
            } else {
                mask.luminance = true;
            }
            break;
        case nat::M3ColorChannelSelect::RGBA:
            mask.channel = tx::Channel::A;
            mask.luminance = !byAlpha;
            mask.alphaWeighted = !byAlpha;
            break;
        default:
            mask.channel = AlphaChannelFor(layer.colorType).value_or(tx::Channel::R);
            break;
        }
    }
    const f32 alphaFactor = layer.mapAlpha.initValue;
    mask.scale = alphaFactor > 0.0f ? alphaFactor : 1.0f;
    mask.invert = hasFlag(layer.flags, nat::M3TextureLayerFlag::ColorInvert);
    const f32 multiply = layer.rgbMultiply.initValue;
    mask.postScale = multiply > 0.0f ? multiply : 1.0f;
    mask.bias = layer.rgbAdd.initValue;
    return mask;
}

/// One material's worth of sources, resolved.
struct Sources {
    const tx::Texture* specular = nullptr;
    const tx::Texture* gloss = nullptr;
    const tx::Texture* occlusion = nullptr;
    const tx::Texture* team = nullptr;
    tx::Channel glossChannel = tx::Channel::A;
    bool glossWholeSurface = false;
    tx::Channel occlusionChannel = tx::Channel::R;
    std::optional<tx::Channel> teamChannel;
    bool teamWholeSurface = false;
    std::string ormNameSource;
};

} // namespace

Sc2PbrBakeResult BakeSc2AsReforgedPbr(wem::Document& document, io::IContentProvider* provider,
                                      const tx::pbr::NormalRestatement& normal,
                                      wem::Diagnostics& out, const Vector3f& teamColor) {
    Sc2PbrBakeResult result;
    if (provider == nullptr) {
        return result;
    }

    const wem::ProfileId source = document.carries(wem::ProfileId::Sc2) ? wem::ProfileId::Sc2
                                  : document.carries(wem::ProfileId::Heroes)
                                      ? wem::ProfileId::Heroes
                                      : wem::ProfileId::Count;
    if (source == wem::ProfileId::Count) {
        return result;
    }

    // The derive `StageWemDocument` would have run, run here instead: the PBR
    // slots this fills have to exist before there is anything to fill, and a
    // document that already carries the target is left alone by both.
    if (!document.carries(wem::ProfileId::Wc3Reforged)) {
        document.declare(wem::ProfileId::Wc3Reforged);
        const wem::DeriveResult derived =
            wem::DeriveProfile(document, source, wem::ProfileId::Wc3Reforged);
        out.append(derived.diagnostics);
        if (!derived.ok) {
            return result;
        }
    }

    TextureCache cache(document, provider);
    // Which mask recipe claimed a cutout texture name -- two materials with the
    // same diffuse and the same masks share one baked map; different masks get
    // a suffixed name instead of overwriting each other's pixels.
    std::unordered_map<std::string, std::string> cutoutClaims;

    for (wem::Model& model : document.models) {
        const wem::ProfileMaterialSet* sc2 = model.setFor(source);
        wem::ProfileMaterialSet* wc3 = model.setFor(wem::ProfileId::Wc3Reforged);
        if (sc2 == nullptr || wc3 == nullptr) {
            continue;
        }
        // `DeriveProfile` pushes one material per source material in order, so
        // the two arrays are index-parallel. Anything else is a derive that did
        // not run and there is nothing to fill.
        if (sc2->materials.size() != wc3->materials.size()) {
            continue;
        }

        for (std::size_t m = 0; m < sc2->materials.size(); ++m) {
            const wem::ElementRef where(wem::ElementKind::Material, static_cast<u32>(m));
            const nat::M3Standard* standard = StandardOf(sc2->materials[m]);
            if (standard == nullptr) {
                continue;
            }
            wem::CommonMaterial& common = wc3->materials[m].MutableCommon();
            wem::PbrDeferredBody* body = common.pbr();
            if (body == nullptr) {
                continue;
            }

            // --- the normal map -------------------------------------------
            if (const nat::M3TextureLayer* layer = LayerOf(standard->normalLayer)) {
                const std::string path = Trim(layer->texturePath);
                const u32 index = cache.IndexOf(path);
                if (index != wem::kInvalidIndex && result.baked.find(index) == result.baked.end()) {
                    if (const tx::Texture* decoded = cache.Decode(path)) {
                        std::optional<tx::Texture> restated =
                            tx::pbr::ConvertNormalXInAlpha(*decoded, normal);
                        if (restated) {
                            result.baked.emplace(
                                index, BakedTexture{std::move(*restated), tx::PixelFormat::BC5});
                            ++result.normalsRestated;
                        }
                    }
                }
            }

            // --- what the ORM is made of -----------------------------------
            Sources sources;
            if (const nat::M3TextureLayer* layer = LayerOf(standard->specularLayer)) {
                sources.specular = cache.Decode(layer->texturePath);
                sources.ormNameSource = Trim(layer->texturePath);
            }
            // A gloss layer scales the SPECULARITY, and the shader reads its
            // alpha after the channel-select splat (`psmaterial.fx:579`). A
            // layer whose select is RGB has that alpha forced to 1, so it
            // leaves the exponent alone rather than zeroing it.
            if (const nat::M3TextureLayer* layer = LayerOf(standard->glossLayer)) {
                const std::optional<tx::Channel> channel = AlphaChannelFor(layer->colorType);
                sources.glossWholeSurface = !channel.has_value();
                if (!sources.glossWholeSurface) {
                    sources.gloss = cache.Decode(layer->texturePath);
                    sources.glossChannel = *channel;
                }
            }
            if (const nat::M3TextureLayer* layer = LayerOf(standard->ambientOcclusionLayer)) {
                // The ORM is one map over the base colour's coordinates, the
                // rule the masks and the decal below already follow: an
                // occlusion on another UV set would land on texels it was never
                // painted for. SM_ArmorySpectreCrate's cloak reuses its diffuse
                // over UV 1 as occlusion, and baked, it stained the cloth.
                const nat::M3TextureLayer* paint = LayerOf(standard->diffuseLayer);
                if (paint != nullptr && layer->uvMapping != paint->uvMapping) {
                    out.warn(wem::DiagCode::LossyKindConversion,
                             "the ambient occlusion samples a different UV set than the diffuse; "
                             "it is not baked into the occlusion map",
                             where, wem::ProfileId::Wc3Reforged);
                } else {
                    sources.occlusion = cache.Decode(layer->texturePath);
                    sources.occlusionChannel = ScalarChannelFor(layer->colorType);
                }
            }

            // --- the team-colour mask, which is two mechanisms -------------
            //
            // The one that matters is not a combine op. `psmateriallayer.fx`
            // tints the ALBEDO through the diffuse layer's own alpha whenever
            // that layer's channel select is not RGB --
            // `lerp(teamColour, texel.rgb, pow(texel.a, exponent))` -- so the
            // team *weight* is `1 - diffuse.a`, the exponent is engine-set to
            // 1, and this is how nearly every unit in both games is coloured.
            // The renderer says so in as many words: "the alpha-mask layers
            // times the alpha factor -- NOT the diffuse alpha, which is the
            // team-colour mask".
            const nat::M3TextureLayer* diffuseLayer = LayerOf(standard->diffuseLayer);
            // A material with no specular map still names its ORM after a real
            // texture, so a constants-only bake (a whole-surface team glow)
            // lands beside the model's other maps instead of at the root.
            if (sources.ormNameSource.empty() && diffuseLayer != nullptr) {
                sources.ormNameSource = Trim(diffuseLayer->texturePath);
            }
            const bool diffuseIsTeamMasked =
                diffuseLayer != nullptr &&
                diffuseLayer->colorType != nat::M3ColorChannelSelect::RGB;
            const tx::Texture* diffuse =
                diffuseLayer != nullptr ? cache.Decode(diffuseLayer->texturePath) : nullptr;

            // The other is the `TeamColor*Add` op on an emissive layer, and it
            // is the rare one: 10,527 of 176,955 shipped standard materials
            // across both games, every one of them `TeamColorEmissiveAdd`,
            // 9,905 on emissive 1 and 861 on emissive 2, and not one on the
            // decal's `layerBlendMode`.
            const nat::M3TextureLayer* teamLayer = nullptr;
            std::size_t teamSlot = 0;
            if (IsTeamOp(standard->emissiveBlendMode1)) {
                teamLayer = LayerOf(standard->emissiveLayer1);
                teamSlot = 1;
            }
            if (teamLayer == nullptr && IsTeamOp(standard->emissiveBlendMode2)) {
                teamLayer = LayerOf(standard->emissiveLayer2);
                teamSlot = 2;
            }
            if (teamLayer != nullptr) {
                sources.team = cache.Decode(teamLayer->texturePath);
                sources.teamChannel = AlphaChannelFor(teamLayer->colorType);
                sources.teamWholeSurface = !sources.teamChannel.has_value();
            }

            const bool anyTeam =
                sources.team != nullptr || (diffuseIsTeamMasked && diffuse != nullptr);

            // --- the coverage, which never used to cross at all ------------
            //
            // StarCraft II's per-texel coverage is its ALPHA-MASK LAYERS --
            // `cFinal.a = mask1.a * mask2.a * alphaFactor` (psmaterial.fx:380)
            // -- and Reforged reads coverage off the base colour's alpha.
            // 104,869 of 176,955 shipped materials carry a mask, and without
            // this an alpha-tested export either leaked the team mask as the
            // cutout (unbaked diffuse) or erased the cutout (baked opaque).
            tx::pbr::ScalarInput coverage1{nullptr, tx::Channel::A, false, 1.0f};
            tx::pbr::ScalarInput coverage2{nullptr, tx::Channel::A, false, 1.0f};
            // StarCraft II tests the COMPOSED coverage against a per-material
            // threshold; Warcraft III's Transparent filter tests the written
            // alpha at a fixed 0.75. A keyed surface therefore bakes the
            // test's RESULT (0/255 at the source's own cut-off).
            const bool alphaKeyed = standard->blendMode == nat::M3BlendMode::Opaque &&
                                    standard->alphaTestThreshold > 0;
            // The blends that READ the written alpha. Additive and the two
            // modulates ignore it on both engines; for them there is nothing
            // to compose and nothing to leak.
            const bool readsAlpha = alphaKeyed ||
                                    standard->blendMode == nat::M3BlendMode::AlphaBlend ||
                                    standard->blendMode == nat::M3BlendMode::AlphaAdd;
            // Every alpha-reading surface takes the composed coverage — with
            // no masks that is a constant 1, and writing THAT is still the
            // fix: StarCraft II's coverage was 1 (`vertColor.a * alphaFactor`;
            // the masks are the only per-texel term), while an unbaked export
            // hands Warcraft III whatever the texture's own alpha holds.
            bool hasCoverage = readsAlpha;
            if (readsAlpha) {
                const nat::M3TextureLayer* masks[2] = {MaskLayerOf(standard->alphaLayer1),
                                                       MaskLayerOf(standard->alphaLayer2)};
                tx::pbr::ScalarInput* into[2] = {&coverage1, &coverage2};
                for (int i = 0; i < 2; ++i) {
                    if (masks[i] == nullptr) {
                        continue;
                    }
                    // The bake samples every source over the base colour's
                    // coordinates, so a mask on another UV set cannot be
                    // composed from pixels alone -- where the two sets land a
                    // texel depends on the mesh. 18,792 of ~119k shipped mask
                    // layers do this. The fallback is the DIFFUSE's own alpha
                    // when the diffuse is not team-masked: a glow texture's
                    // alpha usually resembles its mask, and forcing opaque
                    // instead turned the Aiur light bridge's translucent
                    // beams into a solid white sheet. A team-masked diffuse
                    // still goes opaque -- its alpha is the team mask, and
                    // leaking THAT as coverage is the original P0 bug.
                    if (diffuseLayer != nullptr &&
                        !hasFlag(masks[i]->flags, nat::M3TextureLayerFlag::Color) &&
                        masks[i]->uvMapping != diffuseLayer->uvMapping) {
                        if (!diffuseIsTeamMasked && diffuse != nullptr) {
                            into[i]->texture = diffuse;
                            into[i]->channel = tx::Channel::A;
                            out.info(wem::DiagCode::LossyKindConversion,
                                     std::string("alpha mask ") + (i == 0 ? "1" : "2") +
                                         " samples a different UV set than the diffuse; "
                                         "the diffuse's own alpha stands in for it",
                                     where, wem::ProfileId::Wc3Reforged);
                        } else {
                            out.warn(wem::DiagCode::LossyKindConversion,
                                     std::string("alpha mask ") + (i == 0 ? "1" : "2") +
                                         " samples a different UV set than the diffuse; "
                                         "the cutout cannot be baked and the surface "
                                         "exports opaque there",
                                     where, wem::ProfileId::Wc3Reforged);
                        }
                        continue;
                    }
                    *into[i] = CoverageOf(*masks[i], cache, where, out);
                }
            }

            // The envio decision is repeated here without decoding: the
            // reflectance block below runs after this gate.
            const bool anyEnvio =
                LayerOf(standard->environmentLayer) != nullptr &&
                (standard->layerBlendMode == nat::M3LayerBlendOp::Add ||
                 standard->layerBlendMode == nat::M3LayerBlendOp::Lerp ||
                 standard->layerBlendMode == nat::M3LayerBlendOp::Mod);
            const bool anyOrmSource = sources.specular != nullptr ||
                                      sources.occlusion != nullptr || anyTeam || anyEnvio;
            // A glow-only material (two additive emissives and nothing else)
            // has no ORM to bake and no coverage — but its summed emissive
            // and its rim fresnel still need the blocks below.
            const auto additiveEmissive = [&](const std::optional<nat::M3TextureLayer>& layer,
                                              nat::M3LayerBlendOp op) {
                return LayerOf(layer) != nullptr &&
                       (op == nat::M3LayerBlendOp::Add || op == nat::M3LayerBlendOp::AddNoAlpha);
            };
            const bool emissivePair =
                additiveEmissive(standard->emissiveLayer1, standard->emissiveBlendMode1) &&
                additiveEmissive(standard->emissiveLayer2, standard->emissiveBlendMode2);
            const bool emissiveRim =
                (additiveEmissive(standard->emissiveLayer1, standard->emissiveBlendMode1) &&
                 standard->emissiveLayer1->fresnelMode == nat::M3FresnelMode::Standard) ||
                (additiveEmissive(standard->emissiveLayer2, standard->emissiveBlendMode2) &&
                 standard->emissiveLayer2->fresnelMode == nat::M3FresnelMode::Standard);
            if (!anyOrmSource && !hasCoverage && !emissivePair && !emissiveRim) {
                // Nothing an ORM could say that the engine's neutral does not.
                // A gloss layer alone is not a reason: without a specular map
                // the metalness is zero, and a roughness on a surface with no
                // reflectance is a channel nobody reads.
                continue;
            }

            // --- what the surface reflects, as StarCraft II states it ------
            //
            // The material's exponent, its HDR multiplier and the specular
            // layer's own tint are all part of the reflectance the shader
            // reaches for, and none of them were being read: the bake inferred
            // a roughness from how bright the map was, which pinned every
            // StarCraft II surface near 0.88 and left it with no highlight to
            // convert. `pbr_bake.h` carries the algebra.
            tx::pbr::SpecularReflectance reflectance;
            reflectance.exponent = standard->specularExponent > 0.0f
                                       ? standard->specularExponent
                                       : kDefaultSpecularExponent;
            reflectance.factor = standard->hdrSpecularMultiplier > 0.0f
                                     ? standard->hdrSpecularMultiplier
                                     : 1.0f;
            const bool simulateRoughness =
                (static_cast<u32>(standard->flags) & kSimulateRoughness) != 0;
            reflectance.energyConserving = !simulateRoughness;
            // Under the flag the gloss layer below is `1 - roughness` — the
            // engine blurs the reflection by it — not an exponent scale.
            reflectance.simulateRoughness = simulateRoughness;
            reflectance.exponentScale.texture = sources.gloss;
            reflectance.exponentScale.channel = sources.glossChannel;
            reflectance.exponentScale.constant = 1.0f;
            // The engine squares the LAYER COLOUR's alpha — after the channel
            // select, the invert flag and the multiply-add — not the raw
            // texel (`MaterialSpecularity`, and `M3LayerValue` in our own
            // renderer). The unauthored-zero guard is `LayerTintInto`'s.
            if (const nat::M3TextureLayer* layer = LayerOf(standard->glossLayer)) {
                reflectance.exponentScale.invert =
                    hasFlag(layer->flags, nat::M3TextureLayerFlag::ColorInvert);
                const f32 multiply = layer->rgbMultiply.initValue;
                reflectance.exponentScale.postScale = multiply > 0.0f ? multiply : 1.0f;
                reflectance.exponentScale.bias = layer->rgbAdd.initValue;
            }
            if (const nat::M3TextureLayer* layer = LayerOf(standard->specularLayer)) {
                reflectance.specular.texture = sources.specular;
                reflectance.specular.srgb = kSpecularIsDisplayReferred;
                // `SpecularMode::AlphaOnly` splats the layer's alpha; otherwise
                // the layer's own channel select decides, and RGB — the common
                // case — reads the texture's three channels as they are.
                if (standard->specularMode == nat::M3SpecularMode::AlphaOnly) {
                    reflectance.specular.splat = tx::Channel::A;
                } else if (layer->colorType != nat::M3ColorChannelSelect::RGB &&
                           layer->colorType != nat::M3ColorChannelSelect::RGBA) {
                    reflectance.specular.splat = AlphaChannelFor(layer->colorType);
                }
                LayerTintInto(*layer, 1.0f, reflectance.specular.scale);
                reflectance.specular.bias = layer->rgbAdd.initValue;
            }

            // --- the envio layer, as reflectance ---------------------------
            //
            // `ApplyEnv` adds (or lerps) `cube * tint * mask` into the lit
            // colour; Reforged's probe reflection is `F0*lut.x + lut.y` — so a
            // real reflection crosses as an F0 bump, per texel through the
            // EnvioMask, in the ratio of the two environments' brightness. The
            // op is the material's layerBlendMode (the decal's field,
            // psmaterial.fx:332): Add and Lerp cross as that bump, Mod — the
            // league skins' `lit * cube * mask` — as a reflection in the
            // albedo's colour (`pbr_bake.h`), and every other op draws
            // nothing in the game either. 26,894 shipped materials carry the
            // layer, 24,491 with a mask.
            if (const nat::M3TextureLayer* envLayer = LayerOf(standard->environmentLayer)) {
                const nat::M3LayerBlendOp envOp = standard->layerBlendMode;
                const bool modulated = envOp == nat::M3LayerBlendOp::Mod;
                const bool live = modulated || envOp == nat::M3LayerBlendOp::Add ||
                                  envOp == nat::M3LayerBlendOp::Lerp;
                const tx::Texture* envTexture =
                    live ? cache.Decode(envLayer->texturePath) : nullptr;
                if (envTexture != nullptr) {
                    const f32 envConstant = standard->hdrEnvironmentConstant > 0.0f
                                                ? standard->hdrEnvironmentConstant
                                                : 1.0f;
                    f32 tint[3];
                    LayerTintInto(*envLayer, envConstant, tint);
                    const f32 tintLum = 0.2126f * tint[0] + 0.7152f * tint[1] + 0.0722f * tint[2];
                    reflectance.envReflectance =
                        MeanLinearLuminance(*envTexture) * tintLum / kReforgedProbeLuminance;
                    reflectance.envRoughnessCap = kEnvRoughnessCap;
                    reflectance.envModulates = modulated;
                    const std::optional<nat::M3TextureLayer>& maskSlot =
                        standard->environmentMaskLayer;
                    if (maskSlot.has_value() &&
                        (LayerOf(maskSlot) != nullptr ||
                         hasFlag(maskSlot->flags, nat::M3TextureLayerFlag::Color))) {
                        reflectance.envMask = EnvMaskOf(*maskSlot, envOp, cache, where, out);
                    }
                    if (modulated) {
                        out.info(wem::DiagCode::LossyKindConversion,
                                 "a Mod-op envio layer modulates the lit colour by its "
                                 "reflection; it crosses as a metal in the albedo's colour",
                                 where, wem::ProfileId::Wc3Reforged);
                    }
                }
            }

            // --- the decal, composited -------------------------------------
            //
            // The slot map holds ONE base colour and `exportPbr` has one slot,
            // so a decal crosses as pixels or not at all — the Marine's chest
            // insignia was the standing casualty. Folded into the albedo where
            // `CombineLayerColor` folds it: before the team lerp and the metal
            // gain, in both bakes, so they split the same albedo.
            tx::pbr::ColorInput decalInput;
            tx::pbr::DecalOp decalOp = tx::pbr::DecalOp::Mod;
            const nat::M3TextureLayer* decalLayer = LayerOf(standard->decalLayer);
            if (decalLayer != nullptr && diffuse != nullptr) {
                const std::optional<tx::pbr::DecalOp> op = DecalOpFor(standard->layerBlendMode);
                const tx::Texture* decalTexture = cache.Decode(decalLayer->texturePath);
                if (!op.has_value()) {
                    // A TeamColor op on the decal never ships (0 of 176,955).
                } else if (decalTexture == nullptr) {
                    out.warn(wem::DiagCode::TextureUnresolved,
                             "the decal '" + Trim(decalLayer->texturePath) +
                                 "' could not be decoded; it is not composited",
                             where, wem::ProfileId::Wc3Reforged);
                } else if (decalLayer->uvMapping != diffuseLayer->uvMapping) {
                    out.warn(wem::DiagCode::LossyKindConversion,
                             "the decal samples a different UV set than the diffuse; it "
                             "cannot be composited from pixels alone",
                             where, wem::ProfileId::Wc3Reforged);
                } else {
                    decalInput.texture = decalTexture;
                    decalInput.srgb = true;
                    LayerTintInto(*decalLayer, 1.0f, decalInput.scale);
                    decalInput.bias = decalLayer->rgbAdd.initValue;
                    decalOp = *op;
                }
            }

            tx::pbr::OrmRecipe recipe;
            recipe.reflectance = reflectance;
            recipe.decal = decalInput;
            recipe.decalOp = decalOp;
            recipe.occlusion.texture = sources.occlusion;
            recipe.occlusion.channel = sources.occlusionChannel;
            recipe.occlusion.constant = 1.0f;
            recipe.teamColor[0] = teamColor.x;
            recipe.teamColor[1] = teamColor.y;
            recipe.teamColor[2] = teamColor.z;
            if (diffuseIsTeamMasked) {
                recipe.teamMask.texture = diffuse;
                recipe.teamMask.channel = tx::Channel::A;
                recipe.teamMask.invert = true; // team where the albedo is NOT
            }
            recipe.teamMaskAlt.texture = sources.team;
            recipe.teamMaskAlt.channel = sources.teamChannel.value_or(tx::Channel::A);
            // A team layer whose channel select is RGB has its alpha forced to
            // 1 by `SelectChannels`, so the team colour lands on the whole
            // surface. That is a constant, not a map.
            recipe.teamMaskAlt.constant = sources.teamWholeSurface ? 1.0f : 0.0f;
            if (sources.teamWholeSurface) {
                recipe.teamMaskAlt.texture = nullptr;
            }
            // Not a source of the map, and not optional either: the albedo is
            // what the metalness splits (`m = F0 / (F0 + albedo)`) and what
            // sharpens the team mask from the team's share of the *result* into
            // its share of the *modulation*. `pbr_bake.h` carries both
            // derivations.
            recipe.baseColor.texture = diffuse;
            recipe.baseColor.srgb = true;

            bool ormLanded = false;
            if (anyOrmSource) {
                std::optional<tx::Texture> orm = tx::pbr::BakeOrm(recipe);
                if (!orm) {
                    // The ORM failed; the coverage below may still land, so
                    // this is a report, not a `continue`.
                    ++result.materialsSkipped;
                    out.warn(wem::DiagCode::TextureUnresolved,
                             "no source of the occlusion/roughness/metalness map could be decoded",
                             where, wem::ProfileId::Wc3Reforged);
                } else {
                    const std::string name = OrmNameFor(
                        sources.ormNameSource, sc2->materials[m].name, static_cast<u32>(m));
                    const u32 index = cache.Intern(name);
                    wem::TextureInput input;
                    input.texture = index;
                    input.colorSpace = wem::ColorSpace::Linear;
                    body->set(wem::PbrSlot::Orm, input);
                    result.baked.insert_or_assign(
                        index, BakedTexture{std::move(*orm), tx::PixelFormat::BC3});
                    ++result.ormBaked;
                    ormLanded = true;
                }
            }

            // --- and the base colour pays for both of them -----------------
            //
            // Two rewrites in one pass, and neither is optional. The metalness
            // above took `F0` out of the diffuse lobe, so the albedo has to go
            // up by exactly that much or the model comes back darker than it
            // went in — which is what "we added metalness and it got worse"
            // looks like. And the team tint is lightened under the *combined*
            // mask, not under the diffuse's own alpha: a material whose team
            // colour arrives through the `TeamColor*Add` op has its mask in a
            // different texture entirely, and leaving its albedo as StarCraft II
            // authored it — art that averages 26/255, because the engine was
            // going to replace it — is what makes Warcraft III's modulated tint
            // come out near-black.
            if (diffuse == nullptr && hasCoverage) {
                out.warn(wem::DiagCode::TextureUnresolved,
                         "an alpha-reading material's diffuse could not be decoded; its "
                         "coverage cannot be baked",
                         where, wem::ProfileId::Wc3Reforged);
            }
            if (diffuse != nullptr && diffuseLayer != nullptr) {
                const u32 diffuseIndex = cache.IndexOf(diffuseLayer->texturePath);
                // A composited decal moves the bake to a per-material texture
                // for the same reason coverage does: the diffuse is shared,
                // and only THIS material wears the insignia.
                if ((hasCoverage || recipe.decal.present()) &&
                    diffuseIndex != wem::kInvalidIndex) {
                    // The composed cutout is this MATERIAL's, not the
                    // texture's: an Overlord's opaque Body and blended Sacs
                    // share one diffuse, so writing coverage into the shared
                    // file would either race the opaque bake (first writer
                    // wins) or hand the Body the Sacs' holes. A new texture,
                    // and only this material's slot moves.
                    tx::pbr::BaseColorRecipe base;
                    base.baseColor = recipe.baseColor;
                    base.decal = recipe.decal;
                    base.decalOp = recipe.decalOp;
                    base.teamMask = recipe.teamMask;
                    base.teamMaskAlt = recipe.teamMaskAlt;
                    std::copy_n(recipe.teamColor, 3, base.teamColor);
                    base.coverage1 = coverage1;
                    base.coverage2 = coverage2;
                    if (alphaKeyed) {
                        base.coverageCutoff =
                            static_cast<f32>(standard->alphaTestThreshold) / 255.0f;
                    }
                    base.reflectance = recipe.reflectance;
                    if (std::optional<tx::Texture> rewritten = tx::pbr::BakeBaseColor(base)) {
                        std::string cutName =
                            CutoutNameFor(diffuseLayer->texturePath, static_cast<u32>(m));
                        const std::string maskKey =
                            (standard->alphaLayer1 ? Trim(standard->alphaLayer1->texturePath)
                                                   : std::string()) +
                            "|" +
                            (standard->alphaLayer2 ? Trim(standard->alphaLayer2->texturePath)
                                                   : std::string()) +
                            "|" +
                            (decalInput.present() ? Trim(decalLayer->texturePath)
                                                  : std::string());
                        const auto claim = cutoutClaims.find(cutName);
                        if (claim != cutoutClaims.end() && claim->second != maskKey) {
                            cutName.insert(cutName.size() - 4, "_" + std::to_string(m));
                        }
                        cutoutClaims.emplace(cutName, maskKey);
                        const u32 cutIndex = cache.Intern(cutName);
                        wem::TextureInput slot;
                        if (const wem::TextureInput* existing = body->find(wem::PbrSlot::BaseColor)) {
                            slot = *existing;
                        }
                        slot.texture = cutIndex;
                        body->set(wem::PbrSlot::BaseColor, slot);
                        // BC1's alpha is one bit; the composed cutout needs BC3.
                        result.baked.insert_or_assign(
                            cutIndex, BakedTexture{std::move(*rewritten), tx::PixelFormat::BC3});
                        ++result.baseColorsCleared;
                        ++result.coverageComposed;
                    }
                } else if (hasCoverage) {
                    out.warn(wem::DiagCode::TextureUnresolved,
                             "an alpha-reading material's diffuse has no document entry; "
                             "its coverage cannot be baked",
                             where, wem::ProfileId::Wc3Reforged);
                } else if (diffuseIndex != wem::kInvalidIndex &&
                           result.baked.find(diffuseIndex) == result.baked.end()) {
                    tx::pbr::BaseColorRecipe base;
                    base.baseColor = recipe.baseColor;
                    base.teamMask = recipe.teamMask;
                    base.teamMaskAlt = recipe.teamMaskAlt;
                    std::copy_n(recipe.teamColor, 3, base.teamColor);
                    base.reflectance = recipe.reflectance;
                    if (std::optional<tx::Texture> rewritten = tx::pbr::BakeBaseColor(base)) {
                        result.baked.emplace(
                            diffuseIndex, BakedTexture{std::move(*rewritten), tx::PixelFormat::BC1});
                        ++result.baseColorsCleared;
                    }
                }
            }

            // --- two additive emissives are one map, summed ----------------
            //
            // StarCraft II folds both layers into one accumulator before
            // `fEmissiveMultiplier`; the slot map holds one, so the second was
            // silently gone. A plain (alpha-weighted) add of pixels says it.
            {
                const nat::M3TextureLayer* emissive1 = LayerOf(standard->emissiveLayer1);
                const nat::M3TextureLayer* emissive2 = LayerOf(standard->emissiveLayer2);
                const auto additiveOp = [](nat::M3LayerBlendOp op) {
                    return op == nat::M3LayerBlendOp::Add || op == nat::M3LayerBlendOp::AddNoAlpha;
                };
                if (emissive1 != nullptr && emissive2 != nullptr &&
                    additiveOp(standard->emissiveBlendMode1) &&
                    additiveOp(standard->emissiveBlendMode2)) {
                    tx::pbr::ColorInput first;
                    first.texture = cache.Decode(emissive1->texturePath);
                    first.srgb = true;
                    LayerTintInto(*emissive1, 1.0f, first.scale);
                    first.bias = emissive1->rgbAdd.initValue;
                    tx::pbr::ColorInput second;
                    second.texture = cache.Decode(emissive2->texturePath);
                    second.srgb = true;
                    LayerTintInto(*emissive2, 1.0f, second.scale);
                    second.bias = emissive2->rgbAdd.initValue;
                    if (first.present() && second.present()) {
                        std::optional<tx::Texture> sum = tx::pbr::BakeEmissiveSum(
                            first, standard->emissiveBlendMode1 == nat::M3LayerBlendOp::Add,
                            second, standard->emissiveBlendMode2 == nat::M3LayerBlendOp::Add);
                        if (sum.has_value()) {
                            std::string name = CutoutNameFor(emissive1->texturePath,
                                                             static_cast<u32>(m));
                            name.replace(name.size() - 8, 4, "_glow");
                            const u32 index = cache.Intern(name);
                            wem::TextureInput slot;
                            if (const wem::TextureInput* existing =
                                    body->find(wem::PbrSlot::Emissive)) {
                                slot = *existing;
                            }
                            slot.texture = index;
                            body->set(wem::PbrSlot::Emissive, slot);
                            result.baked.insert_or_assign(
                                index, BakedTexture{std::move(*sum), tx::PixelFormat::BC1});
                            out.info(wem::DiagCode::LossyKindConversion,
                                     "both additive emissive layers were summed into one map",
                                     where, wem::ProfileId::Wc3Reforged);
                        }
                    }
                }
            }

            // --- fresnel: the rim glow crosses, the rest is named ----------
            //
            // The MDX layer carries fresnelColor/Opacity/TeamColor and
            // Reforged lerps the lit colour toward that flat colour by
            // `opacity * (1-NdotV)^2`. The clean case is fresnel on an
            // ADDITIVE emissive — a rim glow (12,226 shipped layers): the
            // colour is the map's mean under its tint and gain, the opacity is
            // the ramp's ceiling. Inverted mode (centre glow) and fresnel on
            // masks or diffuse have no overlay to cross into.
            {
                wem::CommonMaterial& derived = wc3->materials[m].MutableCommon();
                std::erase_if(derived.features, [](const wem::MaterialFeature& feature) {
                    return feature.kind() == wem::FeatureKind::Fresnel;
                });
                const nat::M3TextureLayer* rim = nullptr;
                f32 rimGain = 1.0f;
                for (int slot = 0; slot < 2; ++slot) {
                    const nat::M3TextureLayer* layer = LayerOf(
                        slot == 0 ? standard->emissiveLayer1 : standard->emissiveLayer2);
                    const nat::M3LayerBlendOp op =
                        slot == 0 ? standard->emissiveBlendMode1 : standard->emissiveBlendMode2;
                    if (layer == nullptr || layer->fresnelMode != nat::M3FresnelMode::Standard ||
                        (op != nat::M3LayerBlendOp::Add &&
                         op != nat::M3LayerBlendOp::AddNoAlpha)) {
                        continue;
                    }
                    if (layer->fresnelMin > layer->fresnelMax) {
                        continue; // a deliberately inverted ramp — facing-bright
                    }
                    rim = layer;
                    rimGain = standard->hdrEmissiveMultiplier > 0.0f
                                  ? standard->hdrEmissiveMultiplier
                                  : 1.0f;
                    break;
                }
                if (rim != nullptr) {
                    const tx::Texture* map = cache.Decode(rim->texturePath);
                    f32 tint[3];
                    LayerTintInto(*rim, rimGain, tint);
                    f32 mean[3] = {1.0f, 1.0f, 1.0f};
                    if (map != nullptr) {
                        MeanLinearRgb(*map, mean);
                    }
                    wem::FresnelFeature fresnel;
                    // Capped to a unit maximum, hue kept. The native term ADDS
                    // `emissive * ramp` on top of the lit colour; the overlay
                    // LERPS the lit colour toward this flat colour, so a
                    // gain-multiplied value past 1 does not glow brighter — it
                    // bleaches. The Aiur light bridge's grazing beam planes
                    // went out as a white flood before the cap.
                    f32 rimColor[3] = {tint[0] * mean[0], tint[1] * mean[1], tint[2] * mean[2]};
                    const f32 peak = std::max({rimColor[0], rimColor[1], rimColor[2], 1.0f});
                    fresnel.color = Vector3f{rimColor[0] / peak, rimColor[1] / peak,
                                             rimColor[2] / peak};
                    fresnel.exponent = rim->fresnelExponent > 0.0f ? rim->fresnelExponent : 1.0f;
                    fresnel.outMin = rim->fresnelMin;
                    fresnel.outMax = rim->fresnelMax > 0.0f ? rim->fresnelMax : 1.0f;
                    wem::MaterialFeature feature;
                    feature.id = wem::NextFeatureId(derived.features);
                    feature.layer = wem::kWholeMaterial;
                    feature.payload = fresnel;
                    derived.features.push_back(feature);
                    out.info(wem::DiagCode::LossyKindConversion,
                             "the emissive rim fresnel became the layer's fresnel overlay",
                             where, wem::ProfileId::Wc3Reforged);
                }
            }

            // --- the team layer is not an emissive map ---------------------
            //
            // It reached the emissive slot because `channelOf` reads the layer
            // it sits in and not the op it is combined by, and a mask bound as
            // an emissive map is a surface glowing its own coverage. Its
            // content is in the ORM's alpha now.
            if (teamLayer != nullptr && ormLanded) {
                const u32 teamIndex = cache.IndexOf(teamLayer->texturePath);
                const wem::TextureInput* emissive = body->find(wem::PbrSlot::Emissive);
                if (emissive != nullptr && emissive->texture == teamIndex) {
                    const nat::M3TextureLayer* other = teamSlot == 1
                                                           ? LayerOf(standard->emissiveLayer2)
                                                           : LayerOf(standard->emissiveLayer1);
                    const bool otherIsTeam = teamSlot == 1 ? IsTeamOp(standard->emissiveBlendMode2)
                                                           : IsTeamOp(standard->emissiveBlendMode1);
                    const u32 otherIndex = other != nullptr && !otherIsTeam
                                               ? cache.IndexOf(other->texturePath)
                                               : wem::kInvalidIndex;
                    // Erased, not blanked. A slot holding an input with no
                    // texture is not an empty slot to `exportPbr`; it is a slot
                    // whose texture index is `kInvalidIndex`, and that maps to 0.
                    std::erase_if(body->slots, [](const auto& entry) {
                        return entry.first == wem::PbrSlot::Emissive;
                    });
                    if (otherIndex != wem::kInvalidIndex) {
                        wem::TextureInput replacement;
                        replacement.texture = otherIndex;
                        body->set(wem::PbrSlot::Emissive, replacement);
                    }
                    out.info(wem::DiagCode::LossyKindConversion,
                             "the team-colour layer became the ORM's mask, not the emissive map",
                             where, wem::ProfileId::Wc3Reforged);
                }
            }
        }
    }

    return result;
}

} // namespace whiteout::flakes
