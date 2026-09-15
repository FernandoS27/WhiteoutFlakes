// IGFXCommandList for WebGPU. First BeginRenderPass each frame opens a
// fresh CommandEncoder; subsequent passes keep recording into it until
// Present submits.
//
// FlushBindings translates the pending arrays into one BindGroup per group
// of the bound pipeline's layout at every draw — the WebGPU equivalent of
// vulkan_command_list.cpp's FlushDescriptors.

#include "webgpu_command_list.h"
#include "webgpu_device.h"
#include "webgpu_device_state.h"
#include "webgpu_handles.h"
#include "webgpu_translate.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <unordered_set>
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

// FNV-1a 64-bit. Used to compute a cache key over the materialized
// bind-group entry tuple. Collision risk at our scale (<1k live bind
// groups) is negligible.
inline u64 HashMix(u64 h, u64 v) {
    h ^= v;
    h *= 0x100000001b3ull;
    return h;
}
constexpr u64 kFnv1aOffsetBasis = 0xcbf29ce484222325ull;

// Bound size for an unfilled uniform entry: covers the largest cbuffer the
// shaders declare (WC3 HD PS b1 is 2352 B).
constexpr u64 kUniformHoleBytes = 8192;

// Log a binding mismatch once per (binding, reason) so a bad slot shows up
// without flooding stderr every draw.
void WarnOnce(u32 binding, u32 reason, const char* what) {
    static std::unordered_set<u64> seen;
    if (!seen.insert((static_cast<u64>(reason) << 32) | binding).second)
        return;
    std::fprintf(stderr, "[wgpu] @binding(%u): %s; binding a default instead\n", binding, what);
}

bool HasStencilAspect(wgpu::TextureFormat f) {
    return f == wgpu::TextureFormat::Depth24PlusStencil8 ||
           f == wgpu::TextureFormat::Depth32FloatStencil8 || f == wgpu::TextureFormat::Stencil8;
}

bool IsDepthFormat(wgpu::TextureFormat f) {
    return f == wgpu::TextureFormat::Depth16Unorm || f == wgpu::TextureFormat::Depth24Plus ||
           f == wgpu::TextureFormat::Depth32Float || HasStencilAspect(f);
}

// Whether a texture of format `f` may fill a layout entry of sample type `want`.
bool SampleTypeAccepts(wgpu::TextureSampleType want, wgpu::TextureFormat f, bool float32Filterable) {
    using F = wgpu::TextureFormat;
    using S = wgpu::TextureSampleType;
    S native = S::Float;
    switch (f) {
    case F::Depth16Unorm:
    case F::Depth24Plus:
    case F::Depth24PlusStencil8:
    case F::Depth32Float:
    case F::Depth32FloatStencil8:
        native = S::Depth;
        break;
    case F::Stencil8:
    case F::R8Uint:
    case F::R16Uint:
    case F::R32Uint:
    case F::RG8Uint:
    case F::RG16Uint:
    case F::RG32Uint:
    case F::RGBA8Uint:
    case F::RGBA16Uint:
    case F::RGBA32Uint:
    case F::RGB10A2Uint:
        native = S::Uint;
        break;
    case F::R8Sint:
    case F::R16Sint:
    case F::R32Sint:
    case F::RG8Sint:
    case F::RG16Sint:
    case F::RG32Sint:
    case F::RGBA8Sint:
    case F::RGBA16Sint:
    case F::RGBA32Sint:
        native = S::Sint;
        break;
    case F::R32Float:
    case F::RG32Float:
    case F::RGBA32Float:
        native = float32Filterable ? S::Float : S::UnfilterableFloat;
        break;
    default:
        break;
    }
    if (want == S::UnfilterableFloat)
        return native == S::Float || native == S::UnfilterableFloat || native == S::Depth;
    return want == native;
}

u32 LayerCountFor(wgpu::TextureViewDimension dim, u32 layers) {
    switch (dim) {
    case wgpu::TextureViewDimension::Cube:
        return 6;
    case wgpu::TextureViewDimension::e2DArray:
    case wgpu::TextureViewDimension::CubeArray:
        return layers;
    default:
        return 1;
    }
}

