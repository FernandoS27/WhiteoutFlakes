#pragma once

#include "gfx/gfx.h"
#include "d3d12_resources.h"

#include <memory>
#include <string>
#include <vector>

namespace whiteout::flakes::gfx::d3d12 {

// See gfx::EnumerateDevices — DXGI-backed device name listing.
std::vector<std::string> EnumerateAdapterNames();


class D3D12CommandList;

class D3D12Device final : public IGFXDevice {
public:
    D3D12Device();
    ~D3D12Device() override;

    bool Init(bool enableValidation);

    BufferHandle   CreateBuffer (const BufferDesc&,  const void* initial) override;
    TextureHandle  CreateTexture(const TextureDesc&, const void* initialPixels) override;
    ShaderHandle   CreateShader (ShaderStage, const void* bytecode, usize size) override;
    PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc&) override;
    PipelineHandle CreateComputePipeline (const ComputePipelineDesc&) override;
    SamplerHandle  CreateSampler(const SamplerDesc&) override;

    void Destroy(BufferHandle)   override;
    void Destroy(TextureHandle)  override;
    void Destroy(ShaderHandle)   override;
    void Destroy(PipelineHandle) override;
    void Destroy(SamplerHandle)  override;

    void  UpdateBuffer(BufferHandle, const void* data, usize size) override;
    void* MapBuffer   (BufferHandle) override;
    void  UnmapBuffer (BufferHandle) override;

    SwapChainHandle CreateSwapChain(void* nativeWindowHandle,
                                    i32 width, i32 height,
                                    Format colorFormat) override;
    void          ResizeSwapChain (SwapChainHandle, i32 width, i32 height) override;
    void          DestroySwapChain(SwapChainHandle) override;
    void          Present         (SwapChainHandle) override;
    TextureHandle GetSwapChainBackBuffer(SwapChainHandle) override;
    TextureHandle GetSwapChainBackBufferLinear(SwapChainHandle) override;

    TextureHandle CreateColorTarget(i32 w, i32 h, Format f) override;
    TextureHandle CreateDepthTarget(i32 w, i32 h, Format f) override;

    IGFXCommandList* GetImmediateContext() override;

    Format      PreferredDepthStencilFormat() const override;
    GfxApi      GetApi() const override { return GfxApi::D3D12; }
    const char* GetDeviceName() const override { return deviceName_.c_str(); }

    ID3D12Device*              GetDevice()     const { return device_; }
    ID3D12GraphicsCommandList* GetCmdList()    const { return cmdList_; }
    ID3D12RootSignature*       GetGraphicsRS() const { return graphicsRS_; }
    ID3D12RootSignature*       GetComputeRS()  const { return computeRS_; }

    DescriptorHeapRing& CbvSrvUavRing() { return cbvSrvUavRing_; }
    DescriptorHeapRing& SamplerRing()   { return samplerRing_; }
    UploadRing&         Upload()        { return uploadRing_; }

    BufferEntry*    GetBuffer  (BufferHandle h)   { return buffers_.Get(static_cast<u64>(h)); }
    TextureEntry*   GetTexture (TextureHandle h)  { return textures_.Get(static_cast<u64>(h)); }
    PipelineEntry*  GetPipeline(PipelineHandle h) { return pipelines_.Get(static_cast<u64>(h)); }
    SamplerEntry*   GetSampler (SamplerHandle h)  { return samplers_.Get(static_cast<u64>(h)); }
    SwapChainEntry* GetSwapChain(SwapChainHandle h) { return swapChains_.Get(static_cast<u64>(h)); }

    D3D12_CPU_DESCRIPTOR_HANDLE GetNullSrv() const { return nullSrv_; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetNullUav() const { return nullUav_; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetNullSampler() const { return nullSampler_; }

private:

    bool CreateDeviceAndQueue();
    bool CreateCommandInfra();
    bool CreateDescriptorPools();
    bool CreateRootSignatures();
    bool CreateNullDescriptors();
    bool OpenCommandList();

    void FlushGpu();
    void WaitForFence(u64 value);

    void RefreshProxyTexture(SwapChainEntry& sc);

    ID3D12Device*              device_     = nullptr;
    IDXGIFactory4*             factory_    = nullptr;
    ID3D12CommandQueue*        queue_      = nullptr;

    ID3D12CommandAllocator*    allocators_[kFramesInFlight]{};
    ID3D12GraphicsCommandList* cmdList_    = nullptr;
    ID3D12Fence*               fence_      = nullptr;
    HANDLE                     fenceEvent_ = nullptr;
    u64                   fenceValue_ = 0;
    u64                   frameFenceValues_[kFramesInFlight]{};
    u32                   frameIndex_ = 0;
    bool                       cmdListOpen_ = false;

    ID3D12RootSignature*       graphicsRS_ = nullptr;
    ID3D12RootSignature*       computeRS_  = nullptr;

    CpuDescriptorPool          rtvPool_;
    CpuDescriptorPool          dsvPool_;
    CpuDescriptorPool          cbvSrvUavPool_;
    CpuDescriptorPool          samplerPool_;

    DescriptorHeapRing         cbvSrvUavRing_;
    DescriptorHeapRing         samplerRing_;

    UploadRing                 uploadRing_;

    D3D12_CPU_DESCRIPTOR_HANDLE nullSrv_{0};
    D3D12_CPU_DESCRIPTOR_HANDLE nullUav_{0};
    D3D12_CPU_DESCRIPTOR_HANDLE nullSampler_{0};

    SlotMap<BufferEntry>    buffers_;
    SlotMap<TextureEntry>   textures_;
    SlotMap<ShaderEntry>    shaders_;
    SlotMap<PipelineEntry>  pipelines_;
    SlotMap<SamplerEntry>   samplers_;
    SlotMap<SwapChainEntry> swapChains_;

    struct PendingDelete {
        IUnknown* obj;
        u64  fenceValue;
    };
    std::vector<PendingDelete> pendingDeletes_;
    void DeferredRelease(IUnknown* obj);
    void FlushPendingDeletes(u64 completedFenceValue);

    std::unique_ptr<D3D12CommandList> immediateCtx_;
    std::string                       deviceName_;
    bool                              enableValidation_ = false;
};

}
