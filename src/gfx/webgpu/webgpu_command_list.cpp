// IGFXCommandList for WebGPU. First BeginRenderPass each frame opens a
// fresh CommandEncoder; subsequent passes keep recording into it until
// Present submits.
//
// FlushBindings translates the per-stage pending arrays into three
// BindGroups (CB with dynamic offsets / SRV / sampler) at every draw —
// the WebGPU equivalent of vulkan_command_list.cpp's FlushDescriptors.

#include "webgpu_command_list.h"
#include "webgpu_device.h"
#include "webgpu_device_state.h"
#include "webgpu_handles.h"
#include "webgpu_translate.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace whiteout::flakes::gfx::webgpu {

namespace {

void EnsureEncoderOpen(WebGPUDeviceState& state) {
    auto& frame = state.frames[state.frameIndex];
    if (frame.recording)
        return;
    // Drain completed deleters at the start of every frame — mirrors the
    // DrainPendingDeletes call inside vulkan_command_list.cpp's
    // EnsureRecording.
    DrainPendingDeletes(state);
    wgpu::CommandEncoderDescriptor cd{};
    cd.label = "wf.frameEncoder";
    frame.encoder = state.device.CreateCommandEncoder(&cd);
    frame.recording = true;
    frame.epoch = state.pendingEpoch + 1;
}

u32 SlotIndex(ShaderStage stage, u32 slot) {
    return (stage == ShaderStage::Pixel) ? (slot + kStageBindingShift) : slot;
}

} // namespace

WebGPUCommandList::WebGPUCommandList(WebGPUDevice& device) : device_(device) {}
WebGPUCommandList::~WebGPUCommandList() = default;

void WebGPUCommandList::BeginRenderPass(TextureHandle color, TextureHandle depth,
                                        const f32 clearColor[4], f32 clearDepth, u8 clearStencil) {
    auto& state = device_.State();
    EnsureEncoderOpen(state);
    auto& frame = state.frames[state.frameIndex];

    auto* colorTex = state.textures.Get(static_cast<u64>(color));
    auto* depthTex = state.textures.Get(static_cast<u64>(depth));

    // Acquire-on-first-bind for swap-chain proxies — symmetric with the
    // Vulkan backend (vulkan_command_list.cpp:138).
    if (colorTex && colorTex->swapChainProxy != SwapChainHandle::Invalid) {
        if (auto* sc = state.swapchains.Get(static_cast<u64>(colorTex->swapChainProxy))) {
            AcquireSwapChainImageIfNeeded(state, *sc);
        }
    }

    activeColorAttachment_ = colorTex ? color : TextureHandle::Invalid;
    activeDepthAttachment_ = depthTex ? depth : TextureHandle::Invalid;
    activeColorFormat_ =
        colorTex ? colorTex->format : wgpu::TextureFormat::Undefined;

    wgpu::RenderPassColorAttachment colorAttach{};
    if (colorTex && colorTex->view) {
        colorAttach.view = colorTex->view;
        colorAttach.loadOp = wgpu::LoadOp::Clear;
        colorAttach.storeOp = wgpu::StoreOp::Store;
        colorAttach.clearValue = {clearColor[0], clearColor[1], clearColor[2], clearColor[3]};
        colorAttach.depthSlice = wgpu::kDepthSliceUndefined;
    }
    auto hasStencilAspect = [](wgpu::TextureFormat f) {
        // WebGPU's depth-stencil formats: only the *Stencil8 variants carry
        // a stencil aspect. Setting stencilLoadOp on a depth-only target
        // fails validation, so gate the stencil ops by format.
        return f == wgpu::TextureFormat::Depth24PlusStencil8 ||
               f == wgpu::TextureFormat::Depth32FloatStencil8 ||
               f == wgpu::TextureFormat::Stencil8;
    };

    wgpu::RenderPassDepthStencilAttachment depthAttach{};
    if (depthTex && depthTex->view) {
        depthAttach.view = depthTex->view;
        depthAttach.depthLoadOp = wgpu::LoadOp::Clear;
        depthAttach.depthStoreOp = wgpu::StoreOp::Store;
        depthAttach.depthClearValue = clearDepth;
        if (hasStencilAspect(depthTex->format)) {
            depthAttach.stencilLoadOp = wgpu::LoadOp::Clear;
            depthAttach.stencilStoreOp = wgpu::StoreOp::Store;
            depthAttach.stencilClearValue = clearStencil;
        }
    } else if (colorTex && colorTex->width > 0 && colorTex->height > 0) {
        // Auto-attach transient depth: the renderer didn't ask for one
        // but every PSO in this codebase keeps dsvFormat set to the
        // device's preferred depth-stencil format. WebGPU's strict
        // attachment-state match would reject SetPipeline otherwise.
        // We allocate / resize once per swap-chain size and discard
        // contents on store. Format must match WebGPUDevice::
        // PreferredDepthStencilFormat() — currently Depth24PlusStencil8.
        const u32 w = static_cast<u32>(colorTex->width);
        const u32 h = static_cast<u32>(colorTex->height);
        if (!state.transientDepthView || state.transientDepthW != w ||
            state.transientDepthH != h) {
            wgpu::TextureDescriptor td{};
            td.label = "wf.transientDepth";
            td.size = {w, h, 1};
            td.mipLevelCount = 1;
            td.sampleCount = 1;
            td.format = wgpu::TextureFormat::Depth24PlusStencil8;
            td.dimension = wgpu::TextureDimension::e2D;
            td.usage = wgpu::TextureUsage::RenderAttachment;
            state.transientDepthTexture = state.device.CreateTexture(&td);
            if (state.transientDepthTexture) {
                wgpu::TextureViewDescriptor vd{};
                state.transientDepthView = state.transientDepthTexture.CreateView(&vd);
                state.transientDepthW = w;
                state.transientDepthH = h;
            }
        }
        if (state.transientDepthView) {
            depthAttach.view = state.transientDepthView;
            depthAttach.depthLoadOp = wgpu::LoadOp::Clear;
            depthAttach.depthStoreOp = wgpu::StoreOp::Discard;
            depthAttach.depthClearValue = 1.0f;
            depthAttach.stencilLoadOp = wgpu::LoadOp::Clear;
            depthAttach.stencilStoreOp = wgpu::StoreOp::Discard;
            depthAttach.stencilClearValue = 0;
        }
    }
    const bool hasDepthAttach = (depthTex && depthTex->view) ||
                                (depthAttach.view != nullptr);

    wgpu::RenderPassDescriptor rpd{};
    rpd.label = "wf.renderPass";
    rpd.colorAttachmentCount = colorTex ? 1 : 0;
    rpd.colorAttachments = colorTex ? &colorAttach : nullptr;
    rpd.depthStencilAttachment = hasDepthAttach ? &depthAttach : nullptr;
    pass_ = frame.encoder.BeginRenderPass(&rpd);

    // Fresh pass: bind groups must be re-emitted; previous draw's state
    // doesn't carry across.
    cbSetDirty_ = true;
    srvSetDirty_ = true;
    samplerSetDirty_ = true;
    lastBoundPipeline_ = PipelineHandle::Invalid;
}

