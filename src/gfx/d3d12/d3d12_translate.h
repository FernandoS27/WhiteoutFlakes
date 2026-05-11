#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cassert>

#include "gfx/gfx_pipeline_types.h"

namespace whiteout::flakes::gfx::d3d12 {

template<typename T>
inline void SafeRelease(T*& ptr) {
    if (ptr) { ptr->Release(); ptr = nullptr; }
}

inline DXGI_FORMAT ToDXGI(Format f) {
    switch (f) {

        case Format::R8_UNORM:           return DXGI_FORMAT_R8_UNORM;
        case Format::R8G8_UNORM:         return DXGI_FORMAT_R8G8_UNORM;
        case Format::R8G8B8A8_UNORM:     return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case Format::R8G8B8A8_UINT:      return DXGI_FORMAT_R8G8B8A8_UINT;
        case Format::B8G8R8A8_UNORM:     return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::R16_UNORM:          return DXGI_FORMAT_R16_UNORM;
        case Format::R16G16_UNORM:       return DXGI_FORMAT_R16G16_UNORM;
        case Format::R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case Format::R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::R11G11B10_FLOAT:    return DXGI_FORMAT_R11G11B10_FLOAT;
        case Format::R16_UINT:           return DXGI_FORMAT_R16_UINT;
        case Format::R32_UINT:           return DXGI_FORMAT_R32_UINT;
        case Format::R32_FLOAT:          return DXGI_FORMAT_R32_FLOAT;
        case Format::R32G32_FLOAT:       return DXGI_FORMAT_R32G32_FLOAT;
        case Format::R32G32B32_FLOAT:    return DXGI_FORMAT_R32G32B32_FLOAT;
        case Format::R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;

        case Format::D24_UNORM_S8_UINT:  return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case Format::D32_FLOAT:          return DXGI_FORMAT_D32_FLOAT;
        case Format::D32_FLOAT_S8_UINT:  return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;

        case Format::BC1_UNORM:          return DXGI_FORMAT_BC1_UNORM;
        case Format::BC1_UNORM_SRGB:     return DXGI_FORMAT_BC1_UNORM_SRGB;
        case Format::BC2_UNORM:          return DXGI_FORMAT_BC2_UNORM;
        case Format::BC2_UNORM_SRGB:     return DXGI_FORMAT_BC2_UNORM_SRGB;
        case Format::BC3_UNORM:          return DXGI_FORMAT_BC3_UNORM;
        case Format::BC3_UNORM_SRGB:     return DXGI_FORMAT_BC3_UNORM_SRGB;
        case Format::BC4_UNORM:          return DXGI_FORMAT_BC4_UNORM;
        case Format::BC5_UNORM:          return DXGI_FORMAT_BC5_UNORM;
        case Format::BC6H_UF16:          return DXGI_FORMAT_BC6H_UF16;
        case Format::BC7_UNORM:          return DXGI_FORMAT_BC7_UNORM;
        case Format::BC7_UNORM_SRGB:     return DXGI_FORMAT_BC7_UNORM_SRGB;

        default:                         return DXGI_FORMAT_UNKNOWN;
    }
}

inline D3D12_PRIMITIVE_TOPOLOGY ToD3D12Topology(PrimitiveTopology t) {
    switch (t) {
        case PrimitiveTopology::TriangleList:  return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case PrimitiveTopology::TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case PrimitiveTopology::LineList:      return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        default:                               return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

inline D3D12_PRIMITIVE_TOPOLOGY_TYPE ToD3D12TopologyType(PrimitiveTopology t) {
    switch (t) {
        case PrimitiveTopology::TriangleList:  return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        case PrimitiveTopology::TriangleStrip: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        case PrimitiveTopology::LineList:      return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        default:                               return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

inline D3D12_CULL_MODE ToD3D12(CullMode c) {
    switch (c) {
        case CullMode::None:  return D3D12_CULL_MODE_NONE;
        case CullMode::Back:  return D3D12_CULL_MODE_BACK;
        case CullMode::Front: return D3D12_CULL_MODE_FRONT;
        default:              return D3D12_CULL_MODE_BACK;
    }
}

inline D3D12_FILL_MODE ToD3D12(FillMode f) {
    switch (f) {
        case FillMode::Solid:     return D3D12_FILL_MODE_SOLID;
        case FillMode::Wireframe: return D3D12_FILL_MODE_WIREFRAME;
        default:                  return D3D12_FILL_MODE_SOLID;
    }
}

inline D3D12_COMPARISON_FUNC ToD3D12(CompareOp op) {
    switch (op) {
        case CompareOp::Never:        return D3D12_COMPARISON_FUNC_NEVER;
        case CompareOp::Less:         return D3D12_COMPARISON_FUNC_LESS;
        case CompareOp::LessEqual:    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case CompareOp::Equal:        return D3D12_COMPARISON_FUNC_EQUAL;
        case CompareOp::Greater:      return D3D12_COMPARISON_FUNC_GREATER;
        case CompareOp::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case CompareOp::Always:       return D3D12_COMPARISON_FUNC_ALWAYS;
        default:                      return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    }
}

inline D3D12_BLEND ToD3D12(BlendFactor bf) {
    switch (bf) {
        case BlendFactor::Zero:        return D3D12_BLEND_ZERO;
        case BlendFactor::One:         return D3D12_BLEND_ONE;
        case BlendFactor::SrcAlpha:    return D3D12_BLEND_SRC_ALPHA;
        case BlendFactor::InvSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
        case BlendFactor::SrcColor:    return D3D12_BLEND_SRC_COLOR;
        case BlendFactor::DstColor:    return D3D12_BLEND_DEST_COLOR;
        case BlendFactor::InvSrcColor: return D3D12_BLEND_INV_SRC_COLOR;
        case BlendFactor::InvDstColor: return D3D12_BLEND_INV_DEST_COLOR;
        case BlendFactor::DstAlpha:    return D3D12_BLEND_DEST_ALPHA;
        case BlendFactor::InvDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
        default:                       return D3D12_BLEND_ONE;
    }
}

inline D3D12_BLEND_OP ToD3D12(BlendOp op) {
    switch (op) {
        case BlendOp::Add:      return D3D12_BLEND_OP_ADD;
        case BlendOp::Subtract: return D3D12_BLEND_OP_SUBTRACT;
        default:                return D3D12_BLEND_OP_ADD;
    }
}

inline D3D12_FILTER ToD3D12Filter(Filter minF, Filter magF) {
    if (minF == Filter::Point && magF == Filter::Point)
        return D3D12_FILTER_MIN_MAG_MIP_POINT;
    if (minF == Filter::Linear && magF == Filter::Linear)
        return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    if (minF == Filter::Point && magF == Filter::Linear)
        return D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR;

    return D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
}

inline D3D12_FILTER ToD3D12FilterComparison(Filter minF, Filter magF) {
    if (minF == Filter::Point && magF == Filter::Point)
        return D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    if (minF == Filter::Linear && magF == Filter::Linear)
        return D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    if (minF == Filter::Point && magF == Filter::Linear)
        return D3D12_FILTER_COMPARISON_MIN_POINT_MAG_MIP_LINEAR;
    return D3D12_FILTER_COMPARISON_MIN_LINEAR_MAG_MIP_POINT;
}

inline D3D12_TEXTURE_ADDRESS_MODE ToD3D12(AddressMode a) {
    switch (a) {
        case AddressMode::Wrap:   return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        case AddressMode::Clamp:  return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case AddressMode::Mirror: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        default:                  return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

inline UINT FormatByteSize(Format f) {
    switch (f) {
        case Format::R16_UINT:           return 2;
        case Format::R32_UINT:           return 4;
        case Format::R32_FLOAT:          return 4;
        case Format::R32G32_FLOAT:       return 8;
        case Format::R32G32B32_FLOAT:    return 12;
        case Format::R32G32B32A32_FLOAT: return 16;
        case Format::R8G8B8A8_UNORM:     return 4;
        case Format::R8G8B8A8_UINT:      return 4;
        case Format::B8G8R8A8_UNORM:     return 4;
        default:                         return 0;
    }
}

inline DXGI_FORMAT DepthFormatToSrvFormat(Format f) {
    switch (f) {
        case Format::D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case Format::D32_FLOAT:         return DXGI_FORMAT_R32_FLOAT;
        case Format::D32_FLOAT_S8_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        default:                        return ToDXGI(f);
    }
}

inline DXGI_FORMAT DepthFormatToTypeless(Format f) {
    switch (f) {
        case Format::D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
        case Format::D32_FLOAT:         return DXGI_FORMAT_R32_TYPELESS;
        case Format::D32_FLOAT_S8_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
        default:                        return ToDXGI(f);
    }
}

}
