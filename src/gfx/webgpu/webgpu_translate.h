#pragma once

// gfx::* enum → wgpu::* enum translators. Unknown values fall back to
// Undefined / first variant so the caller fails fast at PSO build time.

#include "gfx/gfx.h"

#include <webgpu/webgpu_cpp.h>

namespace whiteout::flakes::gfx::webgpu {

inline wgpu::TextureFormat ToWgpuFormat(Format f) {
    switch (f) {
    case Format::Unknown:
        return wgpu::TextureFormat::Undefined;
    case Format::R8_UNORM:
        return wgpu::TextureFormat::R8Unorm;
    case Format::R8G8_UNORM:
        return wgpu::TextureFormat::RG8Unorm;
    case Format::R8G8B8A8_UNORM:
        return wgpu::TextureFormat::RGBA8Unorm;
    case Format::R8G8B8A8_UNORM_SRGB:
        return wgpu::TextureFormat::RGBA8UnormSrgb;
    case Format::R8G8B8A8_UINT:
        return wgpu::TextureFormat::RGBA8Uint;
    case Format::B8G8R8A8_UNORM:
        return wgpu::TextureFormat::BGRA8Unorm;
    case Format::B8G8R8A8_UNORM_SRGB:
        return wgpu::TextureFormat::BGRA8UnormSrgb;
    case Format::R16_UNORM:
        // WebGPU core spec doesn't expose R16Unorm; closest portable is
        // R16Float. Adapters that expose the `unorm16-texture-formats`
        // feature can opt back in via a future enable; flag for follow-up.
        return wgpu::TextureFormat::R16Float;
    case Format::R16G16_UNORM:
        return wgpu::TextureFormat::RG16Float;
    case Format::R16G16B16A16_UNORM:
        return wgpu::TextureFormat::RGBA16Float;
    case Format::R16G16B16A16_FLOAT:
        return wgpu::TextureFormat::RGBA16Float;
    case Format::R11G11B10_FLOAT:
        return wgpu::TextureFormat::RG11B10Ufloat;
    case Format::R16_UINT:
        return wgpu::TextureFormat::R16Uint;
    case Format::R32_UINT:
        return wgpu::TextureFormat::R32Uint;
    case Format::R32_FLOAT:
        return wgpu::TextureFormat::R32Float;
    case Format::R32G32_FLOAT:
        return wgpu::TextureFormat::RG32Float;
    case Format::R32G32B32_FLOAT:
        // WebGPU has no 3-channel 32-bit texture format. Vertex-buffer
        // formats expose Float32x3, but TextureFormat doesn't — fall back
        // to RGBA32Float (caller intends to use it as a vertex attribute).
        return wgpu::TextureFormat::RGBA32Float;
    case Format::R32G32B32A32_FLOAT:
        return wgpu::TextureFormat::RGBA32Float;
    case Format::D24_UNORM_S8_UINT:
        return wgpu::TextureFormat::Depth24PlusStencil8;
    case Format::D32_FLOAT:
        return wgpu::TextureFormat::Depth32Float;
    case Format::D32_FLOAT_S8_UINT:
        return wgpu::TextureFormat::Depth32FloatStencil8;
    case Format::BC1_UNORM:
        return wgpu::TextureFormat::BC1RGBAUnorm;
    case Format::BC1_UNORM_SRGB:
        return wgpu::TextureFormat::BC1RGBAUnormSrgb;
    case Format::BC2_UNORM:
        return wgpu::TextureFormat::BC2RGBAUnorm;
    case Format::BC2_UNORM_SRGB:
        return wgpu::TextureFormat::BC2RGBAUnormSrgb;
    case Format::BC3_UNORM:
        return wgpu::TextureFormat::BC3RGBAUnorm;
    case Format::BC3_UNORM_SRGB:
        return wgpu::TextureFormat::BC3RGBAUnormSrgb;
    case Format::BC4_UNORM:
        return wgpu::TextureFormat::BC4RUnorm;
    case Format::BC5_UNORM:
        return wgpu::TextureFormat::BC5RGUnorm;
    case Format::BC6H_UF16:
        return wgpu::TextureFormat::BC6HRGBUfloat;
    case Format::BC7_UNORM:
        return wgpu::TextureFormat::BC7RGBAUnorm;
    case Format::BC7_UNORM_SRGB:
        return wgpu::TextureFormat::BC7RGBAUnormSrgb;
    }
    return wgpu::TextureFormat::Undefined;
}

// sRGB → linear pair for the swap chain's dual-view setup (matches
// vulkan_translate.h::LinearPartnerOf).
inline wgpu::TextureFormat LinearPartnerOf(wgpu::TextureFormat f) {
    switch (f) {
    case wgpu::TextureFormat::RGBA8UnormSrgb:
        return wgpu::TextureFormat::RGBA8Unorm;
    case wgpu::TextureFormat::BGRA8UnormSrgb:
        return wgpu::TextureFormat::BGRA8Unorm;
    case wgpu::TextureFormat::BC1RGBAUnormSrgb:
        return wgpu::TextureFormat::BC1RGBAUnorm;
    case wgpu::TextureFormat::BC2RGBAUnormSrgb:
        return wgpu::TextureFormat::BC2RGBAUnorm;
    case wgpu::TextureFormat::BC3RGBAUnormSrgb:
        return wgpu::TextureFormat::BC3RGBAUnorm;
    case wgpu::TextureFormat::BC7RGBAUnormSrgb:
        return wgpu::TextureFormat::BC7RGBAUnorm;
    default:
        return f;
    }
}

// Vertex-attribute formats live in a separate WebGPU enum from texture
// formats. Renderer feeds the same gfx::Format for both, so we translate
// inside webgpu_pipeline.cpp.
inline wgpu::VertexFormat ToWgpuVertexFormat(Format f) {
    switch (f) {
    case Format::R8_UNORM:
        return wgpu::VertexFormat::Unorm8x2; // no Unorm8x1; pad on the shader side
    case Format::R8G8_UNORM:
        return wgpu::VertexFormat::Unorm8x2;
    case Format::R8G8B8A8_UNORM:
        return wgpu::VertexFormat::Unorm8x4;
    case Format::R8G8B8A8_UINT:
        return wgpu::VertexFormat::Uint8x4;
    case Format::R16G16_UNORM:
        return wgpu::VertexFormat::Unorm16x2;
    case Format::R16G16B16A16_UNORM:
        return wgpu::VertexFormat::Unorm16x4;
    case Format::R16G16B16A16_FLOAT:
        return wgpu::VertexFormat::Float16x4;
    case Format::R32_FLOAT:
        return wgpu::VertexFormat::Float32;
    case Format::R32G32_FLOAT:
        return wgpu::VertexFormat::Float32x2;
    case Format::R32G32B32_FLOAT:
        return wgpu::VertexFormat::Float32x3;
    case Format::R32G32B32A32_FLOAT:
        return wgpu::VertexFormat::Float32x4;
    case Format::R32_UINT:
        return wgpu::VertexFormat::Uint32;
    default:
        return wgpu::VertexFormat::Float32x4;
    }
}

inline wgpu::PrimitiveTopology ToWgpuTopology(PrimitiveTopology t) {
    switch (t) {
    case PrimitiveTopology::TriangleList:
        return wgpu::PrimitiveTopology::TriangleList;
    case PrimitiveTopology::TriangleStrip:
        return wgpu::PrimitiveTopology::TriangleStrip;
    case PrimitiveTopology::LineList:
        return wgpu::PrimitiveTopology::LineList;
    }
    return wgpu::PrimitiveTopology::TriangleList;
}

inline wgpu::CullMode ToWgpuCull(CullMode c) {
    switch (c) {
    case CullMode::None:
        return wgpu::CullMode::None;
    case CullMode::Back:
        return wgpu::CullMode::Back;
    case CullMode::Front:
        return wgpu::CullMode::Front;
    }
    return wgpu::CullMode::None;
}

inline wgpu::CompareFunction ToWgpuCompare(CompareOp c) {
    switch (c) {
    case CompareOp::Never:
        return wgpu::CompareFunction::Never;
    case CompareOp::Less:
        return wgpu::CompareFunction::Less;
    case CompareOp::LessEqual:
        return wgpu::CompareFunction::LessEqual;
    case CompareOp::Equal:
        return wgpu::CompareFunction::Equal;
    case CompareOp::Greater:
        return wgpu::CompareFunction::Greater;
    case CompareOp::GreaterEqual:
        return wgpu::CompareFunction::GreaterEqual;
    case CompareOp::Always:
        return wgpu::CompareFunction::Always;
    }
    return wgpu::CompareFunction::LessEqual;
}

inline wgpu::BlendFactor ToWgpuBlendFactor(BlendFactor b) {
    switch (b) {
    case BlendFactor::Zero:
        return wgpu::BlendFactor::Zero;
    case BlendFactor::One:
        return wgpu::BlendFactor::One;
    case BlendFactor::SrcAlpha:
        return wgpu::BlendFactor::SrcAlpha;
    case BlendFactor::InvSrcAlpha:
        return wgpu::BlendFactor::OneMinusSrcAlpha;
    case BlendFactor::SrcColor:
        return wgpu::BlendFactor::Src;
    case BlendFactor::DstColor:
        return wgpu::BlendFactor::Dst;
    case BlendFactor::InvSrcColor:
        return wgpu::BlendFactor::OneMinusSrc;
    case BlendFactor::InvDstColor:
        return wgpu::BlendFactor::OneMinusDst;
    case BlendFactor::DstAlpha:
        return wgpu::BlendFactor::DstAlpha;
    case BlendFactor::InvDstAlpha:
        return wgpu::BlendFactor::OneMinusDstAlpha;
    }
    return wgpu::BlendFactor::One;
}

inline wgpu::BlendOperation ToWgpuBlendOp(BlendOp o) {
    return (o == BlendOp::Subtract) ? wgpu::BlendOperation::Subtract : wgpu::BlendOperation::Add;
}

inline wgpu::FilterMode ToWgpuFilter(Filter f) {
    return (f == Filter::Linear) ? wgpu::FilterMode::Linear : wgpu::FilterMode::Nearest;
}

inline wgpu::MipmapFilterMode ToWgpuMipFilter(Filter f) {
    return (f == Filter::Linear) ? wgpu::MipmapFilterMode::Linear
                                 : wgpu::MipmapFilterMode::Nearest;
}

inline wgpu::AddressMode ToWgpuAddress(AddressMode m) {
    switch (m) {
    case AddressMode::Wrap:
        return wgpu::AddressMode::Repeat;
    case AddressMode::Clamp:
        return wgpu::AddressMode::ClampToEdge;
    case AddressMode::Mirror:
        return wgpu::AddressMode::MirrorRepeat;
    }
    return wgpu::AddressMode::Repeat;
}

inline wgpu::BufferUsage ToWgpuBufferUsage(BufferUsage u) {
    wgpu::BufferUsage out = wgpu::BufferUsage::None;
    if (hasFlag(u, BufferUsage::Vertex))
        out |= wgpu::BufferUsage::Vertex;
    if (hasFlag(u, BufferUsage::Index))
        out |= wgpu::BufferUsage::Index;
    if (hasFlag(u, BufferUsage::Constant))
        out |= wgpu::BufferUsage::Uniform;
    if (hasFlag(u, BufferUsage::ShaderResource))
        out |= wgpu::BufferUsage::Storage;
    if (hasFlag(u, BufferUsage::UnorderedAccess))
        out |= wgpu::BufferUsage::Storage;
    // CopySrc/CopyDst on every buffer keeps the renderer's free-form
    // CopyBuffer/UpdateBuffer behaviour matching D3D/VK.
    out |= wgpu::BufferUsage::CopySrc | wgpu::BufferUsage::CopyDst;
    return out;
}

} // namespace whiteout::flakes::gfx::webgpu