// 1x1 default of the declared sample type and dimension (textures from
// CreateDefaultResources), cached per combination.
wgpu::TextureView DefaultView(WebGPUDeviceState& state, const wgpu::TextureBindingLayout& tl) {
    const u32 key = (static_cast<u32>(tl.sampleType) << 8) | static_cast<u32>(tl.viewDimension);
    if (auto it = state.defaultViews.find(key); it != state.defaultViews.end())
        return it->second;
    using S = wgpu::TextureSampleType;
    wgpu::Texture tex = tl.viewDimension == wgpu::TextureViewDimension::e3D ? state.defaultTexture3D
                        : tl.sampleType == S::Depth                         ? state.defaultDepthTexture
                        : tl.sampleType == S::Uint                          ? state.defaultUintTexture
                        : tl.sampleType == S::Sint                          ? state.defaultSintTexture
                                                                            : state.defaultTexture;
    wgpu::TextureViewDescriptor vd{};
    vd.dimension = tl.viewDimension;
    if (tl.viewDimension != wgpu::TextureViewDimension::e3D)
        vd.arrayLayerCount = LayerCountFor(tl.viewDimension, 6);
    if (tl.sampleType == S::Depth)
        vd.aspect = wgpu::TextureAspect::DepthOnly;
    wgpu::TextureView view = tex.CreateView(&vd);
    state.defaultViews.emplace(key, view);
    return view;
}

// View of `tex` that satisfies a layout entry, or null when the texture can't
// (wrong sample type, too few layers for a cube, a 3D request). Views of
// another dimension than the texture's own, and depth-only views of
// depth-stencil textures, are created once and cached on the entry.
wgpu::TextureView ViewFor(WebGPUDeviceState& state, TextureEntry& tex,
                          const wgpu::TextureBindingLayout& tl) {
    if (!tex.view || !SampleTypeAccepts(tl.sampleType, tex.format, state.float32Filterable))
        return nullptr;
    const bool depthOnly = HasStencilAspect(tex.format);
    if (tl.viewDimension == tex.viewDimension && !depthOnly)
        return tex.view;
    if (!tex.ownsTexture || !tex.texture)
        return nullptr;
    const u32 layers = static_cast<u32>(std::max(1, tex.arraySize));
    const auto dim = tl.viewDimension;
    if (dim == wgpu::TextureViewDimension::e3D || dim == wgpu::TextureViewDimension::e1D ||
        (dim == wgpu::TextureViewDimension::Cube && layers < 6) ||
        (dim == wgpu::TextureViewDimension::CubeArray && layers % 6 != 0))
        return nullptr;
    auto& slot = tex.altViews[static_cast<u32>(dim) % tex.altViews.size()];
    if (!slot) {
        wgpu::TextureViewDescriptor vd{};
        vd.format = tex.format;
        vd.dimension = dim;
        vd.arrayLayerCount = LayerCountFor(dim, layers);
        vd.aspect = depthOnly ? wgpu::TextureAspect::DepthOnly : wgpu::TextureAspect::All;
        slot = tex.texture.CreateView(&vd);
    }
    return slot;
}

} // namespace

WebGPUCommandList::WebGPUCommandList(WebGPUDevice& device) : device_(device) {}
WebGPUCommandList::~WebGPUCommandList() = default;

void WebGPUCommandList::BeginRenderPass(TextureHandle color, TextureHandle depth,
                                        const f32 clearColor[4], f32 clearDepth, u8 clearStencil) {
    // The shadow pass calls with color=Invalid and clearColor=nullptr —
    // memcpy from a null source would null-deref. Zero-init when null;
    // the downstream MRT path skips the clear if the color handle is
    // Invalid anyway.
    f32 clears[1][4] = {};
    if (clearColor)
        std::memcpy(clears[0], clearColor, sizeof(clears[0]));
    const TextureHandle colors[1] = {color};
    BeginRenderPass(colors, 1, depth, clears, clearDepth, clearStencil);
}

