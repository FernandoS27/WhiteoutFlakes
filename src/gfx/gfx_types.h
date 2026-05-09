#pragma once

#include "common_types.h"

#include <span>

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
        case Format::BC1_UNORM: case Format::BC1_UNORM_SRGB:
        case Format::BC2_UNORM: case Format::BC2_UNORM_SRGB:
        case Format::BC3_UNORM: case Format::BC3_UNORM_SRGB:
        case Format::BC4_UNORM:
        case Format::BC5_UNORM:
        case Format::BC6H_UF16:
        case Format::BC7_UNORM: case Format::BC7_UNORM_SRGB:
            return true;
        default:
            return false;
    }
}

inline u32 FormatBytesPerBlock(Format f) {
    switch (f) {
        case Format::R8_UNORM:                return 1;
        case Format::R8G8_UNORM: case Format::R16_UNORM: case Format::R16_UINT: return 2;
        case Format::R8G8B8A8_UNORM: case Format::R8G8B8A8_UNORM_SRGB:
        case Format::R8G8B8A8_UINT:  case Format::B8G8R8A8_UNORM:
        case Format::R16G16_UNORM:   case Format::R32_UINT:
        case Format::R32_FLOAT:      case Format::D24_UNORM_S8_UINT:
        case Format::D32_FLOAT:
        case Format::R11G11B10_FLOAT: return 4;
        case Format::R16G16B16A16_UNORM: case Format::R16G16B16A16_FLOAT:
        case Format::R32G32_FLOAT:   return 8;
        case Format::R32G32B32_FLOAT: return 12;
        case Format::R32G32B32A32_FLOAT: return 16;
        case Format::BC1_UNORM: case Format::BC1_UNORM_SRGB:
        case Format::BC4_UNORM: return 8;
        case Format::BC2_UNORM: case Format::BC2_UNORM_SRGB:
        case Format::BC3_UNORM: case Format::BC3_UNORM_SRGB:
        case Format::BC5_UNORM:
        case Format::BC6H_UF16:
        case Format::BC7_UNORM: case Format::BC7_UNORM_SRGB:
            return 16;
        case Format::Unknown:
        default: return 0;
    }
}

enum class BufferUsage : u32 {
    None            = 0,
    Vertex          = 1 << 0,
    Index           = 1 << 1,
    Constant        = 1 << 2,
    ShaderResource  = 1 << 3,
    UnorderedAccess = 1 << 4,
    CpuWritable     = 1 << 5,
    GpuWritable     = 1 << 6,
};

inline BufferUsage  operator|(BufferUsage  a, BufferUsage  b) { return static_cast<BufferUsage>(static_cast<u32>(a) | static_cast<u32>(b)); }
inline BufferUsage  operator&(BufferUsage  a, BufferUsage  b) { return static_cast<BufferUsage>(static_cast<u32>(a) & static_cast<u32>(b)); }
inline BufferUsage& operator|=(BufferUsage& a, BufferUsage b) { a = a | b; return a; }
inline bool         hasFlag(BufferUsage v, BufferUsage f)     { return (static_cast<u32>(v) & static_cast<u32>(f)) != 0; }

enum class TextureUsage : u32 {
    None           = 0,
    ShaderResource = 1 << 0,
    RenderTarget   = 1 << 1,
    DepthStencil   = 1 << 2,
};

inline TextureUsage  operator|(TextureUsage  a, TextureUsage  b) { return static_cast<TextureUsage>(static_cast<u32>(a) | static_cast<u32>(b)); }
inline TextureUsage  operator&(TextureUsage  a, TextureUsage  b) { return static_cast<TextureUsage>(static_cast<u32>(a) & static_cast<u32>(b)); }
inline TextureUsage& operator|=(TextureUsage& a, TextureUsage b) { a = a | b; return a; }
inline bool          hasFlag(TextureUsage v, TextureUsage f)     { return (static_cast<u32>(v) & static_cast<u32>(f)) != 0; }

enum class PrimitiveTopology { TriangleList, TriangleStrip, LineList };

enum class CullMode  { None, Back, Front };
enum class FillMode  { Solid, Wireframe };
enum class CompareOp { Never, Less, LessEqual, Equal, Greater, GreaterEqual, Always };

enum class BlendFactor { Zero, One, SrcAlpha, InvSrcAlpha, SrcColor, DstColor,
                         InvSrcColor, InvDstColor, DstAlpha, InvDstAlpha };
enum class BlendOp     { Add, Subtract };

enum class Filter      { Point, Linear };
enum class AddressMode { Wrap, Clamp, Mirror };

enum class ShaderStage { Vertex, Pixel, Compute };

struct BufferDesc {
    u64    size          = 0;
    u32    elementStride = 0;
    BufferUsage usage         = BufferUsage::None;
};

struct TextureDesc {
    i32          width     = 0;
    i32          height    = 0;
    i32          mipLevels = 1;

    i32          arraySize = 1;
    Format       format    = Format::R8G8B8A8_UNORM;
    TextureUsage usage     = TextureUsage::ShaderResource;

    bool         isCube    = false;
};

struct SamplerDesc {
    Filter      minFilter = Filter::Linear;
    Filter      magFilter = Filter::Linear;
    AddressMode addressU  = AddressMode::Wrap;
    AddressMode addressV  = AddressMode::Wrap;
    AddressMode addressW  = AddressMode::Wrap;

    bool        comparison       = false;
    CompareOp   comparisonFunc   = CompareOp::LessEqual;
};

enum class ShaderHandle : u64;

struct InputElement {
    const char* semantic      = nullptr;
    u32    semanticIndex = 0;
    Format      format        = Format::Unknown;
    u32    offset        = 0;
    u32    inputSlot     = 0;
};

struct BlendDesc {
    bool        enable          = false;
    BlendFactor srcColor        = BlendFactor::One;
    BlendFactor dstColor        = BlendFactor::Zero;
    BlendOp     opColor         = BlendOp::Add;
    BlendFactor srcAlpha        = BlendFactor::One;
    BlendFactor dstAlpha        = BlendFactor::Zero;
    BlendOp     opAlpha         = BlendOp::Add;
    bool        alphaToCoverage = false;

    bool        colorWrite      = true;
};

struct DepthStencilDesc {
    bool      depthTest    = true;
    bool      depthWrite   = true;
    CompareOp depthCompare = CompareOp::LessEqual;
};

struct RasterizerDesc {
    CullMode cull          = CullMode::Back;
    FillMode fill          = FillMode::Solid;
    bool     frontCCW      = false;
    bool     scissorEnable = false;

    i32      depthBias              = 0;
    f32      slopeScaledDepthBias   = 0.0f;
    f32      depthBiasClamp         = 0.0f;
};

struct GraphicsPipelineDesc {
    ShaderHandle                  vs       = ShaderHandle{0};
    ShaderHandle                  ps       = ShaderHandle{0};
    std::span<const InputElement> inputLayout;
    PrimitiveTopology             topology     = PrimitiveTopology::TriangleList;
    BlendDesc                     blend;
    DepthStencilDesc              depthStencil;
    RasterizerDesc                rasterizer;
    Format                        rtvFormat    = Format::R8G8B8A8_UNORM;
    Format                        dsvFormat    = Format::D24_UNORM_S8_UINT;
};

struct ComputePipelineDesc {
    ShaderHandle cs = ShaderHandle{0};
};

}
