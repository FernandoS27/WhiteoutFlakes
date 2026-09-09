// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "m3_export.h"

#include "wc3_to_sc2_export.h"

#include "renderer/model/model_source_utils.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/content_ref.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <whiteout/models/m3/writer.h>
#include <whiteout/textures/dds/writer.h>
#include <whiteout/textures/texture.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace whiteout::flakes {

namespace {

namespace fs = std::filesystem;
namespace tx = whiteout::textures;

std::string Lower(std::string value) {
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

/// The reference the loader would resolve this name through — `#<id>` is
/// `ContentRef::Describe`'s spelling of a fileDataID or a SNO.
ContentRef RefForTextureName(const std::string& name) {
    if (name.size() > 1 && name[0] == '#')
        return ContentRef::FromFileId(
            static_cast<u32>(std::strtoul(name.c_str() + 1, nullptr, 10)));
    return ContentRef::FromPath(name);
}

/// Whether any texel is not fully opaque. Decides BC1 against BC3.
bool HasAlpha(const tx::Texture& texture) {
    const std::span<const u8> pixels = texture.mipData(0);
    for (std::size_t i = 3; i < pixels.size(); i += 4) {
        if (pixels[i] != 0xFF)
            return true;
    }
    return false;
}

/// Encode @p source as the `.dds` StarCraft II reads: BC1 where the texture
/// is opaque and BC3 where it is not, mips generated, and the sRGB flag
/// cleared so the writer keeps the legacy `DXT1`/`DXT5` FourCC — colour space
/// in an `.m3` is the SLOT's declaration, not the container's.
std::optional<std::vector<u8>> EncodeForSc2(const tx::Texture& source) {
    try {
        tx::Texture texture = source;
        texture.format(tx::PixelFormat::RGBA8);
        const bool alpha = HasAlpha(texture);
        texture.generateMipmaps(
            tx::computeMaxMipCount(texture.width(), texture.height(), texture.depth()));
        texture.setSrgb(false);
        texture.format(alpha ? tx::PixelFormat::BC3 : tx::PixelFormat::BC1);
        tx::dds::Writer writer;
        std::vector<u8> bytes = writer.write(texture);
        if (bytes.empty())
            return std::nullopt;
        return bytes;
    } catch (const std::exception&) {
        return std::nullopt;
    }
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

    const auto decode = [&](const wem::TextureRef& ref) -> std::optional<tx::Texture> {
        std::string key = ref.path;
        if (key.empty()) {
            if (const auto* fileId = std::get_if<wem::TextureFileDataId>(&ref.key))
                key = "#" + std::to_string(fileId->value);
            else if (const auto* sno = std::get_if<wem::TextureSnoId>(&ref.key))
                key = "#" + std::to_string(sno->id);
        }
        if (key.empty())
            return std::nullopt;
        std::string actualExt;
        std::optional<std::vector<u8>> bytes =
            provider->ReadFile(RefForTextureName(key), &actualExt);
        if (!bytes || bytes->empty())
            return std::nullopt;
        actualExt = Lower(actualExt);
        if (actualExt.empty()) {
            actualExt = renderer::model::SniffTextureExtension(
                std::span<const u8>(bytes->data(), bytes->size()));
        }
        std::optional<tx::Texture> texture = renderer::model::DispatchTextureParser(
            actualExt, [&](auto& parser) { return parser.parse(std::span<const u8>(*bytes)); });
        if (texture)
            texture->format(tx::PixelFormat::RGBA8);
        return texture;
    };

    const auto toLinear = [](f32 v) {
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    const auto toSrgb = [](f32 v) {
        v = std::clamp(v, 0.0f, 1.0f);
        return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    };

    // (seed texture, env texture, N-in-halves) → the baked document index, so
    // materials sharing one fold bake it once.
    std::map<std::tuple<u32, u32, int>, u32> made;

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
                const auto isEnv = [](const wem::CombinerStage& stage) {
                    return stage.input.mapping == wem::UVMappingMode::EnvSphere ||
                           stage.input.mapping == wem::UVMappingMode::EnvCube;
                };
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
                        decode(document.textures[seedStage.input.texture]);
                    std::optional<tx::Texture> env = decode(document.textures[stage.input.texture]);
                    if (!base || !env) {
                        // The declarative env+mask crossing stays; say so once.
                        report.diagnostics.info(
                            wem::DiagCode::LossyKindConversion,
                            "a masked env fold could not bake (texture unreadable); the env "
                            "layer approximation stands");
                        continue;
                    }

                    // The central half-disk, not the full area: a sphere map's
                    // lookup lands near the CENTRE for anything facing the
                    // viewer (reflect(view, n) ≈ view there), so the rim —
                    // half the area, and authored dark on every sheen sprite —
                    // barely gets sampled. The tauren's dragonarmorspec: area
                    // mean 0.39 linear, centre disk 0.65 — the difference
                    // between dimming the plates and the brightened gold the
                    // native render shows.
                    f32 k[3] = {0, 0, 0};
                    {
                        const std::span<const u8> px = env->mipData(0);
                        const std::size_t w = env->width(), h = env->height();
                        double sum[3] = {0, 0, 0};
                        std::size_t count = 0;
                        for (std::size_t y = 0; y < h; ++y) {
                            const f32 dy = (static_cast<f32>(y) - h * 0.5f) / (h * 0.5f);
                            for (std::size_t x = 0; x < w; ++x) {
                                const f32 dx = (static_cast<f32>(x) - w * 0.5f) / (w * 0.5f);
                                if (dx * dx + dy * dy > 0.25f)
                                    continue;
                                const std::size_t p = (y * w + x) * 4;
                                for (int c = 0; c < 3; ++c)
                                    sum[c] += toLinear(px[p + static_cast<std::size_t>(c)] / 255.0f);
                                ++count;
                            }
                        }
                        for (int c = 0; c < 3; ++c)
                            k[c] = count > 0
                                       ? n * static_cast<f32>(sum[c] / static_cast<double>(count))
                                       : n;
                    }

                    tx::Texture out = tx::Texture::create2D(tx::PixelFormat::RGBA8, base->width(),
                                                            base->height());
                    {
                        const std::span<const u8> src = base->mipData(0);
                        const std::span<u8> dst = out.mipData(0);
                        f32 lut[256];
                        for (int v = 0; v < 256; ++v)
                            lut[v] = toLinear(static_cast<f32>(v) / 255.0f);
                        const std::size_t count = src.size() / 4;
                        for (std::size_t p = 0; p < count; ++p) {
                            const f32 a = src[p * 4 + 3] / 255.0f;
                            for (int c = 0; c < 3; ++c) {
                                const std::size_t at = p * 4 + static_cast<std::size_t>(c);
                                const f32 folded = lut[src[at]] * (a + k[c] * (1.0f - a));
                                dst[at] = static_cast<u8>(toSrgb(folded) * 255.0f + 0.5f);
                            }
                            dst[p * 4 + 3] = src[p * 4 + 3];
                        }
                    }

                    const wem::TextureRef& seedRef = document.textures[seedStage.input.texture];
                    std::string stem = seedRef.path;
                    if (!stem.empty()) {
                        std::replace(stem.begin(), stem.end(), '\\', '/');
                        stem = io::PathToUtf8(io::FsPathFromUtf8(stem).stem());
                    }
                    if (stem.empty())
                        stem = "texture_" + std::to_string(seedStage.input.texture);
                    const u32 index = static_cast<u32>(document.textures.size());
                    wem::TextureRef foldRef;
                    foldRef.path = stem + "_fold";
                    foldRef.key = wem::TexturePath{foldRef.path};
                    document.textures.push_back(std::move(foldRef));
                    baked.insert_or_assign(index, BakedTexture{std::move(out),
                                                               tx::PixelFormat::BC3});
                    made.emplace(foldKey, index);
                    seedStage.input.texture = index;
                    stage.rgb = wem::CombinerOp::Pass;
                }
            }
        }
    }
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
/// vocabulary collapsed x4 onto Mod2x at import, with a warning), aligned to
/// chain stages by the same walk d3_core's import does — the K-th surviving
/// stage IS the K-th chain stage. Only the modulate family merges; an add or
/// a mid-chain replace does not commute with the product and the material
/// keeps the declarative crossing instead.
void BakeD3AlphaChains(wem::Document& document, io::IContentProvider* provider,
                       std::map<u32, BakedTexture>& baked, M3ExportReport& report) {
    if (provider == nullptr)
        return;
    constexpr u32 kTagColorCombine = 0xA0016u;
    constexpr u32 kTagAlphaCombine = 0xA001Cu;

    const auto decode = [&](const wem::TextureRef& ref) -> std::optional<tx::Texture> {
        std::string key = ref.path;
        if (key.empty()) {
            if (const auto* fileId = std::get_if<wem::TextureFileDataId>(&ref.key))
                key = "#" + std::to_string(fileId->value);
            else if (const auto* sno = std::get_if<wem::TextureSnoId>(&ref.key))
                key = "#" + std::to_string(sno->id);
        }
        if (key.empty())
            return std::nullopt;
        std::string actualExt;
        std::optional<std::vector<u8>> bytes =
            provider->ReadFile(RefForTextureName(key), &actualExt);
        if (!bytes || bytes->empty())
            return std::nullopt;
        actualExt = Lower(actualExt);
        if (actualExt.empty()) {
            actualExt = renderer::model::SniffTextureExtension(
                std::span<const u8>(bytes->data(), bytes->size()));
        }
        std::optional<tx::Texture> texture = renderer::model::DispatchTextureParser(
            actualExt, [&](auto& parser) { return parser.parse(std::span<const u8>(*bytes)); });
        if (texture)
            texture->format(tx::PixelFormat::RGBA8);
        return texture;
    };

    // Alpha is linear data, so the sample is a straight bilinear read of the
    // texture's own alpha channel, wrapped — the masks tile with their sheet.
    const auto sampleAlpha = [](const tx::Texture& t, f32 u, f32 v) -> f32 {
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
    };

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
                const wem::native::D3RenderState& pass = d3->opaquePasses.front();
                const auto tagOf = [&pass](u32 id) -> const wem::native::D3ShaderTagValue* {
                    for (const wem::native::D3ShaderTagValue& tag : pass.shaderParams) {
                        if (tag.tagId == id)
                            return &tag;
                    }
                    return nullptr;
                };
                std::vector<u32> alphaCodes;
                for (std::size_t s = 0; s < pass.textureStages.size() && s < 6; ++s) {
                    if (pass.textureStages[s].contentStage == 0)
                        continue;
                    const auto* rgb = tagOf(kTagColorCombine + static_cast<u32>(s));
                    const auto* alpha = tagOf(kTagAlphaCombine + static_cast<u32>(s));
                    if (rgb == nullptr && alpha == nullptr)
                        continue;
                    alphaCodes.push_back(alpha != nullptr ? alpha->value : 0u);
                }
                if (alphaCodes.size() != body->stages.size())
                    continue;

                struct Contributor {
                    std::size_t stage;
                    bool isStatic;
                };
                std::vector<Contributor> contributors;
                f32 gainProduct = 1.0f;
                bool bakeable = true;
                for (std::size_t i = 0; i < alphaCodes.size(); ++i) {
                    const u32 code = alphaCodes[i];
                    if (code == 0)
                        continue;
                    const u32 tens = code / 10;
                    const u32 units = code % 10;
                    // Tens 2 is the modulate; a LEADING replace is a modulate
                    // against the neutral head. Anything else — the add, the
                    // erosion tail — does not commute with a merge.
                    if (!(tens == 2 || (tens == 0 && contributors.empty()))) {
                        bakeable = false;
                        break;
                    }
                    if (tens == 2) {
                        if (units == 4 || units == 6)
                            gainProduct *= 2.0f;
                        if (units == 5 || units == 7)
                            gainProduct *= 4.0f;
                    }
                    const wem::CombinerStage& stage = body->stages[i];
                    bool isStatic =
                        stage.input.hasTexture() && stage.input.uvTransform.isIdentity();
                    for (const wem::MaterialFeature& feature : common.features) {
                        if (feature.layer == static_cast<u32>(i) &&
                            feature.uvAnimation() != nullptr) {
                            isStatic = false;
                        }
                    }
                    contributors.push_back({i, isStatic});
                }
                if (!bakeable)
                    continue;
                std::vector<std::size_t> statics;
                for (const Contributor& c : contributors) {
                    if (c.isStatic)
                        statics.push_back(c.stage);
                }
                // One static mask at unit gain is exactly an alpha slot, and
                // the lib plants it; a gain with no static carrier has nowhere
                // to bake.
                if (statics.empty() || (statics.size() < 2 && gainProduct <= 1.0f))
                    continue;

                // The chain-rewrite half, shared by a fresh bake and a cache
                // hit: the carrier takes the product, every other static
                // contributor becomes the identity, and a dynamic
                // contributor's doubling now lives in the bake.
                const auto rewriteChain = [&](u32 bakedIndex) {
                    std::size_t carrier = statics.size();
                    for (std::size_t k = 0; k < statics.size(); ++k) {
                        if (body->stages[statics[k]].rgb == wem::CombinerOp::Pass) {
                            carrier = k;
                            break;
                        }
                    }
                    for (std::size_t k = 0; k < statics.size(); ++k) {
                        if (k != carrier)
                            body->stages[statics[k]].alpha = wem::CombinerOp::Pass;
                    }
                    if (carrier < statics.size()) {
                        wem::CombinerStage& stage = body->stages[statics[carrier]];
                        stage.input = wem::TextureInput{};
                        stage.input.texture = bakedIndex;
                        stage.alpha = wem::CombinerOp::Mod;
                    } else {
                        wem::CombinerStage stage;
                        stage.input.texture = bakedIndex;
                        stage.rgb = wem::CombinerOp::Pass;
                        stage.alpha = wem::CombinerOp::Mod;
                        body->stages.push_back(std::move(stage));
                    }
                    for (const Contributor& c : contributors) {
                        if (!c.isStatic && body->stages[c.stage].alpha == wem::CombinerOp::Mod2x)
                            body->stages[c.stage].alpha = wem::CombinerOp::Mod;
                    }
                };

                std::pair<std::vector<u32>, int> foldKey;
                for (std::size_t s : statics)
                    foldKey.first.push_back(body->stages[s].input.texture);
                foldKey.second = static_cast<int>(gainProduct * 2.0f);
                if (const auto hit = made.find(foldKey); hit != made.end()) {
                    rewriteChain(hit->second);
                    continue;
                }

                std::vector<tx::Texture> maps;
                std::size_t w = 1, h = 1;
                for (std::size_t s : statics) {
                    const u32 texture = body->stages[s].input.texture;
                    if (texture >= document.textures.size()) {
                        maps.clear();
                        break;
                    }
                    std::optional<tx::Texture> map = decode(document.textures[texture]);
                    if (!map) {
                        maps.clear();
                        break;
                    }
                    w = (std::max)(w, static_cast<std::size_t>(map->width()));
                    h = (std::max)(h, static_cast<std::size_t>(map->height()));
                    maps.push_back(std::move(*map));
                }
                if (maps.size() != statics.size()) {
                    report.diagnostics.info(
                        wem::DiagCode::LossyKindConversion,
                        material.name + ": an alpha-chain mask could not decode; the two-slot "
                                        "crossing stands and the gain stays unexpressed");
                    continue;
                }

                tx::Texture outTex = tx::Texture::create2D(tx::PixelFormat::RGBA8, w, h);
                {
                    const std::span<u8> dst = outTex.mipData(0);
                    for (std::size_t y = 0; y < h; ++y) {
                        const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(h);
                        for (std::size_t x = 0; x < w; ++x) {
                            const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(w);
                            f32 a = 1.0f;
                            for (const tx::Texture& map : maps)
                                a *= sampleAlpha(map, u, v);
                            a = std::clamp(a * gainProduct, 0.0f, 1.0f);
                            const std::size_t p = (y * w + x) * 4;
                            dst[p + 0] = 255;
                            dst[p + 1] = 255;
                            dst[p + 2] = 255;
                            dst[p + 3] = static_cast<u8>(a * 255.0f + 0.5f);
                        }
                    }
                }

                const wem::TextureRef& firstRef =
                    document.textures[body->stages[statics.front()].input.texture];
                std::string stem = firstRef.path;
                if (!stem.empty()) {
                    std::replace(stem.begin(), stem.end(), '\\', '/');
                    stem = io::PathToUtf8(io::FsPathFromUtf8(stem).stem());
                }
                if (stem.empty())
                    stem = "texture_" + std::to_string(body->stages[statics.front()].input.texture);
                const u32 index = static_cast<u32>(document.textures.size());
                wem::TextureRef maskRef;
                maskRef.path = stem + "_mask";
                maskRef.key = wem::TexturePath{maskRef.path};
                document.textures.push_back(std::move(maskRef));
                baked.insert_or_assign(index, BakedTexture{std::move(outTex),
                                                           tx::PixelFormat::BC3});
                made.emplace(std::move(foldKey), index);
                rewriteChain(index);
            }
        }
    }
}

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
        const std::string lower = Lower(clip.name);
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
                        // Slower than a tile in ten minutes is a still image.
                        if (period <= 0.0f || period > 600.0f)
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
                        const f32 period = 2.0f * 3.14159265358979f / std::abs(uv->rotateRate);
                        if (period > 600.0f)
                            continue;
                        wem::AnimChannel& channel =
                            makeChannel(wem::Channel::UvRotate, wem::geom::AttrType::Quat);
                        channel.initValue.clear();
                        pushF32(channel.initValue, {0.0f, 0.0f, 0.0f, 1.0f});
                        wem::SubTrack track;
                        track.channel = channel.id;
                        track.interp = wem::Interpolation::Linear;
                        const f32 sign = uv->rotateRate < 0.0f ? -1.0f : 1.0f;
                        for (int k = 0; k <= 8; ++k) {
                            const f32 theta =
                                sign * 2.0f * 3.14159265358979f * static_cast<f32>(k) / 8.0f;
                            track.times.push_back(period * static_cast<f32>(k) / 8.0f);
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

        std::string key = ref.path;
        if (key.empty()) {
            if (const auto* sno = std::get_if<wem::TextureSnoId>(&ref.key))
                key = "#" + std::to_string(sno->id);
            else if (const auto* fileId = std::get_if<wem::TextureFileDataId>(&ref.key))
                key = "#" + std::to_string(fileId->value);
        }
        if (key.empty())
            continue;

        std::string base;
        if (key[0] != '#') {
            std::string normalized = key;
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            base = io::PathToUtf8(io::FsPathFromUtf8(normalized).stem());
        }
        if (base.empty())
            base = modelStem + "_" + std::to_string(i);

        std::string name = base + ".dds";
        const auto [it, fresh] = taken.emplace(Lower(name), i);
        if (!fresh)
            name = base + "_" + std::to_string(i) + ".dds";

        ref.path = name;
        ref.key = wem::TexturePath{name};
        plans.push_back({i, std::move(key), std::move(name)});
    }
    return plans;
}