void WebGPUCommandList::EndRenderPass() {
    if (pass_) {
        pass_.End();
        pass_ = nullptr;
    }
    activeColorAttachment_ = TextureHandle::Invalid;
    activeDepthAttachment_ = TextureHandle::Invalid;
    // Drop every pending SRV / sampler so the next pass starts with a
    // clean slate. FlushBindings materializes ALL 28 layout slots into
    // the bind group on every draw; leftover handles from this pass
    // would otherwise leak into the next one and trigger usage-scope
    // conflicts when the renderer re-targets a sampled texture as a
    // render attachment (the shadow-cascade pass is the typical
    // offender — its dst was just sampled by the HD pass before it).
    for (auto& s : pendingSRVs_)
        s = {};
    for (auto& s : pendingSamplers_)
        s = {};
    for (auto& c : pendingCBs_)
        c = {};
    cbSetDirty_ = true;
    srvSetDirty_ = true;
    samplerSetDirty_ = true;
}

void WebGPUCommandList::SetViewport(const Viewport& vp) {
    if (pass_)
        pass_.SetViewport(vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth);
}

void WebGPUCommandList::SetScissor(const Scissor& sc) {
    if (pass_)
        pass_.SetScissorRect(static_cast<u32>(std::max(0, sc.x)),
                             static_cast<u32>(std::max(0, sc.y)),
                             static_cast<u32>(std::max(0, sc.width)),
                             static_cast<u32>(std::max(0, sc.height)));
}

