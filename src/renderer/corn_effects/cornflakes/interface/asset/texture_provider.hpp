#pragma once

#include <cornflakes/interface/core/types.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace whiteout::cornflakes {

enum class TextureSourceFormat : u8 {
    Bgra8,
    Rgba8,
    Fp32Rgba,
    Bgra4,
    Dxt1,
    Fp16Rgba,
};

struct TextureImageDesc {
    std::span<const std::byte> texels;
    u32 width = 0;
    u32 height = 0;
    TextureSourceFormat format = TextureSourceFormat::Bgra8;
    bool srgb = false;
};

class ITextureResourceProvider {
public:
    ITextureResourceProvider() = default;
    virtual ~ITextureResourceProvider() = default;

    ITextureResourceProvider(const ITextureResourceProvider&) = delete;
    ITextureResourceProvider& operator=(const ITextureResourceProvider&) = delete;
    ITextureResourceProvider(ITextureResourceProvider&&) = delete;
    ITextureResourceProvider& operator=(ITextureResourceProvider&&) = delete;

    virtual TextureImageDesc readTexture(std::string_view path) = 0;
};

}
