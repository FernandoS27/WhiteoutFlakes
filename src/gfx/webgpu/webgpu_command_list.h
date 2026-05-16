#pragma once

// IGFXCommandList for WebGPU. Mirrors src/gfx/vulkan/vulkan_command_list.h:
// FlushBindings translates the per-stage pending arrays into three
// BindGroups (CB with dynamic offsets / SRV / sampler) at every Draw.

#include "gfx/gfx.h"
#include "webgpu_handles.h"

#include <webgpu/webgpu_cpp.h>

#include <array>

namespace whiteout::flakes::gfx::webgpu {

class WebGPUDevice;

class WebGPUCommandList final : public IGFXCommandList {
public:
    explicit WebGPUCommandList(WebGPUDevice& device);
    ~WebGPUCommandList() override;

    void BeginRenderPass(TextureHandle color, TextureHandle depth, const f32 clearColor[4],
                         f32 clearDepth, u8 clearStencil) override;
    void EndRenderPass() override;

    // Tracy doesn't have a WebGPU backend (yet). No-op for now; matches the
    // D3D11/D3D12 backends.
    void BeginGpuZone(const char* /*name*/) override {}
    void EndGpuZone() override {}

    void SetViewport(const Viewport&) override;
    void SetScissor(const Scissor&) override;

    void BindPipeline(PipelineHandle) override;

    void BindVertexBuffer(u32 slot, BufferHandle, u32 stride, u32 offset) override;
    void BindIndexBuffer(BufferHandle, Format) override;
    void BindConstantBuffer(ShaderStage, u32 slot, BufferHandle) override;
    void BindShaderResource(ShaderStage, u32 slot, TextureHandle) override;
    void BindShaderResource(ShaderStage, u32 slot, BufferHandle) override;
    void BindUnorderedAccess(u32 slot, BufferHandle) override;
    void BindSampler(ShaderStage, u32 slot, SamplerHandle) override;

    void ClearDepth(TextureHandle depth, f32 clearDepth, u8 clearStencil) override;
    void CopyBuffer(BufferHandle dst, BufferHandle src) override;

    void Draw(u32 vertexCount, u32 firstVertex) override;
    void DrawIndexed(u32 indexCount, u32 firstIndex, i32 baseVertex) override;
    void Dispatch(u32 gx, u32 gy, u32 gz) override;

private:
    void FlushBindings();

    WebGPUDevice& device_;

    // Active recording state. wgpu::RenderPassEncoder is move-only and
    // ends with End(); we hold one between BeginRenderPass / EndRenderPass.
    wgpu::RenderPassEncoder pass_;

    // Captured per Bind*; FlushBindings consumes them at every Draw/Dispatch.
    // Indexing matches the Vulkan backend: VS in [0, kStageBindingShift),
    // PS in [kStageBindingShift, 2*kStageBindingShift).
    struct PendingCb {
        BufferHandle buffer{};
        u64 offset = 0; // captured at Bind time so MapBuffer rotations don't drift
        u64 size = 0;
    };
    struct PendingSrv {
        TextureHandle texture{};
        BufferHandle storage{}; // for BindShaderResource(buffer) overload
        bool isBuffer = false;
    };
    struct PendingSmp {
        SamplerHandle sampler{};
    };
    std::array<PendingCb, 32> pendingCBs_{};
    std::array<PendingSrv, 32> pendingSRVs_{};
    std::array<PendingSmp, 32> pendingSamplers_{};
    bool cbSetDirty_ = false;
    bool srvSetDirty_ = false;
    bool samplerSetDirty_ = false;

    TextureHandle activeColorAttachment_ = TextureHandle::Invalid;
    TextureHandle activeDepthAttachment_ = TextureHandle::Invalid;
    wgpu::TextureFormat activeColorFormat_ = wgpu::TextureFormat::Undefined;
    PipelineHandle lastBoundPipeline_ = PipelineHandle::Invalid;
};

} // namespace whiteout::flakes::gfx::webgpu