void WebGPUCommandList::BindPipeline(PipelineHandle h) {
    auto& state = device_.State();
    auto* pipe = state.pipelines.Get(static_cast<u64>(h));
    if (!pipe || !pipe->graphics)
        return;
    if (!pass_)
        return;
    pass_.SetPipeline(pipe->graphics);
    // Feed the zero buffer into every phantom vertex slot the pipeline
    // synthesized (see CreateGraphicsPipeline). The renderer never
    // touches these slots via BindVertexBuffer, so Dawn would otherwise
    // reject the draw for missing vertex-buffer state.
    if (state.zeroVertexBuffer) {
        for (u32 slot : pipe->phantomVertexSlots)
            pass_.SetVertexBuffer(slot, state.zeroVertexBuffer, 0, wgpu::kWholeSize);
    }
    lastBoundPipeline_ = h;
}

void WebGPUCommandList::BindVertexBuffer(u32 slot, BufferHandle h, u32 /*stride*/, u32 offset) {
    auto& state = device_.State();
    auto* buf = state.buffers.Get(static_cast<u64>(h));
    if (!buf || !pass_)
        return;
    // currentOffset() = baseOffset + slotStride*currentSlot. For ring
    // sub-allocs the active slot rotates on every Map/UpdateBuffer; we
    // must reference the same slot the data was just written into,
    // not the ring base. Dedicated (non-ring) buffers have slotCount=1
    // so currentOffset() == baseOffset == 0.
    const u64 off = buf->currentOffset() + offset;
    pass_.SetVertexBuffer(slot, buf->buffer, off, wgpu::kWholeSize);
}

void WebGPUCommandList::BindIndexBuffer(BufferHandle h, Format fmt) {
    auto& state = device_.State();
    auto* buf = state.buffers.Get(static_cast<u64>(h));
    if (!buf || !pass_)
        return;
    const wgpu::IndexFormat indexFmt =
        (fmt == Format::R32_UINT) ? wgpu::IndexFormat::Uint32 : wgpu::IndexFormat::Uint16;
    pass_.SetIndexBuffer(buf->buffer, indexFmt, buf->currentOffset(), wgpu::kWholeSize);
}

void WebGPUCommandList::BindConstantBuffer(ShaderStage stage, u32 slot, BufferHandle h) {
    auto& state = device_.State();
    auto* buf = state.buffers.Get(static_cast<u64>(h));
    if (!buf)
        return;
    const u32 idx = (stage == ShaderStage::Pixel) ? (slot + kPsCbBindingOffsetWgsl) : slot;
    if (idx >= kCbBindingCount)
        return;
    pendingCBs_[idx] = {h, buf->currentOffset(), buf->desc.size};
    cbSetDirty_ = true;
}

void WebGPUCommandList::BindShaderResource(ShaderStage stage, u32 slot, TextureHandle h) {
    const u32 idx = SlotIndex(stage, slot);
    pendingSRVs_[idx] = {h, BufferHandle::Invalid, false};
    srvSetDirty_ = true;
}

void WebGPUCommandList::BindShaderResource(ShaderStage stage, u32 slot, BufferHandle h) {
    // Storage-buffer SRV binding. WebGPU's layout encodes binding type at
    // the BindGroupLayout level, so a mixed "SRV is either texture or
    // storage buffer" group needs split layouts — for now record it as a
    // texture-hole entry and emit a warning. Phase-follow-up.
    (void)stage;
    (void)slot;
    (void)h;
    std::fprintf(stderr, "[wgpu] BindShaderResource(buffer) not yet implemented "
                         "(slot %u, stage %d)\n",
                 slot, static_cast<int>(stage));
}

void WebGPUCommandList::BindUnorderedAccess(u32 slot, BufferHandle h) {
    (void)slot;
    (void)h;
    std::fprintf(stderr, "[wgpu] BindUnorderedAccess not yet implemented (slot %u)\n", slot);
}

void WebGPUCommandList::BindSampler(ShaderStage stage, u32 slot, SamplerHandle h) {
    const u32 idx = SlotIndex(stage, slot);
    pendingSamplers_[idx] = {h};
    samplerSetDirty_ = true;
}

