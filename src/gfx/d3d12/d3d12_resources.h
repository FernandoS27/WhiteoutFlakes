#pragma once

#include "d3d12_translate.h"
#include "gfx/common/slot_map.h"
#include "gfx/gfx.h"

#include <array>
#include <cassert>
#include <cstring>
#include <vector>

namespace whiteout::flakes::gfx::d3d12 {

inline constexpr u32 kFramesInFlight = 2;

inline constexpr u32 kSrvsPerStage = 16;
inline constexpr u32 kUavsForCompute = 4;
inline constexpr u32 kSamplersPerStage = 4;

inline constexpr u32 kRootCbvsPerStage = 4;

enum class GraphicsRP : u32 {
    CBV_VS_0 = 0,
    CBV_VS_1,
    CBV_VS_2,
    CBV_VS_3,
    CBV_PS_0,
    CBV_PS_1,
    CBV_PS_2,
    CBV_PS_3,
    SRV_TABLE_VS,
    SRV_TABLE_PS,
    SAMPLER_TABLE_PS,
    Count
};
enum class ComputeRP : u32 {
    CBV_0 = 0,
    CBV_1,
    CBV_2,
    CBV_3,
    SRV_TABLE,
    UAV_TABLE,
    SAMPLER_TABLE,
    Count
};

struct BufferEntry {

    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_STATES currentState = D3D12_RESOURCE_STATE_COMMON;
    BufferDesc desc{};

    D3D12_GPU_VIRTUAL_ADDRESS cpuWritableVA = 0;
    void* cpuWritablePtr = nullptr;

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{0};
    D3D12_CPU_DESCRIPTOR_HANDLE uavCpu{0};
    bool hasSrv = false;
    bool hasUav = false;

    void Release() {
        SafeRelease(resource);
        cpuWritableVA = 0;
        cpuWritablePtr = nullptr;
    }
};

struct TextureEntry {
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_STATES currentState = D3D12_RESOURCE_STATE_COMMON;
    TextureDesc desc{};
    bool ownsResource = true;

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{0};
    D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu{0};
    D3D12_CPU_DESCRIPTOR_HANDLE dsvCpu{0};
    bool hasSrv = false;
    bool hasRtv = false;
    bool hasDsv = false;

    void Release() {
        if (ownsResource)
            SafeRelease(resource);
        else
            resource = nullptr;
    }
};

struct ShaderEntry {
    std::vector<u8> bytecode;
    ShaderStage stage{};

    void Release() {
        bytecode.clear();
    }
};

struct PipelineEntry {
    ID3D12PipelineState* pso = nullptr;
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    bool isCompute = false;

    void Release() {
        SafeRelease(pso);
    }
};

struct SamplerEntry {
    D3D12_CPU_DESCRIPTOR_HANDLE samplerCpu{0};
    bool valid = false;

    void Release() {
        valid = false;
        samplerCpu = {0};
    }
};

struct SwapChainEntry {
    IDXGISwapChain3* swapChain = nullptr;
    HWND hwnd = nullptr;
    Format colorFormat = Format::R8G8B8A8_UNORM_SRGB;

    DXGI_FORMAT rtvDxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    UINT currentBackBufferIndex = 0;

    std::array<ID3D12Resource*, kFramesInFlight> backBuffers{};

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kFramesInFlight> backBufferRtvs{};

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kFramesInFlight> backBufferRtvsLinear{};

    DXGI_FORMAT rtvDxgiFormatLinear = DXGI_FORMAT_R8G8B8A8_UNORM;

    u64 proxyTexHandle = 0;
    u64 proxyTexHandleLinear = 0;

    void ReleaseBackBuffers() {
        for (auto& bb : backBuffers)
            SafeRelease(bb);
    }
    void Release() {
        ReleaseBackBuffers();
        SafeRelease(swapChain);
    }
};

class UploadRing {
public:
    bool Init(ID3D12Device* device, u64 sizeBytes);
    void Release();

    void BeginFrame(u64 nextFenceValue);
    void EndFrame(u64 signaledFenceValue);
    void Retire(u64 completedFenceValue);

    struct Allocation {
        void* cpu = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        ID3D12Resource* resource = nullptr;
        u64 offset = 0;
    };
    Allocation Allocate(u64 size, u64 alignment = 256);

private:
    struct Retired {
        u64 fenceValue = 0;
        u64 head = 0;
    };

    ID3D12Resource* resource_ = nullptr;
    u8* mapped_ = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpuBase_ = 0;
    u64 capacity_ = 0;
    u64 head_ = 0;
    u64 tail_ = 0;

    std::vector<Retired> retiredQueue_;
    u64 currentFrameFence_ = 0;
};

class DescriptorHeapRing {
public:
    bool Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 totalSize);
    void Release();

    void BeginFrame(u64 nextFenceValue);
    void EndFrame(u64 signaledFenceValue);
    void Retire(u64 completedFenceValue);

    struct Slice {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{0};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{0};
    };
    Slice Allocate(u32 count);

    ID3D12DescriptorHeap* Heap() const {
        return heap_;
    }
    u32 Stride() const {
        return stride_;
    }

private:
    struct Retired {
        u64 fenceValue = 0;
        u32 head = 0;
    };

    ID3D12DescriptorHeap* heap_ = nullptr;
    u32 stride_ = 0;
    u32 capacity_ = 0;
    u32 head_ = 0;
    u32 tail_ = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuBase_{0};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuBase_{0};

    std::vector<Retired> retiredQueue_;
    u64 currentFrameFence_ = 0;
};

class CpuDescriptorPool {
public:
    bool Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 capacity);
    void Release();

    D3D12_CPU_DESCRIPTOR_HANDLE Allocate();
    void Free(D3D12_CPU_DESCRIPTOR_HANDLE h);
    u32 Stride() const {
        return stride_;
    }

private:
    ID3D12DescriptorHeap* heap_ = nullptr;
    u32 stride_ = 0;
    u32 capacity_ = 0;
    u32 head_ = 0;
    std::vector<u32> freeList_;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuBase_{0};
};

inline u64 AlignUp(u64 v, u64 a) {
    return (v + a - 1) & ~(a - 1);
}

} // namespace whiteout::flakes::gfx::d3d12
