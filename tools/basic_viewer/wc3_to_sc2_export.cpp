// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "wc3_to_sc2_export.h"

#include "export_texture_cache.h"

#include <whiteout/models/wem/retarget.h>
#include <whiteout/textures/pbr_bake.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace whiteout::flakes {

namespace {

namespace tx = ::whiteout::textures;

/// `"Stand - 1"` → `"Stand 01"`, `"Attack Defend - 2"` → `"Attack Defend 02"`.
/// Anything that does not end in ` - <number>` passes through untouched —
/// `Stand Victory` and `Decay Flesh` are names, not variants.
bool RenameSequence(std::string& name) {
    const std::size_t dash = name.rfind(" - ");
    if (dash == std::string::npos || dash + 3 >= name.size()) {
        return false;
    }
    const std::string digits = name.substr(dash + 3);
    for (const char c : digits) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    name = name.substr(0, dash) + " " + (digits.size() < 2 ? "0" + digits : digits);
    return true;
}

std::string PathOfTexture(const wem::Document& document, u32 index) {
    if (index >= document.textures.size()) {
        return {};
    }
    const wem::TextureRef& ref = document.textures[index];
    if (const auto* path = std::get_if<wem::TexturePath>(&ref.key)) {
        if (!path->value.empty()) {
            return path->value;
        }
    }
    return ref.path;
}

std::string StemOf(const std::string& path) {
    std::string name = path;
    std::replace(name.begin(), name.end(), '\\', '/');
    const std::size_t slash = name.rfind('/');
    if (slash != std::string::npos) {
        name = name.substr(slash + 1);
    }
    const std::size_t dot = name.rfind('.');
    if (dot != std::string::npos) {
        name = name.substr(0, dot);
    }
    return name.empty() ? "material" : name;
}

/// One decoded plane, sampled nearest by normalized coordinates. Absent
/// textures answer their fallback so the loops below stay branch-light.
struct Plane {
    tx::Texture rgba;
    bool present = false;

    static Plane From(const tx::Texture* texture) {
        Plane plane;
        if (texture != nullptr) {
            try {
                plane.rgba = texture->copyAsFormat(tx::PixelFormat::RGBA8);
                plane.present = true;
            } catch (const std::exception&) {
            }
        }
        return plane;
    }