void WebGPUCommandList::ClearDepth(TextureHandle depth, f32 clearDepth, u8 clearStencil) {
    // WebGPU has no mid-pass clear — open a one-attachment pass with
    // LoadOp::Clear and end it. Caller must be outside an active pass.
    if (pass_) {
        std::fprintf(stderr, "[wgpu] ClearDepth called inside a render pass — ignored\n");
        return;
    }
    auto& state = device_.State();
    auto* tex = state.textures.Get(static_cast<u64>(depth));
    if (!tex || !tex->view)
        return;
    EnsureEncoderOpen(state);
    auto& frame = state.frames[state.frameIndex];

    auto hasStencilAspect = [](wgpu::TextureFormat f) {
        return f == wgpu::TextureFormat::Depth24PlusStencil8 ||
               f == wgpu::TextureFormat::Depth32FloatStencil8 ||
               f == wgpu::TextureFormat::Stencil8;
    };
    wgpu::RenderPassDepthStencilAttachment ds{};
    ds.view = tex->view;
    ds.depthLoadOp = wgpu::LoadOp::Clear;
    ds.depthStoreOp = wgpu::StoreOp::Store;
    ds.depthClearValue = clearDepth;
    if (hasStencilAspect(tex->format)) {
        ds.stencilLoadOp = wgpu::LoadOp::Clear;
        ds.stencilStoreOp = wgpu::StoreOp::Store;
        ds.stencilClearValue = clearStencil;
    }
    wgpu::RenderPassDescriptor rpd{};
    rpd.label = "wf.clearDepth";
    rpd.colorAttachmentCount = 0;
    rpd.depthStencilAttachment = &ds;
    wgpu::RenderPassEncoder rp = frame.encoder.BeginRenderPass(&rpd);
    rp.End();
}

void WebGPUCommandList::CopyBuffer(BufferHandle dst, BufferHandle src) {
    auto& state = device_.State();
    auto* dstBuf = state.buffers.Get(static_cast<u64>(dst));
    auto* srcBuf = state.buffers.Get(static_cast<u64>(src));
    if (!dstBuf || !srcBuf)
        return;
    if (pass_) {
        std::fprintf(stderr, "[wgpu] CopyBuffer called inside a render pass — ignored\n");
        return;
    }
    EnsureEncoderOpen(state);
    auto& frame = state.frames[state.frameIndex];
    const u64 copySize = std::min(dstBuf->desc.size, srcBuf->desc.size);
    frame.encoder.CopyBufferToBuffer(srcBuf->buffer, srcBuf->baseOffset, dstBuf->buffer,
                                     dstBuf->baseOffset, copySize);
}

