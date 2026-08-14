#include "shading/unlit_shading.h"

#include "renderer/camera.h"
#include "renderer/core/render_detail.h"
#include "renderer/core/render_profile.h"
#include "renderer/debug/draw_trace_hooks.h"
#include "renderer/model/render_model.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/types.h" // Vertex

#include "compiled_shaders.h"

#include <algorithm>

namespace whiteout::flakes::renderer::shading {

using whiteout::flakes::renderer::model::GPUGeoset;

UnlitShading::~UnlitShading() {
    // Nothing here on purpose. Every handle below belongs to a device that
    // RenderPipeline destroys before this object dies, so releasing at
    // destruction would dispatch through a dead vtable. ReleaseGpu is the
    // ordered teardown.
}

void UnlitShading::Init() {
    if (initTried_)
        return;
    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev)
        return; // called before InitDevice; IsAvailable stays false
    initTried_ = true;

    using namespace whiteout::flakes::Shaders;
    // Same per-backend bytecode selection the line shader uses. Vulkan takes
    // SPIR-V, WebGPU takes WGSL as null-terminated UTF-8 source, Metal takes a
    // metallib, everything else DXBC.
    const gfx::GfxApi api = gfxDev->GetApi();
    const u8* vsBytes = kUnlitVS;
    usize vsSize = sizeof(kUnlitVS);
    const u8* psBytes = kUnlitPS;
    usize psSize = sizeof(kUnlitPS);
    if (api == gfx::GfxApi::Vulkan) {
        vsBytes = kUnlitVSSpv;
        vsSize = sizeof(kUnlitVSSpv);
        psBytes = kUnlitPSSpv;
        psSize = sizeof(kUnlitPSSpv);
    } else if (api == gfx::GfxApi::WebGPU) {
        vsBytes = kUnlitVSWgsl;
        vsSize = sizeof(kUnlitVSWgsl);
        psBytes = kUnlitPSWgsl;
        psSize = sizeof(kUnlitPSWgsl);
    } else if (api == gfx::GfxApi::Metal) {
        vsBytes = kUnlitVSMtl;
        vsSize = sizeof(kUnlitVSMtl);
        psBytes = kUnlitPSMtl;
        psSize = sizeof(kUnlitPSMtl);
    }

    vs_ = gfxDev->CreateShader(gfx::ShaderStage::Vertex, vsBytes, vsSize);
    ps_ = gfxDev->CreateShader(gfx::ShaderStage::Pixel, psBytes, psSize);

    // Mapped once per draw, so the Vulkan CB ring needs room for a busy frame
    // — same reasoning as the BLS per-draw constant buffers, which is where
    // this slot count comes from.
    constexpr u32 kCbRingSlots = 4096;
    cb_ = gfxDev->CreateBuffer({
        .size = sizeof(UnlitCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kCbRingSlots,
    });
}

void UnlitShading::ReleaseGpu() {
    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev)
        return;
    for (auto& e : psos_) {
        if (e.pso != gfx::PipelineHandle::Invalid)
            gfxDev->Destroy(e.pso);
    }
    psos_.clear();
    if (cb_ != gfx::BufferHandle::Invalid)
        gfxDev->Destroy(cb_);
    cb_ = gfx::BufferHandle::Invalid;
    vs_ = gfx::ShaderHandle::Invalid;
    ps_ = gfx::ShaderHandle::Invalid;
    initTried_ = false;
}

bool UnlitShading::IsAvailable() const {
    return vs_ != gfx::ShaderHandle::Invalid && ps_ != gfx::ShaderHandle::Invalid &&
           cb_ != gfx::BufferHandle::Invalid;
}

gfx::PipelineHandle UnlitShading::GetOrBuildPso(const PsoKey& key) {
    auto it = std::find_if(psos_.begin(), psos_.end(),
                           [&](const PsoEntry& e) { return e.key == key; });
    if (it != psos_.end())
        return it->pso;

    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev)
        return gfx::PipelineHandle::Invalid;

    // POSITION at offset 0 with WC3's full 48-byte stride: the buffer is the
    // interleaved Vertex and the remaining 36 bytes are simply not read. This
    // is how a position-only model draws WC3 geometry without de-interleaving
    // anything — see unlit.slang.
    static const gfx::InputElement kInput[] = {
        {"POSITION", 0, gfx::Format::R32G32B32_FLOAT, 0},
    };

    gfx::GraphicsPipelineDesc desc{};
    desc.vs = vs_;
    desc.ps = ps_;
    desc.inputLayout = kInput;
    desc.topology = gfx::PrimitiveTopology::TriangleList;
    desc.blend.enable = false;
    desc.rasterizer.cull = gfx::CullMode::None;
    desc.rasterizer.frontCCW = true;
    desc.rtvFormat = key.rtv;
    desc.dsvFormat = key.dsv;
    desc.extraRtvFormats[0] = key.extra0;
    desc.extraRtvFormats[1] = key.extra1;
    desc.extraRtvCount = key.extraRtvCount;

    PsoEntry e;
    e.key = key;
    e.pso = gfxDev->CreateGraphicsPipeline(desc);
    psos_.push_back(e);
    return e.pso;
}

