// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "wc3_to_sc2_export.h"
#include "wc3_attachment_names.h"

#include "export_texture_cache.h"
#include "renderer/ibl/env_probe.h"
#include "whiteout/flakes/util/team_glow_data.h"

#include <whiteout/models/wem/retarget.h>
#include <whiteout/textures/environment_map.h>
#include <whiteout/textures/pbr_bake.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace whiteout::flakes {

namespace {

namespace tx = ::whiteout::textures;

/// The sequence's real name, with the `.mdx` comment and the variant number
/// taken off it.
///
/// `-` opens a comment in a Warcraft III sequence name and everything after it
/// is the author's note, so `Stand - 1` is the sequence `Stand`; Reforged
/// writes the same variant as `Stand 1`. Either way the trailing number is a
/// variant index, not part of the name — `Stand Victory` and `Decay Flesh`
/// keep theirs, and StarCraft II uses `Decay` itself.
std::string BaseSequenceName(std::string name) {
    name = name.substr(0, name.find('-'));
    const auto space = [&name](std::size_t i) {
        return std::isspace(static_cast<unsigned char>(name[i])) != 0;
    };
    std::size_t end = name.size();
    while (end > 0 && space(end - 1)) {
        --end;
    }
    std::size_t digits = end;
    while (digits > 0 && std::isdigit(static_cast<unsigned char>(name[digits - 1]))) {
        --digits;
    }
    if (digits < end && digits > 0 && space(digits - 1)) {
        end = digits;
        while (end > 0 && space(end - 1)) {
            --end;
        }
    }
    std::size_t begin = 0;
    while (begin < end && space(begin)) {
        ++begin;
    }
    return name.substr(begin, end - begin);
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

    // The reflection every HD material names in its sixth slot, resolved once
    // per texture. Reforged never samples that file: the engine binds the
    // tileset's pre-filtered probe (`ps_ibl.slang` sampleIBL, the specular slice
    // at `roughness * mipEnd`), the one the viewer's HD pass loads, so the probe
    // crosses with every level kept -- its chain IS the blur Simulate Roughness
    // reads down by `(1 - gloss) * range`. The photo panorama the slot names put
    // blue sky and grass on the footman's steel. Without the probe the slot's
    // own map stands in: a 2:1 panorama projected into a cube (`tx::env`), a
    // cube passed through, anything else read as a sphere map.
    struct EnvSource {
        u32 texture = wem::kInvalidIndex;
        wem::UVMappingMode mapping = wem::UVMappingMode::EnvCube;
    };
    std::map<u32, EnvSource> environments;
    int environmentsProjected = 0;
    int probesCrossed = 0;
    const auto environmentOf = [&](const wem::TextureInput* slot, std::size_t material) {
        EnvSource source;
        if (slot == nullptr) {
            return source;
        }
        if (const auto known = environments.find(slot->texture); known != environments.end()) {
            return known->second;
        }
        if (const tx::Texture* probe = cache.Decode(renderer::ibl::kDayIblPath);
            probe != nullptr && (probe->type() == tx::TextureType::TextureCubeArray ||
                                 probe->type() == tx::TextureType::TextureCube)) {
            // Slice 1 is the specular chain (slice 0 the irradiance), read at the
            // world direction's `.xzy * (1, 1, -1)`; StarCraft II looks a
            // reflection up by the z-up direction itself.
            constexpr std::array<f32, 9> kProbeFromWorld = {1, 0, 0, 0, 0, 1, 0, -1, 0};
            std::optional<tx::Texture> cube =
                tx::env::CubeFromCube(*probe, probe->arraySize() > 1 ? 1u : 0u, kProbeFromWorld,
                                      renderer::ibl::kBlizzardProbeFaceOrder);
            if (cube.has_value()) {
                result.baked.insert_or_assign(slot->texture,
                                              BakedTexture{std::move(*cube), tx::PixelFormat::BC1});
                source = {slot->texture, wem::UVMappingMode::EnvCube};
                ++probesCrossed;
                environments.emplace(slot->texture, source);
                return source;
            }
        }
        const std::string path = PathOfTexture(document, slot->texture);
        const tx::Texture* decoded = cache.Decode(path);
        if (decoded == nullptr) {
            out.warn(wem::DiagCode::TextureUnresolved,
                     "the environment map '" + path + "' would not decode; nothing reflects",
                     wem::ElementRef(wem::ElementKind::Material, material), wem::ProfileId::Sc2);
        } else if (decoded->type() == tx::TextureType::TextureCube) {
            source = {slot->texture, wem::UVMappingMode::EnvCube};
        } else if (decoded->width() == 2 * decoded->height()) {
            if (std::optional<tx::Texture> cube = tx::env::CubeFromPanorama(*decoded, 256)) {
                result.baked.insert_or_assign(slot->texture,
                                              BakedTexture{std::move(*cube), tx::PixelFormat::BC1});
                source = {slot->texture, wem::UVMappingMode::EnvCube};
                ++environmentsProjected;
            }
        } else {
            source = {slot->texture, wem::UVMappingMode::EnvSphere};
        }
        environments.emplace(slot->texture, source);
        return source;
    };

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
        const wem::TextureInput* envSlot = SlotOf(*pbr, wem::PbrSlot::Environment);
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
        // roughness medians, and both baked planes.
        bool anyTeam = false;
        bool anyMetal = false;
        std::vector<f32> roughness;
        std::vector<f32> metalRoughness;
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
                    if (o[2] > 0.5f) {
                        metalRoughness.push_back(o[1]);
                    }
                }

