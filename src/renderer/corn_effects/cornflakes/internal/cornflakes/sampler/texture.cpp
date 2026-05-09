#include <cornflakes/core/determinism.hpp>
#include <cornflakes/sampler/texture.hpp>

#include <cmath>

namespace whiteout::cornflakes {

namespace {

constexpr f32 kByteToNorm = 1.0F / 255.0F;

u32 wrapCoord(i32 coord, u32 size, TextureAddressMode mode) noexcept {
    if (size == 0U) {
        return 0U;
    }
    switch (mode) {
    case TextureAddressMode::Clamp:
        if (coord < 0) {
            return 0U;
        }
        if (static_cast<u32>(coord) >= size) {
            return size - 1U;
        }
        return static_cast<u32>(coord);
    case TextureAddressMode::Repeat: {
        const auto s = static_cast<i32>(size);
        const i32 mod = ((coord % s) + s) % s;
        return static_cast<u32>(mod);
    }
    }
    return 0U;
}

Float4 fetchTexel(const TextureSampler& tex, u32 x, u32 y) noexcept {
    const std::size_t offset = ((static_cast<std::size_t>(y) * tex.width) + x) * 4U;
    const u8 r = tex.texels[offset + 0U];
    const u8 g = tex.texels[offset + 1U];
    const u8 b = tex.texels[offset + 2U];
    const u8 a = tex.texels[offset + 3U];
    return Float4{
        static_cast<f32>(r) * kByteToNorm,
        static_cast<f32>(g) * kByteToNorm,
        static_cast<f32>(b) * kByteToNorm,
        static_cast<f32>(a) * kByteToNorm,
    };
}

Float4 lerp4(const Float4& a, const Float4& b, f32 t) noexcept {
    return Float4{
        a.x + t * (b.x - a.x),
        a.y + t * (b.y - a.y),
        a.z + t * (b.z - a.z),
        a.w + t * (b.w - a.w),
    };
}

} // namespace

Float4 sampleTexture2D(const TextureSampler& tex, f32 u, f32 v) noexcept {
    if (tex.width == 0U || tex.height == 0U) {
        return {};
    }
    if (tex.texels.size() <
        (static_cast<std::size_t>(tex.width) * static_cast<std::size_t>(tex.height) * 4U)) {
        return {};
    }

    if (tex.filter == TextureFilter::Point) {

        const i32 rawX = static_cast<i32>(std::floor(u * static_cast<f32>(tex.width)));
        const i32 rawY = static_cast<i32>(std::floor(v * static_cast<f32>(tex.height)));
        const u32 x = wrapCoord(rawX, tex.width, tex.addressMode);
        const u32 y = wrapCoord(rawY, tex.height, tex.addressMode);
        return fetchTexel(tex, x, y);
    }

    const f32 fu = (u * static_cast<f32>(tex.width)) - 0.5F;
    const f32 fv = (v * static_cast<f32>(tex.height)) - 0.5F;
    const i32 x0 = static_cast<i32>(std::floor(fu));
    const i32 y0 = static_cast<i32>(std::floor(fv));
    const f32 tx = fu - static_cast<f32>(x0);
    const f32 ty = fv - static_cast<f32>(y0);

    const u32 xx0 = wrapCoord(x0, tex.width, tex.addressMode);
    const u32 xx1 = wrapCoord(x0 + 1, tex.width, tex.addressMode);
    const u32 yy0 = wrapCoord(y0, tex.height, tex.addressMode);
    const u32 yy1 = wrapCoord(y0 + 1, tex.height, tex.addressMode);

    const Float4 c00 = fetchTexel(tex, xx0, yy0);
    const Float4 c10 = fetchTexel(tex, xx1, yy0);
    const Float4 c01 = fetchTexel(tex, xx0, yy1);
    const Float4 c11 = fetchTexel(tex, xx1, yy1);

    const Float4 cx0 = lerp4(c00, c10, tx);
    const Float4 cx1 = lerp4(c01, c11, tx);
    return lerp4(cx0, cx1, ty);
}

} // namespace whiteout::cornflakes
