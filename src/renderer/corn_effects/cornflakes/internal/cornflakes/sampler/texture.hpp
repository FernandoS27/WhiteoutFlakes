#pragma once

/// @file
/// @brief Tiny CPU-side 2D texture sampler with point/bilinear filtering.

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/service/service_types.hpp>

#include <span>

namespace whiteout::cornflakes {

enum class TextureFilter : u8 {
    Point,
    Bilinear,
};

enum class TextureAddressMode : u8 {
    Clamp,
    Repeat,
};

/// @brief CPU-side 2D RGBA8 texture sampler.
struct TextureSampler {
    std::span<const u8> texels;
    u32 width = 0;
    u32 height = 0;
    TextureFilter filter = TextureFilter::Point;
    TextureAddressMode addressMode = TextureAddressMode::Clamp;
};

/// @brief Sample `tex` at UV `(u,v)`. RGBA channels are returned as floats in `[0,1]`.
Float4 sampleTexture2D(const TextureSampler& tex, f32 u, f32 v) noexcept;

} // namespace whiteout::cornflakes
