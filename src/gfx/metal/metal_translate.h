#pragma once

// gfx::* enum → MTL* enum translators. Header-only inlines, included
// from .mm files only (uses Obj-C types). Unknown values fall back to
// MTLPixelFormatInvalid / first variant so callers fail fast at PSO build.

#include "gfx/gfx.h"

#import <Metal/Metal.h>

namespace whiteout::flakes::gfx::metal {

inline MTLPixelFormat ToMtlPixelFormat(Format f) {
    switch (f) {
    case Format::Unknown:
        return MTLPixelFormatInvalid;
    case Format::R8_UNORM:
        return MTLPixelFormatR8Unorm;
    case Format::R8G8_UNORM:
        return MTLPixelFormatRG8Unorm;
    case Format::R8G8B8A8_UNORM:
        return MTLPixelFormatRGBA8Unorm;
    case Format::R8G8B8A8_UNORM_SRGB:
        return MTLPixelFormatRGBA8Unorm_sRGB;
    case Format::R8G8B8A8_UINT:
        return MTLPixelFormatRGBA8Uint;
    case Format::B8G8R8A8_UNORM:
        return MTLPixelFormatBGRA8Unorm;
    case Format::B8G8R8A8_UNORM_SRGB:
        return MTLPixelFormatBGRA8Unorm_sRGB;
    case Format::R16_UNORM:
        return MTLPixelFormatR16Unorm;
    case Format::R16G16_UNORM:
        return MTLPixelFormatRG16Unorm;
    case Format::R16G16B16A16_UNORM:
        return MTLPixelFormatRGBA16Unorm;
    case Format::R16G16B16A16_FLOAT:
        return MTLPixelFormatRGBA16Float;
    case Format::R11G11B10_FLOAT:
        return MTLPixelFormatRG11B10Float;
    case Format::R16_UINT:
        return MTLPixelFormatR16Uint;
    case Format::R32_UINT:
        return MTLPixelFormatR32Uint;
    case Format::R32_FLOAT:
        return MTLPixelFormatR32Float;
    case Format::R32G32_FLOAT:
        return MTLPixelFormatRG32Float;
    case Format::R32G32B32_FLOAT:
        // Metal has no 3-channel 32-bit MTLPixelFormat. The caller is
        // almost certainly using it as a vertex format — see
        // ToMtlVertexFormat for the matching VS-side mapping. Fall back
        // to RGBA32Float so a texture allocation at least succeeds.
        return MTLPixelFormatRGBA32Float;
    case Format::R32G32B32A32_FLOAT:
        return MTLPixelFormatRGBA32Float;
    case Format::D24_UNORM_S8_UINT:
        // Not supported on Apple Silicon; depth32Stencil8 is the
        // universally available alternative. Callers should prefer
        // PreferredDepthStencilFormat() which already returns
        // D32_FLOAT_S8_UINT on Metal.
        return MTLPixelFormatDepth32Float_Stencil8;
    case Format::D32_FLOAT:
        return MTLPixelFormatDepth32Float;
    case Format::D32_FLOAT_S8_UINT:
        return MTLPixelFormatDepth32Float_Stencil8;
    case Format::BC1_UNORM:
        return MTLPixelFormatBC1_RGBA;
    case Format::BC1_UNORM_SRGB:
        return MTLPixelFormatBC1_RGBA_sRGB;
    case Format::BC2_UNORM:
        return MTLPixelFormatBC2_RGBA;
    case Format::BC2_UNORM_SRGB:
        return MTLPixelFormatBC2_RGBA_sRGB;
    case Format::BC3_UNORM:
        return MTLPixelFormatBC3_RGBA;
    case Format::BC3_UNORM_SRGB:
        return MTLPixelFormatBC3_RGBA_sRGB;
    case Format::BC4_UNORM:
        return MTLPixelFormatBC4_RUnorm;
    case Format::BC5_UNORM:
        return MTLPixelFormatBC5_RGUnorm;
    case Format::BC6H_UF16:
        return MTLPixelFormatBC6H_RGBUfloat;
    case Format::BC7_UNORM:
        return MTLPixelFormatBC7_RGBAUnorm;
    case Format::BC7_UNORM_SRGB:
        return MTLPixelFormatBC7_RGBAUnorm_sRGB;
    }
    return MTLPixelFormatInvalid;
}

// sRGB → linear pair for the swap chain's dual-view setup. Mirrors
// vulkan_translate.h::LinearPartnerOf / webgpu_translate.h::LinearPartnerOf.
inline MTLPixelFormat LinearPartnerOf(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatRGBA8Unorm_sRGB:
        return MTLPixelFormatRGBA8Unorm;
    case MTLPixelFormatBGRA8Unorm_sRGB:
        return MTLPixelFormatBGRA8Unorm;
    case MTLPixelFormatBC1_RGBA_sRGB:
        return MTLPixelFormatBC1_RGBA;
    case MTLPixelFormatBC2_RGBA_sRGB:
        return MTLPixelFormatBC2_RGBA;
    case MTLPixelFormatBC3_RGBA_sRGB:
        return MTLPixelFormatBC3_RGBA;
    case MTLPixelFormatBC7_RGBAUnorm_sRGB:
        return MTLPixelFormatBC7_RGBAUnorm;
    default:
        return f;
    }
}

// Inverse of ToMtlPixelFormat for swap-chain-capable formats only.
// Used by GetSwapChainFormat to report back what CAMetalLayer accepted.
inline Format MtlPixelFormatToGfx(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatRGBA8Unorm:
        return Format::R8G8B8A8_UNORM;
    case MTLPixelFormatRGBA8Unorm_sRGB:
        return Format::R8G8B8A8_UNORM_SRGB;
    case MTLPixelFormatBGRA8Unorm:
        return Format::B8G8R8A8_UNORM;
    case MTLPixelFormatBGRA8Unorm_sRGB:
        return Format::B8G8R8A8_UNORM_SRGB;
    default:
        return Format::Unknown;
    }
}

inline bool IsDepthStencilFormat(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatDepth32Float:
    case MTLPixelFormatDepth32Float_Stencil8:
    case MTLPixelFormatStencil8:
    case MTLPixelFormatDepth16Unorm:
        return true;
    default:
        return false;
    }
}

inline bool HasStencilAspect(MTLPixelFormat f) {
    return f == MTLPixelFormatDepth32Float_Stencil8 || f == MTLPixelFormatStencil8;
}

} // namespace whiteout::flakes::gfx::metal
