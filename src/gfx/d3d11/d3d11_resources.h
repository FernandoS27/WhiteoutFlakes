#pragma once

#include "d3d11_translate.h"
#include "gfx/gfx.h"
#include <vector>
#include <cassert>

namespace WhiteoutDex::gfx::d3d11 {

static constexpr u64 kIndexBits = 48;
static constexpr u64 kIndexMask = (u64{1} << kIndexBits) - 1;
static constexpr u64 kGenShift  = kIndexBits;

inline u64 MakeHandle(u32 index, u16 gen) {
    return (static_cast<u64>(gen) << kGenShift) | static_cast<u64>(index + 1);
}

inline u32 HandleIndex(u64 h) {
    return static_cast<u32>((h & kIndexMask) - 1);
}

inline u16 HandleGen(u64 h) {
    return static_cast<u16>(h >> kGenShift);
}

template<typename Payload>
class SlotMap {
public:
    struct Slot {
        Payload  data{};
        u16 generation = 1;
        bool     alive      = false;
    };

    u64 Insert(Payload&& p) {
        u32 idx;
        if (!freeList_.empty()) {
            idx = freeList_.back();
            freeList_.pop_back();
        } else {
            idx = static_cast<u32>(slots_.size());
            slots_.push_back({});
        }
        auto& s   = slots_[idx];
        s.data     = std::move(p);
        s.alive    = true;
        return MakeHandle(idx, s.generation);
    }

    Payload* Get(u64 h) {
        if (h == 0) return nullptr;
        u32 idx = HandleIndex(h);
        if (idx >= slots_.size()) return nullptr;
        auto& s = slots_[idx];
        if (!s.alive || s.generation != HandleGen(h)) return nullptr;
        return &s.data;
    }

    const Payload* Get(u64 h) const {
        if (h == 0) return nullptr;
        u32 idx = HandleIndex(h);
        if (idx >= slots_.size()) return nullptr;
        auto& s = slots_[idx];
        if (!s.alive || s.generation != HandleGen(h)) return nullptr;
        return &s.data;
    }

    void Remove(u64 h) {
        if (h == 0) return;
        u32 idx = HandleIndex(h);
        if (idx >= slots_.size()) return;
        auto& s = slots_[idx];
        if (!s.alive || s.generation != HandleGen(h)) return;
        s.data  = Payload{};
        s.alive = false;
        s.generation++;
        freeList_.push_back(idx);
    }

    template<typename Fn>
    void ForEach(Fn&& fn) {
        for (auto& s : slots_) {
            if (s.alive) fn(s.data);
        }
    }

    void Clear() {
        slots_.clear();
        freeList_.clear();
    }

private:
    std::vector<Slot>     slots_;
    std::vector<u32> freeList_;
};

struct BufferEntry {
    ID3D11Buffer*              buffer = nullptr;
    ID3D11ShaderResourceView*  srv    = nullptr;
    ID3D11UnorderedAccessView* uav    = nullptr;
    BufferDesc                 desc{};

    void Release() {
        SafeRelease(uav);
        SafeRelease(srv);
        SafeRelease(buffer);
    }
};

struct TextureEntry {
    ID3D11Texture2D*           tex = nullptr;
    ID3D11ShaderResourceView*  srv = nullptr;
    ID3D11RenderTargetView*    rtv = nullptr;
    ID3D11DepthStencilView*    dsv = nullptr;
    TextureDesc                desc{};

    void Release() {
        SafeRelease(dsv);
        SafeRelease(rtv);
        SafeRelease(srv);
        SafeRelease(tex);
    }
};

struct ShaderEntry {
    ID3D11VertexShader*  vs = nullptr;
    ID3D11PixelShader*   ps = nullptr;
    ID3D11ComputeShader* cs = nullptr;
    ShaderStage          stage{};

    std::vector<u8> bytecode;

    void Release() {
        SafeRelease(vs);
        SafeRelease(ps);
        SafeRelease(cs);
        bytecode.clear();
    }
};

struct PipelineEntry {

    ID3D11BlendState*        blendState      = nullptr;
    ID3D11DepthStencilState* depthState      = nullptr;
    ID3D11RasterizerState*   rasterState     = nullptr;
    ID3D11InputLayout*       inputLayout     = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology        = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    bool                     alphaToCoverage = false;

    ID3D11VertexShader*  vs = nullptr;
    ID3D11PixelShader*   ps = nullptr;
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

    void Release() { SafeRelease(sampler); }
};

struct SwapChainEntry {
    IDXGISwapChain*    swapChain = nullptr;
    ID3D11Texture2D*   backBuffer = nullptr;

    DXGI_FORMAT        rtvDxgiFormat       = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

    DXGI_FORMAT        rtvDxgiFormatLinear = DXGI_FORMAT_R8G8B8A8_UNORM;

    u64           backBufferTexHandle       = 0;
    u64           backBufferTexHandleLinear = 0;

    void ReleaseBackBuffer() {
        SafeRelease(backBuffer);
    }
    void Release() {
        ReleaseBackBuffer();
        SafeRelease(swapChain);
    }
};

}
