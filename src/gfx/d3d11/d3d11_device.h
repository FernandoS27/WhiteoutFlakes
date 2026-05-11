#pragma once

#include "gfx/gfx.h"
#include "d3d11_resources.h"
#include <string>
#include <vector>

namespace whiteout::flakes::gfx::d3d11 {

// Enumerates DXGI adapters' marketing names (DXGI_ADAPTER_DESC1::Description).
// Used by gfx::EnumerateDevices to populate the "preferred device" picker
// without having to spin up an actual D3D11 device. Skips DXGI software
// adapters (Microsoft Basic Render Driver etc.).
std::vector<std::string> EnumerateAdapterNames();

class D3D11CommandList;

class D3D11Device final : public IGFXDevice {
public:
    D3D11Device();
    ~D3D11Device() override;

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

    GfxApi      GetApi() const override { return GfxApi::D3D11; }
    const char* GetDeviceName() const override { return deviceName_.c_str(); }

    ID3D11Device*        GetD3DDevice()  const { return device_; }
    ID3D11DeviceContext* GetD3DContext() const { return context_; }

    BufferEntry*   GetBuffer  (BufferHandle h)   { return buffers_.Get(static_cast<u64>(h)); }
    TextureEntry*  GetTexture (TextureHandle h)  { return textures_.Get(static_cast<u64>(h)); }
    PipelineEntry* GetPipeline(PipelineHandle h) { return pipelines_.Get(static_cast<u64>(h)); }
    SamplerEntry*  GetSampler (SamplerHandle h)  { return samplers_.Get(static_cast<u64>(h)); }

private:
    TextureHandle RegisterBackBuffer(ID3D11Texture2D* bb, DXGI_FORMAT rtvFormat);
    void CreateSwapChainViews(SwapChainEntry& sc);

    ID3D11Device*        device_  = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGIFactory1*       factory_ = nullptr;

    SlotMap<BufferEntry>    buffers_;
    SlotMap<TextureEntry>   textures_;
    SlotMap<ShaderEntry>    shaders_;
    SlotMap<PipelineEntry>  pipelines_;
    SlotMap<SamplerEntry>   samplers_;
    SlotMap<SwapChainEntry> swapChains_;

    std::unique_ptr<D3D11CommandList> immediateCtx_;
    std::string deviceName_;
};

}
