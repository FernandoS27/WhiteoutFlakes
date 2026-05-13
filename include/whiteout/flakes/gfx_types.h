#pragma once

// ============================================================================
// WhiteoutFlakes — public gfx-layer types.
//
// The narrow set of GPU-related enums and helpers exposed to consumers:
//   - GfxApi: choice of backend at device init time.
//   - Format: pixel format used by TextureData.
//   - IsBlockCompressed / FormatBytesPerBlock: helpers adapter authors need
//     when uploading or sizing texture data.
//
// Pipeline descriptors (BufferDesc, TextureDesc, BlendDesc,
// GraphicsPipelineDesc, etc.) live in src/gfx/gfx_pipeline_types.h — they're
// internal implementation detail of the gfx abstraction layer and not part
// of the public surface.
// ============================================================================

#include "types.h"

namespace whiteout::flakes::gfx {

enum class GfxApi { D3D11, D3D12, Vulkan };

enum class Format : u16 {
    Unknown,

    R8_UNORM,
    R8G8_UNORM,
    R8G8B8A8_UNORM,
    R8G8B8A8_UNORM_SRGB,
    R8G8B8A8_UINT,
    B8G8R8A8_UNORM,

    R16_UNORM,
    R16G16_UNORM,
    R16G16B16A16_UNORM,
    R16G16B16A16_FLOAT,

    R11G11B10_FLOAT,

    R16_UINT,
    R32_UINT,

    R32_FLOAT,
    R32G32_FLOAT,
    R32G32B32_FLOAT,
    R32G32B32A32_FLOAT,

    D24_UNORM_S8_UINT,
    D32_FLOAT,
    D32_FLOAT_S8_UINT,

    BC1_UNORM,
    BC1_UNORM_SRGB,
    BC2_UNORM,
    BC2_UNORM_SRGB,
    BC3_UNORM,
    BC3_UNORM_SRGB,
    BC4_UNORM,
    BC5_UNORM,
    BC6H_UF16,
    BC7_UNORM,
    BC7_UNORM_SRGB,
};

inline bool IsBlockCompressed(Format f) {
    switch (f) {
    case Format::BC1_UNORM:
    case Format::BC1_UNORM_SRGB:
    case Format::BC2_UNORM:
    case Format::BC2_UNORM_SRGB:
    case Format::BC3_UNORM:
    case Format::BC3_UNORM_SRGB:
    case Format::BC4_UNORM:
    case Format::BC5_UNORM:
    case Format::BC6H_UF16:
    case Format::BC7_UNORM:
    case Format::BC7_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

inline u32 FormatBytesPerBlock(Format f) {
    switch (f) {
    case Format::R8_UNORM:
        return 1;
    case Format::R8G8_UNORM:
    case Format::R16_UNORM:
    case Format::R16_UINT:
        return 2;
    case Format::R8G8B8A8_UNORM:
    case Format::R8G8B8A8_UNORM_SRGB:
    case Format::R8G8B8A8_UINT:
    case Format::B8G8R8A8_UNORM:
    case Format::R16G16_UNORM:
    case Format::R32_UINT:
    case Format::R32_FLOAT:
    case Format::D24_UNORM_S8_UINT:
    case Format::D32_FLOAT:
    case Format::R11G11B10_FLOAT:
        return 4;
    case Format::R16G16B16A16_UNORM:
    case Format::R16G16B16A16_FLOAT:
    case Format::R32G32_FLOAT:
        return 8;
    case Format::R32G32B32_FLOAT:
        return 12;
    case Format::R32G32B32A32_FLOAT:
        return 16;
    case Format::BC1_UNORM:
    case Format::BC1_UNORM_SRGB:
    case Format::BC4_UNORM:
        return 8;
    case Format::BC2_UNORM:
    case Format::BC2_UNORM_SRGB:
    case Format::BC3_UNORM:
    case Format::BC3_UNORM_SRGB:
    case Format::BC5_UNORM:
    case Format::BC6H_UF16:
    case Format::BC7_UNORM:
    case Format::BC7_UNORM_SRGB:
        return 16;
    case Format::Unknown:
    default:
        return 0;
    }
}

} // namespace whiteout::flakes::gfx
