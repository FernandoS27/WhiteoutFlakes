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
#include <vector>

namespace whiteout::flakes::gfx::vulkan {

// See gfx::EnumerateDevices — spins up a throw-away VkInstance and
// returns each physical device's marketing name. The instance is
// created without validation / debug-utils, so this is cheap enough
// to call from a Settings dialog every time it opens.
std::vector<std::string> EnumerateAdapterNames();

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
    // CapturedOffset locks in the buffer's ring slot at BindConstantBuffer
    // time; FlushDescriptors writes (buffer, capturedOffset, slotSize) so
    // each draw reads its own data even if the buffer is rotated by
    // subsequent MapBuffer calls before the descriptor is flushed.
    struct PendingCb  { BufferHandle  buffer{};  u64 offset = 0; };
    struct PendingSrv { TextureHandle texture{}; };
    struct PendingSmp { SamplerHandle sampler{}; };
    // Sized to match the per-set binding budgets in vulkan_resources.h
    // (kCbBindingCount / kSrvBindingCount / kSamplerBindingCount).
    // The arrays are split into per-stage halves:
    //   indices [0, kStageBindingShift)        — VS slots
    //   indices [kStageBindingShift, 2*…)      — PS slots (slot N + 16)
    // BindConstantBuffer/Resource/Sampler does the (stage, slot) →
    // binding translation in vulkan_command_list.cpp. Bind* compares
    // the new value against the pending slot and only sets the
    // corresponding set-dirty flag when they actually differ — most
    // back-to-back draws in the geoset loop bind identical samplers
    // and SRVs, so this short-circuits the push / pool-allocate paths
    // in FlushDescriptors.
    std::array<PendingCb,  32> pendingCBs_{};
    std::array<PendingSrv, 32> pendingSRVs_{};
    std::array<PendingSmp, 32> pendingSamplers_{};
    // Per-set dirty flags. FlushDescriptors skips the corresponding
    // set's push / alloc / write / bind work when its flag is false.
    bool cbSetDirty_      = false;
    bool srvSetDirty_     = false;
    bool samplerSetDirty_ = false;

    TextureHandle activeColorAttachment_ = TextureHandle::Invalid;
    // Format of the color attachment in the active render pass, as a
    // raw u32 (VkFormat). Stored untyped so this header stays free of
    // <vulkan/vulkan.h>. Used in BindPipeline to sanity-check that the
    // bound pipeline's pColorAttachmentFormats[0] matches — the
    // validator's VUID-vkCmdDraw-08910 fires here too, but this lets us
    // name the C++ call site.
    u32           activeColorFormat_   = 0;  // VK_FORMAT_UNDEFINED
    PipelineHandle lastBoundPipeline_  = PipelineHandle::Invalid;
};

class VulkanDevice final : public IGFXDevice {
public:
    VulkanDevice();
    ~VulkanDevice() override;

    // Returns true when instance + physical device + logical device + VMA
    // came up successfully. CreateDevice in gfx_factory.cpp returns nullptr
    // when this returns false.
    bool Init(bool enableValidation);

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

    Format      PreferredDepthStencilFormat() const override;
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
