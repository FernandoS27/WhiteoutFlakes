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
#include <cmath>
#include <cstdio>

namespace whiteout::flakes::renderer::shading {

using whiteout::flakes::renderer::model::GPUGeoset;

namespace {

Vector3f Cross(const Vector3f& a, const Vector3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vector3f Normalized(const Vector3f& v, const Vector3f& fallback) {
    const f32 l2 = v.x * v.x + v.y * v.y + v.z * v.z;
    if (l2 < 1e-12f)
        return fallback;
    const f32 inv = 1.0f / std::sqrt(l2);
    return {v.x * inv, v.y * inv, v.z * inv};
}

// The light constants below are picked by eye, which means they are picked in
// *display* space — "0.25 ambient" is a value someone judged on a monitor. A
// gamma profile writes them straight to an UNORM backbuffer and they land
// exactly as judged. A linear profile writes into an HDR target that a tonemap
// and an sRGB encode still have to run over, so the same numbers get lifted a
// second time: mid-tones crush toward white and the image washes out. Feeding
// that profile the de-gamma'd value is what makes the two frames agree.
f32 SrgbToLinear(f32 c) {
    return (c <= 0.04045f) ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
Vector4f LightColor(f32 r, f32 g, f32 b, f32 w, bool linear) {
    if (!linear)
        return {r, g, b, w};
    return {SrgbToLinear(r), SrgbToLinear(g), SrgbToLinear(b), w};
}

} // namespace

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
    // metallib, everything else DXBC. Five entries now instead of two, so the
    // choice is made once per API rather than once per entry point.
    const gfx::GfxApi api = gfxDev->GetApi();
    auto make = [&](gfx::ShaderStage stage, const u8* dxbc, usize dxbcN, const u8* spv,
                    usize spvN, const u8* wgsl, usize wgslN, const u8* mtl, usize mtlN) {
        switch (api) {
        case gfx::GfxApi::Vulkan:
            return gfxDev->CreateShader(stage, spv, spvN);
        case gfx::GfxApi::WebGPU:
            return gfxDev->CreateShader(stage, wgsl, wgslN);
        case gfx::GfxApi::Metal:
            return gfxDev->CreateShader(stage, mtl, mtlN);
        default:
            return gfxDev->CreateShader(stage, dxbc, dxbcN);
        }
    };
#define WDX_UNLIT_BLOB(name) k##name, sizeof(k##name), k##name##Spv, sizeof(k##name##Spv),        \
                             k##name##Wgsl, sizeof(k##name##Wgsl), k##name##Mtl,                 \
                             sizeof(k##name##Mtl)
    vs_ = make(gfx::ShaderStage::Vertex, WDX_UNLIT_BLOB(UnlitVS));
    ps_ = make(gfx::ShaderStage::Pixel, WDX_UNLIT_BLOB(UnlitPS));
    vsLit_ = make(gfx::ShaderStage::Vertex, WDX_UNLIT_BLOB(UnlitLitVS));
    psLambert_ = make(gfx::ShaderStage::Pixel, WDX_UNLIT_BLOB(UnlitLambertPS));
    psBlinnPhong_ = make(gfx::ShaderStage::Pixel, WDX_UNLIT_BLOB(UnlitBlinnPhongPS));
    vsSkinned_ = make(gfx::ShaderStage::Vertex, WDX_UNLIT_BLOB(UnlitSkinnedVS));
    vsSkinnedLit_ = make(gfx::ShaderStage::Vertex, WDX_UNLIT_BLOB(UnlitSkinnedLitVS));
#undef WDX_UNLIT_BLOB

    // Mapped once per draw, so the Vulkan CB ring needs room for a busy frame
    // — same reasoning as the BLS per-draw constant buffers, which is where
    // this slot count comes from.
    constexpr u32 kCbRingSlots = 4096;
    cb_ = gfxDev->CreateBuffer({
        .size = sizeof(UnlitCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kCbRingSlots,
    });
    // Written once per pass, not per draw — the light is fixed and the eye
    // moves once per frame — so this needs no ring.
    lightCb_ = gfxDev->CreateBuffer({
        .size = sizeof(UnlitLightCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
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
    if (lightCb_ != gfx::BufferHandle::Invalid)
        gfxDev->Destroy(lightCb_);
    cb_ = gfx::BufferHandle::Invalid;
    lightCb_ = gfx::BufferHandle::Invalid;
    vs_ = gfx::ShaderHandle::Invalid;
    ps_ = gfx::ShaderHandle::Invalid;
    vsLit_ = gfx::ShaderHandle::Invalid;
    psLambert_ = gfx::ShaderHandle::Invalid;
    psBlinnPhong_ = gfx::ShaderHandle::Invalid;
    vsSkinned_ = gfx::ShaderHandle::Invalid;
    vsSkinnedLit_ = gfx::ShaderHandle::Invalid;
    warnedNoNormal_.clear();
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

    const bool lit = key.lighting != core::UnlitLightingModel::Flat;

    // What the chosen permutation consumes, in the order its VS input struct
    // declares it. Order is load-bearing: Vulkan, WebGPU and Metal derive a
    // non-ATTR semantic's shader location from its position in this array.
    static constexpr core::VertexSemantic kFlatWants[] = {core::VertexSemantic::Position};
    static constexpr core::VertexSemantic kLitWants[] = {core::VertexSemantic::Position,
                                                        core::VertexSemantic::Normal};
    // The skinned permutations declare NORMAL even when the pixel side is flat,
    // because they share one VS input struct — the flat one simply ignores it.
    // Both skinning attributes come from the same slot-0 buffer as position.
    static constexpr core::VertexSemantic kSkinnedWants[] = {
        core::VertexSemantic::Position, core::VertexSemantic::Normal,
        core::VertexSemantic::BoneWeights, core::VertexSemantic::BoneIndices};

    // POSITION at offset 0 with WC3's full 48-byte stride: the buffer is the
    // interleaved Vertex and the remaining 36 bytes are simply not read. This
    // is how a position-only model draws WC3 geometry without de-interleaving
    // anything — see unlit.slang.
    static const gfx::InputElement kInput[] = {
        {"POSITION", 0, gfx::Format::R32G32B32_FLOAT, 0},
    };

    // Baked geometry describes its own layout, so the elements are the
    // intersection of what the buffer carries and what the permutation reads.
    // Intersecting rather than declaring everything the buffer has sidesteps
    // the question of whether each backend tolerates an unconsumed attribute,
    // and makes "which permutation" and "which elements" one decision.
    std::vector<gfx::InputElement> baked;
    if (key.layoutId != core::VertexLayoutCache::kWc3Interleaved) {
        std::span<const core::VertexSemantic> wants =
            lit ? std::span<const core::VertexSemantic>(kLitWants)
                : std::span<const core::VertexSemantic>(kFlatWants);
        if (key.skinned)
            wants = std::span<const core::VertexSemantic>(kSkinnedWants);
        baked = rs_.Pipeline().VertexLayouts().Subset(key.layoutId, wants);
    }

    gfx::GraphicsPipelineDesc desc{};
    desc.vs = key.skinned ? (lit ? vsSkinnedLit_ : vsSkinned_) : (lit ? vsLit_ : vs_);
    desc.ps = lit ? (key.lighting == core::UnlitLightingModel::BlinnPhong ? psBlinnPhong_
                                                                         : psLambert_)
                  : ps_;
    desc.inputLayout = baked.empty() ? std::span<const gfx::InputElement>(kInput)
                                     : std::span<const gfx::InputElement>(baked);
    // The declared elements stop short of the record's end whenever the
    // permutation reads a subset, so the true stride has to be stated — the
    // backends that bake it into the PSO would otherwise infer it from the
    // last element and walk the buffer wrong. See inputSlotStrides.
    desc.inputSlotStrides[0] = key.stride;
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

bool UnlitShading::ResolveSkinned(const render_detail::RenderableView& view,
                                  const GPUGeoset& geo) const {
    // Needs a palette to pose against.
    if (geo.bonePaletteCb == gfx::BufferHandle::Invalid)
        return false;
    // The interleaved WC3 vertex has no skinning attributes at all — its
    // weights ride a second stream this model does not bind — so layout 0 is
    // never skinned here. Keeping that explicit means the WC3 debug-unlit
    // toggle stays bit-for-bit what it was.
    if (geo.layoutId == core::VertexLayoutCache::kWc3Interleaved)
        return false;
    // Both attributes, in the same buffer as position. A layout carrying only
    // one of them is malformed rather than half-skinned.
    const auto& layouts = rs_.Pipeline().VertexLayouts();
    if (!layouts.Has(geo.layoutId, core::VertexSemantic::BoneWeights) ||
        !layouts.Has(geo.layoutId, core::VertexSemantic::BoneIndices))
        return false;
    // The skinned VS reads a normal from the same struct.
    if (!layouts.Has(geo.layoutId, core::VertexSemantic::Normal))
        return false;
    // An actor-wide palette means the vertex indices are global slots that the
    // loader rewrote — which cannot have happened for a buffer nobody decoded.
    if (view.skinning && view.skinning->UsesPerActorPalette())
        return false;
    return true;
}

core::UnlitLightingModel UnlitShading::ResolveLighting(const render_detail::RenderableView& view,
                                                      const GPUGeoset& geo) {
    if (passLighting_ == core::UnlitLightingModel::Flat)
        return core::UnlitLightingModel::Flat;
    // WC3's interleaved Vertex has a normal at offset 12, but it only ever
    // reaches this model through the debug-unlit toggle, which runs under a
    // WC3 profile and is therefore already Flat above. Treating layout 0 as
    // unlit keeps that path bit-for-bit what it was.
    if (geo.layoutId != core::VertexLayoutCache::kWc3Interleaved &&
        rs_.Pipeline().VertexLayouts().Has(geo.layoutId, core::VertexSemantic::Normal))
        return passLighting_;

    // No normal to light with: draw it flat white rather than shading against
    // undefined bytes. Once per actor — a model with fifty such geosets has
    // one cause, and this runs every frame.
    if (std::find(warnedNoNormal_.begin(), warnedNoNormal_.end(), view.rootActor) ==
        warnedNoNormal_.end()) {
        warnedNoNormal_.push_back(view.rootActor);
        std::fprintf(stderr,
                     "[unlit] actor %u: geometry carries no NORMAL attribute; drawing flat "
                     "white instead of lit\n",
                     view.rootActor);
    }
    return core::UnlitLightingModel::Flat;
}

bool UnlitShading::BeginPass(const core::PassContext& ctx,
                             const render_detail::CollectedDrawLists& lists) {
    (void)lists; // no lights to select from, so no reason to keep the list
    Init();
    if (!IsAvailable())
        return false;
    passView_ = ctx.view;
    passProj_ = ctx.projection;
    passCameraPos_ = ctx.cameraPos;

    // The profile decides how this model lights a surface; a pass built
    // without one, and every WC3 profile, means flat. Degrading to flat when
    // the lit permutations failed to compile keeps a shader-build problem from
    // taking the whole model down — WC3's debug-unlit toggle still draws.
    passLighting_ = ctx.profile ? ctx.profile->UnlitLighting() : core::UnlitLightingModel::Flat;
    if (vsLit_ == gfx::ShaderHandle::Invalid || psLambert_ == gfx::ShaderHandle::Invalid ||
        psBlinnPhong_ == gfx::ShaderHandle::Invalid)
        passLighting_ = core::UnlitLightingModel::Flat;

    if (passLighting_ != core::UnlitLightingModel::Flat) {
        auto* gfxDev = rs_.Pipeline().Gfx();
        if (auto* p = static_cast<UnlitLightCb*>(gfxDev->MapBuffer(lightCb_))) {
            // A three-quarter key light, placed relative to the camera: the
            // model is a thing being *inspected*, so the side facing the viewer
            // is the side that has to be legible, and a fixed world direction
            // leaves it black from half the orbit.
            //
            // Offset up and to the left rather than sitting at the eye. A true
            // headlight puts V == L, which turns the Blinn-Phong half-vector
            // into N·V and collapses the highlight into a Lambert power curve —
            // the two profiles would then be nearly indistinguishable, and
            // telling them apart is what this is for. Still deterministic: the
            // gate poses the camera identically every run.
            // Eye off the pass, target off the camera: PassContext is the
            // authority on where the frame is viewed from, and only the aim
            // point has to be asked for.
            const Vector3f eye = passCameraPos_;
            const Vector3f at = rs_.Pipeline().FrameCamera().GetTarget();
            const Vector3f fwd =
                Normalized({at.x - eye.x, at.y - eye.y, at.z - eye.z}, {0.0f, 1.0f, 0.0f});
            // Z-up, which is what both products author in. Degenerates only
            // when the camera looks straight down the world up axis; the
            // fallback keeps the light in front rather than at zero.
            const Vector3f right = Normalized(Cross(fwd, {0.0f, 0.0f, 1.0f}), {1.0f, 0.0f, 0.0f});
            const Vector3f up = Cross(right, fwd);
            const Vector3f l = Normalized({-fwd.x - 0.45f * right.x + 0.35f * up.x,
                                           -fwd.y - 0.45f * right.y + 0.35f * up.y,
                                           -fwd.z - 0.45f * right.z + 0.35f * up.z},
                                          {0.0f, -1.0f, 0.0f});

            // Authored in display space; de-gamma'd for a profile whose target
            // a tonemap still has to run over. See SrgbToLinear.
            // Budgeted so ambient + diffuse peaks below 1 and only the
            // specular lobe clips. A key light this close to the view axis
            // drives N·L to ~1 across everything facing the camera, so the
            // diffuse term has to be lower than a side light would want —
            // otherwise every front surface saturates to flat white and the
            // shading that is the whole point of this disappears.
            const bool linear = ctx.profile && ctx.profile->LinearShading();
            p->lightDirWS = {l.x, l.y, l.z, 0.0f};
            p->lightColor = LightColor(0.62f, 0.62f, 0.62f, 0.0f, linear);
            p->ambient = LightColor(0.22f, 0.22f, 0.26f, 0.0f, linear);
            p->specular = LightColor(0.28f, 0.28f, 0.28f, 48.0f, linear);
            p->cameraPosWS = {eye.x, eye.y, eye.z, 0.0f};
            gfxDev->UnmapBuffer(lightCb_);
        }
    }
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
    key.layoutId = geo.layoutId;
    key.stride = geo.baseStride;
    key.lighting = ResolveLighting(*item.view, geo);
    key.skinned = ResolveSkinned(*item.view, geo);
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
    cmd->BindVertexBuffer(0, geo.Stream(core::StreamId::Base), geo.baseStride);
    cmd->BindIndexBuffer(geo.ib, gfx::Format::R32_UINT);
    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, cb_);
    if (key.lighting != core::UnlitLightingModel::Flat) {
        // Slot 1 in both stages: the VS never reads it, but Vulkan and WebGPU
        // bind a descriptor set per stage from one layout, and leaving a
        // declared binding unbound is a validation error rather than a
        // harmless omission.
        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 1, lightCb_);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 1, lightCb_);
    }
    if (key.skinned) {
        // Vertex slot 2, matching where the M2 combiners bind theirs. The
        // per-geoset palette is the only one used here: a source whose vertex
        // indices are palette-local is forced onto that path at load time, so
        // an actor-wide palette cannot reach this draw.
        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 2, geo.bonePaletteCb);
    }

    // G1 hook, after the PSO resolved and the CB was written, so the record
    // covers exactly the draws that were submitted.
    if (debug::DrawTraceEnabled()) {
        debug::TraceDraw d;
        d.shadingModel = static_cast<u8>(debug::TraceShadingModel::Unlit);
        d.blendClass = static_cast<u8>(item.key.blend);
        d.streamMask = debug::kStreamBase;
        // The axes a baked buffer adds, folded into the PSO key the trace
        // already carries. All are 0 for WC3 — layout 0, lighting Flat, never
        // skinned through this model — which is what these fields were being
        // hashed as before they were filled in, so no existing baseline moves.
        // The lit permutation is implied by the lighting model rather than
        // given its own term: they cannot disagree.
        //
        // Skinning earns a bit here rather than being left implicit because
        // the fallback is silent: a geoset that loses its palette or its
        // attributes still draws, just in bind pose, and only this makes that
        // visible to the gate.
        d.psoKey = debug::TracePsoKey({
            .psPermute = static_cast<u32>(key.lighting) | (key.skinned ? 0x100u : 0u),
            .vertexLayout = key.layoutId,
            .extraRtvCount = key.extraRtvCount,
        });
        // The palette this draw actually bound, not the one the actor owns.
        // A skinned geoset whose PSO resolved unskinned draws bind pose, and
        // without these two the trace records that as a perfectly healthy
        // frame — which is exactly how a frozen `.m3` got through the gate
        // once. Both stay 0 for WC3, which is never skinned through this
        // model, so no existing baseline moves.
        d.palettePath = key.skinned ? 2 : 0;
        d.paletteSlots = (key.skinned && item.view->skinning)
                             ? item.view->skinning->GeosetPaletteSize(geo.geosetId)
                             : 0;
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
    // Position always; the normal when a profile lights this model. Asked of
    // the model, not of a geoset — this answers "what would this shader read",
    // and whether a given buffer supplies it is ResolveLighting's question.
    return core::VertexNeeds{.position = true,
                             .normal = passLighting_ != core::UnlitLightingModel::Flat};
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
