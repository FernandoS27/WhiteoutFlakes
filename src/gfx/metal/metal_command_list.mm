// MetalCommandList — Phase B implementation.
//
// Render-pass open/close + per-frame command-buffer lifecycle are real;
// resource binds and Draw / Dispatch remain stubbed pending the pipeline
// work in phases (c)..(d). FlushBindings, the per-stage encoder routing,
// and the dirty-tracked (stage, slot) → setVertexBuffer:/setFragmentBuffer:
// /…atIndex: translation land alongside CreateGraphicsPipeline in
// metal_pipeline.mm.

#include "metal_command_list.h"
#include "metal_device.h"
#include "metal_device_state.h"
#include "metal_handles.h"
#include "metal_translate.h"

#import <Metal/Metal.h>

#include <cstdio>

namespace whiteout::flakes::gfx::metal {

// Defined in metal_swap_chain.mm.
void AcquireSwapChainImageIfNeeded(MetalDeviceState& state, SwapChainEntry& sc);
// Defined in metal_delete_queue.mm.
void DrainPendingDeletes(MetalDeviceState& state);

namespace {

// Lazily open the per-frame command buffer at first BeginRenderPass (or
// CopyBuffer / Dispatch when those land). Mirrors webgpu_command_list.cpp's
// EnsureEncoderOpen pattern: the engine model is "one command buffer per
// frame, Present commits".
//
// Also a natural place to drain the deferred-delete queue: by frame
// open, any resource destroyed before the last Present has had its
// owning command buffer retired (the addCompletedHandler ran on the
// Metal scheduler thread).
FrameContext& EnsureFrameOpen(MetalDeviceState& state) {
    auto& frame = state.frames[state.currentFrame];
    if (!frame.commandBuffer) {
        DrainPendingDeletes(state);
        frame.commandBuffer = [state.commandQueue commandBuffer];
        if (state.validationRequested && frame.commandBuffer)
            frame.commandBuffer.label = @"wf.frame";
        frame.recording = true;
    }
    return frame;
}

// Walk the texture slot map to find a swap-chain owner. The BeginRenderPass
// caller may hand us either the sRGB or linear proxy — either way we need
// the parent SwapChainEntry so we can drive [layer nextDrawable].
SwapChainEntry* SwapChainOwnerOfTexture(MetalDeviceState& state, TextureHandle h) {
    if (h == TextureHandle::Invalid)
        return nullptr;
    auto* tex = state.textures.Get(static_cast<u64>(h));
    if (!tex || tex->swapChainProxy == SwapChainHandle::Invalid)
        return nullptr;
    return state.swapchains.Get(static_cast<u64>(tex->swapChainProxy));
}

} // namespace

MetalCommandList::MetalCommandList(MetalDevice& device) : device_(device) {}
MetalCommandList::~MetalCommandList() = default;

void MetalCommandList::BeginRenderPass(TextureHandle color, TextureHandle depth,
                                       const f32 clearColor[4], f32 clearDepth,
                                       u8 clearStencil) {
    @autoreleasepool {
        auto& state = device_.State();

        // If the color attachment is a swap-chain proxy, pull a drawable
        // up front so the proxy's texture is non-nil by the time we build
        // the MTLRenderPassDescriptor.
        if (auto* sc = SwapChainOwnerOfTexture(state, color))
            AcquireSwapChainImageIfNeeded(state, *sc);

        auto* colorTex = state.textures.Get(static_cast<u64>(color));
        if (!colorTex || !colorTex->texture) {
            std::fprintf(stderr,
                "[gfx/metal] BeginRenderPass: color attachment unavailable\n");
            return;
        }

        FrameContext& frame = EnsureFrameOpen(state);

        MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        MTLRenderPassColorAttachmentDescriptor* ca = rpd.colorAttachments[0];
        ca.texture = colorTex->texture;
        ca.loadAction = MTLLoadActionClear;
        ca.storeAction = MTLStoreActionStore;
        ca.clearColor =
            MTLClearColorMake(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);

        // Depth + stencil are optional. The clear-to-color smoke path
        // passes depth=Invalid; Phase D+ wires real depth targets.
        if (auto* depthTex = state.textures.Get(static_cast<u64>(depth))) {
            if (depthTex->texture) {
                rpd.depthAttachment.texture = depthTex->texture;
                rpd.depthAttachment.loadAction = MTLLoadActionClear;
                rpd.depthAttachment.storeAction = MTLStoreActionStore;
                rpd.depthAttachment.clearDepth = clearDepth;
                if (HasStencilAspect(depthTex->format)) {
                    rpd.stencilAttachment.texture = depthTex->texture;
                    rpd.stencilAttachment.loadAction = MTLLoadActionClear;
                    rpd.stencilAttachment.storeAction = MTLStoreActionStore;
                    rpd.stencilAttachment.clearStencil = clearStencil;
                }
            }
        }

        frame.renderEncoder = [frame.commandBuffer renderCommandEncoderWithDescriptor:rpd];
        if (state.validationRequested && frame.renderEncoder)
            frame.renderEncoder.label = @"wf.pass";

        // Clear per-pass dirty state. Bind* calls between Begin and End
        // populate the pending arrays; FlushBindings (Phase D+) drains.
        for (auto& cb : pendingCBs_)
            cb = {};
        for (auto& sr : pendingSRVs_)
            sr = {};
        for (auto& sm : pendingSamplers_)
            sm = {};
        lastBoundPipeline_ = PipelineHandle::Invalid;
    }
}

void MetalCommandList::EndRenderPass() {
    @autoreleasepool {
        auto& state = device_.State();
        FrameContext& frame = state.frames[state.currentFrame];
        if (frame.renderEncoder) {
            [frame.renderEncoder endEncoding];
            frame.renderEncoder = nil;
        }
    }
}

void MetalCommandList::SetViewport(const Viewport& vp) {
    auto& state = device_.State();
    FrameContext& frame = state.frames[state.currentFrame];
    if (!frame.renderEncoder)
        return;
    MTLViewport mvp;
    mvp.originX = vp.x;
    mvp.originY = vp.y;
    mvp.width = vp.width;
    mvp.height = vp.height;
    mvp.znear = vp.minDepth;
    mvp.zfar = vp.maxDepth;
    [frame.renderEncoder setViewport:mvp];
}

void MetalCommandList::SetScissor(const Scissor& sc) {
    auto& state = device_.State();
    FrameContext& frame = state.frames[state.currentFrame];
    if (!frame.renderEncoder)
        return;
    MTLScissorRect r;
    r.x = static_cast<NSUInteger>(sc.x);
    r.y = static_cast<NSUInteger>(sc.y);
    r.width = static_cast<NSUInteger>(sc.width);
    r.height = static_cast<NSUInteger>(sc.height);
    [frame.renderEncoder setScissorRect:r];
}

// ---- Phase B stubs: real bodies land alongside the pipeline / resource
//      work in phases (c)..(d). Keep them no-op so the clear-color path
//      doesn't break.
void MetalCommandList::BindPipeline(PipelineHandle h) {
    lastBoundPipeline_ = h;
}
void MetalCommandList::BindVertexBuffer(u32, BufferHandle, u32, u32) {}
void MetalCommandList::BindIndexBuffer(BufferHandle, Format) {}
void MetalCommandList::BindConstantBuffer(ShaderStage, u32, BufferHandle) {}
void MetalCommandList::BindShaderResource(ShaderStage, u32, TextureHandle) {}
void MetalCommandList::BindShaderResource(ShaderStage, u32, BufferHandle) {}
void MetalCommandList::BindUnorderedAccess(u32, BufferHandle) {}
void MetalCommandList::BindSampler(ShaderStage, u32, SamplerHandle) {}

void MetalCommandList::ClearDepth(TextureHandle, f32, u8) {}
void MetalCommandList::CopyBuffer(BufferHandle, BufferHandle) {}

void MetalCommandList::Draw(u32, u32) {}
void MetalCommandList::DrawIndexed(u32, u32, i32) {}
void MetalCommandList::Dispatch(u32, u32, u32) {}

void MetalCommandList::FlushBindings() {}

} // namespace whiteout::flakes::gfx::metal
