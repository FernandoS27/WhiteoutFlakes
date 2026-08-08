#pragma once

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/service/service_types.hpp>

#include <cstddef>
#include <span>

namespace whiteout::cornflakes {

enum class TextureFilter : u32 {
    Point = 0,
    Linear = 1,
};

enum class TextureAddressMode : u32 {
    Clamp = 0,
    Wrap = 1,
};

enum class TexelFormat : u8 {
    Bgra8,
    Rgba8,
    Fp32Rgba,
    Fp32Lum,
    Bgra4,
    Dxt1,
};

struct TextureImageData {
    std::span<const std::byte> texels;
    u32 width = 0;
    u32 height = 0;
    TexelFormat format = TexelFormat::Bgra8;
    bool powerOfTwo = false;
    u8 log2Width = 0;
};

Float4 sampleTexture2D(const TextureImageData& img, f32 u, f32 v,
                       TextureFilter filter = TextureFilter::Point,
                       TextureAddressMode addressMode = TextureAddressMode::Clamp) noexcept;

Float4 fetchTextureTexel(const TextureImageData& img, u32 x, u32 y) noexcept;

std::size_t textureStorageBytes(const TextureImageData& img) noexcept;

f32 halfToFloat(u16 h) noexcept;

}
