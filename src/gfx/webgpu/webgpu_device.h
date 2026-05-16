#pragma once

// IGFXDevice for WebGPU (Dawn native). All Dawn-specific state hides
// behind the opaque WebGPUDeviceState — see webgpu_device_state.h.
// Mirrors src/gfx/vulkan/vulkan_device.h field-for-field; see WebGPU.md
// for the per-method mapping.

#include "gfx/gfx.h"

#include <memory>
#include <string>
#include <vector>

namespace whiteout::flakes::gfx::webgpu {

// Spins up a throw-away wgpu::Instance to list adapters. Cheap.
std::vector<std::string> EnumerateAdapterNames();

struct WebGPUDeviceState;
class WebGPUCommandList;

class WebGPUDevice final : public IGFXDevice {
public:
    WebGPUDevice();
    ~WebGPUDevice() override;

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
    TextureHandle GetSwapChainBackBuffer(SwapChainHandle) override;
    TextureHandle GetSwapChainBackBufferLinear(SwapChainHandle) override;

    TextureHandle CreateColorTarget(i32 w, i32 h, Format f) override;
    TextureHandle CreateDepthTarget(i32 w, i32 h, Format f) override;

    IGFXCommandList* GetImmediateContext() override;

    Format PreferredDepthStencilFormat() const override;
    GfxApi GetApi() const override {
        return GfxApi::WebGPU;
    }
    const char* GetDeviceName() const override;

    WebGPUDeviceState& State();
    const WebGPUDeviceState& State() const;

private:
    std::unique_ptr<WebGPUDeviceState> state_;
    std::unique_ptr<WebGPUCommandList> immediate_;
    std::string deviceName_;
};

} // namespace whiteout::flakes::gfx::webgpu