/// Encode a texture the export MADE rather than read: the container is fixed
/// by what the map is, the pixels arrive linear, and running it through the
/// alpha sniff would demote a BC3 team-alpha diffuse to BC1.
std::optional<std::vector<u8>> EncodeBaked(const BakedTexture& baked) {
    try {
        tx::Texture texture = baked.texture;
        texture.setSrgb(false);
        texture.generateMipmaps(
            tx::computeMaxMipCount(texture.width(), texture.height(), texture.depth()));
        texture.format(baked.format);
        tx::dds::Writer writer;
        std::vector<u8> bytes = writer.write(texture);
        if (bytes.empty())
            return std::nullopt;
        return bytes;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// Write every planned texture beside the model, converted to `.dds`.
void ExportTextures(const M3ExportRequest& request, const std::vector<TexturePlan>& plans,
                    const std::map<u32, BakedTexture>& baked, M3ExportReport& report) {
    const fs::path targetDir = request.outPath.parent_path();
    for (const TexturePlan& plan : plans) {
        const fs::path outFile = targetDir / io::FsPathFromUtf8(plan.outName);

        std::error_code ec;
        if (fs::exists(outFile, ec)) {
            ++report.texturesSkipped;
            continue;
        }

        // A baked map has no file behind it — it was made out of the source's
        // own maps a moment ago — so it goes straight to the encoder.
        const auto wasBaked = baked.find(static_cast<u32>(plan.index));
        if (wasBaked != baked.end()) {
            std::optional<std::vector<u8>> made = EncodeBaked(wasBaked->second);
            if (!made) {
                ++report.texturesFailed;
                continue;
            }
            fs::create_directories(outFile.parent_path(), ec);
            std::ofstream madeFile(outFile, std::ios::binary);
            if (madeFile)
                madeFile.write(reinterpret_cast<const char*>(made->data()),
                               static_cast<std::streamsize>(made->size()));
            if (!madeFile) {
                ++report.texturesFailed;
                continue;
            }
            ++report.texturesExported;
            continue;
        }
        if (request.provider == nullptr) {
            ++report.texturesFailed;
            continue;
        }

        std::string actualExt;
        std::optional<std::vector<u8>> bytes =
            request.provider->ReadFile(RefForTextureName(plan.sourceKey), &actualExt);
        if (!bytes || bytes->empty()) {
            std::fprintf(stderr, "[viewer] Export M3: texture not readable: %s\n",
                         plan.sourceKey.c_str());
            ++report.texturesFailed;
            continue;
        }
        actualExt = Lower(actualExt);
        if (actualExt.empty()) {
            actualExt = renderer::model::SniffTextureExtension(
                std::span<const u8>(bytes->data(), bytes->size()));
        }

        std::optional<tx::Texture> decoded = renderer::model::DispatchTextureParser(
            actualExt, [&](auto& parser) { return parser.parse(std::span<const u8>(*bytes)); });
        std::optional<std::vector<u8>> encoded = decoded ? EncodeForSc2(*decoded) : std::nullopt;
        if (!encoded) {
            std::fprintf(stderr, "[viewer] Export M3: texture convert failed %s -> .dds\n",
                         plan.sourceKey.c_str());
            ++report.texturesFailed;
            continue;
        }

        fs::create_directories(outFile.parent_path(), ec);
        std::ofstream file(outFile, std::ios::binary);
        if (file)
            file.write(reinterpret_cast<const char*>(encoded->data()),
                       static_cast<std::streamsize>(encoded->size()));
        if (!file) {
            ++report.texturesFailed;
            continue;
        }
        ++report.texturesExported;
    }
}

} // namespace

M3ExportReport ExportModelAsM3(const M3ExportRequest& request) {
    M3ExportReport report;

    if (request.source == nullptr) {
        report.error = "no model on screen";
        return report;
    }
    if (request.profile != wem::ProfileId::Sc2 && request.profile != wem::ProfileId::Heroes) {
        report.error = std::string(wem::Profile(request.profile).displayName) +
                       " is not a StarCraft II profile";
        return report;
    }

    io::WemExportOptions wemOptions;
    wemOptions.documentName = request.modelName;
    wemOptions.materialLook = request.materialLook;
    const io::WemExportResult exported =
        io::ExportModelToWem(*request.source, request.provider, wemOptions);
    report.diagnostics.append(exported.diagnostics);
    if (!exported.ok()) {
        report.error = exported.error;
        return report;
    }
    report.formatId = exported.formatId;

    wem::Document document = std::move(*exported.document);

    // The game-convention pass: a Warcraft III document gets StarCraft II's
    // spellings — sequence names, the team conventions, and for Reforged the
    // whole inverse PBR bake — before anything downstream reads it. A no-op
    // for every other source.
    const Wc3ToSc2Result restated =
        RestateWc3AsSc2(document, request.provider, report.diagnostics, request.wc3);

    // The masked-env folds and the Diablo III alpha-chain merges, baked into
    // new maps while the source's own textures are still resolvable — before
    // the plan renames them.
    std::map<u32, BakedTexture> baked = restated.baked;
    if (request.exportTextures) {
        BakeMaskedFolds(document, request.provider, baked, report);
        BakeD3AlphaChains(document, request.provider, baked, report);
    }

    // A constant-rate UV scroll becomes a keyed global-loop clip; the derive
    // twins its channels into the target set with everything else. Diablo III
    // clip names take the StarCraft II vocabulary on the way.
    RestateD3SequenceNames(document, report.diagnostics);
    SynthesizeUvRateClips(document);

    // The repoint precedes the conversion (see PlanTextures); the writes can
    // wait until the model exists, so a conversion failure costs no files.
    std::vector<TexturePlan> plans;
    if (request.exportTextures)
        plans = PlanTextures(document, io::PathToUtf8(request.outPath.stem()));

    io::M3ExportOptions m3Options;
    m3Options.profile = request.profile;
    m3Options.exactPasses = request.wc3.exactPasses;
    m3Options.textureAlphaClasses = restated.textureAlphaClasses;
    io::M3ExportResult converted = io::ConvertWemToM3(document, m3Options);
    report.diagnostics.append(converted.diagnostics);
    if (!converted.ok()) {
        report.error = converted.error;
        return report;
    }
    report.scale = converted.scale;
    report.derived = converted.derived;

    if (request.exportTextures)
        ExportTextures(request, plans, baked, report);

    std::error_code dirError;
    fs::create_directories(request.outPath.parent_path(), dirError);
    try {
        ::whiteout::m3::Writer writer;
        writer.write(io::PathToUtf8(request.outPath), *converted.model);
    } catch (const std::exception& e) {
        report.error = std::string("could not write the model: ") + e.what();
        return report;
    }
    if (!fs::exists(request.outPath)) {
        report.error = "could not write the model to " + io::PathToUtf8(request.outPath);
        return report;
    }

    report.ok = true;
    return report;
}

} // namespace whiteout::flakes
