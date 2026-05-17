#pragma once

// IGFXDevice for Vulkan. vulkan.hpp / VMA entanglement is hidden
// behind the opaque VulkanDeviceState — see vulkan_device_state.h.

#include "gfx/gfx.h"

#include <memory>
#include <string>
#include <vector>

namespace whiteout::flakes::gfx::vulkan {

// Spins up a throw-away VkInstance to list adapters. Cheap.
std::vector<std::string> EnumerateAdapterNames();

struct VulkanDeviceState;
class VulkanCommandList;

class VulkanDevice final : public IGFXDevice {
public:
    VulkanDevice();
    ~VulkanDevice() override;

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
    Format GetSwapChainFormat(SwapChainHandle) const override;

    TextureHandle CreateColorTarget(i32 w, i32 h, Format f) override;
    TextureHandle CreateDepthTarget(i32 w, i32 h, Format f) override;

    IGFXCommandList* GetImmediateContext() override;

    Format PreferredDepthStencilFormat() const override;
    GfxApi GetApi() const override {
        return GfxApi::Vulkan;
    }
    const char* GetDeviceName() const override;
    void* GetNativeInstance() const override;

    VulkanDeviceState& State();
    const VulkanDeviceState& State() const;

private:
    std::unique_ptr<VulkanDeviceState> state_;
    std::unique_ptr<VulkanCommandList> immediate_;
    std::string deviceName_;
};

} // namespace whiteout::flakes::gfx::vulkan
