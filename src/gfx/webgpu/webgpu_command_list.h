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
    void BeginRenderPass(const TextureHandle* colors, u32 colorCount, TextureHandle depth,
                         const f32 (*clearColors)[4], f32 clearDepth, u8 clearStencil) override;
    void BeginRenderPassLoad(TextureHandle color, TextureHandle depth, f32 clearDepth,
                             u8 clearStencil, bool loadDepth) override;
    bool BeginDepthSlicePass(TextureHandle depth, u32 arraySlice, f32 clearDepth,
                             u8 clearStencil) override;
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
    void ResetPassState();
    // Set the renderer's requested buffer on `slot` if the encoder holds another.
    void ApplyVertexBuffer(u32 slot);

    WebGPUDevice& device_;

    // Active recording state. wgpu::RenderPassEncoder is move-only and
    // ends with End(); we hold one between BeginRenderPass / EndRenderPass.
    wgpu::RenderPassEncoder pass_;

    // Captured per Bind*; FlushBindings consumes them at every Draw.
    // Indexed by the WGSL @binding number (see webgpu_handles.h).
    struct PendingCb {
        BufferHandle buffer{};
        u64 offset = 0; // captured at Bind time so MapBuffer rotations don't drift
        u64 size = 0;
    };
    struct PendingSrv {
        TextureHandle texture{};
        BufferHandle storage{}; // for BindShaderResource(buffer) overload
        u64 storageOffset = 0;
        bool isBuffer = false;
    };
    struct PendingSmp {
        SamplerHandle sampler{};
    };
    std::array<PendingCb, kMaxBindingIndex> pendingCBs_{};
    std::array<PendingSrv, kMaxBindingIndex> pendingSRVs_{};
    std::array<PendingSmp, kMaxBindingIndex> pendingSamplers_{};
    u8 dirtyKinds_ = 0; // kBindKind* families changed since the last flush

    // Layouts of the bound pipeline's groups (indices into bindLayouts).
    u32 groupCount_ = 0;
    std::array<u32, kMaxBindGroups> groupLayouts_{};

    TextureHandle activeColorAttachment_ = TextureHandle::Invalid;
    TextureHandle activeDepthAttachment_ = TextureHandle::Invalid;
    // Array slice the next BeginRenderPass attaches as depth (BeginDepthSlicePass).
    static constexpr u32 kNoDepthSlice = ~0u;
    u32 depthSlice_ = kNoDepthSlice;
    wgpu::TextureFormat activeColorFormat_ = wgpu::TextureFormat::Undefined;
    PipelineHandle lastBoundPipeline_ = PipelineHandle::Invalid;

    // Per-slot vertex buffers: what the renderer asked for, and what the pass
    // encoder holds (which differs on the bound pipeline's phantom slot).
    // Reset at BeginRenderPass — wgpu::RenderPassEncoder loses its state
    // when the pass ends.
    struct VbBinding {
        BufferHandle buffer{};
        u64 offset = 0;
        bool zero = false; // the shared zero buffer
    };
    std::array<VbBinding, kMaxVertexInputSlots> requestedVBs_{};
    std::array<VbBinding, kMaxVertexInputSlots> encoderVBs_{};
    i32 phantomSlot_ = -1;
    BufferHandle lastIndexBuffer_{};
    u64 lastIndexOffset_ = 0;
    Format lastIndexFormat_ = Format::R16_UINT;

    // Scratch BindGroupEntry array reused across FlushBindings calls so a
    // draw doesn't allocate.
    std::array<wgpu::BindGroupEntry, kMaxBindingIndex> scratchEntries_{};

    // Last-applied bind-group cache key per group, reset at BeginRenderPass.
    // The key covers the layout id and every resource, so a flush that
    // resolves to the bound group skips pass_.SetBindGroup.
    std::array<u64, kMaxBindGroups> lastGroupKey_{};
    std::array<bool, kMaxBindGroups> lastGroupKeySet_{};
};

} // namespace whiteout::flakes::gfx::webgpu
