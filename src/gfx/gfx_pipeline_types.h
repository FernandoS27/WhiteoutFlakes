#pragma once

// ============================================================================
// WhiteoutFlakes — internal gfx pipeline descriptors.
//
// Buffer / texture / sampler / PSO descriptors and the enums that feed them.
// Used by the gfx backend (D3D11 / D3D12) and the pipeline layer that builds
// PSOs. NOT part of the public API surface — the public set lives in
// include/whiteout/flakes/gfx_types.h.
// ============================================================================

#include "whiteout/flakes/gfx_types.h"

#include <span>

namespace whiteout::flakes::gfx {

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

}  // namespace whiteout::flakes::gfx