    void sample(f32 u, f32 v, f32 out[4], const f32 fallback[4]) const {
        if (!present) {
            for (int c = 0; c < 4; ++c) {
                out[c] = fallback[c];
            }
            return;
        }
        const u32 w = rgba.width();
        const u32 h = rgba.height();
        const u32 x = std::min(w - 1, static_cast<u32>(u * static_cast<f32>(w)));
        const u32 y = std::min(h - 1, static_cast<u32>(v * static_cast<f32>(h)));
        const std::span<const u8> px = rgba.mipData(0);
        const std::size_t at = (static_cast<std::size_t>(y) * w + x) * 4;
        for (int c = 0; c < 4; ++c) {
            out[c] = px[at + c] / 255.0f;
        }
    }
};

const wem::TextureInput* SlotOf(const wem::PbrDeferredBody& body, wem::PbrSlot slot) {
    const wem::TextureInput* input = body.find(slot);
    return input != nullptr && input->hasTexture() ? input : nullptr;
}

/// The composite layer every arm below appends: @p texture into @p target.
wem::CompositeLayer LayerOf(u32 texture, wem::SurfaceChannel target, wem::CompositeOp op,
                            const wem::TextureInput* like = nullptr) {
    wem::CompositeLayer layer;
    if (like != nullptr) {
        layer.input = *like;
    }
    layer.input.texture = texture;
    layer.target = target;
    layer.op = op;
    return layer;
}

/// The Reforged half: rebuild each PBR material's StarCraft II composite from
/// the inverse bake. See the header for the equations and their provenance.
void RestateReforgedMaterials(wem::Document& document, io::IContentProvider* provider,
                              Wc3ToSc2Result& result, wem::Diagnostics& out) {
    if (document.models.empty()) {
        return;
    }
    wem::Model& model = document.models.front();

    const wem::ProfileMaterialSet* source = nullptr;
    for (const wem::ProfileMaterialSet& set : model.profileSets) {
        if (set.profile == wem::ProfileId::Wc3Reforged) {
            source = &set;
        }
    }
    if (source == nullptr) {
        return; // a Classic document; nothing to invert.
    }
    bool anyPbr = false;
    for (const wem::Material& material : source->materials) {
        if (material.Common().pbr() != nullptr) {
            anyPbr = true;
            break;
        }
    }
    if (!anyPbr) {
        return; // the SD half only; the Classic fold owns it.
    }

    if (!document.carries(wem::ProfileId::Sc2)) {
        document.declare(wem::ProfileId::Sc2);
        const wem::DeriveResult derived =
            wem::DeriveProfile(document, wem::ProfileId::Wc3Reforged, wem::ProfileId::Sc2);
        out.append(derived.diagnostics);
        if (!derived.ok) {
            out.warn(wem::DiagCode::LossyKindConversion,
                     "the StarCraft II set would not derive; the inverse bake was skipped",
                     wem::ElementRef(wem::ElementKind::Document, 0), wem::ProfileId::Sc2);
            return;
        }
    }
    // The derive pushed a set, which reallocates `profileSets` -- the source
    // pointer from before it is dangling. Find both again.
    source = nullptr;
    wem::ProfileMaterialSet* target = nullptr;
    for (wem::ProfileMaterialSet& set : model.profileSets) {
        if (set.profile == wem::ProfileId::Wc3Reforged) {
            source = &set;
        }
        if (set.profile == wem::ProfileId::Sc2) {
            target = &set;
        }
    }
    if (source == nullptr) {
        return;
    }
    if (target == nullptr || target->materials.size() != source->materials.size()) {
        out.warn(wem::DiagCode::LossyKindConversion,
                 "the derived StarCraft II set does not align with the Reforged one; the "
                 "inverse bake was skipped",
                 wem::ElementRef(wem::ElementKind::Document, 0), wem::ProfileId::Sc2);
        return;
    }

    ExportTextureCache cache(document, provider);
    // The synthetic team under-layer: a pathless replaceable-1 reference is
    // the exact shape a Classic team stack has, so the material fold's team
    // branch (the RGBA select) fires for both generations from one rule.
    u32 teamRef = wem::kInvalidIndex;
    const auto teamUnderRef = [&]() {
        if (teamRef == wem::kInvalidIndex) {
            wem::TextureRef ref;
            ref.replaceableId = 1;
            document.textures.push_back(std::move(ref));
            teamRef = static_cast<u32>(document.textures.size() - 1);
        }
        return teamRef;
    };

    for (std::size_t m = 0; m < source->materials.size(); ++m) {
        const wem::PbrDeferredBody* pbr = source->materials[m].Common().pbr();
        if (pbr == nullptr) {
            continue;
        }
        const wem::TextureInput* baseSlot = SlotOf(*pbr, wem::PbrSlot::BaseColor);
        const wem::TextureInput* ormSlot = SlotOf(*pbr, wem::PbrSlot::Orm);
        const wem::TextureInput* normalSlot = SlotOf(*pbr, wem::PbrSlot::Normal);
        const wem::TextureInput* emissiveSlot = SlotOf(*pbr, wem::PbrSlot::Emissive);
        const wem::TextureInput* teamSlot = SlotOf(*pbr, wem::PbrSlot::TeamColorMask);
        if (baseSlot == nullptr) {
            ++result.materialsSkipped;
            continue;
        }
        const std::string basePath = PathOfTexture(document, baseSlot->texture);
        const Plane base = Plane::From(cache.Decode(basePath));
        const Plane orm = Plane::From(
            ormSlot != nullptr ? cache.Decode(PathOfTexture(document, ormSlot->texture))
                               : nullptr);
        const Plane team = Plane::From(
            teamSlot != nullptr ? cache.Decode(PathOfTexture(document, teamSlot->texture))
                                : nullptr);
        if (!base.present) {
            ++result.materialsSkipped;
            out.warn(wem::DiagCode::LossyKindConversion,
                     "a Reforged material's base colour would not decode; its StarCraft II "
                     "material keeps the derive's plain diffuse",
                     wem::ElementRef(wem::ElementKind::Material, m), wem::ProfileId::Sc2);
            continue;
        }

        const u32 width = base.rgba.width();
        const u32 height = base.rgba.height();
        constexpr f32 kOrmFallback[4] = {1.0f, 0.55f, 0.0f, 0.0f}; // AO 1, n~20, dielectric
        constexpr f32 kZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};

        // One pass over the texels gathers everything: the team share, the
        // roughness median, and both baked planes.
        bool anyTeam = false;
        std::vector<f32> roughness;
        roughness.reserve(static_cast<std::size_t>(width) * height / 16 + 1);
        tx::Texture teamDiffuse = tx::Texture::create2D(tx::PixelFormat::RGBA8, width, height, 1);
        tx::Texture specular = tx::Texture::create2D(tx::PixelFormat::RGBA8, width, height, 1);
        const std::span<u8> teamPx = teamDiffuse.mipData(0);
        const std::span<u8> specPx = specular.mipData(0);
        const std::span<const u8> basePx = base.rgba.mipData(0);

        for (u32 y = 0; y < height; ++y) {
            const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(height);
            for (u32 x = 0; x < width; ++x) {
                const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(width);
                const std::size_t at = (static_cast<std::size_t>(y) * width + x) * 4;
                f32 o[4];
                orm.sample(u, v, o, kOrmFallback);
                f32 t[4];
                team.sample(u, v, t, kZero);
                const f32 share = std::max(o[3], t[3]);
                anyTeam = anyTeam || share > 0.02f;
                if (((x | y) & 3u) == 0u) {
                    roughness.push_back(o[1]);
                }

                f32 albedo[3];
                for (int c = 0; c < 3; ++c) {
                    albedo[c] = basePx[at + static_cast<std::size_t>(c)] / 255.0f;
                    // `spec = metalness * albedo` — Reforged's F0 exactly
                    // (`reference_reforged_metalness_is_specular`).
                    specPx[at + static_cast<std::size_t>(c)] =
                        static_cast<u8>(std::clamp(o[2] * albedo[c], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
                // Reforged BLENDS the team hue in and keeps the art's
                // brightness; StarCraft II REPLACES the texel where the
                // diffuse alpha is low. Solving the one for the other makes
                // the share the texel's brightness, so the shading painted
                // under the mask comes through as shades of the team colour
                // instead of one flat plate (`tx::pbr::TeamReplaceFromBlend`).
                f32 replaced[3];
                const f32 alpha = tx::pbr::TeamReplaceFromBlend(albedo, share, true, replaced);
                for (int c = 0; c < 3; ++c) {
                    teamPx[at + static_cast<std::size_t>(c)] =
                        static_cast<u8>(std::clamp(replaced[c], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
                teamPx[at + 3] = static_cast<u8>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
                specPx[at + 3] = 255;
            }
        }

        // The exponent from the roughness median; the amplitude parked in the
        // HDR multiplier so the renderer's peak-referenced scale cancels it.
        f32 exponent = 20.0f;
        if (!roughness.empty()) {
            const std::size_t mid = roughness.size() / 2;
            std::nth_element(roughness.begin(), roughness.begin() + mid, roughness.end());
            exponent = tx::pbr::ExponentFromRoughness(roughness[mid]);
        }

        wem::CompositeBody body;
        body.specularExponent = exponent;
        body.specularFactor = Vector4f{(exponent + 2.0f) / 8.0f, 0, 0, 1};

        const std::string stem = StemOf(basePath);
        if (anyTeam) {
            const u32 teamTexture = cache.Intern(stem + "_team.dds");
            result.baked.insert_or_assign(teamTexture,
                                          BakedTexture{std::move(teamDiffuse),
                                                       tx::PixelFormat::BC3});
            body.layers.push_back(
                LayerOf(teamUnderRef(), wem::SurfaceChannel::Color, wem::CompositeOp::Set));
            body.layers.push_back(LayerOf(teamTexture, wem::SurfaceChannel::Color,
                                          wem::CompositeOp::AlphaBlend, baseSlot));
        } else {
            body.layers.push_back(LayerOf(baseSlot->texture, wem::SurfaceChannel::Color,
                                          wem::CompositeOp::Set, baseSlot));
        }

        const u32 specTexture = cache.Intern(stem + "_spec.dds");
        result.baked.insert_or_assign(specTexture,
                                      BakedTexture{std::move(specular), tx::PixelFormat::BC1});
        body.layers.push_back(
            LayerOf(specTexture, wem::SurfaceChannel::Specular, wem::CompositeOp::Set, baseSlot));

        // The normal map crosses through a RESTATEMENT, not a copy: Reforged
        // ships BC5 (x in red, y in green), StarCraft II decodes DXT5nm
        // (`psmaterial.fx` DecodeNormal reads .ag) -- retail art (Marine,
        // Zealot) measures R=255, G=y, B=0, A=x, the DXT5nm layout with a white
        // red so the block's colour endpoints spend their bits on green; the
        // war3 mod's own maps left x in red instead. The restatement is a
        // channel MOVE and nothing else: the axes agree between the engines.
        // It has to be a move
        // because re-encoding the BC5 block as BC3 leaves x riding a
        // constant-1 alpha, which lit the whole model from the side -- and
        // that defect is what once smuggled in a swap+invert pair that
        // rotated every normal 90 degrees. The HD footman shield measures the
        // rotation as a washed-out rim (edge energy 16.3 against 18.6 for the
        // move, native at 21.0), and the forward reaper sweep agrees
        // (identity 5.92, rotation 6.07, flat 6.02).
        if (normalSlot != nullptr) {
            const Plane normal =
                Plane::From(cache.Decode(PathOfTexture(document, normalSlot->texture)));
            if (normal.present) {
                const u32 nw = normal.rgba.width();
                const u32 nh = normal.rgba.height();
                tx::Texture restated = tx::Texture::create2D(tx::PixelFormat::RGBA8, nw, nh, 1);
                const std::span<const u8> in = normal.rgba.mipData(0);
                const std::span<u8> outPx = restated.mipData(0);
                for (std::size_t i = 0; i + 3 < in.size(); i += 4) {
                    outPx[i + 0] = 255;
                    outPx[i + 1] = in[i + 1]; // sc2.y = reforged.y (green stays)
                    outPx[i + 2] = 0;
                    outPx[i + 3] = in[i + 0]; // sc2.x = reforged.x (red into alpha)
                }
                const u32 normalTexture = cache.Intern(stem + "_norm.dds");
                result.baked.insert_or_assign(
                    normalTexture, BakedTexture{std::move(restated), tx::PixelFormat::BC3});
                body.layers.push_back(LayerOf(normalTexture, wem::SurfaceChannel::Normal,
                                              wem::CompositeOp::Set, normalSlot));
            } else {
                body.layers.push_back(LayerOf(normalSlot->texture, wem::SurfaceChannel::Normal,
                                              wem::CompositeOp::Set, normalSlot));
            }
        }
        // `Black32.dds` is Reforged's stock "no emissive" fill; an additive
        // black layer says nothing and clutters every material.
        if (emissiveSlot != nullptr &&
            StemOf(PathOfTexture(document, emissiveSlot->texture)) != "Black32") {
            body.layers.push_back(LayerOf(emissiveSlot->texture, wem::SurfaceChannel::Emissive,
                                          wem::CompositeOp::Add, emissiveSlot));
        }

        wem::Material& derived = target->materials[m];
        // Reforged tests and blends by the base colour's alpha (a Transparent
        // layer keys at 0.75). The team bake took that alpha for its share, so
        // the coverage names the SOURCE map, whose alpha is still the cutout.
        const wem::BlendMode blend = derived.Common().blend;
        if (blend == wem::BlendMode::AlphaKey || blend == wem::BlendMode::Transparent ||
            blend == wem::BlendMode::AlphaBlend || blend == wem::BlendMode::AdditiveAlpha) {
            body.layers.push_back(LayerOf(baseSlot->texture, wem::SurfaceChannel::Coverage,
                                          wem::CompositeOp::Set, baseSlot));
        }
        wem::CommonMaterial& common = derived.MutableCommon();
        common.body = std::move(body);
        // The rim overlay rode the HD layer as a per-layer feature; the rebuilt
        // stack has no such layer, so it is the material's.
        std::erase_if(common.features, [](const wem::MaterialFeature& feature) {
            return feature.kind() == wem::FeatureKind::Fresnel;
        });
        for (const wem::MaterialFeature& feature : source->materials[m].Common().features) {
            if (const wem::FresnelFeature* fresnel = feature.fresnel()) {
                wem::MaterialFeature rim;
                rim.id = wem::NextFeatureId(common.features);
                rim.layer = wem::kWholeMaterial;
                rim.payload = *fresnel;
                common.features.push_back(rim);
            }
        }
        ++result.materialsRestated;
    }

    if (result.materialsRestated != 0) {
        out.info(wem::DiagCode::LossyKindConversion,
                 std::to_string(result.materialsRestated) +
                     " Reforged material(s) restated as specular/gloss (the inverse bake); " +
                     std::to_string(result.materialsSkipped) + " kept the derive's diffuse",
                 wem::ElementRef(wem::ElementKind::Document, 0), wem::ProfileId::Sc2);
    }
}

/// The 8x8 solid the fold's RGBA select turns entirely into team colour: alpha
/// 0 everywhere is `lerp(team, rgb, pow(0, k)) = team`. Blizzard's own
/// conversions name it `war3_TeamColor00.dds`.
BakedTexture StockTeamTexture() {
    tx::Texture texture = tx::Texture::create2D(tx::PixelFormat::RGBA8, 8, 8, 1);
    const std::span<u8> px = texture.mipData(0);
    for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
        px[i + 0] = 128;
        px[i + 1] = 128;
        px[i + 2] = 128;
        px[i + 3] = 0;
    }
    return BakedTexture{std::move(texture), tx::PixelFormat::BC3};
}

/// What a texture's alpha holds, as the fold's `TextureAlphaClass` byte: 1
/// opaque, 2 keyed (under 2% of texels between 8 and 247 -- the sweep's own
/// rule), 3 a gradient.
u8 AlphaClassOf(const tx::Texture& source) {
    tx::Texture rgba;
    try {
        rgba = source.copyAsFormat(tx::PixelFormat::RGBA8);
    } catch (const std::exception&) {
        return 0;
    }
    const std::span<const u8> px = rgba.mipData(0);
    std::size_t texels = 0, low = 0, high = 0, mid = 0;
    for (std::size_t i = 3; i < px.size(); i += 4) {
        ++texels;
        if (px[i] < 8) {
            ++low;
        } else if (px[i] > 247) {
            ++high;
        } else {
            ++mid;
        }
    }
    if (texels == 0) {
        return 0;
    }
    if (high == texels) {
        return 1;
    }
    return mid * 50 < texels ? u8{2} : u8{3};
}

/// Whether any Classic material draws the team plate ON ITS OWN -- nothing
/// keyed, blended or modulated over it -- which is the one shape the fold
/// answers with the RGBA select on the plate's own reference and therefore
/// the one that needs the alpha-0 stock (design §5.2 R3). Under a textured
/// pass the plate's reference is never sampled; under an additive one it is a
/// solid-colour carrier.
bool TeamPlateDrawsAlone(const wem::Document& document) {
    if (document.models.empty()) {
        return false;
    }
    const auto isPlate = [&](const wem::TextureInput& input) {
        return input.hasTexture() && input.texture < document.textures.size() &&
               document.textures[input.texture].replaceableId == 1;
    };
    for (const wem::ProfileMaterialSet& set : document.models.front().profileSets) {
        if (set.profile != wem::ProfileId::Wc3Classic) {
            continue;
        }
        for (const wem::Material& material : set.materials) {
            const wem::CompositeBody* body = material.Common().composite();
            if (body == nullptr) {
                continue;
            }
            std::vector<const wem::CompositeLayer*> passes;
            for (const wem::CompositeLayer& layer : body->layers) {
                if (layer.target == wem::SurfaceChannel::Color) {
                    passes.push_back(&layer);
                }
            }
            for (std::size_t i = 0; i < passes.size(); ++i) {
                if (!isPlate(passes[i]->input)) {
                    continue;
                }
                const wem::CompositeLayer* next = i + 1 < passes.size() ? passes[i + 1] : nullptr;
                const bool covered =
                    next != nullptr && !isPlate(next->input) &&
                    (next->op == wem::CompositeOp::AlphaKey ||
                     next->op == wem::CompositeOp::AlphaBlend ||
                     next->op == wem::CompositeOp::Modulate ||
                     next->op == wem::CompositeOp::Modulate2x ||
                     next->op == wem::CompositeOp::Add || next->op == wem::CompositeOp::AddAlpha);
                // A later plate at less than full weight is a carrier too.
                const bool later = i > 0 && passes[i]->input.weight < 0.999f;
                if (!covered && !later) {
                    return true;
                }
            }
        }
    }
    return false;
}

/// The Classic arm's texture work: every replaceable the game resolves gets
/// its path (and the team plate its stock, where a material needs one), every
/// texture its alpha class, and -- when asked -- a keyed pass over another
/// pass a binary alpha.
void RestateClassicTextures(wem::Document& document, io::IContentProvider* provider,
                            const Wc3ToSc2Options& options, Wc3ToSc2Result& result,
                            wem::Diagnostics& out) {
    // Replaceable 2 names a file the GAME supplies
    // (`ReplaceableTextures\TeamGlow\TeamGlow00.blp`); giving the reference
    // that path lets the texture pass export it like any other, while the
    // replaceable id stays for the material fold to read. Replaceable 1 gets
    // the alpha-0 stock, so a plate with nothing textured over it is a solid
    // team colour (design §5.2 R3); 11 and 31–37 resolve through the tileset
    // the way the renderer resolves them.
    bool tablesLoaded = false;
    int resolved = 0;
    const bool plateAlone = TeamPlateDrawsAlone(document);
    for (std::size_t i = 0; i < document.textures.size(); ++i) {
        wem::TextureRef& ref = document.textures[i];
        if (ref.replaceableId == 0 || !ref.path.empty()) {
            continue;
        }
        std::string path;
        if (ref.replaceableId == 2) {
            path = "ReplaceableTextures/TeamGlow/TeamGlow00.blp";
        } else if (ref.replaceableId == 1) {
            if (!plateAlone) {
                continue; // never sampled: the RGBA select reads the texture over it
            }
            path = "Star2/war3_TeamColor00.dds";
            result.baked.insert_or_assign(static_cast<u32>(i), StockTeamTexture());
        } else if (ref.replaceableId == 11 ||
                   (ref.replaceableId >= 31 && ref.replaceableId <= 37)) {
            if (!tablesLoaded && provider != nullptr) {
                LoadGameDataFiles(provider);
                tablesLoaded = true;
            }
            path = ReplaceableCanonicalPath(static_cast<i32>(ref.replaceableId), options.tileset);
        }
        if (path.empty()) {
            continue;
        }
        ref.path = path;
        ref.key = wem::TexturePath{path};
        ++resolved;
    }
    if (resolved != 0) {
        out.info(wem::DiagCode::LossyKindConversion,
                 std::to_string(resolved) + " replaceable texture(s) resolved to the file the "
                                            "game would supply (tileset " +
                     std::string(io::TilesetName(options.tileset)) + ")",
                 wem::ElementRef(wem::ElementKind::Document, 0), wem::ProfileId::Sc2);
    }
    if (provider == nullptr || document.models.empty()) {
        return;
    }

    ExportTextureCache cache(document, provider);
    const auto classOf = [&](u32 index) -> u8 {
        if (index >= document.textures.size() || document.textures[index].replaceableId != 0) {
            return 0;
        }
        const tx::Texture* texture = cache.Decode(PathOfTexture(document, index));
        return texture != nullptr ? AlphaClassOf(*texture) : u8{0};
    };

    // A keyed pass over another pass: the fold lerps by its alpha, and a
    // gradient alpha softens what Warcraft III cut at 0.75. Baked binary, the
    // lerp IS the key.
    if (options.sharpenTeamKey) {
        wem::Model& model = document.models.front();
        std::map<u32, u32> sharpened; // source texture -> baked texture
        for (wem::ProfileMaterialSet& set : model.profileSets) {
            if (set.profile != wem::ProfileId::Wc3Classic) {
                continue;
            }
            for (wem::Material& material : set.materials) {
                const wem::CompositeBody* peek = material.Common().composite();
                if (peek == nullptr) {
                    continue;
                }
                bool anyKeyed = false;
                for (std::size_t i = 1; i < peek->layers.size(); ++i) {
                    anyKeyed = anyKeyed || (peek->layers[i].target == wem::SurfaceChannel::Color &&
                                            peek->layers[i].op == wem::CompositeOp::AlphaKey &&
                                            peek->layers[i].input.hasTexture());
                }
                if (!anyKeyed) {
                    continue; // `MutableCommon` would mark the native block edited for nothing
                }
                wem::CompositeBody* body = material.MutableCommon().composite();
                for (std::size_t i = 1; i < body->layers.size(); ++i) {
                    wem::CompositeLayer& layer = body->layers[i];
                    if (layer.target != wem::SurfaceChannel::Color ||
                        layer.op != wem::CompositeOp::AlphaKey || !layer.input.hasTexture()) {
                        continue;
                    }
                    const u32 source = layer.input.texture;
                    if (sharpened.count(source) == 0) {
                        if (classOf(source) != 3) {
                            continue; // opaque or already keyed: nothing to sharpen
                        }
                        const tx::Texture* decoded =
                            cache.Decode(PathOfTexture(document, source));
                        if (decoded == nullptr) {
                            continue;
                        }
                        tx::Texture keyed = decoded->copyAsFormat(tx::PixelFormat::RGBA8);
                        const std::span<u8> px = keyed.mipData(0);
                        for (std::size_t k = 3; k < px.size(); k += 4) {
                            px[k] = px[k] >= 192 ? u8{255} : u8{0};
                        }
                        const u32 baked = cache.Intern(
                            StemOf(PathOfTexture(document, source)) + "_key.dds");
                        result.baked.insert_or_assign(baked,
                                                      BakedTexture{std::move(keyed),
                                                                   tx::PixelFormat::BC3});
                        sharpened.emplace(source, baked);
                        ++result.texturesSharpened;
                    }
                    layer.input.texture = sharpened[source];
                }
            }
        }
    }

    if (!options.classifyTextures) {
        return;
    }
    result.textureAlphaClasses.assign(document.textures.size(), 0);
    for (std::size_t i = 0; i < document.textures.size(); ++i) {
        if (result.baked.count(static_cast<u32>(i)) != 0) {
            // A baked map is what the bake made it: keyed, or the plate.
            result.textureAlphaClasses[i] = document.textures[i].replaceableId == 1 ? 0 : 2;
            continue;
        }
        result.textureAlphaClasses[i] = classOf(static_cast<u32>(i));
    }
}

} // namespace

Wc3ToSc2Result RestateWc3AsSc2(wem::Document& document, io::IContentProvider* provider,
                               wem::Diagnostics& diagnostics, const Wc3ToSc2Options& options) {
    Wc3ToSc2Result result;
    if (document.defaultProfile != wem::ProfileId::Wc3Classic &&
        document.defaultProfile != wem::ProfileId::Wc3Reforged) {
        return result;
    }

    int renamed = 0;
    for (wem::Clip& clip : document.clips) {
        if (RenameSequence(clip.name)) {
            ++renamed;
        }
    }

    RestateClassicTextures(document, provider, options, result, diagnostics);
    RestateReforgedMaterials(document, provider, result, diagnostics);

    if (renamed != 0) {
        diagnostics.info(wem::DiagCode::LossyKindConversion,
                         std::to_string(renamed) +
                             " sequence(s) renamed to the StarCraft II convention "
                             "('Stand - 1' -> 'Stand 01')",
                         wem::ElementRef(wem::ElementKind::Document, 0),
                         wem::ProfileId::Sc2);
    }
    return result;
}

} // namespace whiteout::flakes
