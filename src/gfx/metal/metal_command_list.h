#pragma once

// IGFXCommandList for Metal. The PIMPL boundary keeps Obj-C / Metal
// state out of this header — the per-frame command buffer + encoder
// pointers live in MetalDeviceState (metal_device_state.h, .mm-only).
// Bind* methods stash to plain-C++ scratch arrays and FlushBindings
// (called from Draw / DrawIndexed / Dispatch) translates them into
// per-stage Metal setVertexBuffer:/setFragmentBuffer:/setVertexTexture:
// /setFragmentTexture:/setVertexSamplerState:/setFragmentSamplerState:
// calls. See the Metal-backend plan for the (stage, slot) → Metal index
// mapping rationale.

#include "gfx/gfx.h"
#include "metal_handles.h"

#include <array>

namespace whiteout::flakes::gfx::metal {

class MetalDevice;

class MetalCommandList final : public IGFXCommandList {
public:
    explicit MetalCommandList(MetalDevice& device);
    ~MetalCommandList() override;

    void BeginRenderPass(TextureHandle color, TextureHandle depth, const f32 clearColor[4],
                         f32 clearDepth, u8 clearStencil) override;
    void EndRenderPass() override;

    // Tracy has a Metal backend but wiring it up is a polish task.
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

    MetalDevice& device_;

    // Captured per Bind*; FlushBindings consumes them at every Draw / Dispatch.
    // VS lives in [0, kStageBindingShift); PS in [kStageBindingShift, 2*kStageBindingShift).
    // Per-stage indices are reconstructed at flush time (PS-side subtracts
    // kStageBindingShift so slang's [[buffer(N)]]/[[texture(N)]] indices line up).
    struct PendingCb {
        BufferHandle buffer{};
        u64 offset = 0;
        u64 size = 0;
    };
    struct PendingSrv {
        TextureHandle texture{};
        BufferHandle storage{};
        bool isBuffer = false;
    };
    struct PendingSmp {
        SamplerHandle sampler{};
    };

    std::array<PendingCb, 2 * kStageBindingShift> pendingCBs_{};
    std::array<PendingSrv, 2 * kStageBindingShift> pendingSRVs_{};
    std::array<PendingSmp, 2 * kStageBindingShift> pendingSamplers_{};

    PipelineHandle lastBoundPipeline_ = PipelineHandle::Invalid;
};

} // namespace whiteout::flakes::gfx::metal
