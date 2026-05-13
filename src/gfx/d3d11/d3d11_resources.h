#pragma once

#include <cassert>
#include <vector>
#include "d3d11_translate.h"
#include "gfx/common/slot_map.h"
#include "gfx/gfx.h"

namespace whiteout::flakes::gfx::d3d11 {

struct BufferEntry {
    ID3D11Buffer* buffer = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    BufferDesc desc{};

    void Release() {
        SafeRelease(uav);
        SafeRelease(srv);
        SafeRelease(buffer);
    }
};

struct TextureEntry {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    TextureDesc desc{};

    void Release() {
        SafeRelease(dsv);
        SafeRelease(rtv);
        SafeRelease(srv);
        SafeRelease(tex);
    }
};

struct ShaderEntry {
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11ComputeShader* cs = nullptr;
    ShaderStage stage{};

    std::vector<u8> bytecode;

    void Release() {
        SafeRelease(vs);
        SafeRelease(ps);
        SafeRelease(cs);
        bytecode.clear();
    }
};

struct PipelineEntry {

    ID3D11BlendState* blendState = nullptr;
    ID3D11DepthStencilState* depthState = nullptr;
    ID3D11RasterizerState* rasterState = nullptr;
    ID3D11InputLayout* inputLayout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    bool alphaToCoverage = false;

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11ComputeShader* cs = nullptr;

    bool isCompute = false;

    void Release() {
        SafeRelease(inputLayout);
        SafeRelease(rasterState);
        SafeRelease(depthState);
        SafeRelease(blendState);
    }
};

struct SamplerEntry {
    ID3D11SamplerState* sampler = nullptr;

    void Release() {
        SafeRelease(sampler);
    }
};

struct SwapChainEntry {
    IDXGISwapChain* swapChain = nullptr;
    ID3D11Texture2D* backBuffer = nullptr;

    DXGI_FORMAT rtvDxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

    DXGI_FORMAT rtvDxgiFormatLinear = DXGI_FORMAT_R8G8B8A8_UNORM;

    u64 backBufferTexHandle = 0;
    u64 backBufferTexHandleLinear = 0;

    void ReleaseBackBuffer() {
        SafeRelease(backBuffer);
    }
    void Release() {
        ReleaseBackBuffer();
        SafeRelease(swapChain);
    }
};

} // namespace whiteout::flakes::gfx::d3d11
