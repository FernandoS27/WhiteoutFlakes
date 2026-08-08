#include <cornflakes/core/determinism.hpp>
#include <cornflakes/sampler/texture.hpp>

#include <cmath>
#include <cstring>

namespace whiteout::cornflakes {

namespace {

constexpr f32 kByteToNorm = 1.0F / 255.0F;

std::size_t bytesPerTexel(TexelFormat f) noexcept {
    switch (f) {
    case TexelFormat::Bgra8:
    case TexelFormat::Rgba8:
        return 4U;
    case TexelFormat::Fp32Rgba:
        return 16U;
    case TexelFormat::Fp32Lum:
        return 4U;
    case TexelFormat::Bgra4:
        return 2U;
    case TexelFormat::Dxt1:
        return 0U;
    }
    return 4U;
}

f32 loadF32(const std::byte* p) noexcept {
    f32 v = 0.0F;
    std::memcpy(&v, p, sizeof(f32));
    return v;
}

u16 loadU16(const std::byte* p) noexcept {
    u16 v = 0;
    std::memcpy(&v, p, sizeof(u16));
    return v;
}

u32 loadU32(const std::byte* p) noexcept {
    u32 v = 0;
    std::memcpy(&v, p, sizeof(u32));
    return v;
}

Float4 decodeBgra4(u16 v) noexcept {
    constexpr f32 kInv15 = 1.0F / 15.0F;
    return Float4{
        static_cast<f32>((v >> 8) & 0xFU) * kInv15,
        static_cast<f32>((v >> 4) & 0xFU) * kInv15,
        static_cast<f32>(v & 0xFU) * kInv15,
        static_cast<f32>((v >> 12) & 0xFU) * kInv15,
    };
}

Float4 decodeDxt1(const std::byte* block, u32 subIndex) noexcept {
    const u16 c0 = loadU16(block);
    const u16 c1 = loadU16(block + 2);
    const u32 line = loadU32(block + 4);
    const u32 lerpBits = (line >> (2U * subIndex)) & 3U;
    const bool punchthrough = c0 <= c1;

    f32 w0 = 0.0F;
    f32 w1 = 0.0F;
    bool opaque = true;
    switch (lerpBits) {
    case 0U:
        w0 = 1.0F;
        break;
    case 1U:
        w1 = 1.0F;
        break;
    case 2U:
        if (punchthrough) {
            w0 = 0.5F;
            w1 = 0.5F;
        } else {
            w0 = 2.0F / 3.0F;
            w1 = 1.0F / 3.0F;
        }
        break;
    default:
        if (punchthrough) {
            opaque = false;
        } else {
            w0 = 1.0F / 3.0F;
            w1 = 2.0F / 3.0F;
        }
        break;
    }

    const auto chan = [&](u32 shift, u32 mask, f32 inv) noexcept {
        const f32 a = static_cast<f32>((c0 >> shift) & mask);
        const f32 b = static_cast<f32>((c1 >> shift) & mask);
        return ((a * w0) + (b * w1)) * inv;
    };
    return Float4{
        chan(11U, 0x1FU, 1.0F / 31.0F),
        chan(5U, 0x3FU, 1.0F / 63.0F),
        chan(0U, 0x1FU, 1.0F / 31.0F),
        opaque ? (w0 + w1) : 0.0F,
    };
}

Float4 fetchTexelXY(const TextureImageData& img, u32 x, u32 y) noexcept {
    if (x >= img.width || y >= img.height) {
        return {};
    }
    if (img.format == TexelFormat::Dxt1) {
        const std::size_t blocksPerRow = img.width / 4U;
        const std::size_t off =
            8U * ((blocksPerRow * static_cast<std::size_t>(y / 4U)) + (x / 4U));
        if (off + 8U > img.texels.size()) {
            return {};
        }
        return decodeDxt1(img.texels.data() + off, (x & 3U) + (4U * (y & 3U)));
    }
    const std::size_t stride = bytesPerTexel(img.format);
    const auto cell = (static_cast<std::size_t>(y) * img.width) + x;
    const std::size_t off = cell * stride;
    if (off + stride > img.texels.size()) {
        return {};
    }
    const std::byte* p = img.texels.data() + off;
    switch (img.format) {
    case TexelFormat::Bgra4:
        return decodeBgra4(loadU16(p));
    case TexelFormat::Dxt1:
        return {};

    case TexelFormat::Bgra8:
        return Float4{
            static_cast<f32>(std::to_integer<u8>(p[2])) * kByteToNorm,
            static_cast<f32>(std::to_integer<u8>(p[1])) * kByteToNorm,
            static_cast<f32>(std::to_integer<u8>(p[0])) * kByteToNorm,
            static_cast<f32>(std::to_integer<u8>(p[3])) * kByteToNorm,
        };
    case TexelFormat::Rgba8:
        return Float4{
            static_cast<f32>(std::to_integer<u8>(p[0])) * kByteToNorm,
            static_cast<f32>(std::to_integer<u8>(p[1])) * kByteToNorm,
            static_cast<f32>(std::to_integer<u8>(p[2])) * kByteToNorm,
            static_cast<f32>(std::to_integer<u8>(p[3])) * kByteToNorm,
        };
    case TexelFormat::Fp32Rgba:
        return Float4{
            loadF32(p),
            loadF32(p + 4),
            loadF32(p + 8),
            loadF32(p + 12),
        };
    case TexelFormat::Fp32Lum: {
        const f32 l = loadF32(p);
        return Float4{l, 0.0F, 0.0F, 0.0F};
    }
    }
    return {};
}

f32 fracSign(f32 v) noexcept { return v - std::trunc(v); }

f32 fracFloor(f32 v) noexcept { return v - std::floor(v); }

u32 cellCoord(f32 scaled, u32 dim) noexcept {
    const f32 hi = static_cast<f32>(dim - 1U);
    const f32 c = (scaled < 0.0F) ? 0.0F : ((scaled > hi) ? hi : scaled);
    return static_cast<u32>(static_cast<i32>(c));
}

Float4 lerp4(const Float4& a, const Float4& b, f32 t) noexcept {
    return Float4{
        ((b.x - a.x) * t) + a.x,
        ((b.y - a.y) * t) + a.y,
        ((b.z - a.z) * t) + a.z,
        ((b.w - a.w) * t) + a.w,
    };
}

Float4 samplePoint(const TextureImageData& img, f32 u, f32 v, TextureAddressMode mode) noexcept {
    f32 su = u;
    f32 sv = v;
    if (mode == TextureAddressMode::Wrap) {
        const bool negU = su < 0.0F;
        const bool negV = sv < 0.0F;
        su = fracSign(su);
        sv = fracSign(sv);
        if (negU) {
            su += 1.0F + (0.5F / static_cast<f32>(img.width));
        }
        if (negV) {
            sv += 1.0F + (0.5F / static_cast<f32>(img.height));
        }
    }
    const u32 x = cellCoord(su * static_cast<f32>(img.width), img.width);
    const u32 y = cellCoord(sv * static_cast<f32>(img.height), img.height);
    return fetchTexelXY(img, x, y);
}

Float4 sampleLinear(const TextureImageData& img, f32 u, f32 v, TextureAddressMode mode) noexcept {
    const f32 hu = u - (0.5F / static_cast<f32>(img.width));
    const f32 hv = v - (0.5F / static_cast<f32>(img.height));
    const f32 sx = hu * static_cast<f32>(img.width);
    const f32 sy = hv * static_cast<f32>(img.height);

    const i32 bx = static_cast<i32>(std::trunc(sx));
    const i32 by = static_cast<i32>(std::trunc(sy));
    i32 x0 = bx + (hu < 0.0F ? -1 : 0);
    i32 x1 = bx + (hu < 0.0F ? 0 : 1);
    i32 y0 = by + (hv < 0.0F ? -1 : 0);
    i32 y1 = by + (hv < 0.0F ? 0 : 1);

    if (mode == TextureAddressMode::Clamp) {
        const auto cl = [](i32 c, u32 dim) noexcept {
            const auto hi = static_cast<i32>(dim - 1U);
            return (c < 0) ? 0 : ((c > hi) ? hi : c);
        };
        x0 = cl(x0, img.width);
        x1 = cl(x1, img.width);
        y0 = cl(y0, img.height);
        y1 = cl(y1, img.height);
    }
    const auto mx = static_cast<i32>(img.width - 1U);
    const auto my = static_cast<i32>(img.height - 1U);
    const auto ux0 = static_cast<u32>(x0 & mx);
    const auto ux1 = static_cast<u32>(x1 & mx);
    const auto uy0 = static_cast<u32>(y0 & my);
    const auto uy1 = static_cast<u32>(y1 & my);

    const f32 cx = fracFloor(sx);
    const f32 cy = fracFloor(sy);

    const Float4 c00 = fetchTexelXY(img, ux0, uy0);
    const Float4 c10 = fetchTexelXY(img, ux1, uy0);
    const Float4 c01 = fetchTexelXY(img, ux0, uy1);
    const Float4 c11 = fetchTexelXY(img, ux1, uy1);

    return lerp4(lerp4(c00, c10, cx), lerp4(c01, c11, cx), cy);
}

}

std::size_t textureStorageBytes(const TextureImageData& img) noexcept {
    if (img.width == 0U || img.height == 0U) {
        return 0U;
    }
    if (img.format == TexelFormat::Dxt1) {
        const std::size_t bx = (static_cast<std::size_t>(img.width) + 3U) / 4U;
        const std::size_t by = (static_cast<std::size_t>(img.height) + 3U) / 4U;
        return bx * by * 8U;
    }
    return static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height) *
           bytesPerTexel(img.format);
}

