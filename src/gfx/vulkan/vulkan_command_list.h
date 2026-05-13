#pragma once

// IGFXCommandList for Vulkan. Holds per-frame Bind* pending state
// that FlushDescriptors translates into push-descriptor / pool-set
// writes at the top of every Draw / Dispatch.

#include "gfx/gfx.h"

#include <array>
#include <memory>
#include <vector>

#if defined(TRACY_ENABLE)
namespace tracy { class VkCtxScope; }
#endif

namespace whiteout::flakes::gfx::vulkan {

class VulkanDevice;

class VulkanCommandList final : public IGFXCommandList {
public:
    explicit VulkanCommandList(VulkanDevice& device);
    ~VulkanCommandList() override;

    void BeginRenderPass(TextureHandle color, TextureHandle depth,
                         const f32 clearColor[4], f32 clearDepth,
                         u8 clearStencil) override;
    void EndRenderPass() override;

    void BeginGpuZone(const char* name) override;
    void EndGpuZone() override;

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
    void FlushDescriptors();

    VulkanDevice& device_;

    // CapturedOffset locks in the buffer's ring slot at Bind time so
    // intervening MapBuffer rotations don't move the data this draw
    // expects to read.
    struct PendingCb  { BufferHandle  buffer{};  u64 offset = 0; };
    struct PendingSrv { TextureHandle texture{}; };
    struct PendingSmp { SamplerHandle sampler{}; };
    // Sized to match kCbBindingCount / kSrvBindingCount /
    // kSamplerBindingCount: VS in [0, kStageBindingShift), PS upper.
    std::array<PendingCb,  32> pendingCBs_{};
    std::array<PendingSrv, 32> pendingSRVs_{};
    std::array<PendingSmp, 32> pendingSamplers_{};
    bool cbSetDirty_      = false;
    bool srvSetDirty_     = false;
    bool samplerSetDirty_ = false;

    TextureHandle activeColorAttachment_ = TextureHandle::Invalid;
    // Raw VkFormat held as u32 so this header stays free of vulkan.h.
    u32           activeColorFormat_   = 0;
    PipelineHandle lastBoundPipeline_  = PipelineHandle::Invalid;

#if defined(TRACY_ENABLE)
    // Tracy's only runtime-named GPU-zone API is the RAII VkCtxScope;
    // heap-allocate per BeginGpuZone, destroy in EndGpuZone.
    std::vector<std::unique_ptr<tracy::VkCtxScope>> gpuZoneStack_;
#endif
};

}  // namespace whiteout::flakes::gfx::vulkan
