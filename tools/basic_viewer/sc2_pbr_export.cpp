// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "sc2_pbr_export.h"

#include "renderer/model/model_source_utils.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/content_ref.h"

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

/// `MaterialFlag::SimulateRoughness`. Its absence is what turns
/// `FakeEnergyConservingSpec` on, and it is absent on 13,015 of 13,091
/// measured materials.
constexpr u32 kSimulateRoughness = 0x800u;

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
                                      wem::Diagnostics& out) {
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
                sources.occlusion = cache.Decode(layer->texturePath);
                sources.occlusionChannel = ScalarChannelFor(layer->colorType);
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
            if (sources.specular == nullptr && sources.occlusion == nullptr && !anyTeam) {
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
            reflectance.energyConserving =
                (static_cast<u32>(standard->flags) & kSimulateRoughness) == 0;
            reflectance.exponentScale.texture = sources.gloss;
            reflectance.exponentScale.channel = sources.glossChannel;
            reflectance.exponentScale.constant = 1.0f;
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

            tx::pbr::OrmRecipe recipe;
            recipe.reflectance = reflectance;
            recipe.occlusion.texture = sources.occlusion;
            recipe.occlusion.channel = sources.occlusionChannel;
            recipe.occlusion.constant = 1.0f;
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

            std::optional<tx::Texture> orm = tx::pbr::BakeOrm(recipe);
            if (!orm) {
                ++result.materialsSkipped;
                out.warn(wem::DiagCode::TextureUnresolved,
                         "no source of the occlusion/roughness/metalness map could be decoded",
                         where, wem::ProfileId::Wc3Reforged);
                continue;
            }

            const std::string name =
                OrmNameFor(sources.ormNameSource, sc2->materials[m].name, static_cast<u32>(m));
            const u32 index = cache.Intern(name);
            wem::TextureInput input;
            input.texture = index;
            input.colorSpace = wem::ColorSpace::Linear;
            body->set(wem::PbrSlot::Orm, input);
            result.baked.insert_or_assign(index,
                                          BakedTexture{std::move(*orm), tx::PixelFormat::BC3});
            ++result.ormBaked;

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
            if (diffuse != nullptr && diffuseLayer != nullptr) {
                const u32 diffuseIndex = cache.IndexOf(diffuseLayer->texturePath);
                if (diffuseIndex != wem::kInvalidIndex &&
                    result.baked.find(diffuseIndex) == result.baked.end()) {
                    tx::pbr::BaseColorRecipe base;
                    base.baseColor = recipe.baseColor;
                    base.teamMask = recipe.teamMask;
                    base.teamMaskAlt = recipe.teamMaskAlt;
                    base.reflectance = recipe.reflectance;
                    if (std::optional<tx::Texture> rewritten = tx::pbr::BakeBaseColor(base)) {
                        result.baked.emplace(
                            diffuseIndex, BakedTexture{std::move(*rewritten), tx::PixelFormat::BC1});
                        ++result.baseColorsCleared;
                    }
                }
            }

            // --- the team layer is not an emissive map ---------------------
            //
            // It reached the emissive slot because `channelOf` reads the layer
            // it sits in and not the op it is combined by, and a mask bound as
            // an emissive map is a surface glowing its own coverage. Its
            // content is in the ORM's alpha now.
            if (teamLayer != nullptr) {
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
