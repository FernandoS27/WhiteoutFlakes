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

    // Per-slot vertex-buffer bind state, for redundant-bind suppression.
    // Reset at BeginRenderPass — wgpu::RenderPassEncoder loses its state
    // when the pass ends.
    struct LastVB {
        BufferHandle buffer{};
        u64 offset = 0;
    };
    std::array<LastVB, 16> lastVBs_{};
    BufferHandle lastIndexBuffer_{};
    u64 lastIndexOffset_ = 0;
    Format lastIndexFormat_ = Format::R16_UINT;

    // Scratch BindGroupEntry arrays owned by the command list — reused
    // across every FlushBindings call so we don't allocate a fresh
    // std::vector per draw. At 200+ draws/frame × 3 groups, that was
    // 600+ heap allocs/frame. Sized to the layout binding counts (see
    // kCbBindingCount / kSrvBindingCount / kSamplerBindingCount in
    // webgpu_init.cpp — all 32 here).
    std::array<wgpu::BindGroupEntry, 32> scratchCbEntries_{};
    std::array<wgpu::BindGroupEntry, 32> scratchSrvEntries_{};
    std::array<wgpu::BindGroupEntry, 32> scratchSamplerEntries_{};

    // Last-applied bind-group cache key per group, reset at
    // BeginRenderPass. When a dirty flush ends up resolving the same
    // key as the previous flush (e.g. a redundant Bind* flip-flopped),
    // we skip pass_.SetBindGroup entirely — the binding is already
    // active on the encoder. Saves a per-draw API call on Firefox.
    u64 lastCbKey_ = 0;
    u64 lastSrvKey_ = 0;
    u64 lastSamplerKey_ = 0;
    bool lastCbKeySet_ = false;
    bool lastSrvKeySet_ = false;
    bool lastSamplerKeySet_ = false;
};

} // namespace whiteout::flakes::gfx::webgpu
