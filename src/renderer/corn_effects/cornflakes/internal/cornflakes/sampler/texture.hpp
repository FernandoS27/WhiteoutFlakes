#pragma once

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

struct TextureSampler {
    std::span<const u8> texels;
    u32 width = 0;
    u32 height = 0;
    TextureFilter filter = TextureFilter::Point;
    TextureAddressMode addressMode = TextureAddressMode::Clamp;
};

Float4 sampleTexture2D(const TextureSampler& tex, f32 u, f32 v) noexcept;

}
