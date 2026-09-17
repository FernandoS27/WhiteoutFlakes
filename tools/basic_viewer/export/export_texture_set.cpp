// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "export_texture_set.h"

#include "export_text.h"

#include <whiteout/textures/texture.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <optional>
#include <span>

namespace whiteout::flakes {

namespace {

namespace m3 = ::whiteout::m3;
namespace mdx = ::whiteout::mdx;
namespace tx = ::whiteout::textures;

/// Every texture path slot @p model carries, handed to @p visit. One walk for
/// the reader and the rename, so the two can never disagree on what counts.
template <typename ModelT, typename Visit>
void ForEachTexturePath(ModelT& model, Visit&& visit) {
    const auto layer = [&](auto& slot) {
        if (slot && !slot->texturePath.empty())
            visit(slot->texturePath);
    };
    for (auto& m : model.standardMaterials) {
        layer(m.diffuseLayer);
        layer(m.decalLayer);
        layer(m.specularLayer);
        layer(m.glossLayer);
        layer(m.emissiveLayer1);
        layer(m.emissiveLayer2);
        layer(m.environmentLayer);
        layer(m.environmentMaskLayer);
        layer(m.alphaLayer1);
        layer(m.alphaLayer2);
        layer(m.normalLayer);
        layer(m.heightLayer);
        layer(m.lightMapLayer);
        layer(m.ambientOcclusionLayer);
        layer(m.normalBlend1MaskLayer);
        layer(m.normalBlend2MaskLayer);
        layer(m.normalBlend1Layer);
        layer(m.normalBlend2Layer);
    }
    for (auto& m : model.displacementMaterials) {
        layer(m.normalMap);
        layer(m.strengthMap);
    }
    for (auto& m : model.terrainMaterials)
        layer(m.terrainMap);
    for (auto& m : model.volumeMaterials) {
        layer(m.colorMap);
        layer(m.noiseMap1);
        layer(m.noiseMap2);
    }
    for (auto& m : model.hairMaterials) {
        layer(m.layerBase);
        layer(m.layerSpecShift);
        layer(m.layerSpecNoise);
        layer(m.layerAO);
    }
    for (auto& m : model.creepMaterials)
        layer(m.maskMap);
    for (auto& m : model.volumeNoiseMaterials) {
        layer(m.colorMap);
        layer(m.noiseMap1);
        layer(m.noiseMap2);
    }
    for (auto& m : model.stbMaterials) {
        layer(m.diffuseMap);
        layer(m.normalMap);
        layer(m.specularMap);
    }
    for (auto& m : model.reflectionMaterials) {
        layer(m.reflectionMap);
        layer(m.displacementMap);
        layer(m.blurMap);
    }
    for (auto& m : model.lensFlareMaterials) {
        layer(m.flareMap);
        layer(m.maskMap);
    }
    for (auto& m : model.dataDrivenMaterials)
        for (auto& path : m.texturePaths)
            if (!path.empty())
                visit(path);
}

/// Each key's value of a texture-id track; a smooth track stores tangents too.
template <typename TrackT, typename Visit>
void ForEachKeyValue(TrackT& track, Visit&& visit) {
    if (mdx::isSmoothInterpolation(track.interpolationType)) {
        for (auto& key : track.tangentKeys())
            visit(key.value);
    } else {
        for (auto& key : track.keys())
            visit(key);
    }
}

/// Every texture id @p model's records read, handed to @p visit.
template <typename ModelT, typename Visit>
void ForEachTextureId(ModelT& model, Visit&& visit) {
    for (auto& material : model.materials) {
        for (auto& layer : material.layers) {
            visit(layer.textureId);
            ForEachKeyValue(layer.textureIdTracks, visit);
            for (auto& sub : layer.subTextures) {
                visit(sub.textureId);
                ForEachKeyValue(sub.tracks, visit);
            }
        }
    }
    for (auto& emitter : model.particleEmitters2)
        visit(emitter.textureId);
}

/// Level 0 as RGBA8, or nothing for what is not one plain 2D image.
std::optional<tx::Texture> Rgba8(const tx::Texture& texture) {
    if (texture.type() != tx::TextureType::Texture2D || texture.width() == 0 ||
        texture.height() == 0)
        return std::nullopt;
    try {
        return texture.copyAsFormat(tx::PixelFormat::RGBA8);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// @p texture's level 0 averaged down by @p factor on both axes.
std::vector<f64> BoxAverage(const tx::Texture& texture, u32 factor) {
    const u32 width = texture.width() / factor;
    const u32 height = texture.height() / factor;
    std::vector<f64> out(static_cast<std::size_t>(width) * height * 4, 0.0);
    const std::span<const u8> texels = texture.mipData(0);
    const f64 scale = 1.0 / (static_cast<f64>(factor) * factor);
    for (u32 y = 0; y < height * factor; ++y) {
        for (u32 x = 0; x < width * factor; ++x) {
            const std::size_t from = (static_cast<std::size_t>(y) * texture.width() + x) * 4;
            const std::size_t to = (static_cast<std::size_t>(y / factor) * width + x / factor) * 4;
            for (int c = 0; c < 4; ++c)
                out[to + c] += texels[from + c] * scale;
        }
    }
    return out;
}

} // namespace

std::string TexturePathKey(std::string_view path) {
    return export_text::Lower(export_text::ForwardSlashes(std::string(path)));
}

std::unordered_set<std::string> M3TexturePathsNamed(const m3::Model& model) {
    std::unordered_set<std::string> named;
    ForEachTexturePath(model, [&](const std::string& path) { named.insert(TexturePathKey(path)); });
    return named;
}

int RenameM3TexturePath(m3::Model& model, std::string_view from, const std::string& to) {
    const std::string fromKey = TexturePathKey(from);
    int renamed = 0;
    ForEachTexturePath(model, [&](std::string& path) {
        if (TexturePathKey(path) == fromKey) {
            path = to;
            ++renamed;
        }
    });
    return renamed;
}

std::vector<bool> MdxTexturesUsed(const mdx::Model& model) {
    std::vector<bool> used(model.textures.size(), false);
    ForEachTextureId(model, [&](const u32& id) {
        if (id < used.size())
            used[id] = true;
    });
    return used;
}

void PruneMdxTextures(mdx::Model& model, const std::vector<bool>& keep) {
    const std::size_t count = model.textures.size();
    std::vector<u32> renumbered(count, 0);
    std::vector<mdx::Texture> kept;
    for (std::size_t i = 0; i < count; ++i) {
        renumbered[i] = static_cast<u32>(kept.size());
        if (i >= keep.size() || keep[i])
            kept.push_back(std::move(model.textures[i]));
    }
    // An id past the table named nothing before and keeps naming nothing.
    ForEachTextureId(model, [&](u32& id) {
        if (id < count)
            id = renumbered[id];
    });
    model.textures = std::move(kept);
}

std::string War3ModTextureName(std::string_view sourcePath) {
    const std::size_t slash = sourcePath.find_last_of("/\\");
    const std::size_t start = slash == std::string_view::npos ? 0 : slash + 1;
    std::size_t end = sourcePath.rfind('.');
    if (end == std::string_view::npos || end < start)
        end = sourcePath.size();
    if (end <= start)
        return {};
    return "war3_" + std::string(sourcePath.substr(start, end - start)) + ".dds";
}

bool SameTexturePicture(const tx::Texture& ours, const tx::Texture& shipped) {
    // Measured on the model textures of a 300-file sample of War3 (Mod)
    // against Warcraft III's own copies: this accepts 127 of 133 SD copies,
    // 115 of 128 of Reforged's HD remasters, and 1 of 3,000 unrelated textures
    // of the same shape (a red star against a white one). The levels catch
    // what correlation cannot see, two flat fills of different colours. A
    // picture that fails is written, as every texture was before.
    constexpr f64 kMinCorrelation = 0.90;
    constexpr f64 kMaxMeanError = 16.0;
    // Less spread than this is a flat fill, compared by its level alone.
    constexpr f64 kFlatSpread = 4.0;
    constexpr f64 kFlatLevelError = 8.0;

    std::optional<tx::Texture> a = Rgba8(ours);
    std::optional<tx::Texture> b = Rgba8(shipped);
    if (!a || !b)
        return false;
    // The same shape at a whole-number scale, compared at the smaller size.
    if (static_cast<u64>(a->width()) * b->height() != static_cast<u64>(a->height()) * b->width())
        return false;
    const bool oursLarger = a->width() >= b->width();
    const tx::Texture& large = oursLarger ? *a : *b;
    const tx::Texture& small = oursLarger ? *b : *a;
    const u32 factor = large.width() / small.width();
    if (large.width() != small.width() * factor || large.height() != small.height() * factor)
        return false;
    const std::vector<f64> lhs = BoxAverage(large, factor);
    const std::vector<f64> rhs = BoxAverage(small, 1);
    const f64 n = static_cast<f64>(lhs.size() / 4);

    f64 colourError = 0.0;
    for (int c = 0; c < 4; ++c) {
        f64 sumA = 0, sumB = 0, sumAA = 0, sumBB = 0, sumAB = 0, error = 0;
        for (std::size_t i = static_cast<std::size_t>(c); i < lhs.size(); i += 4) {
            sumA += lhs[i];
            sumB += rhs[i];
            sumAA += lhs[i] * lhs[i];
            sumBB += rhs[i] * rhs[i];
            sumAB += lhs[i] * rhs[i];
            error += std::abs(lhs[i] - rhs[i]);
        }
        const f64 meanA = sumA / n;
        const f64 meanB = sumB / n;
        const f64 spreadA = std::sqrt((std::max)(sumAA / n - meanA * meanA, 0.0));
        const f64 spreadB = std::sqrt((std::max)(sumBB / n - meanB * meanB, 0.0));
        if (spreadA <= kFlatSpread && spreadB <= kFlatSpread) {
            if (std::abs(meanA - meanB) > kFlatLevelError)
                return false;
        } else if (spreadA <= kFlatSpread || spreadB <= kFlatSpread) {
            return false;
        } else if ((sumAB / n - meanA * meanB) / (spreadA * spreadB) < kMinCorrelation) {
            return false;
        }
        if (c < 3)
            colourError += error / n / 3.0;
        else if (error / n > kMaxMeanError)
            return false;
    }
    return colourError <= kMaxMeanError;
}

} // namespace whiteout::flakes