bool UnlitShading::BeginPass(const core::PassContext& ctx,
                             const render_detail::CollectedDrawLists& lists) {
    (void)lists; // no lights to select from, so no reason to keep the list
    Init();
    if (!IsAvailable())
        return false;
    passView_ = ctx.view;
    passProj_ = ctx.projection;
    return true;
}

void UnlitShading::Draw(const render_detail::DrawItem& item, const core::PassContext& ctx) {
    if (!item.view || item.geoIdx < 0 || !item.view->geosets)
        return;
    const auto& geosets = *item.view->geosets;
    if (static_cast<usize>(item.geoIdx) >= geosets.size())
        return;
    const GPUGeoset& geo = geosets[static_cast<usize>(item.geoIdx)];

    auto* gfxDev = rs_.Pipeline().Gfx();
    auto* cmd = gfxDev ? gfxDev->GetImmediateContext() : nullptr;
    if (!cmd)
        return;

    PsoKey key;
    key.rtv = rs_.Pipeline().SceneTargetFormat();
    key.dsv = rs_.Pipeline().DepthStencilFormat();
    // An MRT scene pass binds a three-attachment G-buffer, and the PSO has to
    // declare the same attachment count or Vulkan and WebGPU reject the bind —
    // even though this shader writes SV_Target0 alone and the backend masks
    // the other two. That is a pipeline-format concern and not an output
    // signature: `Emits` below still answers Color|Depth, honestly.
    gfx::Format extra[2] = {gfx::Format::Unknown, gfx::Format::Unknown};
    key.extraRtvCount = rs_.Pipeline().SceneExtraRtvFormats(extra);
    key.extra0 = extra[0];
    key.extra1 = extra[1];
    const gfx::PipelineHandle pso = GetOrBuildPso(key);
    if (pso == gfx::PipelineHandle::Invalid)
        return;

    if (auto* p = static_cast<UnlitCb*>(gfxDev->MapBuffer(cb_))) {
        p->world = item.view->worldTransform.transpose();
        p->view = passView_.transpose();
        p->projection = passProj_.transpose();
        gfxDev->UnmapBuffer(cb_);
    }

    cmd->BindPipeline(pso);
    cmd->BindVertexBuffer(0, geo.Stream(core::StreamId::Base), sizeof(Vertex));
    cmd->BindIndexBuffer(geo.ib, gfx::Format::R32_UINT);
    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, cb_);

    // G1 hook, after the PSO resolved and the CB was written, so the record
    // covers exactly the draws that were submitted.
    if (debug::DrawTraceEnabled()) {
        debug::TraceDraw d;
        d.shadingModel = static_cast<u8>(debug::TraceShadingModel::Unlit);
        d.blendClass = static_cast<u8>(item.key.blend);
        d.streamMask = debug::kStreamBase;
        d.psoKey = debug::TracePsoKey({
            .extraRtvCount = key.extraRtvCount,
        });
        debug::RecordUnlitDraw(d, *item.view, geo, ctx);
    }

    cmd->DrawIndexed(geo.indexCount);
}

core::SurfaceClass UnlitShading::Classify(const render_detail::RenderableView& view,
                                          const GPUGeoset& geo) const {
    (void)view;
    (void)geo;
    // No animated visibility and no blend: an unlit surface is always drawn,
    // always opaque. The WC3 rule is animated because WC3 materials are; this
    // model has no materials at all, so there is nothing to animate.
    return {.visible = true, .blend = core::BlendClass::Opaque, .needsDepthFill = false};
}

core::VertexNeeds UnlitShading::Needs(u32 surface) const {
    (void)surface;
    // Position and nothing else — the literal statement of what this model is.
    return core::VertexNeeds{.position = true};
}

i32 UnlitShading::SelectLights(bls::FrameInputs& frame, const bls::LightingContext& lighting,
                               const Matrix44f& viewMat, const Vector3f& surfaceWS) const {
    (void)frame;
    (void)lighting;
    (void)viewMat;
    (void)surfaceWS;
    return 0; // unlit: the palette is never read, so never fill it
}

core::EmitMask UnlitShading::Emits(u32 surface, core::PassSlot pass) const {
    (void)surface;
    // Colour and depth in every pass, including GBuffer — this shader has no
    // normal to write and no linear depth to compute. That is the point of
    // Emits being per-model rather than per-pass: the HD profile declares the
    // G-buffer attachments because *some* model fills them, and this one
    // saying otherwise is information, not a contradiction.
    if (pass == core::PassSlot::ShadowMap || pass == core::PassSlot::DepthPrepass)
        return core::EmitMask::Depth;
    return core::EmitMask::DefaultColor;
}

} // namespace whiteout::flakes::renderer::shading
