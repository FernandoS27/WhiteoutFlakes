// MetalCommandList — Phase D implementation.
//
// Real per-stage binding + Draw / DrawIndexed. Bind* stashes into
// scratch arrays; FlushBindings translates the dirty subset into
// setVertex*/setFragment* calls on the active render encoder right
// before each Draw. The (stage, slot) → Metal per-stage buffer/texture/
// sampler index mapping mirrors slangc's emit:
//   • VS  slot N → vertex   stage index N
//   • PS  slot N → fragment stage index (N - kStageBindingShift)
// kStageBindingShift = 16 keeps PS slot bookkeeping in the upper half
// of the abstract index space (consistent with the Vulkan / WebGPU
// backends), while the per-stage Metal call sees a clean 0-based index.

#include "metal_command_list.h"
#include "metal_device.h"
#include "metal_device_state.h"
#include "metal_handles.h"
#include "metal_translate.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <cstdio>

namespace whiteout::flakes::gfx::metal {

// Defined in metal_swap_chain.mm.
void AcquireSwapChainImageIfNeeded(MetalDeviceState& state, SwapChainEntry& sc);
// Defined in metal_delete_queue.mm.
void DrainPendingDeletes(MetalDeviceState& state);

namespace {

// Lazily open the per-frame command buffer at first BeginRenderPass /
// CopyBuffer / Dispatch. Mirrors webgpu_command_list.cpp's
// EnsureEncoderOpen pattern: one command buffer per frame, Present
// commits, addCompletedHandler bumps completedEpoch so the deferred-
// delete queue (drained right here at frame-open) can release safely.
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

SwapChainEntry* SwapChainOwnerOfTexture(MetalDeviceState& state, TextureHandle h) {
    if (h == TextureHandle::Invalid)
        return nullptr;
    auto* tex = state.textures.Get(static_cast<u64>(h));
    if (!tex || tex->swapChainProxy == SwapChainHandle::Invalid)
        return nullptr;
    return state.swapchains.Get(static_cast<u64>(tex->swapChainProxy));
}

// Translate a (stage, slot) into the per-stage Metal index. Encoded
// (idx) lives in [0, kStageBindingShift) for VS and
// [kStageBindingShift, 2*kStageBindingShift) for PS. The flush picks
// per-stage setters from the encoded idx — VS first, then PS minus the
// shift.
inline bool IsPixelIdx(u32 idx) {
    return idx >= kStageBindingShift;
}
inline u32 StageLocalIdx(u32 idx) {
    return IsPixelIdx(idx) ? idx - kStageBindingShift : idx;
}
inline u32 EncodeIdx(ShaderStage stage, u32 slot) {
    return (stage == ShaderStage::Pixel ? kStageBindingShift : 0u) + slot;
}

} // namespace

MetalCommandList::MetalCommandList(MetalDevice& device) : device_(device) {}
MetalCommandList::~MetalCommandList() = default;

// ============================================================
// BeginRenderPass / EndRenderPass
// ============================================================

void MetalCommandList::BeginRenderPass(TextureHandle color, TextureHandle depth,
                                       const f32 clearColor[4], f32 clearDepth,
                                       u8 clearStencil) {
    @autoreleasepool {
        auto& state = device_.State();

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

        // Reset per-pass state. Encoders don't carry over state across
        // their own end/begin pair, so any cached "last bound" must
        // re-issue. We re-issue lazily through the dirty arrays.
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

// ============================================================
// Viewport / Scissor
// ============================================================

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

// ============================================================
// Pipeline + binds (record-only; Flush at Draw time)
// ============================================================

void MetalCommandList::BindPipeline(PipelineHandle h) {
    lastBoundPipeline_ = h;
    // Apply immediately (cheap on Metal — it's one ObjC call). Draw
    // doesn't need to re-issue. The encoder still requires a fresh
    // setRenderPipelineState every pass because pass state doesn't
    // carry across encoders; FlushBindings re-applies on first Draw of
    // a pass via the same lastBoundPipeline_ check.
    auto& state = device_.State();
    FrameContext& frame = state.frames[state.currentFrame];
    if (!frame.renderEncoder)
        return;
    auto* pso = state.pipelines.Get(static_cast<u64>(h));
    if (!pso || !pso->graphics)
        return;
    [frame.renderEncoder setRenderPipelineState:pso->graphics];
    if (pso->depthStencil)
        [frame.renderEncoder setDepthStencilState:pso->depthStencil];
    [frame.renderEncoder setCullMode:pso->cull];
    [frame.renderEncoder setFrontFacingWinding:pso->winding];
}

void MetalCommandList::BindVertexBuffer(u32 slot, BufferHandle h, u32 /*stride*/,
                                        u32 offset) {
    if (slot >= kMaxVertexBufferSlots)
        return;
    auto& state = device_.State();
    FrameContext& frame = state.frames[state.currentFrame];
    if (!frame.renderEncoder)
        return;
    auto* buf = state.buffers.Get(static_cast<u64>(h));
    if (!buf || !buf->buffer)
        return;
    // Stride is baked into the PSO's vertex descriptor at create time
    // (per Wc3 / WebGPU/Vulkan parity); Metal reads it from
    // MTLVertexBufferLayoutDescriptor.stride. We ignore the runtime
    // stride arg.
    [frame.renderEncoder setVertexBuffer:buf->buffer
                                  offset:buf->currentOffset() + offset
                                 atIndex:kVertexBufferIndexBase + slot];
}

void MetalCommandList::BindIndexBuffer(BufferHandle h, Format fmt) {
    auto& state = device_.State();
    // Stash for DrawIndexed; Metal binds the index buffer at draw time,
    // not via a SetIndexBuffer-style encoder method.
    auto* buf = state.buffers.Get(static_cast<u64>(h));
    if (!buf)
        return;
    pendingIndex_.buffer = buf->buffer;
    pendingIndex_.offset = buf->currentOffset();
    pendingIndex_.format = (fmt == Format::R32_UINT) ? MTLIndexTypeUInt32
                                                     : MTLIndexTypeUInt16;
}

void MetalCommandList::BindConstantBuffer(ShaderStage stage, u32 slot, BufferHandle h) {
    if (slot >= kCbBindingCount)
        return;
    const u32 idx = EncodeIdx(stage, slot);
    auto& state = device_.State();
    auto* buf = state.buffers.Get(static_cast<u64>(h));
    if (!buf || !buf->buffer) {
        pendingCBs_[idx] = {};
        return;
    }
    pendingCBs_[idx].buffer = h;
    pendingCBs_[idx].offset = buf->currentOffset();
    pendingCBs_[idx].size = buf->desc.size;
}

void MetalCommandList::BindShaderResource(ShaderStage stage, u32 slot, TextureHandle h) {
    if (slot >= kSrvBindingCount)
        return;
    const u32 idx = EncodeIdx(stage, slot);
    pendingSRVs_[idx].texture = h;
    pendingSRVs_[idx].storage = BufferHandle::Invalid;
    pendingSRVs_[idx].isBuffer = false;
}

void MetalCommandList::BindShaderResource(ShaderStage stage, u32 slot, BufferHandle h) {
    if (slot >= kSrvBindingCount)
        return;
    const u32 idx = EncodeIdx(stage, slot);
    pendingSRVs_[idx].texture = TextureHandle::Invalid;
    pendingSRVs_[idx].storage = h;
    pendingSRVs_[idx].isBuffer = true;
}

void MetalCommandList::BindUnorderedAccess(u32 /*slot*/, BufferHandle /*h*/) {
    // UAVs land in Phase G alongside compute. Wc3's graphics pipeline
    // doesn't use them; this stub keeps the IGFXCommandList surface
    // complete.
}

void MetalCommandList::BindSampler(ShaderStage stage, u32 slot, SamplerHandle h) {
    if (slot >= kSamplerBindingCount)
        return;
    const u32 idx = EncodeIdx(stage, slot);
    pendingSamplers_[idx].sampler = h;
}

// ============================================================
// FlushBindings — translate pending arrays → per-stage Metal calls
// ============================================================

void MetalCommandList::FlushBindings() {
    auto& state = device_.State();
    FrameContext& frame = state.frames[state.currentFrame];
    if (!frame.renderEncoder)
        return;

    // CBs. Iterate both halves of the encoded index space; route to
    // setVertexBuffer / setFragmentBuffer based on the half. Note: we
    // could dedupe-suppress identical re-binds here for perf, but Wc3
    // updates the bound buffer offset every draw (ring-allocated CBs)
    // so the per-draw cost is unavoidable.
    for (u32 i = 0; i < pendingCBs_.size(); ++i) {
        const auto& pc = pendingCBs_[i];
        if (pc.buffer == BufferHandle::Invalid)
            continue;
        auto* buf = state.buffers.Get(static_cast<u64>(pc.buffer));
        if (!buf || !buf->buffer)
            continue;
        const u32 stageIdx = StageLocalIdx(i);
        // Re-read currentOffset() in case the buffer rotated between
        // Bind and FlushBindings (the renderer typically Maps then
        // Binds, so the slot is settled — but UpdateBuffer paths can
        // rotate after Bind).
        const u64 off = buf->currentOffset();
        if (IsPixelIdx(i))
            [frame.renderEncoder setFragmentBuffer:buf->buffer
                                            offset:off
                                           atIndex:stageIdx];
        else
            [frame.renderEncoder setVertexBuffer:buf->buffer
                                          offset:off
                                         atIndex:stageIdx];
    }

    // SRVs (textures + storage buffers). Storage-buffer SRVs share the
    // buffer index space with CBs on Metal — we slot them above the CB
    // range to avoid colliding with [[buffer(0..15)]] CBs.
    for (u32 i = 0; i < pendingSRVs_.size(); ++i) {
        const auto& ps = pendingSRVs_[i];
        const u32 stageIdx = StageLocalIdx(i);
        if (!ps.isBuffer && ps.texture != TextureHandle::Invalid) {
            auto* tex = state.textures.Get(static_cast<u64>(ps.texture));
            if (!tex || !tex->texture)
                continue;
            if (IsPixelIdx(i))
                [frame.renderEncoder setFragmentTexture:tex->texture atIndex:stageIdx];
            else
                [frame.renderEncoder setVertexTexture:tex->texture atIndex:stageIdx];
        } else if (ps.isBuffer && ps.storage != BufferHandle::Invalid) {
            auto* buf = state.buffers.Get(static_cast<u64>(ps.storage));
            if (!buf || !buf->buffer)
                continue;
            // Storage buffers go at [[buffer(kCbBindingCount + slot)]]
            // — slang's set=1 binding=N collapses to per-stage index
            // kCbBindingCount + N (a fixed offset, applied symmetrically
            // both stages). Keeps the CB index range clean.
            const u32 sbIdx = kCbBindingCount + stageIdx;
            const u64 off = buf->currentOffset();
            if (IsPixelIdx(i))
                [frame.renderEncoder setFragmentBuffer:buf->buffer
                                                offset:off
                                               atIndex:sbIdx];
            else
                [frame.renderEncoder setVertexBuffer:buf->buffer
                                              offset:off
                                             atIndex:sbIdx];
        }
    }

    // Samplers.
    for (u32 i = 0; i < pendingSamplers_.size(); ++i) {
        const auto& smp = pendingSamplers_[i];
        if (smp.sampler == SamplerHandle::Invalid)
            continue;
        auto* s = state.samplers.Get(static_cast<u64>(smp.sampler));
        if (!s || !s->sampler)
            continue;
        const u32 stageIdx = StageLocalIdx(i);
        if (IsPixelIdx(i))
            [frame.renderEncoder setFragmentSamplerState:s->sampler atIndex:stageIdx];
        else
            [frame.renderEncoder setVertexSamplerState:s->sampler atIndex:stageIdx];
    }
}

// ============================================================
// Draw / DrawIndexed / Dispatch
// ============================================================

void MetalCommandList::Draw(u32 vertexCount, u32 firstVertex) {
    auto& state = device_.State();
    FrameContext& frame = state.frames[state.currentFrame];
    if (!frame.renderEncoder || vertexCount == 0)
        return;
    FlushBindings();
    auto* pso = state.pipelines.Get(static_cast<u64>(lastBoundPipeline_));
    const MTLPrimitiveType prim = pso ? pso->primitive : MTLPrimitiveTypeTriangle;
    [frame.renderEncoder drawPrimitives:prim
                            vertexStart:firstVertex
                            vertexCount:vertexCount];
}

void MetalCommandList::DrawIndexed(u32 indexCount, u32 firstIndex, i32 baseVertex) {
    auto& state = device_.State();
    FrameContext& frame = state.frames[state.currentFrame];
    if (!frame.renderEncoder || indexCount == 0 || !pendingIndex_.buffer)
        return;
    FlushBindings();
    auto* pso = state.pipelines.Get(static_cast<u64>(lastBoundPipeline_));
    const MTLPrimitiveType prim = pso ? pso->primitive : MTLPrimitiveTypeTriangle;
    const NSUInteger indexSize =
        (pendingIndex_.format == MTLIndexTypeUInt32) ? 4 : 2;
    [frame.renderEncoder drawIndexedPrimitives:prim
                                    indexCount:indexCount
                                     indexType:pendingIndex_.format
                                   indexBuffer:pendingIndex_.buffer
                             indexBufferOffset:pendingIndex_.offset
                                                 + firstIndex * indexSize
                                 instanceCount:1
                                    baseVertex:baseVertex
                                  baseInstance:0];
}

void MetalCommandList::Dispatch(u32, u32, u32) {
    // Phase G.
}

// ============================================================
// Misc: ClearDepth / CopyBuffer
// ============================================================

void MetalCommandList::ClearDepth(TextureHandle depth, f32 clearDepth, u8 clearStencil) {
    @autoreleasepool {
        auto& state = device_.State();
        auto* depthTex = state.textures.Get(static_cast<u64>(depth));
        if (!depthTex || !depthTex->texture)
            return;
        FrameContext& frame = EnsureFrameOpen(state);
        // Render-pass without a color attachment that only clears depth
        // / stencil. The encoder is closed immediately after the
        // implicit load — Metal performs the clear during render-pass
        // setup, no draw needed.
        MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
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
        id<MTLRenderCommandEncoder> enc =
            [frame.commandBuffer renderCommandEncoderWithDescriptor:rpd];
        [enc endEncoding];
    }
}

void MetalCommandList::CopyBuffer(BufferHandle dst, BufferHandle src) {
    @autoreleasepool {
        auto& state = device_.State();
        auto* d = state.buffers.Get(static_cast<u64>(dst));
        auto* s = state.buffers.Get(static_cast<u64>(src));
        if (!d || !s || !d->buffer || !s->buffer)
            return;
        const u64 size = std::min(d->desc.size, s->desc.size);
        if (size == 0)
            return;
        FrameContext& frame = EnsureFrameOpen(state);
        if (frame.renderEncoder) {
            // Blit and render encoders can't coexist on the same
            // command buffer; close the render encoder before opening
            // blit. Caller is responsible for re-binding on the next
            // BeginRenderPass — the engine pattern is "EndRenderPass
            // then CopyBuffer then BeginRenderPass".
            [frame.renderEncoder endEncoding];
            frame.renderEncoder = nil;
        }
        id<MTLBlitCommandEncoder> blit = [frame.commandBuffer blitCommandEncoder];
        [blit copyFromBuffer:s->buffer
                sourceOffset:s->currentOffset()
                    toBuffer:d->buffer
           destinationOffset:d->currentOffset()
                        size:size];
        [blit endEncoding];
    }
}

} // namespace whiteout::flakes::gfx::metal
