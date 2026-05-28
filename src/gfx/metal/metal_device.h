#pragma once

// IGFXDevice for Apple Metal. All Obj-C / Metal state hides behind the
// opaque MetalDeviceState — see metal_device_state.h (only ever
// included from .mm translation units). Mirrors src/gfx/webgpu/webgpu_device.h
// field-for-field; see the Metal-backend plan in repository history /
// chat logs for the per-method mapping.

#include "gfx/gfx.h"

#include <memory>
#include <string>
#include <vector>

namespace whiteout::flakes::gfx::metal {

// MTLCopyAllDevices-driven device list. Cheap on Apple Silicon (single
// integrated GPU); on multi-GPU Macs (eGPU, MacPro2019) returns the full
// list in driver-reported order.
std::vector<std::string> EnumerateAdapterNames();

struct MetalDeviceState;
class MetalCommandList;

class MetalDevice final : public IGFXDevice {
public:
    MetalDevice();
    ~MetalDevice() override;

    bool Init(bool enableValidation);

    // ---- IGFXDevice ----
    BufferHandle CreateBuffer(const BufferDesc&, const void* initial) override;
    TextureHandle CreateTexture(const TextureDesc&, const void* initialPixels) override;
    ShaderHandle CreateShader(ShaderStage, const void* bytecode, usize size) override;
    PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc&) override;
    PipelineHandle CreateComputePipeline(const ComputePipelineDesc&) override;
    SamplerHandle CreateSampler(const SamplerDesc&) override;

    void Destroy(BufferHandle) override;
    void Destroy(TextureHandle) override;
    void Destroy(ShaderHandle) override;
    void Destroy(PipelineHandle) override;
    void Destroy(SamplerHandle) override;

    void UpdateBuffer(BufferHandle, const void* data, usize size) override;
    void* MapBuffer(BufferHandle) override;
    void UnmapBuffer(BufferHandle) override;

    SwapChainHandle CreateSwapChain(void* nativeWindowHandle, i32 width, i32 height,
                                    Format colorFormat) override;
    void ResizeSwapChain(SwapChainHandle, i32 width, i32 height) override;
    void DestroySwapChain(SwapChainHandle) override;
    void Present(SwapChainHandle) override;
    void WaitIdle() override;
    TextureHandle GetSwapChainBackBuffer(SwapChainHandle) override;
    TextureHandle GetSwapChainBackBufferLinear(SwapChainHandle) override;
    Format GetSwapChainFormat(SwapChainHandle) const override;

    TextureHandle CreateColorTarget(i32 w, i32 h, Format f) override;
    TextureHandle CreateDepthTarget(i32 w, i32 h, Format f) override;

    IGFXCommandList* GetImmediateContext() override;

    Format PreferredDepthStencilFormat() const override;
    GfxApi GetApi() const override {
        return GfxApi::Metal;
    }
    const char* GetDeviceName() const override;
    u64 LiveGpuBytes() const override;

    MetalDeviceState& State();
    const MetalDeviceState& State() const;

private:
    std::unique_ptr<MetalDeviceState> state_;
    std::unique_ptr<MetalCommandList> immediate_;
    std::string deviceName_;
};

} // namespace whiteout::flakes::gfx::metal