void WebGPUCommandList::FlushBindings() {
    if (!pass_)
        return;
    auto& state = device_.State();

    // ---- Group 0: constant buffers (per-draw rebuild, embedded offset)
    // We can't use dynamic offsets — the WebGPU spec caps
    // maxDynamicUniformBuffersPerPipelineLayout at ~8-11 across the whole
    // layout, well below our 32 CB slots. Instead we bake the captured
    // ring-slot offset into BindGroupEntry::offset and rebuild a fresh
    // BindGroup whenever any CB binding changes.
    if (cbSetDirty_) {
        std::vector<wgpu::BindGroupEntry> entries;
        entries.reserve(kCbBindingCount);
        for (u32 i = 0; i < kCbBindingCount; ++i) {
            wgpu::BindGroupEntry e{};
            e.binding = i;
            const auto& pending = pendingCBs_[i];
            if (pending.buffer != BufferHandle{}) {
                auto* buf = state.buffers.Get(static_cast<u64>(pending.buffer));
                if (buf) {
                    e.buffer = buf->buffer;
                    e.offset = pending.offset; // full offset captured at Bind time
                    // Use slotStride (= desc.size rounded up to
                    // minUniformBufferAlign, ≥ 256 bytes) instead of
                    // desc.size. Slang emits WGSL ConstantBuffers under
                    // std140 rules which inflate the struct past what
                    // the engine actually uploads (e.g. SDClassicPSPerDraw
                    // is 48 bytes on the wire but ends up 64 bytes in
                    // WGSL). Dawn rejects the draw when the bound size
                    // is < shader-expected size; the extra padding bytes
                    // belong to this sub-alloc's slot so it's safe.
                    e.size = std::max<u64>(buf->slotStride, 16);
                } else {
                    e.buffer = state.sharedCbBuffer;
                    e.offset = 0;
                    e.size = 16;
                }
            } else {
                // Hole — point at the shared ring base. WebGPU requires
                // every binding slot to be populated.
                e.buffer = state.sharedCbBuffer;
                e.offset = 0;
                e.size = std::max<u64>(state.minUniformBufferAlign, 16);
            }
            entries.push_back(e);
        }
        wgpu::BindGroupDescriptor bgd{};
        bgd.label = "wf.cb";
        bgd.layout = state.cbBgLayout;
        bgd.entryCount = static_cast<u32>(entries.size());
        bgd.entries = entries.data();
        wgpu::BindGroup bg = state.device.CreateBindGroup(&bgd);
        pass_.SetBindGroup(0, bg, 0, nullptr);
        cbSetDirty_ = false;
    }

    // Slot-specific defaults: shadow PS slots need the Depth32Float
    // texture + comparison sampler; IBL probe slots need the
    // cube-array view; everything else falls back to the 2D Float
    // default. Picking the wrong default trips Dawn's sampleType /
    // viewDimension layout match.
    auto defaultTexFor = [&](u32 i) -> wgpu::TextureView {
        if (i >= kPsShadowStartBinding && i < kPsShadowEndBinding)
            return state.defaultDepthTextureView;
        if (i == kPsIblCubeFromBinding || i == kPsIblCubeToBinding)
            return state.defaultCubeArrayTextureView;
        return state.defaultTextureView;
    };
    auto defaultSmpFor = [&](u32 i) -> wgpu::Sampler {
        if (i >= kPsShadowStartBinding && i < kPsShadowEndBinding)
            return state.defaultComparisonSampler;
        return state.defaultSampler;
    };

    // ---- Group 1: SRVs (textures) -------------------------------------
    if (srvSetDirty_) {
        std::vector<wgpu::BindGroupEntry> entries;
        entries.reserve(kSrvBindingCount);
        for (u32 i = 0; i < kSrvBindingCount; ++i) {
            wgpu::BindGroupEntry e{};
            e.binding = i;
            const auto& pending = pendingSRVs_[i];
            wgpu::TextureView view = defaultTexFor(i);
            if (!pending.isBuffer && pending.texture != TextureHandle{}) {
                if (auto* tex = state.textures.Get(static_cast<u64>(pending.texture)))
                    if (tex->view)
                        view = tex->view;
            }
            e.textureView = view;
            entries.push_back(e);
        }
        wgpu::BindGroupDescriptor bgd{};
        bgd.label = "wf.srv";
        bgd.layout = state.srvBgLayout;
        bgd.entryCount = static_cast<u32>(entries.size());
        bgd.entries = entries.data();
        wgpu::BindGroup bg = state.device.CreateBindGroup(&bgd);
        pass_.SetBindGroup(1, bg, 0, nullptr);
        srvSetDirty_ = false;
    }

    // ---- Group 2: samplers --------------------------------------------
    if (samplerSetDirty_) {
        std::vector<wgpu::BindGroupEntry> entries;
        entries.reserve(kSamplerBindingCount);
        for (u32 i = 0; i < kSamplerBindingCount; ++i) {
            wgpu::BindGroupEntry e{};
            e.binding = i;
            const auto& pending = pendingSamplers_[i];
            wgpu::Sampler smp = defaultSmpFor(i);
            if (pending.sampler != SamplerHandle{}) {
                if (auto* s = state.samplers.Get(static_cast<u64>(pending.sampler)))
                    if (s->sampler)
                        smp = s->sampler;
            }
            e.sampler = smp;
            entries.push_back(e);
        }
        wgpu::BindGroupDescriptor bgd{};
        bgd.label = "wf.sampler";
        bgd.layout = state.samplerBgLayout;
        bgd.entryCount = static_cast<u32>(entries.size());
        bgd.entries = entries.data();
        wgpu::BindGroup bg = state.device.CreateBindGroup(&bgd);
        pass_.SetBindGroup(2, bg, 0, nullptr);
        samplerSetDirty_ = false;
    }
}

void WebGPUCommandList::Draw(u32 vertexCount, u32 firstVertex) {
    if (!pass_)
        return;
    FlushBindings();
    pass_.Draw(vertexCount, 1, firstVertex, 0);
}

void WebGPUCommandList::DrawIndexed(u32 indexCount, u32 firstIndex, i32 baseVertex) {
    if (!pass_)
        return;
    FlushBindings();
    pass_.DrawIndexed(indexCount, 1, firstIndex, baseVertex, 0);
}

void WebGPUCommandList::Dispatch(u32 gx, u32 gy, u32 gz) {
    // Compute path: opens its own ComputePassEncoder, dispatches once,
    // ends. Bind-group flush for compute is unimplemented in this pass
    // — none of the renderer's current passes touch Dispatch().
    (void)gx;
    (void)gy;
    (void)gz;
    std::fprintf(stderr, "[wgpu] Dispatch not yet implemented\n");
}

} // namespace whiteout::flakes::gfx::webgpu
