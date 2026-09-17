// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "m3_export.h"

#include "color_math.h"
#include "export_text.h"
#include "export_texture_set.h"
#include "texture_codec.h"
#include "texture_io.h"
#include "wc3_to_sc2_export.h"

#include "io/d3/d3_types.h"
#include "io/mdx_model_adapter.h"
#include "io/wem/wem_export.h"
#include "io/storage/casc_source.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <whiteout/models/cross/mdx_m3_effects.h>
#include <whiteout/models/m3/writer.h>
#include <whiteout/models/mdx/parser.h>
#include <whiteout/textures/texture.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_set>
#include <variant>
#include <vector>

namespace whiteout::flakes {

namespace {

namespace fs = std::filesystem;
namespace tx = ::whiteout::textures;
namespace wem = ::whiteout::models::wem;

/// Where a `.m3` names its textures. Every one of 9,397 texture paths across
/// 900 shipped models is `Assets/Textures/<file>` -- three segments, never
/// deeper and never bare -- because the path is resolved against the mod root,
/// not against the model. Writing the bare file name, as this did, resolved to
/// nothing: every layer fell back to the engine's null texture, and a diffuse
/// that reads back black with a zero alpha is a whole model painted in its
/// team colour. It doubles as the directory the files are written into.
constexpr const char* kTextureDir = "Assets/Textures/";

/// Where a spawned model is written, as the model particle names it: the
/// `Assets/<category>/<file>` shape shipped `PAR_` model paths take, beside
/// the textures' `Assets/Textures/`.
constexpr const char* kSpawnedModelDir = "Assets/Models/";
/// A spawned model's own spawns are followed this deep, and no further.
constexpr int kMaxSpawnDepth = 2;

/// Where StarCraft II's War3 (Mod) keeps the Warcraft III textures it ships,
/// each flattened to `war3_<name>.dds`.
constexpr const char* kWar3ModTextureDir = "mods/war3.sc2mod/base.sc2assets/assets/textures/";

/// The path the team-glow mask is named by in the written model.
constexpr const char* kTeamGlowMaskPath = "ReplaceableTextures/TeamGlow/TeamGlowMask.blp";

/// What one export chain shares: the model asked for, and every model its
/// emitters spawn, however deep.
struct M3ExportChain {
    /// The directory every written model's `Assets/...` paths resolve under:
    /// the first model's own folder.
    fs::path assetRoot;
    /// The models written for model-spawning emitters, by lower-cased source
    /// path, so a model two emitters spawn is written once. Held empty while
    /// one is written, so a model that spawns itself stops.
    std::map<std::string, std::string> spawnedModels;
};

M3ExportReport ExportInChain(const M3ExportRequest& request, M3ExportChain& chain, int depth);

/// The document texture @p ref, read and decoded as RGBA8 for a bake's pixel
/// loop. Nothing when it has no name or will not decode.
std::optional<tx::Texture> DecodeRgba8(io::IContentProvider& provider, const wem::TextureRef& ref) {
    const std::string key = ref.path.empty() ? TextureIdKey(ref) : ref.path;
    if (key.empty())
        return std::nullopt;
    TextureDecode decoded = ReadDecodedTexture(provider, ContentRefForKey(key));
    if (decoded.texture)
        decoded.texture->format(tx::PixelFormat::RGBA8);
    return std::move(decoded.texture);
}

/// The file stem a bake of document texture @p index is named after.
std::string BakeStem(const wem::Document& document, u32 index) {
    std::string stem = document.textures[index].path;
    if (!stem.empty())
        stem = io::PathToUtf8(io::FsPathFromUtf8(export_text::ForwardSlashes(stem)).stem());
    if (stem.empty())
        stem = "texture_" + std::to_string(index);
    return stem;
}

/// Append a texture the export made to @p document as @p path, with its
/// pixels in @p baked as BC3. Returns its document index.
u32 AddBakedTexture(wem::Document& document, std::map<u32, BakedTexture>& baked,
                    std::string path, tx::Texture pixels, const BakedChannels& channels) {
    const u32 index = static_cast<u32>(document.textures.size());
    wem::TextureRef ref;
    ref.path = std::move(path);
    ref.key = wem::TexturePath{ref.path};
    document.textures.push_back(std::move(ref));
    baked.insert_or_assign(index, BakedTexture(std::move(pixels), tx::PixelFormat::BC3, channels));
    return index;
}

// ---------------------------------------------------------------------------
// Masked env folds
// ---------------------------------------------------------------------------

/// The directional mean an env sample is approximated by, times @p gain: the
/// central half-disk, not the full area. A sphere map's lookup lands near the
/// CENTRE for anything facing the viewer (reflect(view, n) ≈ view there), so
/// the rim — half the area, and authored dark on every sheen sprite — barely
/// gets sampled. The tauren's dragonarmorspec: area mean 0.39 linear, centre
/// disk 0.65 — the difference between dimming the plates and the brightened
/// gold the native render shows.
std::array<f32, 3> CentreDiskMean(const tx::Texture& env, f32 gain) {
    constexpr f32 kHalfDiskRadiusSquared = 0.25f;
    const std::span<const u8> px = env.mipData(0);
    const std::size_t w = env.width(), h = env.height();
    double sum[3] = {0, 0, 0};
    std::size_t count = 0;
    for (std::size_t y = 0; y < h; ++y) {
        const f32 dy = (static_cast<f32>(y) - h * 0.5f) / (h * 0.5f);
        for (std::size_t x = 0; x < w; ++x) {
            const f32 dx = (static_cast<f32>(x) - w * 0.5f) / (w * 0.5f);
            if (dx * dx + dy * dy > kHalfDiskRadiusSquared)
                continue;
            const std::size_t p = (y * w + x) * 4;
            for (int c = 0; c < 3; ++c)
                sum[c] += color::SrgbToLinear(px[p + static_cast<std::size_t>(c)] / 255.0f);
            ++count;
        }
    }
    std::array<f32, 3> k{};
    for (int c = 0; c < 3; ++c)
        k[c] = count > 0 ? gain * static_cast<f32>(sum[c] / static_cast<double>(count)) : gain;
    return k;
}

/// `t0 · lerp(k, 1, t0.a)` in linear light, alpha kept.
tx::Texture FoldEnvIntoBase(const tx::Texture& base, const std::array<f32, 3>& k) {
    tx::Texture out = tx::Texture::create2D(tx::PixelFormat::RGBA8, base.width(), base.height());
    const std::span<const u8> src = base.mipData(0);
    const std::span<u8> dst = out.mipData(0);
    f32 lut[256];
    for (int v = 0; v < 256; ++v)
        lut[v] = color::SrgbToLinear(static_cast<f32>(v) / 255.0f);
    const std::size_t count = src.size() / 4;
    for (std::size_t p = 0; p < count; ++p) {
        const f32 a = src[p * 4 + 3] / 255.0f;
        for (int c = 0; c < 3; ++c) {
            const std::size_t at = p * 4 + static_cast<std::size_t>(c);
            const f32 folded = lut[src[at]] * (a + k[c] * (1.0f - a));
            dst[at] = static_cast<u8>(color::LinearToSrgb(folded) * 255.0f + 0.5f);
        }
        dst[p * 4 + 3] = src[p * 4 + 3];
    }
    return out;
}

/// Bake the `_Alpha` masked-env fold into the diffuse map.
///
/// WoW's `Opaque_Mod2xNA_Alpha` family is `c·t0·lerp(t1·N, 1, t0.a)` — the env
/// sample MODULATES the base where the base map's alpha opens. No M3 env op
/// can spell that: Mod's mask blackens the closed regions, Lerp REPLACES the
/// base with the raw env sample (taurenprimalist's golden plates turned
/// silver), and Add adds it untinted (same wash, brighter). Every dynamic
/// spelling loses the base's own colour, which is what the eye reads.
///
/// So the fold is baked: the env sample is approximated by its directional
/// mean (the flat colour a sphere map averages to over a convex body — the
/// PBR bake's own `MeanLinearRgb` argument), and the diffuse becomes
/// `t0 · lerp(mean(t1)·N, 1, t0.a)` in linear light, written as a NEW texture
/// so the other materials sharing t0 (the tauren's cutout) keep the original.
/// The masked stage then collapses to `Pass` and the diffuse layer points at
/// the baked map. What is lost is the view-dependence of the sheen; what is
/// kept is its colour, which the env-layer spellings all got wrong.
///
/// Runs on the source profile set BEFORE the derive (the derive copies the
/// rewritten chain), and only where both maps decode — a material the
/// provider cannot feed keeps the declarative env+mask crossing instead.
void BakeMaskedFolds(wem::Document& document, io::IContentProvider* provider,
                     std::map<u32, BakedTexture>& baked, M3ExportReport& report) {
    if (provider == nullptr)
        return;

    // (seed texture, env texture, N-in-halves) → the baked document index, so
    // materials sharing one fold bake it once.
    std::map<std::tuple<u32, u32, int>, u32> made;

    const auto isEnv = [](const wem::CombinerStage& stage) {
        return stage.input.mapping == wem::UVMappingMode::EnvSphere ||
               stage.input.mapping == wem::UVMappingMode::EnvCube;
    };
    for (wem::Model& model : document.models) {
        for (wem::ProfileMaterialSet& set : model.profileSets) {
            for (wem::Material& material : set.materials) {
                // `InitCommon`, not `MutableCommon`: the bake restates what
                // the native block already says, and marking the block stale
                // blinded every later native reading — the alpha-chain bake's
                // combine codes and the `_pma` mode both sit there.
                wem::CombinersBody* body = material.InitCommon().combiners();
                if (body == nullptr)
                    continue;
                std::size_t seed = body->stages.size();
                for (std::size_t i = 0; i < body->stages.size(); ++i) {
                    if (!isEnv(body->stages[i])) {
                        seed = i;
                        break;
                    }
                }
                if (seed >= body->stages.size() || !body->stages[seed].input.hasTexture())
                    continue;
                wem::CombinerStage& seedStage = body->stages[seed];

                for (wem::CombinerStage& stage : body->stages) {
                    const bool masked = stage.rgb == wem::CombinerOp::MaskedMod ||
                                        stage.rgb == wem::CombinerOp::MaskedMod2x;
                    if (!masked || !isEnv(stage) || !stage.input.hasTexture())
                        continue;
                    const f32 n = stage.rgb == wem::CombinerOp::MaskedMod2x ? 2.0f : 1.0f;
                    const std::tuple<u32, u32, int> foldKey{seedStage.input.texture,
                                                            stage.input.texture,
                                                            static_cast<int>(n * 2.0f)};
                    if (const auto hit = made.find(foldKey); hit != made.end()) {
                        seedStage.input.texture = hit->second;
                        stage.rgb = wem::CombinerOp::Pass;
                        continue;
                    }
                    if (seedStage.input.texture >= document.textures.size() ||
                        stage.input.texture >= document.textures.size())
                        continue;

                    std::optional<tx::Texture> base =
                        DecodeRgba8(*provider, document.textures[seedStage.input.texture]);
                    std::optional<tx::Texture> env =
                        DecodeRgba8(*provider, document.textures[stage.input.texture]);
                    if (!base || !env) {
                        // The declarative env+mask crossing stays; say so once.
                        report.diagnostics.info(
                            wem::DiagCode::LossyKindConversion,
                            "a masked env fold could not bake (texture unreadable); the env "
                            "layer approximation stands");
                        continue;
                    }

                    const u32 index = AddBakedTexture(
                        document, baked, BakeStem(document, seedStage.input.texture) + "_fold",
                        FoldEnvIntoBase(*base, CentreDiskMean(*env, n)),
                        // Written through `toSrgb`; the alpha is the base's own.
                        {tx::TextureKind::Diffuse, tx::TextureKind::TransparencyMask, true});
                    made.emplace(foldKey, index);
                    seedStage.input.texture = index;
                    stage.rgb = wem::CombinerOp::Pass;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Diablo III alpha chains
// ---------------------------------------------------------------------------

/// A `Legacy.fx` combine code is two decimal digits: the tens name the
/// operation and the units its output gain.
constexpr u32 kCombineCodeBase = 10;
/// Tens of a modulate.
constexpr u32 kCombineModulate = 2;
/// Tens of a replace.
constexpr u32 kCombineReplace = 0;

/// The output gain a modulate's units state: x2 for 4 and 6, x4 for 5 and 7.
f32 ModulateGain(u32 units) {
    if (units == 4 || units == 6)
        return 2.0f;
    if (units == 5 || units == 7)
        return 4.0f;
    return 1.0f;
}

/// The alpha combine code of every live chain stage of @p pass, in chain order:
/// the K-th surviving stage IS the K-th stage of the WEM chain, by the same walk
/// d3_core's import does. Zero for a stage that states only a colour op.
std::vector<u32> AlphaCombineCodes(const wem::native::D3RenderState& pass) {
    const auto tagOf = [&pass](u32 id) -> const wem::native::D3ShaderTagValue* {
        for (const wem::native::D3ShaderTagValue& tag : pass.shaderParams) {
            if (tag.tagId == id)
                return &tag;
        }
        return nullptr;
    };
    std::vector<u32> codes;
    for (std::size_t s = 0; s < pass.textureStages.size() && s < io::kD3MaxChainStages; ++s) {
        if (pass.textureStages[s].contentStage == 0)
            continue;
        const auto* rgb = tagOf(io::kD3TagStageColor + static_cast<u32>(s));
        const auto* alpha = tagOf(io::kD3TagStageAlpha + static_cast<u32>(s));
        if (rgb == nullptr && alpha == nullptr)
            continue;
        codes.push_back(alpha != nullptr ? alpha->value : 0u);
    }
    return codes;
}

/// Which stages multiply into a material's alpha, and what the product gains.
struct AlphaChain {
    struct Contributor {
        std::size_t stage;
        bool isStatic;
    };
    std::vector<Contributor> contributors;
    /// Of the contributors, the ones a bake can merge: textured, unmoving.
    std::vector<std::size_t> statics;
    f32 gain = 1.0f;
};

/// Only the modulate family merges; an add or a mid-chain replace does not
/// commute with the product, and the material keeps the declarative crossing.
std::optional<AlphaChain> PlanAlphaChain(const std::vector<u32>& codes,
                                         const wem::CombinersBody& body,
                                         const wem::CommonMaterial& common) {
    AlphaChain chain;
    for (std::size_t i = 0; i < codes.size(); ++i) {
        const u32 code = codes[i];
        if (code == 0)
            continue;
        const u32 op = code / kCombineCodeBase;
        const u32 units = code % kCombineCodeBase;
        // A LEADING replace is a modulate against the neutral head. Anything
        // else — the add, the erosion tail — does not commute with a merge.
        if (!(op == kCombineModulate || (op == kCombineReplace && chain.contributors.empty())))
            return std::nullopt;
        if (op == kCombineModulate)
            chain.gain *= ModulateGain(units);
        const wem::CombinerStage& stage = body.stages[i];
        bool isStatic = stage.input.hasTexture() && stage.input.uvTransform.isIdentity();
        for (const wem::MaterialFeature& feature : common.features) {
            if (feature.layer == static_cast<u32>(i) && feature.uvAnimation() != nullptr)
                isStatic = false;
        }
        chain.contributors.push_back({i, isStatic});
    }
    for (const AlphaChain::Contributor& c : chain.contributors) {
        if (c.isStatic)
            chain.statics.push_back(c.stage);
    }
    return chain;
}

/// The carrier takes the product, every other static contributor becomes the
/// identity, and a dynamic contributor's doubling now lives in the bake.
void RewriteAlphaChain(wem::CombinersBody& body, const AlphaChain& chain, u32 bakedIndex) {
    const std::vector<std::size_t>& statics = chain.statics;
    std::size_t carrier = statics.size();
    for (std::size_t k = 0; k < statics.size(); ++k) {
        if (body.stages[statics[k]].rgb == wem::CombinerOp::Pass) {
            carrier = k;
            break;
        }
    }
    for (std::size_t k = 0; k < statics.size(); ++k) {
        if (k != carrier)
            body.stages[statics[k]].alpha = wem::CombinerOp::Pass;
    }
    if (carrier < statics.size()) {
        wem::CombinerStage& stage = body.stages[statics[carrier]];
        stage.input = wem::TextureInput{};
        stage.input.texture = bakedIndex;
        stage.alpha = wem::CombinerOp::Mod;
    } else {
        wem::CombinerStage stage;
        stage.input.texture = bakedIndex;
        stage.rgb = wem::CombinerOp::Pass;
        stage.alpha = wem::CombinerOp::Mod;
        body.stages.push_back(std::move(stage));
    }
    for (const AlphaChain::Contributor& c : chain.contributors) {
        if (!c.isStatic && body.stages[c.stage].alpha == wem::CombinerOp::Mod2x)
            body.stages[c.stage].alpha = wem::CombinerOp::Mod;
    }
}

/// Alpha is linear data, so the sample is a straight bilinear read of the
/// texture's own alpha channel, wrapped — the masks tile with their sheet.
f32 SampleAlphaWrapped(const tx::Texture& t, f32 u, f32 v) {
    const std::span<const u8> px = t.mipData(0);
    const i64 w = static_cast<i64>(t.width());
    const i64 h = static_cast<i64>(t.height());
    const auto wrapI = [](i64 i, i64 n) {
        i %= n;
        return static_cast<std::size_t>(i < 0 ? i + n : i);
    };
    const f32 fx = u * static_cast<f32>(w) - 0.5f;
    const f32 fy = v * static_cast<f32>(h) - 0.5f;
    const i64 x0 = static_cast<i64>(std::floor(fx));
    const i64 y0 = static_cast<i64>(std::floor(fy));
    const f32 dx = fx - static_cast<f32>(x0);
    const f32 dy = fy - static_cast<f32>(y0);
    const auto at = [&](i64 xi, i64 yi) {
        return static_cast<f32>(
                   px[(wrapI(yi, h) * static_cast<std::size_t>(w) + wrapI(xi, w)) * 4 + 3]) /
               255.0f;
    };
    return (at(x0, y0) * (1 - dx) + at(x0 + 1, y0) * dx) * (1 - dy) +
           (at(x0, y0 + 1) * (1 - dx) + at(x0 + 1, y0 + 1) * dx) * dy;
}

/// White RGB, the product of every mask's alpha times @p gain in alpha, at
/// the largest mask's size.
tx::Texture BakeAlphaProduct(const std::vector<tx::Texture>& maps, std::size_t w, std::size_t h,
                             f32 gain) {
    tx::Texture out = tx::Texture::create2D(tx::PixelFormat::RGBA8, w, h);
    const std::span<u8> dst = out.mipData(0);
    for (std::size_t y = 0; y < h; ++y) {
        const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(h);
        for (std::size_t x = 0; x < w; ++x) {
            const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(w);
            f32 a = 1.0f;
            for (const tx::Texture& map : maps)
                a *= SampleAlphaWrapped(map, u, v);
            a = std::clamp(a * gain, 0.0f, 1.0f);
            const std::size_t p = (y * w + x) * 4;
            dst[p + 0] = 255;
            dst[p + 1] = 255;
            dst[p + 2] = 255;
            dst[p + 3] = static_cast<u8>(a * 255.0f + 0.5f);
        }
    }
    return out;
}

/// Merge a Diablo III chain's STATIC alpha masks into one texture.
///
/// A `Legacy.fx` wing multiplies up to three mask alphas and closes on a
/// x2/x4 output gain ("the alpha gain is what makes a wing a wing" —
/// D3_MATERIAL_DESIGN.md §7); an M3 material has two alpha slots and no gain
/// anywhere. The static part of that product is a pure function of the
/// textures, so it bakes: one texture (white rgb, product alpha, the chain's
/// whole scalar gain folded in) carried by the first colour-inert static
/// stage; every other static contributor's alpha op becomes the identity, and
/// a DYNAMIC contributor — a scrolling sheet — keeps its own stage, with
/// Mod2x downgraded to Mod because its doubling now lives in the bake.
///
/// The exact gains come from the native block's combine codes (the wem
/// vocabulary collapsed x4 onto Mod2x at import, with a warning).
void BakeD3AlphaChains(wem::Document& document, io::IContentProvider* provider,
                       std::map<u32, BakedTexture>& baked, M3ExportReport& report) {
    if (provider == nullptr)
        return;

    // (static texture indices, gain in halves) → the baked document index —
    // a look's eight variant materials share one mask set and must share one
    // bake.
    std::map<std::pair<std::vector<u32>, int>, u32> made;

    for (wem::Model& model : document.models) {
        for (wem::ProfileMaterialSet& set : model.profileSets) {
            for (wem::Material& material : set.materials) {
                if (material.nativeKind() != wem::NativeKind::D3 ||
                    !material.NativeIsAuthoritative()) {
                    continue;
                }
                // `InitCommon`, not `MutableCommon`: the bake restates what the
                // native block says, and marking the block stale would blind
                // the exporter's own native readings (the `_pma` mode).
                wem::CommonMaterial& common = material.InitCommon();
                wem::CombinersBody* body = common.combiners();
                if (body == nullptr || body->stages.empty())
                    continue;
                const bool blendsAlpha = common.blend == wem::BlendMode::AlphaKey ||
                                         common.blend == wem::BlendMode::Transparent ||
                                         common.blend == wem::BlendMode::AlphaBlend ||
                                         common.blend == wem::BlendMode::AdditiveAlpha ||
                                         common.blend == wem::BlendMode::PremultipliedAlpha;
                if (!blendsAlpha)
                    continue;
                const auto* d3 = std::get_if<wem::native::D3Material>(&material.Native());
                if (d3 == nullptr || d3->opaquePasses.empty())
                    continue;
                const std::vector<u32> codes = AlphaCombineCodes(d3->opaquePasses.front());
                if (codes.size() != body->stages.size())
                    continue;
                const std::optional<AlphaChain> chain = PlanAlphaChain(codes, *body, common);
                // One static mask at unit gain is exactly an alpha slot, and
                // the lib plants it; a gain with no static carrier has nowhere
                // to bake.
                if (!chain || chain->statics.empty() ||
                    (chain->statics.size() < 2 && chain->gain <= 1.0f))
                    continue;

                std::pair<std::vector<u32>, int> foldKey;
                for (std::size_t s : chain->statics)
                    foldKey.first.push_back(body->stages[s].input.texture);
                foldKey.second = static_cast<int>(chain->gain * 2.0f);
                if (const auto hit = made.find(foldKey); hit != made.end()) {
                    RewriteAlphaChain(*body, *chain, hit->second);
                    continue;
                }

                std::vector<tx::Texture> maps;
                std::size_t w = 1, h = 1;
                for (std::size_t s : chain->statics) {
                    const u32 texture = body->stages[s].input.texture;
                    std::optional<tx::Texture> map =
                        texture < document.textures.size()
                            ? DecodeRgba8(*provider, document.textures[texture])
                            : std::nullopt;
                    if (!map) {
                        maps.clear();
                        break;
                    }
                    w = (std::max)(w, static_cast<std::size_t>(map->width()));
                    h = (std::max)(h, static_cast<std::size_t>(map->height()));
                    maps.push_back(std::move(*map));
                }
                if (maps.size() != chain->statics.size()) {
                    report.diagnostics.info(
                        wem::DiagCode::LossyKindConversion,
                        material.name + ": an alpha-chain mask could not decode; the two-slot "
                                        "crossing stands and the gain stays unexpressed");
                    continue;
                }

                const u32 first = body->stages[chain->statics.front()].input.texture;
                const u32 index =
                    AddBakedTexture(document, baked, BakeStem(document, first) + "_mask",
                                    BakeAlphaProduct(maps, w, h, chain->gain),
                                    // White RGB nothing reads; the product in alpha.
                                    {tx::TextureKind::Unused, tx::TextureKind::TransparencyMask});
                made.emplace(std::move(foldKey), index);
                RewriteAlphaChain(*body, *chain, index);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Clips
// ---------------------------------------------------------------------------

/// Rename Diablo III clips to the StarCraft II vocabulary.
///
/// A D3 clip is named for its `.ani` — `barbarian_male_1HS_Attack_01` — and
/// the StarCraft II editor binds behaviour to the LEADING token of a sequence
/// name ('Stand 01', 'Attack 02'). The intent is a substring of the source
/// name, so the rename is a substring table; a name no row matches keeps its
/// own (it stays selectable, just not auto-bound), and variants number per
/// target in encounter order.
void RestateD3SequenceNames(wem::Document& document, wem::Diagnostics& diagnostics) {
    if (document.defaultProfile != wem::ProfileId::Diablo3) {
        return;
    }
    struct Row {
        const char* token;
        const char* target;
    };
    // Ordered: the first matching row wins, so the specific intents sit above
    // the generic ones ("townwalk" is a Walk by the "walk" row already).
    static constexpr Row kRows[] = {
        {"gethit", "Stand Hit"}, {"idle", "Stand"},  {"walk", "Walk"},
        {"run", "Run"},          {"attack", "Attack"}, {"death", "Death"},
        {"dead", "Death"},       {"spawn", "Birth"},
    };
    std::map<std::string, int> counts;
    int renamed = 0;
    for (wem::Clip& clip : document.clips) {
        const std::string lower = export_text::Lower(clip.name);
        for (const Row& row : kRows) {
            if (lower.find(row.token) == std::string::npos) {
                continue;
            }
            const int n = ++counts[row.target];
            clip.name = std::string(row.target) + (n < 10 ? " 0" : " ") + std::to_string(n);
            ++renamed;
            break;
        }
    }
    if (renamed != 0) {
        diagnostics.info(wem::DiagCode::LossyKindConversion,
                         std::to_string(renamed) +
                             " clip(s) renamed to the StarCraft II vocabulary "
                             "('..._Idle_01' -> 'Stand 01')",
                         wem::ElementRef(wem::ElementKind::Document, 0), wem::ProfileId::Sc2);
    }
}

/// Slower than a tile (or a turn) in ten minutes is a still image.
constexpr f32 kStillUvPeriodSeconds = 600.0f;
/// A rotation is keyed at eighth-turns: close enough for a linear quaternion
/// lerp to stay on the circle.
constexpr int kUvRotateKeysPerTurn = 8;

/// Give a constant-rate UV animation the keyed clip the M3 export can carry.
///
/// Diablo III states UV motion as a RATE (`UvAnimationFeature`), and only the
/// MDX exporter grew a consumer for that — the M3 side has none, so Imperius's
/// flames froze. The M3 spelling of "a loop the model runs on its own, off a
/// clock that is not the play's" is a SEQS `AlwaysGlobal` sequence, which WEM
/// writes as `AutoPlay | Persistent | WorldClocked` — and the container must be
/// CONCURRENT (`reference_m3_global_loops_and_subtracks`). One synthesized clip
/// per model carries linear `UvTranslate` keys over each rate's seamless
/// period (travel rounded to whole tiles, mdx_anim's argument: off by at most
/// half a tile per period against a visible snap at every wrap) and quaternion
/// `UvRotate` keys at eighth-turns; `convertUvTracks` and the layer wiring do
/// the rest, exactly as they do for a keyed source.
void SynthesizeUvRateClips(wem::Document& document) {
    constexpr f32 kTwoPi = 2.0f * std::numbers::pi_v<f32>;
    const auto pushF32 = [](std::vector<u8>& values, std::initializer_list<f32> parts) {
        for (const f32 part : parts) {
            const u8* bytes = reinterpret_cast<const u8*>(&part);
            values.insert(values.end(), bytes, bytes + sizeof(f32));
        }
    };
    for (std::size_t m = 0; m < document.models.size(); ++m) {
        wem::Model& model = document.models[m];
        std::vector<wem::SubTrack> tracks;
        f32 duration = 0.0f;
        for (wem::ProfileMaterialSet& set : model.profileSets) {
            const u32 look = set.defaultLook;
            for (std::size_t slot = 0; slot < set.slotBindings.size(); ++slot) {
                if (look >= set.slotBindings[slot].byLook.size())
                    continue;
                const u32 index = set.slotBindings[slot].byLook[look];
                if (index >= set.materials.size())
                    continue;
                const wem::CommonMaterial& common = set.materials[index].Common();
                for (const wem::MaterialFeature& feature : common.features) {
                    const wem::UvAnimationFeature* uv = feature.uvAnimation();
                    if (uv == nullptr || !uv->isConstantRate())
                        continue;
                    const auto makeChannel = [&](wem::Channel property,
                                                 wem::geom::AttrType type) -> wem::AnimChannel& {
                        wem::AnimChannel channel;
                        channel.id = model.animChannels.nextFreeId();
                        channel.target.kind = wem::TrackTarget::Kind::MaterialFeature;
                        channel.target.material.profile = set.profile;
                        channel.target.material.slot = static_cast<u32>(slot);
                        channel.target.material.look = look;
                        channel.target.sub = feature.id;
                        channel.target.channel = property;
                        channel.valueType = type;
                        model.animChannels.channels.push_back(std::move(channel));
                        return model.animChannels.channels.back();
                    };
                    if (uv->scrollRate.x != 0.0f || uv->scrollRate.y != 0.0f) {
                        f32 period = 0.0f;
                        for (const f32 axis : {uv->scrollRate.x, uv->scrollRate.y}) {
                            if (axis != 0.0f)
                                period = (std::max)(period, 1.0f / std::abs(axis));
                        }
                        if (period <= 0.0f || period > kStillUvPeriodSeconds)
                            continue;
                        wem::AnimChannel& channel =
                            makeChannel(wem::Channel::UvTranslate, wem::geom::AttrType::F32x3);
                        channel.initValue.assign(sizeof(f32) * 3, 0);
                        wem::SubTrack track;
                        track.channel = channel.id;
                        track.interp = wem::Interpolation::Linear;
                        track.times = {0.0f, period};
                        pushF32(track.values, {0.0f, 0.0f, 0.0f});
                        pushF32(track.values, {std::round(uv->scrollRate.x * period),
                                               std::round(uv->scrollRate.y * period), 0.0f});
                        tracks.push_back(std::move(track));
                        duration = (std::max)(duration, period);
                    }
                    if (uv->rotateRate != 0.0f) {
                        const f32 period = kTwoPi / std::abs(uv->rotateRate);
                        if (period > kStillUvPeriodSeconds)
                            continue;
                        wem::AnimChannel& channel =
                            makeChannel(wem::Channel::UvRotate, wem::geom::AttrType::Quat);
                        channel.initValue.clear();
                        pushF32(channel.initValue, {0.0f, 0.0f, 0.0f, 1.0f});
                        wem::SubTrack track;
                        track.channel = channel.id;
                        track.interp = wem::Interpolation::Linear;
                        const f32 sign = uv->rotateRate < 0.0f ? -1.0f : 1.0f;
                        for (int k = 0; k <= kUvRotateKeysPerTurn; ++k) {
                            const f32 theta = sign * kTwoPi * static_cast<f32>(k) /
                                              static_cast<f32>(kUvRotateKeysPerTurn);
                            track.times.push_back(period * static_cast<f32>(k) /
                                                  static_cast<f32>(kUvRotateKeysPerTurn));
                            pushF32(track.values, {0.0f, 0.0f, std::sin(theta * 0.5f),
                                                   std::cos(theta * 0.5f)});
                        }
                        tracks.push_back(std::move(track));
                        duration = (std::max)(duration, period);
                    }
                }
            }
        }
        if (tracks.empty())
            continue;
        wem::Clip clip;
        clip.name = "uv scroll";
        clip.model = static_cast<u32>(m);
        clip.duration = duration;
        clip.looping = true;
        clip.flags = wem::ClipFlags::AutoPlay | wem::ClipFlags::Persistent |
                     wem::ClipFlags::WorldClocked;
        wem::SubTrackContainer container;
        container.name = clip.name;
        container.concurrent = true;
        container.subTracks = std::move(tracks);
        clip.containers.push_back(std::move(container));
        document.clips.push_back(std::move(clip));
    }
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

/// One planned texture write: which document texture, what it was called at
/// the source, and where it lands relative to the model.
struct TexturePlan {
    std::size_t index = 0;
    std::string sourceKey; ///< The path or `#<id>` the provider resolves.
    std::string outName;   ///< The relative name the model now carries.
};

/// Name every file-backed texture's output and repoint the DOCUMENT at it.
///
/// Before the conversion, not after: an `.m3` has no texture table — `toM3`
/// builds its path map from `document.textures` in order and stamps each
/// layer's own path string — so the document is the one place a rename
/// reaches every reader.
///
/// The name is the source's own basename (what a StarCraft II path would say
/// under `Assets/Textures/`), disambiguated by index on collision, and the
/// model's stem for a texture the source only had an id for. Replaceables
/// (`replaceableId != 0`) name a runtime team colour or glow that no file
/// backs, in either game; they are left alone for the surface pass to spend.
std::vector<TexturePlan> PlanTextures(wem::Document& document, const std::string& modelStem) {
    std::vector<TexturePlan> plans;
    std::map<std::string, std::size_t> taken;
    for (std::size_t i = 0; i < document.textures.size(); ++i) {
        wem::TextureRef& ref = document.textures[i];
        // A replaceable that RESOLVED is exportable: the WC3 driver hands the
        // team glow its game file as a path, and a WoW creature skin arrives
        // as the fileDataID the display record baked (`BakeM2Replaceables`).
        // Only a slot still keyed on nothing stays a runtime slot — skipping
        // every pathless replaceable left the Earth Spirit's type-11 body
        // layers with empty paths and the whole model black.
        if (ref.replaceableId != 0 && ref.path.empty() &&
            wem::KeyKind(ref.key) == wem::TextureKeyKind::None)
            continue;

        std::string key = ref.path.empty() ? TextureIdKey(ref) : ref.path;
        if (key.empty())
            continue;

        std::string base;
        if (key[0] != '#')
            base = io::PathToUtf8(io::FsPathFromUtf8(export_text::ForwardSlashes(key)).stem());
        if (base.empty())
            base = modelStem + "_" + std::to_string(i);

        std::string name = kTextureDir + base + ".dds";
        const auto [it, fresh] = taken.emplace(export_text::Lower(name), i);
        if (!fresh)
            name = kTextureDir + base + "_" + std::to_string(i) + ".dds";

        ref.path = name;
        ref.key = wem::TexturePath{name};
        plans.push_back({i, std::move(key), std::move(name)});
    }
    return plans;
}

/// Read and decode the texture @p sourceKey names, saying why when it cannot.
std::optional<tx::Texture> DecodeSource(io::IContentProvider* provider,
                                        const std::string& sourceKey, wem::Diagnostics& out) {
    if (provider == nullptr)
        return std::nullopt;
    const std::optional<TextureFile> file = ReadTextureFile(*provider, ContentRefForKey(sourceKey));
    if (!file) {
        out.warn(wem::DiagCode::TextureUnresolved, "texture '" + sourceKey + "' is not readable");
        return std::nullopt;
    }
    TextureDecode decoded = DecodeTexture(file->bytes, file->extension);
    if (!decoded.texture) {
        out.warn(wem::DiagCode::TextureUnresolved,
                 "texture '" + sourceKey + "' did not decode: " + decoded.problem);
    }
    return std::move(decoded.texture);
}

/// The War3 (Mod) copies, read out of a StarCraft II install on demand.
class War3ModCopies {
public:
    /// The copies, or why there are none.
    struct Opened {
        std::unique_ptr<War3ModCopies> copies;
        std::string error;
    };

    static Opened Open(const std::string& install) {
        if (install.empty())
            return {nullptr, "no StarCraft II install was found"};
#if WHITEOUT_HAS_CASC
        std::string error;
        std::unique_ptr<io::IStorageSource> storage = io::CascSource::Open(install, {}, error);
        if (!storage)
            return {nullptr, std::move(error)};
        return {std::make_unique<War3ModCopies>(std::move(storage)), {}};
#else
        return {nullptr, "this build reads no CASC storage"};
#endif
    }

    explicit War3ModCopies(std::unique_ptr<io::IStorageSource> storage)
        : storage_(std::move(storage)) {}

    /// The decoded copy named @p name, or nothing when the mod ships none.
    std::optional<tx::Texture> Read(const std::string& name) const {
        io::SourceRead read;
        if (!storage_->Read(kWar3ModTextureDir + export_text::Lower(name), read) ||
            read.data.empty())
            return std::nullopt;
        return DecodeTexture(read.data, ".dds").texture;
    }

private:
    std::unique_ptr<io::IStorageSource> storage_;
};

/// Write every planned texture the written @p model reads under @p assetRoot,
/// converted to `.dds` -- or, given @p war3, name War3 (Mod)'s own copy where
/// it ships the same picture.
void ExportTextures(const M3ExportRequest& request, const fs::path& assetRoot,
                    const std::vector<TexturePlan>& plans,
                    const std::map<u32, BakedTexture>& baked, const War3ModCopies* war3,
                    ::whiteout::m3::Model& model, M3ExportReport& report) {
    io::IContentProvider* provider = request.subject.provider;
    TextureExportCounters& counters = report.textures;
    // Asked of the written model rather than the plan, which names every
    // document texture: a Reforged source's diffuse, normal and ORM are what
    // the bake turned into StarCraft II maps, and nothing reads them after.
    const std::unordered_set<std::string> named = M3TexturePathsNamed(model);
    for (const TexturePlan& plan : plans) {
        if (!named.contains(TexturePathKey(plan.outName))) {
            ++counters.unused;
            continue;
        }
        const fs::path outFile = assetRoot / io::FsPathFromUtf8(plan.outName);

        // A baked map has no file behind it — it was made out of the source's
        // own maps a moment ago — so it goes straight to the encoder, and over
        // any file already there: that file is an earlier export's bake, and
        // keeping it hid every fix to the bake (the footman's plume kept its
        // old cutout in the map).
        if (const auto wasBaked = baked.find(static_cast<u32>(plan.index));
            wasBaked != baked.end()) {
            const EncodedTexture made = EncodeBaked(wasBaked->second);
            if (!made.note.empty()) {
                report.diagnostics.warn(wem::DiagCode::Unspecified,
                                        "the baked texture '" + plan.outName + "' " + made.note);
            }
            if (!made) {
                report.diagnostics.warn(wem::DiagCode::Unspecified,
                                        "the baked texture '" + plan.outName +
                                            "' did not encode: " + made.problem);
                ++counters.failed;
                continue;
            }
            if (!WriteFileCreatingDirs(outFile, made.bytes)) {
                report.diagnostics.warn(wem::DiagCode::Unspecified,
                                        "could not write '" + io::PathToUtf8(outFile) + "'");
                ++counters.failed;
                continue;
            }
            ++counters.exported;
            continue;
        }

        // Before a file already there is kept: the mod's copy is the point of
        // asking, and that file is only an earlier export's.
        std::optional<tx::Texture> decoded;
        bool decodeTried = false;
        if (war3 != nullptr && plan.sourceKey[0] != '#') {
            const std::string name = War3ModTextureName(plan.sourceKey);
            std::optional<tx::Texture> shipped = name.empty() ? std::nullopt : war3->Read(name);
            if (shipped) {
                decoded = DecodeSource(provider, plan.sourceKey, report.diagnostics);
                decodeTried = true;
                if (decoded && SameTexturePicture(*decoded, *shipped)) {
                    RenameM3TexturePath(model, plan.outName, kTextureDir + name);
                    ++counters.inWar3Mod;
                    continue;
                }
            }
        }

        std::error_code ec;
        if (fs::exists(outFile, ec)) {
            ++counters.skipped;
            continue;
        }
        if (!decodeTried)
            decoded = DecodeSource(provider, plan.sourceKey, report.diagnostics);
        if (!decoded) {
            ++counters.failed;
            continue;
        }
        const EncodedTexture encoded = EncodeGameDds(*decoded);
        if (!encoded) {
            report.diagnostics.warn(wem::DiagCode::TextureUnresolved,
                                    "texture '" + plan.sourceKey +
                                        "' did not convert to .dds: " + encoded.problem);
            ++counters.failed;
            continue;
        }
        if (!WriteFileCreatingDirs(outFile, encoded.bytes)) {
            report.diagnostics.warn(wem::DiagCode::Unspecified,
                                    "could not write '" + io::PathToUtf8(outFile) + "'");
            ++counters.failed;
            continue;
        }
        ++counters.exported;
    }
}

// ---------------------------------------------------------------------------
// Warcraft III extras
// ---------------------------------------------------------------------------

/// Warcraft III's team glow as a mask: the glow's shape is on its red over a
/// flat alpha, which an emissive slot would read gamma-decoded, so a texture
/// with that red moved into alpha carries the shape to the team op
/// (`mdx_m3_effects.cpp`, row 22). Only when an emitter draws the glow; the
/// mask's document index, or `kInvalidIndex`.
u32 AddTeamGlowMask(const renderer::model::IModelSource& source, wem::Document& document,
                    std::map<u32, BakedTexture>& baked) {
    const auto* mdx = dynamic_cast<const io::MdxModelAdapter*>(&source);
    if (mdx == nullptr)
        return wem::kInvalidIndex;
    const auto& emitters = mdx->SourceModel().particleEmitters2;
    const bool drawsGlow = std::any_of(emitters.begin(), emitters.end(),
                                       [](const ::whiteout::mdx::ParticleEmitter2& pe) {
                                           return pe.replaceableId == kReplaceableTeamGlow;
                                       });
    std::optional<tx::Texture> glow = drawsGlow ? WhiteTeamGlow() : std::nullopt;
    if (!glow)
        return wem::kInvalidIndex;
    const std::span<u8> px = glow->mipData(0);
    for (usize i = 0; i + 3 < px.size(); i += 4) {
        px[i + 3] = px[i];
        px[i + 0] = 255;
        px[i + 1] = 255;
        px[i + 2] = 255;
    }
    return AddBakedTexture(document, baked, kTeamGlowMaskPath, std::move(*glow),
                           {tx::TextureKind::Unused, tx::TextureKind::AlphaMask});
}

/// The targeting volume's size is its bone's scale, a rest no retarget keeps,
/// so it is stated here on the bone `toM3` made for the node -- the IREF
/// inverting position and scale, as Blizzard's 1,149 volumes do -- and the
/// ATVL is a unit sphere's half on it.
void StateTargetVolume(const Wc3ToSc2Result& restated, const io::M3ExportResult& converted,
                       ::whiteout::m3::Model& model) {
    constexpr f32 kUnitSphereHalf = 0.5f;
    if (restated.volTargetNode >= converted.map.nodeBone.size())
        return;
    const u32 bone = converted.map.nodeBone[restated.volTargetNode];
    if (bone >= model.bones.size() || bone >= model.initialReference.size())
        return;
    ::whiteout::m3::Bone& target = model.bones[bone];
    target.scale.initValue = restated.volTargetScale;
    Matrix44f bind = Matrix44f::identity();
    bind.data[0][0] = restated.volTargetScale.x;
    bind.data[1][1] = restated.volTargetScale.y;
    bind.data[2][2] = restated.volTargetScale.z;
    bind.data[3][0] = target.position.initValue.x;
    bind.data[3][1] = target.position.initValue.y;
    bind.data[3][2] = target.position.initValue.z;
    model.initialReference[bone].matrix = Matrix44f::inverse(bind);
    ::whiteout::m3::AttachmentVolume volume;
    volume.bone1 = bone;
    volume.bone2 = bone;
    volume.boneIndex = static_cast<u16>(bone);
    volume.shapeType = ::whiteout::m3::HitTestShapeType::Sphere;
    volume.sizeX = kUnitSphereHalf;
    model.attachmentVolumes.push_back(std::move(volume));
    model.attachmentVolumesAddon0.push_back(0);
    model.attachmentVolumesAddon1.push_back(0);
}

/// Warcraft III's model-spawning emitters name `.mdl` files, which the viewer
/// resolves like any model. Each is exported once as a `.m3` of its own under
/// the chain's asset root, sharing its textures folder, and the model particle
/// names it by that path (WC3_TO_SC2_COMPLETION_PLAN.md C8.1). Index by index
/// with `source.particleEmitters`; empty where nothing was written.
std::vector<std::string> ExportSpawnedModels(const ::whiteout::mdx::Model& source,
                                             const M3ExportRequest& request, M3ExportChain& chain,
                                             int depth, M3ExportReport& report) {
    io::IContentProvider* provider = request.subject.provider;
    std::vector<std::string> paths(source.particleEmitters.size());
    if (source.particleEmitters.empty() || provider == nullptr)
        return paths;
    if (depth >= kMaxSpawnDepth) {
        report.diagnostics.warn(wem::DiagCode::FeatureDropped,
                                "a spawned model spawns models of its own " +
                                    std::to_string(kMaxSpawnDepth) +
                                    " levels down; they were not written",
                                wem::ElementRef());
        return paths;
    }
    std::map<std::string, std::string>& written = chain.spawnedModels;
    for (usize i = 0; i < source.particleEmitters.size(); ++i) {
        const std::string& spawn = source.particleEmitters[i].spawnModelFileName;
        if (spawn.empty())
            continue;
        const std::string key = export_text::ForwardSlashes(export_text::Lower(spawn));
        if (const auto done = written.find(key); done != written.end()) {
            paths[i] = done->second;
            continue;
        }
        written[key] = std::string();

        std::string actualExt;
        std::optional<std::vector<u8>> bytes = provider->ReadFile(spawn, &actualExt);
        if (!bytes || bytes->empty()) {
            report.diagnostics.warn(wem::DiagCode::AssetUnresolved,
                                    "spawned model '" + spawn + "' was not found",
                                    wem::ElementRef());
            continue;
        }
        const std::string ext = export_text::Lower(
            actualExt.empty() ? fs::path(key).extension().string() : actualExt);
        ::whiteout::mdx::Model model;
        try {
            ::whiteout::mdx::Parser parser;
            model = parser.parse(std::span<const u8>(bytes->data(), bytes->size()),
                                 ext == ".mdl" ? ::whiteout::mdx::MDLXFormat::MDL
                                               : ::whiteout::mdx::MDLXFormat::MDX);
        } catch (const std::exception& e) {
            report.diagnostics.warn(wem::DiagCode::AssetUnresolved,
                                    "spawned model '" + spawn + "' did not parse: " + e.what(),
                                    wem::ElementRef());
            continue;
        }

        // Named by the file's stem, once: a second file with the same stem
        // takes a number (the names meet on a case-blind file system).
        const std::string stem =
            io::PathToUtf8(io::FsPathFromUtf8(export_text::ForwardSlashes(spawn)).stem());
        std::string modelPath = kSpawnedModelDir + stem + ".m3";
        for (int n = 1; std::any_of(written.begin(), written.end(),
                                    [&](const auto& entry) {
                                        return export_text::Lower(entry.second) ==
                                               export_text::Lower(modelPath);
                                    });
             ++n)
            modelPath = kSpawnedModelDir + stem + "_" + std::to_string(n) + ".m3";

        io::MdxModelAdapter adapter(std::move(model), {}, provider);
        M3ExportRequest child;
        child.subject = {&adapter, provider, chain.assetRoot / io::FsPathFromUtf8(modelPath), stem};
        child.options = request.options;
        child.tileset = request.tileset;
        child.starCraft2Install = request.starCraft2Install;
        const M3ExportReport made = ExportInChain(child, chain, depth + 1);
        if (!made.ok) {
            report.diagnostics.warn(wem::DiagCode::AssetUnresolved,
                                    "spawned model '" + spawn + "' was not written: " + made.error,
                                    wem::ElementRef());
            continue;
        }
        report.diagnostics.info(wem::DiagCode::LossyKindConversion,
                                "spawned model '" + spawn + "' written as " + modelPath + " (" +
                                    std::to_string(made.diagnostics.size()) +
                                    " diagnostic(s) of its own)",
                                wem::ElementRef());
        written[key] = modelPath;
        paths[i] = modelPath;
        report.spawnedModels += 1 + made.spawnedModels;
        report.textures += made.textures;
    }
    return paths;
}

/// Warcraft III's emitters cross native to native, outside WEM, which stores
/// their nodes and not the systems they run (WEM_DESIGN §18): the records
/// need the parsed `.mdx` itself.
void CrossEffects(const ::whiteout::mdx::Model& emitters, const wem::Document& document,
                  u32 teamGlowMask, const M3ExportRequest& request, M3ExportChain& chain,
                  int depth, io::M3ExportResult& converted, M3ExportReport& report) {
    ::whiteout::models::cross::Wc3EffectOptions effects;
    effects.lengthScale = converted.scale;
    // The document's first textures are the `.mdx`'s, in order, and the plan
    // has already repointed them at the names this export writes.
    for (usize t = 0; t < emitters.textures.size(); ++t)
        effects.texturePaths.push_back(t < document.textures.size() ? document.textures[t].path
                                                                    : std::string());
    if (teamGlowMask < document.textures.size())
        effects.teamGlowMaskPath = document.textures[teamGlowMask].path;
    effects.modelParticlePaths = ExportSpawnedModels(emitters, request, chain, depth, report);

    const ::whiteout::models::cross::Wc3EffectReport crossed =
        ::whiteout::models::cross::CrossWc3Effects(emitters, *converted.staged, converted.map,
                                                   effects, *converted.model);
    report.diagnostics.append(crossed.diagnostics);
    report.particleRecords = static_cast<int>(crossed.particleRecords);
    report.ribbonRecords = static_cast<int>(crossed.ribbonRecords);
    report.cameraRecords = static_cast<int>(crossed.cameraRecords);
    report.hitTests = static_cast<int>(crossed.hitTests);
    report.modelParticles = static_cast<int>(crossed.modelParticleRecords);
}

/// War3 (Mod) ships Warcraft III's textures, and only StarCraft II loads it.
std::unique_ptr<War3ModCopies> OpenWar3Mod(const M3ExportRequest& request, M3ExportReport& report) {
    if (request.options.profile != wem::ProfileId::Sc2) {
        report.diagnostics.warn(wem::DiagCode::Unspecified,
                                "War3 (Mod) is StarCraft II's; a Heroes of the Storm model "
                                "writes every texture");
        return nullptr;
    }
    War3ModCopies::Opened opened = War3ModCopies::Open(request.starCraft2Install);
    if (!opened.copies) {
        report.diagnostics.warn(wem::DiagCode::Unspecified,
                                "War3 (Mod) textures were asked for, but " + opened.error +
                                    "; every texture is written");
    }
    return std::move(opened.copies);
}

M3ExportReport ExportInChain(const M3ExportRequest& request, M3ExportChain& chain, int depth) {
    M3ExportReport report;
    const ExportSubject& subject = request.subject;
    const M3ExportOptions& options = request.options;

    if (subject.source == nullptr) {
        report.error = "no model on screen";
        return report;
    }
    if (options.profile != wem::ProfileId::Sc2 && options.profile != wem::ProfileId::Heroes) {
        report.error = std::string(wem::Profile(options.profile).displayName) +
                       " is not a StarCraft II profile";
        return report;
    }

    io::WemExportOptions wemOptions;
    wemOptions.documentName = subject.modelName;
    std::optional<wem::Document> exported = ConvertThroughWem(subject, wemOptions, report);
    if (!exported)
        return report;
    wem::Document& document = *exported;

    // The game-convention pass: a Warcraft III document gets StarCraft II's
    // spellings — sequence names, the team conventions, and for Reforged the
    // whole inverse PBR bake — before anything downstream reads it. A no-op
    // for every other source.
    Wc3ToSc2Options wc3;
    wc3.tileset = request.tileset;
    wc3.sharpenTeamKey = options.sharpenTeamKey;
    wc3.standardRefs = options.standardRefs;
    Wc3ToSc2Result restated =
        RestateWc3AsSc2(document, subject.provider, report.diagnostics, wc3);

    // The masked-env folds and the Diablo III alpha-chain merges, baked into
    // new maps while the source's own textures are still resolvable — before
    // the plan renames them.
    std::map<u32, BakedTexture> baked = std::move(restated.baked);
    if (options.textures) {
        BakeMaskedFolds(document, subject.provider, baked, report);
        BakeD3AlphaChains(document, subject.provider, baked, report);
    }

    const u32 teamGlowMask = options.effects && options.textures
                                 ? AddTeamGlowMask(*subject.source, document, baked)
                                 : wem::kInvalidIndex;

    // A constant-rate UV scroll becomes a keyed global-loop clip; the derive
    // twins its channels into the target set with everything else. Diablo III
    // clip names take the StarCraft II vocabulary on the way.
    RestateD3SequenceNames(document, report.diagnostics);
    SynthesizeUvRateClips(document);

    // The repoint precedes the conversion (see PlanTextures); the writes can
    // wait until the model exists, so a conversion failure costs no files.
    std::vector<TexturePlan> plans;
    if (options.textures)
        plans = PlanTextures(document, io::PathToUtf8(subject.outPath.stem()));

    const bool warcraft = document.defaultProfile == wem::ProfileId::Wc3Classic ||
                          document.defaultProfile == wem::ProfileId::Wc3Reforged;

    const ::whiteout::mdx::Model* source = nullptr;
    if (warcraft) {
        if (const auto* mdx = dynamic_cast<const io::MdxModelAdapter*>(subject.source))
            source = &mdx->SourceModel();
    }
    // Unconditional: it also puts emitter nodes back under their parents, which
    // their bones need whether or not anything rides them.
    if (source != nullptr)
        ::whiteout::models::cross::PrepareWc3Effects(*source, document, report.diagnostics,
                                                     options.effects);
    const ::whiteout::mdx::Model* emitters = options.effects ? source : nullptr;

    io::M3ExportOptions m3Options;
    m3Options.profile = options.profile;
    m3Options.exactPasses = options.exactPasses;
    m3Options.textureAlphaClasses = std::move(restated.textureAlphaClasses);
    // Warcraft III's emitter, light and camera nodes each move and hide on
    // their own; StarCraft II says that with a bone apiece and a leaf for a
    // visibility that must not reach the node's children.
    m3Options.effectNodeBones = warcraft;
    m3Options.keepStagedDocument = emitters != nullptr;
    io::M3ExportResult converted = io::ConvertWemToM3(document, m3Options);
    report.diagnostics.append(converted.diagnostics);
    if (!converted.ok()) {
        report.error = converted.error;
        return report;
    }
    report.scale = converted.scale;

    StateTargetVolume(restated, converted, *converted.model);

    if (emitters != nullptr && converted.staged)
        CrossEffects(*emitters, document, teamGlowMask, request, chain, depth, converted, report);

    std::unique_ptr<War3ModCopies> war3;
    if (options.textures && options.war3ModTextures && warcraft)
        war3 = OpenWar3Mod(request, report);

    if (options.textures)
        ExportTextures(request, chain.assetRoot, plans, baked, war3.get(), *converted.model,
                       report);

    report.error = WriteModelFile(subject.outPath, [&](const std::string& path) {
        ::whiteout::m3::Writer writer;
        writer.write(path, *converted.model);
    });
    if (!report.error.empty())
        return report;

    report.ok = true;
    return report;
}

} // namespace

M3ExportReport ExportModelAsM3(const M3ExportRequest& request) {
    M3ExportChain chain;
    chain.assetRoot = request.subject.outPath.parent_path();
    return ExportInChain(request, chain, 0);
}

} // namespace whiteout::flakes