void WebGPUCommandList::BeginRenderPass(const TextureHandle* colors, u32 colorCount,
                                        TextureHandle depth, const f32 (*clearColors)[4],
                                        f32 clearDepth, u8 clearStencil) {
    assert(colorCount <= kMaxColorAttachments && "WebGPU BeginRenderPass: too many color attachments");
    auto& state = device_.State();
    EnsureEncoderOpen(state);
    auto& frame = state.frames[state.frameIndex];

    // Lookup every requested color attachment; track slot 0 in the legacy
    // active* fields so existing single-target consumers (PSO format match
    // and friends) keep working.
    wgpu::RenderPassColorAttachment colorAttaches[kMaxColorAttachments] = {};
    u32 boundColorCount = 0;
    auto* slot0Tex = (colorCount > 0) ? state.textures.Get(static_cast<u64>(colors[0])) : nullptr;
    auto* depthTex = state.textures.Get(static_cast<u64>(depth));

    // Acquire-on-first-bind for swap-chain proxies — symmetric with the
    // Vulkan backend (vulkan_command_list.cpp:138). Walk every attachment
    // so an MRT pass that includes the back-buffer in any slot still works.
    for (u32 i = 0; i < colorCount; ++i) {
        auto* tex = state.textures.Get(static_cast<u64>(colors[i]));
        if (tex && tex->swapChainProxy != SwapChainHandle::Invalid) {
            if (auto* sc = state.swapchains.Get(static_cast<u64>(tex->swapChainProxy))) {
                AcquireSwapChainImageIfNeeded(state, *sc);
            }
        }
    }

    activeColorAttachment_ = slot0Tex ? colors[0] : TextureHandle::Invalid;
    activeDepthAttachment_ = depthTex ? depth : TextureHandle::Invalid;
    activeColorFormat_ = slot0Tex ? slot0Tex->format : wgpu::TextureFormat::Undefined;

    for (u32 i = 0; i < colorCount; ++i) {
        auto* tex = state.textures.Get(static_cast<u64>(colors[i]));
        if (!tex || !tex->view)
            continue;
        auto& a = colorAttaches[boundColorCount++];
        a.view = tex->view;
        a.loadOp = wgpu::LoadOp::Clear;
        a.storeOp = wgpu::StoreOp::Store;
        a.clearValue = {clearColors[i][0], clearColors[i][1], clearColors[i][2], clearColors[i][3]};
        a.depthSlice = wgpu::kDepthSliceUndefined;
    }
    // Compatibility alias for code further down that still references the
    // pre-MRT single-color variable name.
    auto* colorTex = slot0Tex;
    (void)colorTex;
    auto hasStencilAspect = [](wgpu::TextureFormat f) {
        // WebGPU's depth-stencil formats: only the *Stencil8 variants carry
        // a stencil aspect. Setting stencilLoadOp on a depth-only target
        // fails validation, so gate the stencil ops by format.
        return f == wgpu::TextureFormat::Depth24PlusStencil8 ||
               f == wgpu::TextureFormat::Depth32FloatStencil8 || f == wgpu::TextureFormat::Stencil8;
    };

    wgpu::RenderPassDepthStencilAttachment depthAttach{};
    if (depthTex && depthTex->view) {
        // BeginDepthSlicePass routes through here with a slice selected.
        depthAttach.view = (depthSlice_ < depthTex->sliceViews.size())
                               ? depthTex->sliceViews[depthSlice_]
                               : depthTex->view;
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
        if (!state.transientDepthView || state.transientDepthW != w || state.transientDepthH != h) {
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
    const bool hasDepthAttach = (depthTex && depthTex->view) || (depthAttach.view != nullptr);

    wgpu::RenderPassDescriptor rpd{};
    rpd.label = "wf.renderPass";
    rpd.colorAttachmentCount = boundColorCount;
    rpd.colorAttachments = boundColorCount ? colorAttaches : nullptr;
    rpd.depthStencilAttachment = hasDepthAttach ? &depthAttach : nullptr;
    pass_ = frame.encoder.BeginRenderPass(&rpd);

    // Fresh pass: bind groups must be re-emitted; previous draw's state
    // doesn't carry across.
    ResetPassState();
}

void WebGPUCommandList::ResetPassState() {
    dirtyKinds_ = kBindKindConstant | kBindKindResource | kBindKindSampler;
    lastBoundPipeline_ = PipelineHandle::Invalid;
    groupCount_ = 0;
    requestedVBs_ = {};
    encoderVBs_ = {};
    phantomSlot_ = -1;
    lastIndexBuffer_ = BufferHandle{};
    lastIndexOffset_ = 0;
    lastGroupKeySet_ = {};
}

bool WebGPUCommandList::BeginDepthSlicePass(TextureHandle depth, u32 arraySlice, f32 clearDepth,
                                            u8 clearStencil) {
    auto& state = device_.State();
    auto* tex = state.textures.Get(static_cast<u64>(depth));
    if (!tex || !tex->texture || !IsDepthFormat(tex->format) ||
        arraySlice >= static_cast<u32>(std::max(1, tex->arraySize)))
        return false;
    if (tex->sliceViews.size() <= arraySlice)
        tex->sliceViews.resize(arraySlice + 1);
    if (!tex->sliceViews[arraySlice]) {
        wgpu::TextureViewDescriptor vd{};
        vd.format = tex->format;
        vd.dimension = wgpu::TextureViewDimension::e2D;
        vd.baseArrayLayer = arraySlice;
        vd.arrayLayerCount = 1;
        vd.mipLevelCount = 1;
        tex->sliceViews[arraySlice] = tex->texture.CreateView(&vd);
    }
    depthSlice_ = arraySlice;
    BeginRenderPass(nullptr, 0, depth, nullptr, clearDepth, clearStencil);
    depthSlice_ = kNoDepthSlice;
    return true;
}

void WebGPUCommandList::BeginRenderPassLoad(TextureHandle color, TextureHandle depth,
                                            f32 clearDepth, u8 clearStencil, bool loadDepth) {
    auto& state = device_.State();
    EnsureEncoderOpen(state);
    auto& frame = state.frames[state.frameIndex];

    auto* colorTex = state.textures.Get(static_cast<u64>(color));
    auto* depthTex = state.textures.Get(static_cast<u64>(depth));

    if (colorTex && colorTex->swapChainProxy != SwapChainHandle::Invalid) {
        if (auto* sc = state.swapchains.Get(static_cast<u64>(colorTex->swapChainProxy))) {
            AcquireSwapChainImageIfNeeded(state, *sc);
        }
    }

    activeColorAttachment_ = colorTex ? color : TextureHandle::Invalid;
    activeDepthAttachment_ = depthTex ? depth : TextureHandle::Invalid;
    activeColorFormat_ = colorTex ? colorTex->format : wgpu::TextureFormat::Undefined;

    // Color attachment with loadOp=Load — preserves prior contents.
    wgpu::RenderPassColorAttachment colorAttach{};
    if (colorTex && colorTex->view) {
        colorAttach.view = colorTex->view;
        colorAttach.loadOp = wgpu::LoadOp::Load;
        colorAttach.storeOp = wgpu::StoreOp::Store;
        colorAttach.depthSlice = wgpu::kDepthSliceUndefined;
    }

    auto hasStencilAspect = [](wgpu::TextureFormat f) {
        return f == wgpu::TextureFormat::Depth24PlusStencil8 ||
               f == wgpu::TextureFormat::Depth32FloatStencil8 || f == wgpu::TextureFormat::Stencil8;
    };

    wgpu::RenderPassDepthStencilAttachment depthAttach{};
    if (depthTex && depthTex->view) {
        depthAttach.view = depthTex->view;
        depthAttach.depthLoadOp = loadDepth ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear;
        depthAttach.depthStoreOp = wgpu::StoreOp::Store;
        depthAttach.depthClearValue = clearDepth;
        if (hasStencilAspect(depthTex->format)) {
            depthAttach.stencilLoadOp = loadDepth ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear;
            depthAttach.stencilStoreOp = wgpu::StoreOp::Store;
            depthAttach.stencilClearValue = clearStencil;
        }
    }
    const bool hasDepthAttach = (depthTex && depthTex->view);

    wgpu::RenderPassDescriptor rpd{};
    rpd.label = "wf.renderPassLoad";
    rpd.colorAttachmentCount = colorTex ? 1u : 0u;
    rpd.colorAttachments = colorTex ? &colorAttach : nullptr;
    rpd.depthStencilAttachment = hasDepthAttach ? &depthAttach : nullptr;
    pass_ = frame.encoder.BeginRenderPass(&rpd);
    ResetPassState();
}

void WebGPUCommandList::EndRenderPass() {
    if (pass_) {
        pass_.End();
        pass_ = nullptr;
    }
    activeColorAttachment_ = TextureHandle::Invalid;
    activeDepthAttachment_ = TextureHandle::Invalid;
    // Drop every pending SRV / sampler so the next pass starts with a
    // clean slate. FlushBindings materializes every layout entry into
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
    dirtyKinds_ = kBindKindConstant | kBindKindResource | kBindKindSampler;
}

void WebGPUCommandList::SetViewport(const Viewport& vp) {
    if (pass_)
        pass_.SetViewport(vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth);
}

void WebGPUCommandList::SetScissor(const Scissor& sc) {
    if (pass_)
        pass_.SetScissorRect(
            static_cast<u32>(std::max(0, sc.x)), static_cast<u32>(std::max(0, sc.y)),
            static_cast<u32>(std::max(0, sc.width)), static_cast<u32>(std::max(0, sc.height)));
}

void WebGPUCommandList::BindPipeline(PipelineHandle h) {
    auto& state = device_.State();
    if (h == lastBoundPipeline_)
        return; // already set on the active pass — skip the API call.
    auto* pipe = state.pipelines.Get(static_cast<u64>(h));
    if (!pipe || !pipe->graphics)
        return;
    if (!pass_)
        return;
    pass_.SetPipeline(pipe->graphics);
    // FlushBindings compares each group's key, which starts with the layout
    // id, so a pipeline with other layouts re-sets exactly the groups that differ.
    if (groupCount_ != pipe->groupCount || groupLayouts_ != pipe->groupLayouts)
        dirtyKinds_ = kBindKindConstant | kBindKindResource | kBindKindSampler;
    groupCount_ = pipe->groupCount;
    groupLayouts_ = pipe->groupLayouts;
    // The phantom slot (see CreateGraphicsPipeline) gets the zero buffer. It
    // can be a slot the renderer bound real data to for another pipeline —
    // a rigid PSO's phantoms land on the skinned PSOs' bone slot — and D3D
    // keeps that binding across pipeline changes, so the renderer won't
    // rebind it. Put the requested buffer back when the slot stops being
    // this pipeline's phantom.
    const i32 previousPhantom = phantomSlot_;
    phantomSlot_ = state.zeroVertexBuffer ? pipe->phantomVertexSlot : -1;
    if (previousPhantom >= 0 && previousPhantom != phantomSlot_)
        ApplyVertexBuffer(static_cast<u32>(previousPhantom));
    if (phantomSlot_ >= 0) {
        auto& have = encoderVBs_[phantomSlot_];
        if (!have.zero) {
            pass_.SetVertexBuffer(static_cast<u32>(phantomSlot_), state.zeroVertexBuffer, 0,
                                  wgpu::kWholeSize);
            have = {BufferHandle{}, 0, true};
        }
    }
    lastBoundPipeline_ = h;
}

void WebGPUCommandList::ApplyVertexBuffer(u32 slot) {
    const VbBinding& want = requestedVBs_[slot];
    VbBinding& have = encoderVBs_[slot];
    if (!have.zero && have.buffer == want.buffer && have.offset == want.offset)
        return;
    auto* buf = device_.State().buffers.Get(static_cast<u64>(want.buffer));
    if (!buf)
        return;
    pass_.SetVertexBuffer(slot, buf->buffer, want.offset, wgpu::kWholeSize);
    have = want;
}

void WebGPUCommandList::BindVertexBuffer(u32 slot, BufferHandle h, u32 /*stride*/, u32 offset) {
    auto& state = device_.State();
    auto* buf = state.buffers.Get(static_cast<u64>(h));
    if (!buf || !pass_ || slot >= requestedVBs_.size())
        return;
    // currentOffset() = baseOffset + slotStride*currentSlot. For ring
    // sub-allocs the active slot rotates on every Map/UpdateBuffer; we
    // must reference the same slot the data was just written into,
    // not the ring base. Dedicated (non-ring) buffers have slotCount=1
    // so currentOffset() == baseOffset == 0.
    requestedVBs_[slot] = {h, buf->currentOffset() + offset, false};
    if (static_cast<i32>(slot) != phantomSlot_)
        ApplyVertexBuffer(slot);
}

void WebGPUCommandList::BindIndexBuffer(BufferHandle h, Format fmt) {
    auto& state = device_.State();
    auto* buf = state.buffers.Get(static_cast<u64>(h));
    if (!buf || !pass_)
        return;
    const u64 off = buf->currentOffset();
    if (lastIndexBuffer_ == h && lastIndexOffset_ == off && lastIndexFormat_ == fmt)
        return;
    lastIndexBuffer_ = h;
    lastIndexOffset_ = off;
    lastIndexFormat_ = fmt;
    const wgpu::IndexFormat indexFmt =
        (fmt == Format::R32_UINT) ? wgpu::IndexFormat::Uint32 : wgpu::IndexFormat::Uint16;
    pass_.SetIndexBuffer(buf->buffer, indexFmt, off, wgpu::kWholeSize);
}

void WebGPUCommandList::BindConstantBuffer(ShaderStage stage, u32 slot, BufferHandle h) {
    auto& state = device_.State();
    auto* buf = state.buffers.Get(static_cast<u64>(h));
    if (!buf)
        return;
    const u32 idx = (stage == ShaderStage::Pixel) ? (slot + kPsCbBindingOffsetWgsl) : slot;
    if (idx >= kMaxBindingIndex)
        return;
    const u64 off = buf->currentOffset();
    const u64 sz  = buf->desc.size;
    // Skip the dirty flip when the binding is identical to the current
    // captured value — saves a rebuild + cache lookup in FlushBindings.
    auto& cur = pendingCBs_[idx];
    if (cur.buffer == h && cur.offset == off && cur.size == sz) return;
    cur = {h, off, sz};
    dirtyKinds_ |= kBindKindConstant;
}

void WebGPUCommandList::BindShaderResource(ShaderStage stage, u32 slot, TextureHandle h) {
    const u32 idx = SlotIndex(stage, slot);
    if (idx >= kMaxBindingIndex)
        return;
    auto& cur = pendingSRVs_[idx];
    if (!cur.isBuffer && cur.texture == h) return;
    cur = {h, BufferHandle::Invalid, 0, false};
    dirtyKinds_ |= kBindKindResource;
}

// Structured buffers land on `var<storage, read>` entries: a PS buffer on the
// binding a texture of its register would use, a VS buffer past both stages'
// texture ranges (VsBufferBindingOffset in cb_structs.slang), since VS t16
// would otherwise share PS t4's binding.
void WebGPUCommandList::BindShaderResource(ShaderStage stage, u32 slot, BufferHandle h) {
    const u32 idx = (stage == ShaderStage::Pixel) ? SlotIndex(stage, slot)
                                                  : 2 * kStageBindingShift + slot;
    if (idx >= kMaxBindingIndex)
        return;
    auto* buf = device_.State().buffers.Get(static_cast<u64>(h));
    const u64 off = buf ? buf->currentOffset() : 0;
    auto& cur = pendingSRVs_[idx];
    if (cur.isBuffer && cur.storage == h && cur.storageOffset == off) return;
    cur = {TextureHandle::Invalid, h, off, true};
    dirtyKinds_ |= kBindKindResource;
}

void WebGPUCommandList::BindUnorderedAccess(u32 slot, BufferHandle h) {
    (void)slot;
    (void)h;
    std::fprintf(stderr, "[wgpu] BindUnorderedAccess not yet implemented (slot %u)\n", slot);
}

void WebGPUCommandList::BindSampler(ShaderStage stage, u32 slot, SamplerHandle h) {
    const u32 idx = SlotIndex(stage, slot);
    if (idx >= kMaxBindingIndex)
        return;
    auto& cur = pendingSamplers_[idx];
    if (cur.sampler == h) return;
    cur = {h};
    dirtyKinds_ |= kBindKindSampler;
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
               f == wgpu::TextureFormat::Depth32FloatStencil8 || f == wgpu::TextureFormat::Stencil8;
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
    if (!pass_ || groupCount_ == 0)
        return;
    auto& state = device_.State();

    // One bind group per layout group. Uniform entries bake the ring-slot
    // offset into BindGroupEntry::offset instead of using dynamic offsets
    // (maxDynamicUniformBuffersPerPipelineLayout is ~8-11), so a group is
    // re-resolved whenever a family it draws from changed.
    for (u32 g = 0; g < groupCount_; ++g) {
        const u32 layoutId = groupLayouts_[g];
        const auto& layout = state.bindLayouts[layoutId];
        if (lastGroupKeySet_[g] && (layout.kindMask & dirtyKinds_) == 0)
            continue;

        u64 key = HashMix(kFnv1aOffsetBasis, layoutId);
        const u32 count = static_cast<u32>(layout.entries.size());
        for (u32 i = 0; i < count; ++i) {
            const wgpu::BindGroupLayoutEntry& le = layout.entries[i];
            wgpu::BindGroupEntry& e = scratchEntries_[i];
            e = {};
            e.binding = le.binding;
            u64 resKey = 0; // 0 == this entry's default

            if (le.buffer.type == wgpu::BufferBindingType::Uniform) {
                const auto& pending = pendingCBs_[le.binding];
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
                    resKey = static_cast<u64>(pending.buffer);
                } else {
                    // Hole — point at the shared ring base, sized for the
                    // largest uniform struct a shader declares.
                    e.buffer = state.sharedCbBuffer;
                    e.offset = 0;
                    e.size = kUniformHoleBytes;
                }
            } else if (le.buffer.type != wgpu::BufferBindingType::BindingNotUsed) {
                const auto& pending = pendingSRVs_[le.binding];
                auto* buf = pending.isBuffer ? state.buffers.Get(static_cast<u64>(pending.storage))
                                             : nullptr;
                if (buf && buf->desc.size >= 4) {
                    e.buffer = buf->buffer;
                    e.offset = pending.storageOffset;
                    e.size = buf->desc.size & ~u64{3}; // storage bindings are 4-byte aligned
                    resKey = static_cast<u64>(pending.storage);
                } else {
                    e.buffer = state.defaultStorageBuffer;
                    e.offset = 0;
                    e.size = kDefaultStorageBufferBytes;
                }
            } else if (le.sampler.type != wgpu::SamplerBindingType::BindingNotUsed) {
                const bool wantCompare = le.sampler.type == wgpu::SamplerBindingType::Comparison;
                e.sampler = wantCompare ? state.defaultComparisonSampler : state.defaultSampler;
                const auto& pending = pendingSamplers_[le.binding];
                if (auto* s = state.samplers.Get(static_cast<u64>(pending.sampler))) {
                    if (s->sampler && s->comparison == wantCompare) {
                        e.sampler = s->sampler;
                        resKey = static_cast<u64>(pending.sampler);
                    } else if (s->sampler) {
                        WarnOnce(le.binding, 1, "sampler comparison mode doesn't match the shader");
                    }
                }
            } else {
                const auto& pending = pendingSRVs_[le.binding];
                wgpu::TextureView view;
                if (!pending.isBuffer) {
                    if (auto* tex = state.textures.Get(static_cast<u64>(pending.texture))) {
                        view = ViewFor(state, *tex, le.texture);
                        if (view)
                            resKey = static_cast<u64>(pending.texture);
                        else
                            WarnOnce(le.binding, 2, "texture format/dimension doesn't match the shader");
                    }
                }
                e.textureView = view ? view : DefaultView(state, le.texture);
            }
            key = HashMix(key, le.binding);
            key = HashMix(key, resKey);
            key = HashMix(key, e.offset);
        }

        if (lastGroupKeySet_[g] && key == lastGroupKey_[g])
            continue;
        auto& cache = state.bgCaches[g];
        wgpu::BindGroup bg = cache.Get(key);
        if (!bg) {
            wgpu::BindGroupDescriptor bgd{};
            bgd.label = "wf.bindGroup";
            bgd.layout = layout.layout;
            bgd.entryCount = count;
            bgd.entries = count ? scratchEntries_.data() : nullptr;
            bg = state.device.CreateBindGroup(&bgd);
            cache.Put(key, bg);
        }
        pass_.SetBindGroup(g, bg, 0, nullptr);
        lastGroupKey_[g] = key;
        lastGroupKeySet_[g] = true;
    }
    dirtyKinds_ = 0;
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