                f32 albedo[3];
                for (int c = 0; c < 3; ++c) {
                    albedo[c] = basePx[at + static_cast<std::size_t>(c)] / 255.0f;
                    // `spec = metalness * albedo` — Reforged's F0 exactly
                    // (`reference_reforged_metalness_is_specular`) — dimmed by
                    // the team share. Reforged tints the albedo before taking
                    // its F0, so a team plate reflects in the TEAM colour;
                    // StarCraft II's reflection is untinted, and a grey one
                    // washed the footman's shield to grey-green. The
                    // highlight loses nothing: retail's team specular replaces
                    // it with the team colour wherever the team shows.
                    specPx[at + static_cast<std::size_t>(c)] = static_cast<u8>(
                        std::clamp(o[2] * albedo[c] * (1.0f - share), 0.0f, 1.0f) * 255.0f +
                        0.5f);
                }
                // The diffuse is what the metal leaves: Reforged lights a metal
                // only through its reflection, and the whole base colour kept as
                // the diffuse lit the footman's steel twice, washed silver. The
                // team's share of the metal is the exception: its reflection is
                // dimmed out above, so it stays in the diffuse for the team
                // select to colour. Split off too, the banshee's red armbands,
                // belt and skirt panels (metal 0.5-0.9) kept 13% of the team.
                f32 diffuse[3];
                tx::pbr::DiffuseFromMetalness(albedo, o[2] * (1.0f - share), true, diffuse);
                anyMetal = anyMetal || o[2] >= 1.0f / 255.0f;
                // Reforged BLENDS the team hue in and keeps the art's
                // brightness; StarCraft II REPLACES the texel where the
                // diffuse alpha is low. Solving the one for the other makes
                // the share the texel's brightness, so the shading painted
                // under the mask comes through as shades of the team colour
                // instead of one flat plate (`tx::pbr::TeamReplaceFromBlend`).
                f32 replaced[3];
                const f32 alpha = tx::pbr::TeamReplaceFromBlend(diffuse, share, true, replaced);
                for (int c = 0; c < 3; ++c) {
                    teamPx[at + static_cast<std::size_t>(c)] =
                        static_cast<u8>(std::clamp(replaced[c], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
                teamPx[at + 3] = static_cast<u8>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
                // The gloss rides the spec map's alpha, where StarCraft II's
                // own roughness-simulating materials keep it.
                specPx[at + 3] =
                    static_cast<u8>(tx::pbr::GlossFromRoughness(o[1]) * 255.0f + 0.5f);
            }
        }

        // The roughness median of the METAL texels -- the only ones with a
        // specular to shape; the whole map when nothing is metal -- fixes the
        // exponent: the gloss layer scales the material's down per texel, so
        // the material's is the ceiling that lands the median on its own
        // GGX-matched width.
        std::vector<f32>& shaping =
            metalRoughness.size() * 100 >= roughness.size() ? metalRoughness : roughness;
        f32 medianRoughness = 0.55f; // exponent 20
        if (!shaping.empty()) {
            const std::size_t mid = shaping.size() / 2;
            std::nth_element(shaping.begin(), shaping.begin() + mid, shaping.end());
            medianRoughness = shaping[mid];
        }
        wem::CompositeBody body;
        body.specularExponent = tx::pbr::GlossCeilingExponent(medianRoughness);
        // StarCraft II's own roughness-simulating materials park 2 here -- the
        // StarTools guide's "~2", none of 637 shipped above 10. The exponent-
        // normalised `(n + 2) / 8` this was put the footman's pauldrons at 11.9
        // and its shield at 20.3, and the editor blew both out to flat yellow.
        body.specularFactor = Vector4f{2.0f, 0, 0, 1};
        // The gloss is a roughness: no fake energy dim, and the reflection
        // blurs by it (the StarTools "Simulate Roughness" guide).
        body.simulateRoughness = true;

        const std::string stem = StemOf(basePath);
        u32 coverageTexture = baseSlot->texture;
        if (anyTeam) {
            const u32 teamTexture = cache.Intern(stem + "_team.dds");
            result.baked.insert_or_assign(teamTexture,
                                          BakedTexture{std::move(teamDiffuse),
                                                       tx::PixelFormat::BC3});
            body.layers.push_back(
                LayerOf(teamUnderRef(), wem::SurfaceChannel::Color, wem::CompositeOp::Set));
            body.layers.push_back(LayerOf(teamTexture, wem::SurfaceChannel::Color,
                                          wem::CompositeOp::AlphaBlend, baseSlot));
        } else if (anyMetal) {
            // No team, but the metal still leaves its diffuse; the base's own
            // alpha rides along so the coverage can name this map, not a copy
            // of the source.
            for (std::size_t at = 3; at < teamPx.size(); at += 4) {
                teamPx[at] = basePx[at];
            }
            const u32 diffuseTexture = cache.Intern(stem + "_diff.dds");
            coverageTexture = diffuseTexture;
            result.baked.insert_or_assign(diffuseTexture,
                                          BakedTexture{std::move(teamDiffuse),
                                                       tx::PixelFormat::BC3});
            body.layers.push_back(LayerOf(diffuseTexture, wem::SurfaceChannel::Color,
                                          wem::CompositeOp::Set, baseSlot));
        } else {
            body.layers.push_back(LayerOf(baseSlot->texture, wem::SurfaceChannel::Color,
                                          wem::CompositeOp::Set, baseSlot));
        }

        const u32 specTexture = cache.Intern(stem + "_spec.dds");
        result.baked.insert_or_assign(specTexture,
                                      BakedTexture{std::move(specular), tx::PixelFormat::BC3});
        body.layers.push_back(
            LayerOf(specTexture, wem::SurfaceChannel::Specular, wem::CompositeOp::Set, baseSlot));
        body.layers.push_back(
            LayerOf(specTexture, wem::SurfaceChannel::Gloss, wem::CompositeOp::Set, baseSlot));

        // The reflection: the environment as a cube, masked by the F0 map.
        // That is the StarTools recipe (the spec map doubles as the RGB envio
        // mask, gloss in its alpha) and what 1,219 of the 1,523 shipped
        // roughness-simulating env materials do; the mask being the
        // reflectance, a dielectric (F0 = 0 in Reforged) reflects nothing and
        // a coloured metal reflects in its own colour, as Reforged's
        // `F0 * prefiltered` does.
        if (const EnvSource env = environmentOf(envSlot, m); env.texture != wem::kInvalidIndex) {
            wem::CompositeLayer reflection =
                LayerOf(env.texture, wem::SurfaceChannel::Environment, wem::CompositeOp::Add);
            reflection.input.mapping = env.mapping;
            body.layers.push_back(reflection);
            body.layers.push_back(LayerOf(specTexture, wem::SurfaceChannel::Environment,
                                          wem::CompositeOp::Modulate, baseSlot));
            body.environmentFactor = 1.0f;
        }

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
        // layer keys at 0.75), and so does StarCraft II's mask
        // (`psmainshading.fx` tests `alphaFactor * mask`). The team diffuse's
        // alpha is the team select alone: a cutout baked into it read as team
        // wherever filtering met the cut, and the banshee's hair and ragged
        // cloth came out edged in red.
        const wem::BlendMode blend = derived.Common().blend;
        if (blend == wem::BlendMode::AlphaKey || blend == wem::BlendMode::Transparent ||
            blend == wem::BlendMode::AlphaBlend || blend == wem::BlendMode::AdditiveAlpha) {
            body.layers.push_back(LayerOf(coverageTexture, wem::SurfaceChannel::Coverage,
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
                     std::to_string(result.materialsSkipped) + " kept the derive's diffuse; " +
                     std::to_string(probesCrossed) + " reflection(s) from the tileset probe, " +
                     std::to_string(environmentsProjected) + " from a projected panorama",
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

/// The hero glow, synthesized rather than read: Warcraft III's own
/// `TeamGlow00.blp` is 32x32, which is the whole glow's resolution on screen.
/// The fold selects the layer's RED (`m3_core.cpp`, R4), so the ramp goes on
/// the RGB and the alpha stays 255 -- an Add-family emissive is `rgb * a`, and
/// a shaped alpha would square it. Blizzard's conversions name it
/// `war3_TeamGlow00.dds`.
BakedTexture StockTeamGlowTexture() {
    i32 w = 0;
    i32 h = 0;
    const std::vector<u8> glow = io::DecodeTeamGlow(255, 255, 255, w, h);
    tx::Texture texture = tx::Texture::create2D(tx::PixelFormat::RGBA8, static_cast<u32>(w),
                                                static_cast<u32>(h), 1);
    const std::span<u8> px = texture.mipData(0);
    std::copy_n(glow.begin(), std::min(px.size(), glow.size()), px.begin());
    // Uncompressed, alone among the bakes: BC1/BC3 keep RGB in a colour block
    // whose endpoints hold five bits of red, and the fold reads THE RED, so a
    // ramp this shallow came out in 8-level rings (measured 16/8/0 in the
    // tail, the 5-bit grid exactly). A 512-square glow costs 1 MB.
    return BakedTexture{std::move(texture), tx::PixelFormat::RGBA8};
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
    // Replaceable 2 is the hero glow the GAME supplies, and Warcraft III's own
    // file is 32x32 -- exporting it like any other texture shipped that
    // resolution as the glow. It gets the ramp we synthesize for the renderer
    // instead, at `kTeamGlowSize`, under the name Blizzard's own conversions
    // use; the replaceable id stays for the material fold to read. Replaceable 1 gets
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
            path = "Star2/war3_TeamGlow00.dds";
            result.baked.insert_or_assign(static_cast<u32>(i), StockTeamGlowTexture());
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

/// The references StarCraft II's gameplay reads that Warcraft III models do not
/// state (WC3_TO_SC2_COMPLETION_PLAN.md C3.2). Each is a root helper -- a bone
/// wherever `toM3` meets one -- with the attachment on it: `Ref_Origin` at the
/// origin (523 of 543 paired models that lack one), `Ref_OverHead` at 62 units
/// up (the default Blizzard's tool leaves; no source quantity predicts the
/// placed ones), `Vol_Target` around the collision shapes with `Ref_Target` on
/// it (1,069 of 1,070), and `Ref_Center` at the volume's height (998 of 1,030).
void AddStandardRefs(wem::Model& model, Wc3ToSc2Result& result, wem::Diagnostics& diagnostics) {
    const auto named = [&model](std::string_view wanted) {
        for (const wem::Node& node : model.nodes.nodes) {
            if (node.kind != wem::NodeKind::Attachment || node.name.size() != wanted.size()) {
                continue;
            }
            bool same = true;
            for (std::size_t i = 0; i < wanted.size() && same; ++i) {
                same = std::tolower(static_cast<unsigned char>(node.name[i])) ==
                       std::tolower(static_cast<unsigned char>(wanted[i]));
            }
            if (same) {
                return true;
            }
        }
        return false;
    };
    // Pivot-relative like the rest of the import: the rest is the pivot.
    const auto add = [&model](const std::string& name, wem::NodeKind kind, u32 parent,
                              const Vector3f& at) {
        wem::Node node;
        node.name = name;
        node.kind = kind;
        node.resetPayloadForKind();
        node.parent = parent;
        node.pivot = at;
        node.local.translation =
            parent == wem::kInvalidNode ? at : Vector3f{0.0f, 0.0f, 0.0f};
        node.poses.push_back(node.local);
        return model.nodes.add(std::move(node));
    };
    const auto point = [&](const std::string& bone, const std::string& attachment,
                           const Vector3f& at) {
        const u32 helper = add(bone, wem::NodeKind::Helper, wem::kInvalidNode, at);
        add(attachment, wem::NodeKind::Attachment, helper, at);
    };

    std::string added;
    if (!named("Ref_Origin")) {
        point("Ref_Origin", "Ref_Origin", Vector3f{0.0f, 0.0f, 0.0f});
        added += " Ref_Origin";
    }
    if (!named("Ref_Overhead")) {
        point("Ref_OverHead", "Ref_Overhead", Vector3f{0.0f, 0.0f, 62.0f});
        added += " Ref_Overhead";
    }
    const Wc3TargetVolume volume = Wc3TargetVolumeOf(model);
    const u32 target = add("Vol_Target", wem::NodeKind::Helper, wem::kInvalidNode, volume.center);
    result.volTargetNode = target;
    result.volTargetScale = volume.scale;
    if (!named("Ref_Target")) {
        add("Ref_Target", wem::NodeKind::Attachment, target, volume.center);
        added += " Ref_Target";
    }
    if (!named("Ref_Center")) {
        point("Ref_Center", "Ref_Center", Vector3f{0.0f, 0.0f, volume.center.z});
        added += " Ref_Center";
    }
    diagnostics.info(wem::DiagCode::LossyKindConversion,
                     "added StarCraft II's targeting volume" +
                         std::string(volume.fromCollision ? " around the collision shapes"
                                                          : " at its default") +
                         (added.empty() ? std::string() : " and" + added),
                     wem::ElementRef(wem::ElementKind::Node, target), wem::ProfileId::Sc2);
}

} // namespace

Wc3ToSc2Result RestateWc3AsSc2(wem::Document& document, io::IContentProvider* provider,
                               wem::Diagnostics& diagnostics, const Wc3ToSc2Options& options) {
    Wc3ToSc2Result result;
    if (document.defaultProfile != wem::ProfileId::Wc3Classic &&
        document.defaultProfile != wem::ProfileId::Wc3Reforged) {
        return result;
    }

    // Warcraft III's variant numbering is neither contiguous nor ordered — this
    // footman ships Stand 1, 2, 4 and 6 — and StarCraft II's is both. So the
    // clips are regrouped under their real names and renumbered: a lone variant
    // carries the bare name (2,418 of the 2,462 single-variant groups in the
    // shipped corpus do), and several carry `<name> <NN>` counting from 01,
    // always two digits (571 of 571). Least rare first, because `Rarity` is
    // what decides how often Warcraft III picks one.
    int renamed = 0;
    std::vector<std::string> order;
    std::map<std::string, std::vector<std::size_t>> variants;
    for (std::size_t i = 0; i < document.clips.size(); ++i) {
        std::string base = BaseSequenceName(document.clips[i].name);
        if (base.empty()) {
            continue;
        }
        const auto placed = variants.try_emplace(base);
        if (placed.second) {
            order.push_back(base);
        }
        placed.first->second.push_back(i);
    }
    for (const std::string& base : order) {
        std::vector<std::size_t>& group = variants[base];
        std::stable_sort(group.begin(), group.end(), [&document](std::size_t a, std::size_t b) {
            return document.clips[a].native.value("rarity", 0) <
                   document.clips[b].native.value("rarity", 0);
        });
        for (std::size_t k = 0; k < group.size(); ++k) {
            std::string name = base;
            if (group.size() > 1) {
                const std::string digits = std::to_string(k + 1);
                name += digits.size() < 2 ? " 0" + digits : " " + digits;
            }
            if (document.clips[group[k]].name != name) {
                document.clips[group[k]].name = std::move(name);
                ++renamed;
            }
        }
    }

    // Attachment points take StarCraft II's names (WC3_TO_SC2_COMPLETION_PLAN.md
    // §4.1). On the WEM node, so the ATT_ and the bone that carries it agree,
    // as they do in Blizzard's own conversions.
    if (options.attachmentNames) {
        for (wem::Model& model : document.models) {
            std::map<std::string, u32> seen;
            for (u32 n = 0; n < model.nodes.size(); ++n) {
                wem::Node& node = model.nodes.nodes[n];
                if (node.kind != wem::NodeKind::Attachment) {
                    continue;
                }
                const Wc3AttachmentName renamed = Wc3AttachmentNameToSc2(node.name);
                if (!renamed.known) {
                    diagnostics.info(wem::DiagCode::LossyKindConversion,
                                     "attachment '" + node.name + "' has no StarCraft II name of "
                                         "its own; written as '" + renamed.name + "'",
                                     wem::ElementRef(wem::ElementKind::Node, n),
                                     wem::ProfileId::Sc2);
                }
                if (++seen[renamed.name] == 2) {
                    diagnostics.info(wem::DiagCode::LossyKindConversion,
                                     "more than one attachment is named '" + renamed.name +
                                         "'; Warcraft III allows it and so does the file",
                                     wem::ElementRef(wem::ElementKind::Node, n),
                                     wem::ProfileId::Sc2);
                }
                node.name = renamed.name;
            }
        }
    }

    if (options.standardRefs && !document.models.empty()) {
        AddStandardRefs(document.models.front(), result, diagnostics);
    }

    RestateClassicTextures(document, provider, options, result, diagnostics);
    RestateReforgedMaterials(document, provider, result, diagnostics);

    if (renamed != 0) {
        diagnostics.info(wem::DiagCode::LossyKindConversion,
                         std::to_string(renamed) +
                             " sequence(s) renamed to the StarCraft II convention "
                             "(a lone variant takes the bare name, several count from 01 "
                             "least-rare-first)",
                         wem::ElementRef(wem::ElementKind::Document, 0),
                         wem::ProfileId::Sc2);
    }
    return result;
}

} // namespace whiteout::flakes
