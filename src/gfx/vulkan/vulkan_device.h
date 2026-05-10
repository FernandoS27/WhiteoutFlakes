#pragma once

// ============================================================================
// Vulkan IGFXDevice / IGFXCommandList implementation.
//
// Targets Vulkan 1.3 + dynamic rendering (VK_KHR_dynamic_rendering, core in
// 1.3). VMA owns memory; resources are stored in shared SlotMap<>s and
// referenced through opaque u64-encoded handles. Phase 1 stops at a
// rendered grid + viewcube; full MDX rendering parity is Phase 2.
//
// All bodies — including the trivial getters — live in the .cpp files so
// the public header here stays free of <vulkan/vulkan.h> entanglement.
// Only the IGFXDevice / IGFXCommandList interface declarations are
// visible across the backend; per-resource entry types live alongside
// vulkan_resources.h once we need them.
// ============================================================================

#include "gfx/common/slot_map.h"
#include "gfx/gfx.h"

#include <memory>
#include <string>

namespace whiteout::flakes::gfx::vulkan {

class VulkanDevice;

// Forward-declared opaque state. Defined in vulkan_device.cpp so the
// vulkan-vendor headers don't leak into anything else in WhiteoutFlakesGfx.
struct VulkanDeviceState;

class VulkanCommandList final : public IGFXCommandList {
public:
    explicit VulkanCommandList(VulkanDevice& device);
    ~VulkanCommandList() override;

    void BeginRenderPass(TextureHandle color, TextureHandle depth,
                         const f32 clearColor[4], f32 clearDepth,
                         u8 clearStencil) override;
    void EndRenderPass() override;

    void SetViewport(const Viewport&) override;
    void SetScissor (const Scissor&) override;

    void BindPipeline(PipelineHandle) override;

    void BindVertexBuffer  (u32 slot, BufferHandle, u32 stride, u32 offset) override;
    void BindIndexBuffer   (BufferHandle, Format) override;
    void BindConstantBuffer(ShaderStage, u32 slot, BufferHandle) override;
    void BindShaderResource(ShaderStage, u32 slot, TextureHandle) override;
    void BindShaderResource(ShaderStage, u32 slot, BufferHandle ) override;
    void BindUnorderedAccess(u32 slot, BufferHandle) override;
    void BindSampler       (ShaderStage, u32 slot, SamplerHandle) override;

    void ClearDepth(TextureHandle depth, f32 clearDepth, u8 clearStencil) override;
    void CopyBuffer(BufferHandle dst, BufferHandle src) override;

    void Draw       (u32 vertexCount, u32 firstVertex)             override;
    void DrawIndexed(u32 indexCount,  u32 firstIndex, i32 baseVertex) override;
    void Dispatch   (u32 gx, u32 gy, u32 gz) override;

private:
    // Flushes any dirty Bind* state to the active command buffer via
    // vkCmdPushDescriptorSetKHR. Called at the top of every Draw* /
    // Dispatch.
    void FlushDescriptors();

    VulkanDevice& device_;

    // Pending Bind* writes — buffered until the next Draw* / Dispatch
    // flushes them via vkCmdPushDescriptorSetKHR. Slot indices match
    // the public BindXxx(stage, slot, ...) API; the layout in
    // vulkan_resources.h decides which descriptor binding each slot
    // lands on.
    struct PendingCb  { BufferHandle  buffer{};  bool dirty = false; };
    struct PendingSrv { TextureHandle texture{}; bool dirty = false; };
    struct PendingSmp { SamplerHandle sampler{}; bool dirty = false; };
    std::array<PendingCb,  4> pendingCBs_{};
    std::array<PendingSrv, 4> pendingSRVs_{};
    std::array<PendingSmp, 4> pendingSamplers_{};
    bool                      anyDescriptorDirty_ = false;
};

class VulkanDevice final : public IGFXDevice {
public:
    VulkanDevice();
    ~VulkanDevice() override;

    // Returns true when instance + physical device + logical device + VMA
    // came up successfully. CreateDevice in gfx_factory.cpp returns nullptr
    // when this returns false.
    bool Init();

    // ---- IGFXDevice ----
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
    TextureHandle GetSwapChainBackBuffer      (SwapChainHandle) override;
    TextureHandle GetSwapChainBackBufferLinear(SwapChainHandle) override;

    TextureHandle CreateColorTarget(i32 w, i32 h, Format f) override;
    TextureHandle CreateDepthTarget(i32 w, i32 h, Format f) override;

    IGFXCommandList* GetImmediateContext() override;

    GfxApi      GetApi() const override { return GfxApi::Vulkan; }
    const char* GetDeviceName() const override;

    // Accessors used by VulkanCommandList / vulkan_pipeline.cpp /
    // vulkan_swap_chain.cpp. Defined in vulkan_device.cpp.
    VulkanDeviceState&       State();
    const VulkanDeviceState& State() const;

private:
    std::unique_ptr<VulkanDeviceState> state_;
    std::unique_ptr<VulkanCommandList> immediate_;
    std::string                        deviceName_;
};

}  // namespace whiteout::flakes::gfx::vulkan
