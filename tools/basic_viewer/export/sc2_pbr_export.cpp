// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "sc2_pbr_export.h"

#include "color_math.h"
#include "export_text.h"
#include "texture_io.h"

#include "whiteout/flakes/util/texture_image_usage.h"

#include <whiteout/models/wem/materials/native.h>
#include <whiteout/models/wem/retarget.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes {

namespace {

namespace nat = ::whiteout::models::wem::native;
namespace tx = ::whiteout::textures;
namespace wem = ::whiteout::models::wem;

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

/// The container every baked map name ends in.
constexpr std::string_view kDdsExtension = ".dds";

using nat::M3LayerBlendOp;
using nat::M3TextureLayer;
using nat::M3TextureLayerFlag;

/// A colour field nobody touched: an exporter writes all zeros, which is not
/// "multiply by black".
bool IsUnauthored(const auto& c) {
    return c.r == 0 && c.g == 0 && c.b == 0 && c.a == 0;
}

const M3TextureLayer* LayerOf(const std::optional<M3TextureLayer>& layer) {
    if (!layer.has_value() || export_text::TrimFixedWidth(layer->texturePath).empty()) {
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

/// The mean linear RGB of a texture's top mip — the flat colour a map
/// collapses to when its target has no texture to give it.
void MeanLinearRgb(const tx::Texture& texture, f32 out[3]) {
    out[0] = out[1] = out[2] = 1.0f;
    const tx::Texture rgba = texture.copyAsFormat(tx::PixelFormat::RGBA8);
    const std::span<const u8> px = rgba.mipData(0);
    if (px.size() < 4) {
        return;
    }
    f64 sum[3] = {0.0, 0.0, 0.0};
    for (std::size_t i = 0; i < px.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            sum[c] += color::SrgbToLinear(px[i + static_cast<std::size_t>(c)] / 255.0f);
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
    f64 sum = 0.0;
    const std::size_t count = px.size() / 4;
    for (std::size_t i = 0; i < px.size(); i += 4) {
        sum += 0.2126 * color::SrgbToLinear(px[i] / 255.0f) +
               0.7152 * color::SrgbToLinear(px[i + 1] / 255.0f) +
               0.0722 * color::SrgbToLinear(px[i + 2] / 255.0f);
    }
    return static_cast<f32>(sum / static_cast<f64>(count));
}

/// M3's layer blend vocabulary onto the bake's decal fold.
std::optional<tx::pbr::DecalOp> DecalOpFor(M3LayerBlendOp op) {
    switch (op) {
    case M3LayerBlendOp::Mod:
        return tx::pbr::DecalOp::Mod;
    case M3LayerBlendOp::Mod2x:
        return tx::pbr::DecalOp::Mod2x;
    case M3LayerBlendOp::Add:
        return tx::pbr::DecalOp::AddScaled;
    case M3LayerBlendOp::AddNoAlpha:
        return tx::pbr::DecalOp::Add;
    case M3LayerBlendOp::Lerp:
        return tx::pbr::DecalOp::Lerp;
    default:
        // The two TeamColor ops are masks, not colour.
        return std::nullopt;
    }
}

bool IsTeamOp(M3LayerBlendOp op) {
    return op == M3LayerBlendOp::TeamColorEmissiveAdd || op == M3LayerBlendOp::TeamColorDiffuseAdd;
}

bool IsAdditiveOp(M3LayerBlendOp op) {
    return op == M3LayerBlendOp::Add || op == M3LayerBlendOp::AddNoAlpha;
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

/// @p sourcePath without its NULs and its extension, folders kept.
std::string StemOf(const std::string& sourcePath) {
    std::string base = export_text::TrimFixedWidth(sourcePath);
    const std::size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > base.find_last_of("/\\") + 1) {
        base = base.substr(0, dot);
    }
    return base;
}

/// `Assets/Textures/Marine_Specular_Blood.dds` -> `.../Marine_Blood_orm.dds`,
/// which is the shape Reforged's own content uses (`..._Diffuse` beside
/// `..._ORM`) and therefore what a modder opening the folder expects. A known
/// role suffix is replaced rather than appended so the name does not read
/// `_specular_orm`.
std::string OrmNameFor(const std::string& sourcePath, const std::string& materialName,
                       u32 materialIndex) {
    std::string base = export_text::TrimFixedWidth(sourcePath);
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
    base = StemOf(base);
    // Longest first: `_specular` before `_spec`, or the cut leaves `_ular`.
    static constexpr std::string_view kRoles[] = {"_specular", "_diffuse", "_emissive", "_normal",
                                                  "_emiss",    "_gloss",   "_spec",     "_diff",
                                                  "_norms",    "_norm",    "_nrm",      "_nom"};
    const std::string lowered = export_text::Lower(base);
    for (std::string_view role : kRoles) {
        const std::size_t at = lowered.rfind(role);
        if (at != std::string::npos) {
            // Cut the role out wherever it sits, not only off the end:
            // `Marine_Specular_Blood` is the shipped spelling and
            // `Marine_Specular_Blood_orm` names the wrong map twice.
            base.erase(at, role.size());
            break;
        }
    }
    return base + "_orm" + std::string(kDdsExtension);
}

/// `.../Overlord_Diffuse.dds` -> `.../Overlord_Diffuse<suffix>.dds`: a map this
/// material composed out of that one, which only its own slot may name.
std::string ComposedNameFor(const std::string& sourcePath, u32 materialIndex,
                            std::string_view suffix) {
    std::string base = StemOf(sourcePath);
    if (base.empty()) {
        base = "material" + std::to_string(materialIndex);
    }
    return base + std::string(suffix) + std::string(kDdsExtension);
}

/// The bind-pose tint of a layer, which is what the source's shader multiplies
/// its sample by. `LayerTint` in `m3_surface_table.cpp` says the same thing from
/// the renderer's side, including both unauthored-sentinel guards: an all-zero
/// `ColorBGRA` and a zero `rgbMultiply` are what an exporter writes for a field
/// nobody touched, not "multiply by nothing".
void LayerTintInto(const M3TextureLayer& layer, f32 extraMul, f32 out[3]) {
    const auto c = layer.color.initValue;
    f32 rgb[3] = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f};
    if (IsUnauthored(c)) {
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

/// A layer's colour as a bake input: its texture decoded as sRGB, its tint and
/// its add.
tx::pbr::ColorInput ColorInputOf(const M3TextureLayer& layer, const tx::Texture* texture) {
    tx::pbr::ColorInput input;
    input.texture = texture;
    input.srgb = true;
    LayerTintInto(layer, 1.0f, input.scale);
    input.bias = layer.rgbAdd.initValue;
    return input;
}

/// An alpha-mask layer is active with no texture when it is a solid colour
/// (flag `Color`), which `LayerOf`'s path test misses — 3,466 shipped masks.
const M3TextureLayer* MaskLayerOf(const std::optional<M3TextureLayer>& layer) {
    if (!layer.has_value()) {
        return nullptr;
    }
    if (hasFlag(layer->flags, M3TextureLayerFlag::Color)) {
        return &*layer;
    }
    return LayerOf(layer);
}

/// The `* alphaFactor`, invert, `* rgbMultiply + rgbAdd` tail every scalar
/// mask shares, with the unauthored-zero guards.
void ApplyMaskTail(const M3TextureLayer& layer, tx::pbr::ScalarInput& mask) {
    const f32 alphaFactor = layer.mapAlpha.initValue;
    mask.scale = alphaFactor > 0.0f ? alphaFactor : 1.0f;
    mask.invert = hasFlag(layer.flags, M3TextureLayerFlag::ColorInvert);
    const f32 multiply = layer.rgbMultiply.initValue;
    mask.postScale = multiply > 0.0f ? multiply : 1.0f;
    mask.bias = layer.rgbAdd.initValue;
}

/// The coverage term one alpha-mask layer states, as the engine computes its
/// alpha (`ComputeLayerColorInternal`): channel select, `* alphaFactor`,
/// invert, `* rgbMultiply + rgbAdd`.
tx::pbr::ScalarInput CoverageOf(const M3TextureLayer& layer, ExportTextureCache& cache,
                                const wem::ElementRef& where, wem::Diagnostics& out) {
    tx::pbr::ScalarInput mask;
    mask.constant = 1.0f;
    if (hasFlag(layer.flags, M3TextureLayerFlag::Color)) {
        // A solid-colour layer's alpha is its constant colour's, with the
        // all-zero unauthored guard.
        const auto c = layer.color.initValue;
        mask.constant = !IsUnauthored(c) ? static_cast<f32>(c.a) / 255.0f : 1.0f;
    } else if (const std::optional<tx::Channel> channel = AlphaChannelFor(layer.colorType)) {
        mask.texture = cache.Decode(layer.texturePath);
        mask.channel = *channel;
        if (mask.texture == nullptr) {
            out.warn(wem::DiagCode::TextureUnresolved,
                     "the alpha mask '" + export_text::TrimFixedWidth(layer.texturePath) +
                         "' could not be decoded; the surface exports opaque",
                     where, wem::ProfileId::Wc3Reforged);
        }
    }
    // An RGB select forces the alpha to 1 (`SelectChannels`) — and the sweep
    // found zero shipped masks that use it — so the fall-through constant is
    // exactly what the engine reads.
    ApplyMaskTail(layer, mask);
    return mask;
}

/// The envio mask as `ApplyEnv` multiplies it: `cMaskValue.rgb` under Add
/// and Mod — the decoded RGB's luminance for an RGB select, RGB times alpha
/// for RGBA, one channel splatted otherwise — and `.a` under Lerp, where an
/// RGB select is a constant 1. The texture's own colour-space policy decides
/// the decode, the way the renderer binds it (a `_spec` map is sRGB).
tx::pbr::ScalarInput EnvMaskOf(const M3TextureLayer& layer, M3LayerBlendOp op,
                               ExportTextureCache& cache, const wem::ElementRef& where,
                               wem::Diagnostics& out) {
    tx::pbr::ScalarInput mask;
    mask.constant = 1.0f;
    const bool byAlpha = op == M3LayerBlendOp::Lerp;
    if (hasFlag(layer.flags, M3TextureLayerFlag::Color)) {
        const auto c = layer.color.initValue;
        if (!IsUnauthored(c)) {
            mask.constant = byAlpha ? static_cast<f32>(c.a) / 255.0f
                                    : (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) / 255.0f;
        }
    } else {
        const std::string path = export_text::TrimFixedWidth(layer.texturePath);
        mask.texture = cache.Decode(layer.texturePath);
        if (mask.texture == nullptr) {
            out.warn(wem::DiagCode::TextureUnresolved,
                     "the envio mask '" + path +
                         "' could not be decoded; the reflection covers the surface",
                     where, wem::ProfileId::Wc3Reforged);
        }
        mask.srgb = !io::IsLinearImageUsage(io::DetermineImageUsage(path));
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
    ApplyMaskTail(layer, mask);
    return mask;
}

/// Which mask set claimed a composed base colour's name -- two materials with
/// the same diffuse and the same masks share one baked map; different masks get
/// a suffixed name instead of overwriting each other's pixels.
using ComposedClaims = std::unordered_map<std::string, std::array<std::string, 3>>;

/// One StarCraft II material's crossing into the Reforged material the derive
/// made for it, stage by stage in the order the stages depend on each other.
class MaterialBake {
public:
    MaterialBake(const nat::M3Standard& standard, const wem::Material& sc2Material,
                 wem::Material& wc3Material, u32 index, ExportTextureCache& cache,
                 ComposedClaims& claims, const tx::pbr::NormalRestatement& normal,
                 const Vector3f& teamColor, Sc2PbrBakeResult& result, wem::Diagnostics& out)
        : standard_(standard), sc2Material_(sc2Material), wc3Material_(wc3Material),
          index_(index), where_(wem::ElementKind::Material, index), cache_(cache),
          claims_(claims), normal_(normal), teamColor_(teamColor), result_(result), out_(out),
          emissiveLayers_{&standard.emissiveLayer1, &standard.emissiveLayer2},
          emissiveOps_{standard.emissiveBlendMode1, standard.emissiveBlendMode2} {}

    void Run();

private:
    struct Sources {
        const tx::Texture* specular = nullptr;
        const tx::Texture* gloss = nullptr;
        const tx::Texture* occlusion = nullptr;
        const tx::Texture* team = nullptr;
        tx::Channel glossChannel = tx::Channel::A;
        tx::Channel occlusionChannel = tx::Channel::R;
        std::optional<tx::Channel> teamChannel;
        bool teamWholeSurface = false;
        std::string ormNameSource;
    };

    void RestateNormal();
    void ResolveOrmSources();
    void ResolveTeamMasks();
    void ResolveCoverage();
    bool HasOrmSource() const;
    bool HasAnythingToBake() const;
    tx::pbr::SpecularReflectance Reflectance();
    void AddEnvioReflectance(tx::pbr::SpecularReflectance& reflectance);
    void ResolveDecal();
    tx::pbr::OrmRecipe OrmRecipeFrom(const tx::pbr::SpecularReflectance& reflectance) const;
    bool BakeOrmMap(const tx::pbr::OrmRecipe& recipe);
    void RewriteBaseColor(const tx::pbr::OrmRecipe& recipe);
    void SumAdditiveEmissives();
    void CrossRimFresnel();
    void DropTeamEmissive();

    /// The texture slot @p slot of the derived material holds, repointed at @p texture.
    void SetSlotTexture(wem::PbrSlot slot, u32 texture);

    const nat::M3Standard& standard_;
    const wem::Material& sc2Material_;
    wem::Material& wc3Material_;
    const u32 index_;
    const wem::ElementRef where_;
    ExportTextureCache& cache_;
    ComposedClaims& claims_;
    const tx::pbr::NormalRestatement& normal_;
    const Vector3f& teamColor_;
    Sc2PbrBakeResult& result_;
    wem::Diagnostics& out_;

    const std::array<const std::optional<M3TextureLayer>*, 2> emissiveLayers_;
    const std::array<M3LayerBlendOp, 2> emissiveOps_;

    wem::PbrDeferredBody* body_ = nullptr;
    Sources sources_;
    const M3TextureLayer* diffuseLayer_ = nullptr;
    const tx::Texture* diffuse_ = nullptr;
    bool diffuseIsTeamMasked_ = false;
    const M3TextureLayer* teamLayer_ = nullptr;
    std::size_t teamSlot_ = 0;
    bool anyTeam_ = false;
    tx::pbr::ScalarInput coverage_[2] = {{nullptr, tx::Channel::A, false, 1.0f},
                                         {nullptr, tx::Channel::A, false, 1.0f}};
    bool alphaKeyed_ = false;
    bool hasCoverage_ = false;
    const M3TextureLayer* decalLayer_ = nullptr;
    tx::pbr::ColorInput decal_;
    tx::pbr::DecalOp decalOp_ = tx::pbr::DecalOp::Mod;
    bool ormLanded_ = false;
};

void MaterialBake::Run() {
    body_ = wc3Material_.MutableCommon().pbr();
    if (body_ == nullptr) {
        return;
    }
    RestateNormal();
    ResolveOrmSources();
    ResolveTeamMasks();
    ResolveCoverage();
    if (!HasAnythingToBake()) {
        // Nothing an ORM could say that the engine's neutral does not. A gloss
        // layer alone is not a reason: without a specular map the metalness is
        // zero, and a roughness on a surface with no reflectance is a channel
        // nobody reads.
        return;
    }
    const tx::pbr::SpecularReflectance reflectance = Reflectance();
    ResolveDecal();
    const tx::pbr::OrmRecipe recipe = OrmRecipeFrom(reflectance);
    ormLanded_ = HasOrmSource() && BakeOrmMap(recipe);
    RewriteBaseColor(recipe);
    SumAdditiveEmissives();
    CrossRimFresnel();
    DropTeamEmissive();
}

// --- the normal map ------------------------------------------------------
void MaterialBake::RestateNormal() {
    const M3TextureLayer* layer = LayerOf(standard_.normalLayer);
    if (layer == nullptr) {
        return;
    }
    const std::string path = export_text::TrimFixedWidth(layer->texturePath);
    const u32 index = cache_.IndexOf(path);
    if (index == wem::kInvalidIndex || result_.baked.find(index) != result_.baked.end()) {
        return;
    }
    if (const tx::Texture* decoded = cache_.Decode(path)) {
        if (std::optional<tx::Texture> restated = tx::pbr::ConvertNormalXInAlpha(*decoded, normal_)) {
            result_.baked.emplace(index, BakedTexture::Declared(std::move(*restated), tx::PixelFormat::BC5));
        }
    }
}

// --- what the ORM is made of ---------------------------------------------
void MaterialBake::ResolveOrmSources() {
    if (const M3TextureLayer* layer = LayerOf(standard_.specularLayer)) {
        sources_.specular = cache_.Decode(layer->texturePath);
        sources_.ormNameSource = export_text::TrimFixedWidth(layer->texturePath);
    }
    // A gloss layer scales the SPECULARITY, and the shader reads its alpha
    // after the channel-select splat (`psmaterial.fx:579`). A layer whose
    // select is RGB has that alpha forced to 1, so it leaves the exponent
    // alone rather than zeroing it.
    if (const M3TextureLayer* layer = LayerOf(standard_.glossLayer)) {
        if (const std::optional<tx::Channel> channel = AlphaChannelFor(layer->colorType)) {
            sources_.gloss = cache_.Decode(layer->texturePath);
            sources_.glossChannel = *channel;
        }
    }
    if (const M3TextureLayer* layer = LayerOf(standard_.ambientOcclusionLayer)) {
        // The ORM is one map over the base colour's coordinates, the rule the
        // masks and the decal below already follow: an occlusion on another UV
        // set would land on texels it was never painted for.
        // SM_ArmorySpectreCrate's cloak reuses its diffuse over UV 1 as
        // occlusion, and baked, it stained the cloth.
        const M3TextureLayer* paint = LayerOf(standard_.diffuseLayer);
        if (paint != nullptr && layer->uvMapping != paint->uvMapping) {
            out_.warn(wem::DiagCode::LossyKindConversion,
                      "the ambient occlusion samples a different UV set than the diffuse; "
                      "it is not baked into the occlusion map",
                      where_, wem::ProfileId::Wc3Reforged);
        } else {
            sources_.occlusion = cache_.Decode(layer->texturePath);
            sources_.occlusionChannel = ScalarChannelFor(layer->colorType);
        }
    }
}

// --- the team-colour mask, which is two mechanisms -----------------------
//
// The one that matters is not a combine op. `psmateriallayer.fx` tints the
// ALBEDO through the diffuse layer's own alpha whenever that layer's channel
// select is not RGB -- `lerp(teamColour, texel.rgb, pow(texel.a, exponent))` --
// so the team *weight* is `1 - diffuse.a`, the exponent is engine-set to 1, and
// this is how nearly every unit in both games is coloured. The renderer says so
// in as many words: "the alpha-mask layers times the alpha factor -- NOT the
// diffuse alpha, which is the team-colour mask".
//
// The other is the `TeamColor*Add` op on an emissive layer, and it is the rare
// one: 10,527 of 176,955 shipped standard materials across both games, every
// one of them `TeamColorEmissiveAdd`, 9,905 on emissive 1 and 861 on emissive
// 2, and not one on the decal's `layerBlendMode`.
void MaterialBake::ResolveTeamMasks() {
    diffuseLayer_ = LayerOf(standard_.diffuseLayer);
    // A material with no specular map still names its ORM after a real
    // texture, so a constants-only bake (a whole-surface team glow) lands
    // beside the model's other maps instead of at the root.
    if (sources_.ormNameSource.empty() && diffuseLayer_ != nullptr) {
        sources_.ormNameSource = export_text::TrimFixedWidth(diffuseLayer_->texturePath);
    }
    diffuseIsTeamMasked_ =
        diffuseLayer_ != nullptr && diffuseLayer_->colorType != nat::M3ColorChannelSelect::RGB;
    diffuse_ = diffuseLayer_ != nullptr ? cache_.Decode(diffuseLayer_->texturePath) : nullptr;

    for (std::size_t slot = 0; slot < emissiveLayers_.size(); ++slot) {
        if (teamLayer_ == nullptr && IsTeamOp(emissiveOps_[slot])) {
            teamLayer_ = LayerOf(*emissiveLayers_[slot]);
            teamSlot_ = slot;
        }
    }
    if (teamLayer_ != nullptr) {
        sources_.team = cache_.Decode(teamLayer_->texturePath);
        sources_.teamChannel = AlphaChannelFor(teamLayer_->colorType);
        sources_.teamWholeSurface = !sources_.teamChannel.has_value();
    }
    anyTeam_ = sources_.team != nullptr || (diffuseIsTeamMasked_ && diffuse_ != nullptr);
}

// --- the coverage, which never used to cross at all ----------------------
//
// StarCraft II's per-texel coverage is its ALPHA-MASK LAYERS -- `cFinal.a =
// mask1.a * mask2.a * alphaFactor` (psmaterial.fx:380) -- and Reforged reads
// coverage off the base colour's alpha. 104,869 of 176,955 shipped materials
// carry a mask, and without this an alpha-tested export either leaked the team
// mask as the cutout (unbaked diffuse) or erased the cutout (baked opaque).
void MaterialBake::ResolveCoverage() {
    // StarCraft II tests the COMPOSED coverage against a per-material
    // threshold; Warcraft III's Transparent filter tests the written alpha at a
    // fixed 0.75. A keyed surface therefore bakes the test's RESULT (0/255 at
    // the source's own cut-off).
    alphaKeyed_ = standard_.blendMode == nat::M3BlendMode::Opaque && standard_.alphaTestThreshold > 0;
    // Every surface whose blend READS the written alpha takes the composed
    // coverage — with no masks that is a constant 1, and writing THAT is
    // still the fix: StarCraft II's coverage was 1 (`vertColor.a *
    // alphaFactor`; the masks are the only per-texel term), while an unbaked
    // export hands Warcraft III whatever the texture's own alpha holds.
    // Additive and the two modulates ignore it on both engines.
    hasCoverage_ = alphaKeyed_ || standard_.blendMode == nat::M3BlendMode::AlphaBlend ||
                   standard_.blendMode == nat::M3BlendMode::AlphaAdd;
    if (!hasCoverage_) {
        return;
    }
    const std::array<const M3TextureLayer*, 2> masks = {MaskLayerOf(standard_.alphaLayer1),
                                                       MaskLayerOf(standard_.alphaLayer2)};
    for (std::size_t i = 0; i < masks.size(); ++i) {
        if (masks[i] == nullptr) {
            continue;
        }
        // The bake samples every source over the base colour's coordinates, so
        // a mask on another UV set cannot be composed from pixels alone --
        // where the two sets land a texel depends on the mesh. 18,792 of ~119k
        // shipped mask layers do this. The fallback is the DIFFUSE's own alpha
        // when the diffuse is not team-masked: a glow texture's alpha usually
        // resembles its mask, and forcing opaque instead turned the Aiur light
        // bridge's translucent beams into a solid white sheet. A team-masked
        // diffuse still goes opaque -- its alpha is the team mask, and leaking
        // THAT as coverage is the original P0 bug.
        if (diffuseLayer_ != nullptr && !hasFlag(masks[i]->flags, M3TextureLayerFlag::Color) &&
            masks[i]->uvMapping != diffuseLayer_->uvMapping) {
            if (!diffuseIsTeamMasked_ && diffuse_ != nullptr) {
                coverage_[i].texture = diffuse_;
                coverage_[i].channel = tx::Channel::A;
                out_.info(wem::DiagCode::LossyKindConversion,
                          std::string("alpha mask ") + (i == 0 ? "1" : "2") +
                              " samples a different UV set than the diffuse; "
                              "the diffuse's own alpha stands in for it",
                          where_, wem::ProfileId::Wc3Reforged);
            } else {
                out_.warn(wem::DiagCode::LossyKindConversion,
                          std::string("alpha mask ") + (i == 0 ? "1" : "2") +
                              " samples a different UV set than the diffuse; "
                              "the cutout cannot be baked and the surface "
                              "exports opaque there",
                          where_, wem::ProfileId::Wc3Reforged);
            }
            continue;
        }
        coverage_[i] = CoverageOf(*masks[i], cache_, where_, out_);
    }
}

bool MaterialBake::HasOrmSource() const {
    // The envio decision is repeated here without decoding: the reflectance
    // stage runs after this gate.
    const bool anyEnvio = LayerOf(standard_.environmentLayer) != nullptr &&
                          (standard_.layerBlendMode == M3LayerBlendOp::Add ||
                           standard_.layerBlendMode == M3LayerBlendOp::Lerp ||
                           standard_.layerBlendMode == M3LayerBlendOp::Mod);
    return sources_.specular != nullptr || sources_.occlusion != nullptr || anyTeam_ || anyEnvio;
}

bool MaterialBake::HasAnythingToBake() const {
    // A glow-only material (two additive emissives and nothing else) has no ORM
    // to bake and no coverage — but its summed emissive and its rim fresnel
    // still need the stages below.
    const auto additiveEmissive = [&](std::size_t slot) {
        return LayerOf(*emissiveLayers_[slot]) != nullptr && IsAdditiveOp(emissiveOps_[slot]);
    };
    const auto rim = [&](std::size_t slot) {
        return additiveEmissive(slot) &&
               (*emissiveLayers_[slot])->fresnelMode == nat::M3FresnelMode::Standard;
    };
    const bool emissivePair = additiveEmissive(0) && additiveEmissive(1);
    const bool emissiveRim = rim(0) || rim(1);
    return HasOrmSource() || hasCoverage_ || emissivePair || emissiveRim;
}

// --- what the surface reflects, as StarCraft II states it ----------------
//
// The material's exponent, its HDR multiplier and the specular layer's own tint
// are all part of the reflectance the shader reaches for, and none of them were
// being read: the bake inferred a roughness from how bright the map was, which
// pinned every StarCraft II surface near 0.88 and left it with no highlight to
// convert. `pbr_bake.h` carries the algebra.
tx::pbr::SpecularReflectance MaterialBake::Reflectance() {
    tx::pbr::SpecularReflectance reflectance;
    reflectance.exponent = standard_.specularExponent > 0.0f ? standard_.specularExponent
                                                             : kDefaultSpecularExponent;
    reflectance.factor =
        standard_.hdrSpecularMultiplier > 0.0f ? standard_.hdrSpecularMultiplier : 1.0f;
    const bool simulateRoughness = (static_cast<u32>(standard_.flags) & kSimulateRoughness) != 0;
    reflectance.energyConserving = !simulateRoughness;
    // Under the flag the gloss layer below is `1 - roughness` — the engine
    // blurs the reflection by it — not an exponent scale.
    reflectance.simulateRoughness = simulateRoughness;
    reflectance.exponentScale.texture = sources_.gloss;
    reflectance.exponentScale.channel = sources_.glossChannel;
    reflectance.exponentScale.constant = 1.0f;
    // The engine squares the LAYER COLOUR's alpha — after the channel select,
    // the invert flag and the multiply-add — not the raw texel
    // (`MaterialSpecularity`, and `M3LayerValue` in our own renderer). The
    // unauthored-zero guard is `LayerTintInto`'s.
    if (const M3TextureLayer* layer = LayerOf(standard_.glossLayer)) {
        reflectance.exponentScale.invert = hasFlag(layer->flags, M3TextureLayerFlag::ColorInvert);
        const f32 multiply = layer->rgbMultiply.initValue;
        reflectance.exponentScale.postScale = multiply > 0.0f ? multiply : 1.0f;
        reflectance.exponentScale.bias = layer->rgbAdd.initValue;
    }
    if (const M3TextureLayer* layer = LayerOf(standard_.specularLayer)) {
        reflectance.specular.texture = sources_.specular;
        reflectance.specular.srgb = kSpecularIsDisplayReferred;
        // `SpecularMode::AlphaOnly` splats the layer's alpha; otherwise the
        // layer's own channel select decides, and RGB — the common case — reads
        // the texture's three channels as they are.
        if (standard_.specularMode == nat::M3SpecularMode::AlphaOnly) {
            reflectance.specular.splat = tx::Channel::A;
        } else if (layer->colorType != nat::M3ColorChannelSelect::RGB &&
                   layer->colorType != nat::M3ColorChannelSelect::RGBA) {
            reflectance.specular.splat = AlphaChannelFor(layer->colorType);
        }
        LayerTintInto(*layer, 1.0f, reflectance.specular.scale);
        reflectance.specular.bias = layer->rgbAdd.initValue;
    }
    AddEnvioReflectance(reflectance);
    return reflectance;
}

// --- the envio layer, as reflectance -------------------------------------
//
// `ApplyEnv` adds (or lerps) `cube * tint * mask` into the lit colour;
// Reforged's probe reflection is `F0*lut.x + lut.y` — so a real reflection
// crosses as an F0 bump, per texel through the EnvioMask, in the ratio of the
// two environments' brightness. The op is the material's layerBlendMode (the
// decal's field, psmaterial.fx:332): Add and Lerp cross as that bump, Mod — the
// league skins' `lit * cube * mask` — as a reflection in the albedo's colour
// (`pbr_bake.h`), and every other op draws nothing in the game either. 26,894
// shipped materials carry the layer, 24,491 with a mask.
void MaterialBake::AddEnvioReflectance(tx::pbr::SpecularReflectance& reflectance) {
    const M3TextureLayer* envLayer = LayerOf(standard_.environmentLayer);
    if (envLayer == nullptr) {
        return;
    }
    const M3LayerBlendOp envOp = standard_.layerBlendMode;
    const bool modulated = envOp == M3LayerBlendOp::Mod;
    const bool live = modulated || envOp == M3LayerBlendOp::Add || envOp == M3LayerBlendOp::Lerp;
    const tx::Texture* envTexture = live ? cache_.Decode(envLayer->texturePath) : nullptr;
    if (envTexture == nullptr) {
        return;
    }
    const f32 envConstant =
        standard_.hdrEnvironmentConstant > 0.0f ? standard_.hdrEnvironmentConstant : 1.0f;
    f32 tint[3];
    LayerTintInto(*envLayer, envConstant, tint);
    const f32 tintLum = color::Rec709Luminance(tint[0], tint[1], tint[2]);
    reflectance.envReflectance = MeanLinearLuminance(*envTexture) * tintLum / kReforgedProbeLuminance;
    reflectance.envRoughnessCap = kEnvRoughnessCap;
    reflectance.envModulates = modulated;
    const std::optional<M3TextureLayer>& maskSlot = standard_.environmentMaskLayer;
    if (maskSlot.has_value() &&
        (LayerOf(maskSlot) != nullptr || hasFlag(maskSlot->flags, M3TextureLayerFlag::Color))) {
        reflectance.envMask = EnvMaskOf(*maskSlot, envOp, cache_, where_, out_);
    }
    if (modulated) {
        out_.info(wem::DiagCode::LossyKindConversion,
                  "a Mod-op envio layer modulates the lit colour by its "
                  "reflection; it crosses as a metal in the albedo's colour",
                  where_, wem::ProfileId::Wc3Reforged);
    }
}

// --- the decal, composited -----------------------------------------------
//
// The slot map holds ONE base colour and `exportPbr` has one slot, so a decal
// crosses as pixels or not at all — the Marine's chest insignia was the
// standing casualty. Folded into the albedo where `CombineLayerColor` folds it:
// before the team lerp and the metal gain, in both bakes, so they split the
// same albedo.
void MaterialBake::ResolveDecal() {
    decalLayer_ = LayerOf(standard_.decalLayer);
    if (decalLayer_ == nullptr || diffuse_ == nullptr) {
        return;
    }
    const std::optional<tx::pbr::DecalOp> op = DecalOpFor(standard_.layerBlendMode);
    const tx::Texture* decalTexture = cache_.Decode(decalLayer_->texturePath);
    if (!op.has_value()) {
        // A TeamColor op on the decal never ships (0 of 176,955).
    } else if (decalTexture == nullptr) {
        out_.warn(wem::DiagCode::TextureUnresolved,
                  "the decal '" + export_text::TrimFixedWidth(decalLayer_->texturePath) +
                      "' could not be decoded; it is not composited",
                  where_, wem::ProfileId::Wc3Reforged);
    } else if (decalLayer_->uvMapping != diffuseLayer_->uvMapping) {
        out_.warn(wem::DiagCode::LossyKindConversion,
                  "the decal samples a different UV set than the diffuse; it "
                  "cannot be composited from pixels alone",
                  where_, wem::ProfileId::Wc3Reforged);
    } else {
        decal_ = ColorInputOf(*decalLayer_, decalTexture);
        decalOp_ = *op;
    }
}

tx::pbr::OrmRecipe MaterialBake::OrmRecipeFrom(const tx::pbr::SpecularReflectance& reflectance) const {
    tx::pbr::OrmRecipe recipe;
    recipe.reflectance = reflectance;
    recipe.decal = decal_;
    recipe.decalOp = decalOp_;
    recipe.occlusion.texture = sources_.occlusion;
    recipe.occlusion.channel = sources_.occlusionChannel;
    recipe.occlusion.constant = 1.0f;
    recipe.teamColor[0] = teamColor_.x;
    recipe.teamColor[1] = teamColor_.y;
    recipe.teamColor[2] = teamColor_.z;
    if (diffuseIsTeamMasked_) {
        recipe.teamMask.texture = diffuse_;
        recipe.teamMask.channel = tx::Channel::A;
        recipe.teamMask.invert = true; // team where the albedo is NOT
    }
    // A team layer whose channel select is RGB has its alpha forced to 1 by
    // `SelectChannels`, so the team colour lands on the whole surface. That is
    // a constant, not a map.
    recipe.teamMaskAlt.texture = sources_.teamWholeSurface ? nullptr : sources_.team;
    recipe.teamMaskAlt.channel = sources_.teamChannel.value_or(tx::Channel::A);
    recipe.teamMaskAlt.constant = sources_.teamWholeSurface ? 1.0f : 0.0f;
    // Not a source of the map, and not optional either: the albedo is what the
    // metalness splits (`m = F0 / (F0 + albedo)`) and what sharpens the team
    // mask from the team's share of the *result* into its share of the
    // *modulation*. `pbr_bake.h` carries both derivations.
    recipe.baseColor.texture = diffuse_;
    recipe.baseColor.srgb = true;
    return recipe;
}

void MaterialBake::SetSlotTexture(wem::PbrSlot slot, u32 texture) {
    wem::TextureInput input;
    if (const wem::TextureInput* existing = body_->find(slot)) {
        input = *existing;
    }
    input.texture = texture;
    body_->set(slot, input);
}

bool MaterialBake::BakeOrmMap(const tx::pbr::OrmRecipe& recipe) {
    std::optional<tx::Texture> orm = tx::pbr::BakeOrm(recipe);
    if (!orm) {
        // The ORM failed; the coverage below may still land, so this is a
        // report, not a stop.
        out_.warn(wem::DiagCode::TextureUnresolved,
                  "no source of the occlusion/roughness/metalness map could be decoded", where_,
                  wem::ProfileId::Wc3Reforged);
        return false;
    }
    const u32 index =
        cache_.Intern(OrmNameFor(sources_.ormNameSource, sc2Material_.name, index_));
    wem::TextureInput input;
    input.texture = index;
    input.colorSpace = wem::ColorSpace::Linear;
    body_->set(wem::PbrSlot::Orm, input);
    result_.baked.insert_or_assign(index, BakedTexture::Declared(std::move(*orm), tx::PixelFormat::BC3));
    return true;
}

// --- and the base colour pays for both of them ---------------------------
//
// Two rewrites in one pass, and neither is optional. The metalness above took
// `F0` out of the diffuse lobe, so the albedo has to go up by exactly that much
// or the model comes back darker than it went in — which is what "we added
// metalness and it got worse" looks like. And the team tint is lightened under
// the *combined* mask, not under the diffuse's own alpha: a material whose team
// colour arrives through the `TeamColor*Add` op has its mask in a different
// texture entirely, and leaving its albedo as StarCraft II authored it — art
// that averages 26/255, because the engine was going to replace it — is what
// makes Warcraft III's modulated tint come out near-black.
void MaterialBake::RewriteBaseColor(const tx::pbr::OrmRecipe& recipe) {
    if (diffuse_ == nullptr && hasCoverage_) {
        out_.warn(wem::DiagCode::TextureUnresolved,
                  "an alpha-reading material's diffuse could not be decoded; its "
                  "coverage cannot be baked",
                  where_, wem::ProfileId::Wc3Reforged);
    }
    if (diffuse_ == nullptr || diffuseLayer_ == nullptr) {
        return;
    }
    tx::pbr::BaseColorRecipe base;
    base.baseColor = recipe.baseColor;
    base.teamMask = recipe.teamMask;
    base.teamMaskAlt = recipe.teamMaskAlt;
    std::copy_n(recipe.teamColor, 3, base.teamColor);
    base.reflectance = recipe.reflectance;

    const u32 diffuseIndex = cache_.IndexOf(diffuseLayer_->texturePath);
    // A composited decal moves the bake to a per-material texture for the same
    // reason coverage does: the diffuse is shared, and only THIS material wears
    // the insignia.
    if ((hasCoverage_ || recipe.decal.present()) && diffuseIndex != wem::kInvalidIndex) {
        // The composed cutout is this MATERIAL's, not the texture's: an
        // Overlord's opaque Body and blended Sacs share one diffuse, so writing
        // coverage into the shared file would either race the opaque bake
        // (first writer wins) or hand the Body the Sacs' holes. A new texture,
        // and only this material's slot moves.
        base.decal = recipe.decal;
        base.decalOp = recipe.decalOp;
        base.coverage1 = coverage_[0];
        base.coverage2 = coverage_[1];
        if (alphaKeyed_) {
            base.coverageCutoff = static_cast<f32>(standard_.alphaTestThreshold) / 255.0f;
        }
        std::optional<tx::Texture> rewritten = tx::pbr::BakeBaseColor(base);
        if (!rewritten) {
            return;
        }
        std::string cutName = ComposedNameFor(diffuseLayer_->texturePath, index_, "_cut");
        const auto pathOf = [](const std::optional<M3TextureLayer>& layer) {
            return layer ? export_text::TrimFixedWidth(layer->texturePath) : std::string();
        };
        std::array<std::string, 3> masks = {
            pathOf(standard_.alphaLayer1), pathOf(standard_.alphaLayer2),
            decal_.present() ? export_text::TrimFixedWidth(decalLayer_->texturePath)
                             : std::string()};
        const auto claim = claims_.find(cutName);
        if (claim != claims_.end() && claim->second != masks) {
            cutName.insert(cutName.size() - kDdsExtension.size(), "_" + std::to_string(index_));
        }
        claims_.emplace(cutName, std::move(masks));
        const u32 cutIndex = cache_.Intern(cutName);
        SetSlotTexture(wem::PbrSlot::BaseColor, cutIndex);
        // BC1's alpha is one bit; the composed cutout needs BC3.
        result_.baked.insert_or_assign(cutIndex,
                                       BakedTexture::Declared(std::move(*rewritten), tx::PixelFormat::BC3));
    } else if (hasCoverage_) {
        out_.warn(wem::DiagCode::TextureUnresolved,
                  "an alpha-reading material's diffuse has no document entry; "
                  "its coverage cannot be baked",
                  where_, wem::ProfileId::Wc3Reforged);
    } else if (diffuseIndex != wem::kInvalidIndex &&
               result_.baked.find(diffuseIndex) == result_.baked.end()) {
        if (std::optional<tx::Texture> rewritten = tx::pbr::BakeBaseColor(base)) {
            result_.baked.emplace(diffuseIndex,
                                  BakedTexture::Declared(std::move(*rewritten), tx::PixelFormat::BC1));
        }
    }
}

// --- two additive emissives are one map, summed --------------------------
//
// StarCraft II folds both layers into one accumulator before
// `fEmissiveMultiplier`; the slot map holds one, so the second was silently
// gone. A plain (alpha-weighted) add of pixels says it.
void MaterialBake::SumAdditiveEmissives() {
    const M3TextureLayer* emissive1 = LayerOf(*emissiveLayers_[0]);
    const M3TextureLayer* emissive2 = LayerOf(*emissiveLayers_[1]);
    if (emissive1 == nullptr || emissive2 == nullptr || !IsAdditiveOp(emissiveOps_[0]) ||
        !IsAdditiveOp(emissiveOps_[1])) {
        return;
    }
    const tx::pbr::ColorInput first =
        ColorInputOf(*emissive1, cache_.Decode(emissive1->texturePath));
    const tx::pbr::ColorInput second =
        ColorInputOf(*emissive2, cache_.Decode(emissive2->texturePath));
    if (!first.present() || !second.present()) {
        return;
    }
    std::optional<tx::Texture> sum =
        tx::pbr::BakeEmissiveSum(first, emissiveOps_[0] == M3LayerBlendOp::Add, second,
                                 emissiveOps_[1] == M3LayerBlendOp::Add);
    if (!sum.has_value()) {
        return;
    }
    const u32 index = cache_.Intern(ComposedNameFor(emissive1->texturePath, index_, "_glow"));
    SetSlotTexture(wem::PbrSlot::Emissive, index);
    result_.baked.insert_or_assign(index, BakedTexture::Declared(std::move(*sum), tx::PixelFormat::BC1));
    out_.info(wem::DiagCode::LossyKindConversion,
              "both additive emissive layers were summed into one map", where_,
              wem::ProfileId::Wc3Reforged);
}

// --- fresnel: the rim glow crosses, the rest is named --------------------
//
// The MDX layer carries fresnelColor/Opacity/TeamColor and Reforged lerps the
// lit colour toward that flat colour by `opacity * (1-NdotV)^2`. The clean case
// is fresnel on an ADDITIVE emissive — a rim glow (12,226 shipped layers): the
// colour is the map's mean under its tint and gain, the opacity is the ramp's
// ceiling. Inverted mode (centre glow) and fresnel on masks or diffuse have no
// overlay to cross into.
void MaterialBake::CrossRimFresnel() {
    wem::CommonMaterial& derived = wc3Material_.MutableCommon();
    std::erase_if(derived.features, [](const wem::MaterialFeature& feature) {
        return feature.kind() == wem::FeatureKind::Fresnel;
    });
    const M3TextureLayer* rim = nullptr;
    for (std::size_t slot = 0; slot < emissiveLayers_.size(); ++slot) {
        const M3TextureLayer* layer = LayerOf(*emissiveLayers_[slot]);
        if (layer == nullptr || layer->fresnelMode != nat::M3FresnelMode::Standard ||
            !IsAdditiveOp(emissiveOps_[slot])) {
            continue;
        }
        if (layer->fresnelMin > layer->fresnelMax) {
            continue; // a deliberately inverted ramp — facing-bright
        }
        rim = layer;
        break;
    }
    if (rim == nullptr) {
        return;
    }
    const f32 rimGain =
        standard_.hdrEmissiveMultiplier > 0.0f ? standard_.hdrEmissiveMultiplier : 1.0f;
    const tx::Texture* map = cache_.Decode(rim->texturePath);
    f32 tint[3];
    LayerTintInto(*rim, rimGain, tint);
    f32 mean[3] = {1.0f, 1.0f, 1.0f};
    if (map != nullptr) {
        MeanLinearRgb(*map, mean);
    }
    wem::FresnelFeature fresnel;
    // Capped to a unit maximum, hue kept. The native term ADDS `emissive *
    // ramp` on top of the lit colour; the overlay LERPS the lit colour toward
    // this flat colour, so a gain-multiplied value past 1 does not glow brighter
    // — it bleaches. The Aiur light bridge's grazing beam planes went out as a
    // white flood before the cap.
    f32 rimColor[3] = {tint[0] * mean[0], tint[1] * mean[1], tint[2] * mean[2]};
    const f32 peak = std::max({rimColor[0], rimColor[1], rimColor[2], 1.0f});
    fresnel.color = Vector3f{rimColor[0] / peak, rimColor[1] / peak, rimColor[2] / peak};
    fresnel.exponent = rim->fresnelExponent > 0.0f ? rim->fresnelExponent : 1.0f;
    fresnel.outMin = rim->fresnelMin;
    fresnel.outMax = rim->fresnelMax > 0.0f ? rim->fresnelMax : 1.0f;
    wem::MaterialFeature feature;
    feature.id = wem::NextFeatureId(derived.features);
    feature.layer = wem::kWholeMaterial;
    feature.payload = fresnel;
    derived.features.push_back(feature);
    out_.info(wem::DiagCode::LossyKindConversion,
              "the emissive rim fresnel became the layer's fresnel overlay", where_,
              wem::ProfileId::Wc3Reforged);
}

// --- the team layer is not an emissive map -------------------------------
//
// It reached the emissive slot because `channelOf` reads the layer it sits in
// and not the op it is combined by, and a mask bound as an emissive map is a
// surface glowing its own coverage. Its content is in the ORM's alpha now.
void MaterialBake::DropTeamEmissive() {
    if (teamLayer_ == nullptr || !ormLanded_) {
        return;
    }
    const u32 teamIndex = cache_.IndexOf(teamLayer_->texturePath);
    const wem::TextureInput* emissive = body_->find(wem::PbrSlot::Emissive);
    if (emissive == nullptr || emissive->texture != teamIndex) {
        return;
    }
    const std::size_t otherSlot = 1 - teamSlot_;
    const M3TextureLayer* other = LayerOf(*emissiveLayers_[otherSlot]);
    const u32 otherIndex = other != nullptr && !IsTeamOp(emissiveOps_[otherSlot])
                               ? cache_.IndexOf(other->texturePath)
                               : wem::kInvalidIndex;
    // Erased, not blanked. A slot holding an input with no texture is not an
    // empty slot to `exportPbr`; it is a slot whose texture index is
    // `kInvalidIndex`, and that maps to 0.
    std::erase_if(body_->slots,
                  [](const auto& entry) { return entry.first == wem::PbrSlot::Emissive; });
    if (otherIndex != wem::kInvalidIndex) {
        wem::TextureInput replacement;
        replacement.texture = otherIndex;
        body_->set(wem::PbrSlot::Emissive, replacement);
    }
    out_.info(wem::DiagCode::LossyKindConversion,
              "the team-colour layer became the ORM's mask, not the emissive map", where_,
              wem::ProfileId::Wc3Reforged);
}

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

    ExportTextureCache cache(document, provider);
    ComposedClaims claims;

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
            if (const nat::M3Standard* standard = StandardOf(sc2->materials[m])) {
                MaterialBake(*standard, sc2->materials[m], wc3->materials[m],
                             static_cast<u32>(m), cache, claims, normal, teamColor, result, out)
                    .Run();
            }
        }
    }

    return result;
}

} // namespace whiteout::flakes
