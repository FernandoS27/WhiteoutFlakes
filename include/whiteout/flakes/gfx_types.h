#pragma once

/// @file gfx_types.h
/// @brief Public gfx-layer enums + the two helpers texture-adapter authors
///        need (block-compressed test + per-block byte count).
///
/// Pipeline descriptors (`BufferDesc`, `TextureDesc`, `BlendDesc`,
/// `GraphicsPipelineDesc`, …) live in `src/gfx/gfx_pipeline_types.h` —
/// they're implementation detail of the gfx abstraction and not part of
/// the public surface.

#include "types.h"

namespace whiteout::flakes::gfx {

/// @brief Selects the GPU backend at device-init time.
///
/// The viewer picks via CLI flag, INI setting, or the per-platform
/// default. D3D11 / D3D12 are Windows-only; Vulkan is cross-platform
/// (MoltenVK on macOS). WebGPU is opt-in (CMake -DWDX_ENABLE_WEBGPU=ON)
/// and uses Dawn's native runtime — see `WebGPU.md` for the design.
/// Metal is macOS-only and opt-in (-DWDX_ENABLE_METAL=ON, default ON
/// on Apple platforms). It is the preferred native backend on macOS.
enum class GfxApi { D3D11, D3D12, Vulkan, WebGPU, Metal };

/// @brief Pixel format used by `TextureData` and swap-chain surfaces.
///
/// Mirrors the D3D / VK / MoltenVK common subset; the gfx backends map
/// each value to their native enum. `Unknown` is the default-constructed
/// sentinel and is rejected by every backend.
enum class Format : u16 {
    Unknown,

    R8_UNORM,
    R8G8_UNORM,
    R8G8B8A8_UNORM,
    R8G8B8A8_UNORM_SRGB,
    R8G8B8A8_UINT,
    B8G8R8A8_UNORM,
    B8G8R8A8_UNORM_SRGB,

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

/// @brief `true` if @p f is one of the BCn block-compressed families.
///
/// Callers uploading texture data use this to decide whether to pass
/// `width * bpp` or `(width / 4) * blockBytes` as the row pitch.
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

/// @brief Bytes per addressable block.
///
/// For block-compressed formats this is the bytes per 4x4 texel block
/// (8 for BC1/BC4, 16 for BC2/BC3/BC5/BC6H/BC7). For uncompressed
/// formats it's the bytes per single pixel. Returns 0 for `Unknown`.
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
    case Format::B8G8R8A8_UNORM_SRGB:
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
