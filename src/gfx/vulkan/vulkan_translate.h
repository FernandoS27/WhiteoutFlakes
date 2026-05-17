#pragma once

// gfx::* enum → vk::* enum translators. Unknown values fall back to
// eUndefined / eNone so the caller fails fast at PSO build time.

#include "gfx/gfx.h"

#include <vulkan/vulkan.hpp>

namespace whiteout::flakes::gfx::vulkan {

inline vk::Format ToVkFormat(Format f) {
    switch (f) {
    case Format::Unknown:
        return vk::Format::eUndefined;
    case Format::R8_UNORM:
        return vk::Format::eR8Unorm;
    case Format::R8G8_UNORM:
        return vk::Format::eR8G8Unorm;
    case Format::R8G8B8A8_UNORM:
        return vk::Format::eR8G8B8A8Unorm;
    case Format::R8G8B8A8_UNORM_SRGB:
        return vk::Format::eR8G8B8A8Srgb;
    case Format::R8G8B8A8_UINT:
        return vk::Format::eR8G8B8A8Uint;
    case Format::B8G8R8A8_UNORM:
        return vk::Format::eB8G8R8A8Unorm;
    case Format::B8G8R8A8_UNORM_SRGB:
        return vk::Format::eB8G8R8A8Srgb;
    case Format::R16_UNORM:
        return vk::Format::eR16Unorm;
    case Format::R16G16_UNORM:
        return vk::Format::eR16G16Unorm;
    case Format::R16G16B16A16_UNORM:
        return vk::Format::eR16G16B16A16Unorm;
    case Format::R16G16B16A16_FLOAT:
        return vk::Format::eR16G16B16A16Sfloat;
    case Format::R11G11B10_FLOAT:
        return vk::Format::eB10G11R11UfloatPack32;
    case Format::R16_UINT:
        return vk::Format::eR16Uint;
    case Format::R32_UINT:
        return vk::Format::eR32Uint;
    case Format::R32_FLOAT:
        return vk::Format::eR32Sfloat;
    case Format::R32G32_FLOAT:
        return vk::Format::eR32G32Sfloat;
    case Format::R32G32B32_FLOAT:
        return vk::Format::eR32G32B32Sfloat;
    case Format::R32G32B32A32_FLOAT:
        return vk::Format::eR32G32B32A32Sfloat;
    case Format::D24_UNORM_S8_UINT:
        return vk::Format::eD24UnormS8Uint;
    case Format::D32_FLOAT:
        return vk::Format::eD32Sfloat;
    case Format::D32_FLOAT_S8_UINT:
        return vk::Format::eD32SfloatS8Uint;
    case Format::BC1_UNORM:
        return vk::Format::eBc1RgbaUnormBlock;
    case Format::BC1_UNORM_SRGB:
        return vk::Format::eBc1RgbaSrgbBlock;
    case Format::BC2_UNORM:
        return vk::Format::eBc2UnormBlock;
    case Format::BC2_UNORM_SRGB:
        return vk::Format::eBc2SrgbBlock;
    case Format::BC3_UNORM:
        return vk::Format::eBc3UnormBlock;
    case Format::BC3_UNORM_SRGB:
        return vk::Format::eBc3SrgbBlock;
    case Format::BC4_UNORM:
        return vk::Format::eBc4UnormBlock;
    case Format::BC5_UNORM:
        return vk::Format::eBc5UnormBlock;
    case Format::BC6H_UF16:
        return vk::Format::eBc6HUfloatBlock;
    case Format::BC7_UNORM:
        return vk::Format::eBc7UnormBlock;
    case Format::BC7_UNORM_SRGB:
        return vk::Format::eBc7SrgbBlock;
    }
    return vk::Format::eUndefined;
}

// sRGB → linear pair for the swap chain's dual-view setup.
inline vk::Format LinearPartnerOf(vk::Format f) {
    switch (f) {
    case vk::Format::eR8G8B8A8Srgb:
        return vk::Format::eR8G8B8A8Unorm;
    case vk::Format::eB8G8R8A8Srgb:
        return vk::Format::eB8G8R8A8Unorm;
    case vk::Format::eBc1RgbaSrgbBlock:
        return vk::Format::eBc1RgbaUnormBlock;
    case vk::Format::eBc2SrgbBlock:
        return vk::Format::eBc2UnormBlock;
    case vk::Format::eBc3SrgbBlock:
        return vk::Format::eBc3UnormBlock;
    case vk::Format::eBc7SrgbBlock:
        return vk::Format::eBc7UnormBlock;
    default:
        return f; // already linear
    }
}

inline vk::PrimitiveTopology ToVkTopology(PrimitiveTopology t) {
    switch (t) {
    case PrimitiveTopology::TriangleList:
        return vk::PrimitiveTopology::eTriangleList;
    case PrimitiveTopology::TriangleStrip:
        return vk::PrimitiveTopology::eTriangleStrip;
    case PrimitiveTopology::LineList:
        return vk::PrimitiveTopology::eLineList;
    }
    return vk::PrimitiveTopology::eTriangleList;
}

inline vk::CullModeFlags ToVkCull(CullMode c) {
    switch (c) {
    case CullMode::None:
        return vk::CullModeFlagBits::eNone;
    case CullMode::Back:
        return vk::CullModeFlagBits::eBack;
    case CullMode::Front:
        return vk::CullModeFlagBits::eFront;
    }
    return vk::CullModeFlagBits::eNone;
}

inline vk::PolygonMode ToVkPolygonMode(FillMode m) {
    return (m == FillMode::Wireframe) ? vk::PolygonMode::eLine : vk::PolygonMode::eFill;
}

inline vk::CompareOp ToVkCompareOp(CompareOp c) {
    switch (c) {
    case CompareOp::Never:
        return vk::CompareOp::eNever;
    case CompareOp::Less:
        return vk::CompareOp::eLess;
    case CompareOp::LessEqual:
        return vk::CompareOp::eLessOrEqual;
    case CompareOp::Equal:
        return vk::CompareOp::eEqual;
    case CompareOp::Greater:
        return vk::CompareOp::eGreater;
    case CompareOp::GreaterEqual:
        return vk::CompareOp::eGreaterOrEqual;
    case CompareOp::Always:
        return vk::CompareOp::eAlways;
    }
    return vk::CompareOp::eLessOrEqual;
}

inline vk::BlendFactor ToVkBlendFactor(BlendFactor b) {
    switch (b) {
    case BlendFactor::Zero:
        return vk::BlendFactor::eZero;
    case BlendFactor::One:
        return vk::BlendFactor::eOne;
    case BlendFactor::SrcAlpha:
        return vk::BlendFactor::eSrcAlpha;
    case BlendFactor::InvSrcAlpha:
        return vk::BlendFactor::eOneMinusSrcAlpha;
    case BlendFactor::SrcColor:
        return vk::BlendFactor::eSrcColor;
    case BlendFactor::DstColor:
        return vk::BlendFactor::eDstColor;
    case BlendFactor::InvSrcColor:
        return vk::BlendFactor::eOneMinusSrcColor;
    case BlendFactor::InvDstColor:
        return vk::BlendFactor::eOneMinusDstColor;
    case BlendFactor::DstAlpha:
        return vk::BlendFactor::eDstAlpha;
    case BlendFactor::InvDstAlpha:
        return vk::BlendFactor::eOneMinusDstAlpha;
    }
    return vk::BlendFactor::eOne;
}

inline vk::BlendOp ToVkBlendOp(BlendOp o) {
    return (o == BlendOp::Subtract) ? vk::BlendOp::eSubtract : vk::BlendOp::eAdd;
}

inline vk::Filter ToVkFilter(Filter f) {
    return (f == Filter::Linear) ? vk::Filter::eLinear : vk::Filter::eNearest;
}

inline VkBufferUsageFlags ToVkBufferUsage(BufferUsage u) {
    VkBufferUsageFlags out = 0;
    if (hasFlag(u, BufferUsage::Vertex))
        out |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (hasFlag(u, BufferUsage::Index))
        out |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (hasFlag(u, BufferUsage::Constant))
        out |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (hasFlag(u, BufferUsage::ShaderResource))
        out |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (hasFlag(u, BufferUsage::UnorderedAccess))
        out |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    out |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return out;
}

inline vk::SamplerAddressMode ToVkAddressMode(AddressMode m) {
    switch (m) {
    case AddressMode::Wrap:
        return vk::SamplerAddressMode::eRepeat;
    case AddressMode::Clamp:
        return vk::SamplerAddressMode::eClampToEdge;
    case AddressMode::Mirror:
        return vk::SamplerAddressMode::eMirroredRepeat;
    }
    return vk::SamplerAddressMode::eRepeat;
}

} // namespace whiteout::flakes::gfx::vulkan