Float4 fetchTextureTexel(const TextureImageData& img, u32 x, u32 y) noexcept {
    return fetchTexelXY(img, x, y);
}

f32 halfToFloat(u16 h) noexcept {
    const u32 sign = static_cast<u32>(h >> 15) << 31;
    const u32 exp = (h >> 10) & 0x1FU;
    const u32 mant = h & 0x3FFU;
    u32 bits = 0U;
    if (exp == 0U) {
        if (mant == 0U) {
            bits = sign;
        } else {
            u32 e = 0;
            u32 m = mant;
            while ((m & 0x400U) == 0U) {
                m <<= 1U;
                ++e;
            }
            m &= 0x3FFU;
            bits = sign | ((127U - 15U - e + 1U) << 23U) | (m << 13U);
        }
    } else if (exp == 0x1FU) {
        bits = sign | 0x7F800000U | (mant << 13U);
    } else {
        bits = sign | ((exp - 15U + 127U) << 23U) | (mant << 13U);
    }
    f32 out = 0.0F;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

Float4 sampleTexture2D(const TextureImageData& img, f32 u, f32 v, TextureFilter filter,
                       TextureAddressMode addressMode) noexcept {
    if (img.width == 0U || img.height == 0U) {
        return {};
    }
    if (img.texels.size() < textureStorageBytes(img)) {
        return {};
    }
    const bool linearCapable = img.format != TexelFormat::Dxt1;
    if (filter == TextureFilter::Linear && img.powerOfTwo && linearCapable) {
        return sampleLinear(img, u, v, addressMode);
    }
    return samplePoint(img, u, v, addressMode);
}

}
