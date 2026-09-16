#include "m2_shading.h"

#include "renderer/assets/sampler_asset_manager.h"
#include "renderer/assets/texture_asset_manager.h"
#include "renderer/core/render_detail.h"
#include "renderer/core/render_profile.h"
#include "renderer/debug/draw_trace_hooks.h"
#include "renderer/model/render_model.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/types.h"

#include "compiled_shaders.h"

#include <cstddef>

namespace whiteout::flakes::renderer::profiles::wow {

using whiteout::flakes::renderer::model::GPUGeoset;

namespace {

// The texture-transform matrix for one unit, from the actor's per-frame palette.
//
// The palette entry is the 2x4 affine `M2ModelAdapter::EvaluateTextureTransforms`
// wrote (`u' = row0.x*u + row0.y*v + row0.w`, likewise row1 for v). Widening it
// to a 4x4 loses nothing: the shader feeds (u, v, 0, 1) and reads .xy back, so
// the row and column this fills with identity can never reach the output.
Matrix44f TexMatrixFor(const render_detail::RenderableView& view, i32 transformId) {
    Matrix44f m = Matrix44f::identity();
    if (transformId < 0 || !view.texAnimPalette ||
        static_cast<usize>(transformId) >= view.texAnimPalette->size())
        return m;
    const auto& e = (*view.texAnimPalette)[static_cast<usize>(transformId)];
    m.data[0][0] = e.row0[0];
    m.data[0][1] = e.row1[0];
    m.data[1][0] = e.row0[1];
    m.data[1][1] = e.row1[1];
    m.data[3][0] = e.row0[3];
    m.data[3][1] = e.row1[3];
    return m;
}

// The animated half of a surface, or null when the actor's source animates
// nothing (or fewer surfaces than the table holds) — in which case the caller
// keeps the table's bind-pose constant.
const model::RenderModel::SurfaceAnim* SurfaceAnimFor(const render_detail::RenderableView& view,
                                                      u32 surface) {
    if (!view.surfaceAnim || surface >= view.surfaceAnim->size())
        return nullptr;
    return &(*view.surfaceAnim)[surface];
}

// The model's own alpha — what BeginDraw gates the pass and the cull on. Kept
// apart from the batch's element alpha below, as the client keeps them.
f32 ModelAlpha(const render_detail::RenderableView& view, const GPUGeoset& geo) {
    return view.parentVisibility * geo.geosetAlpha;
}

// `element.alpha = batch.color.alpha * batch.textureWeight * model.alpha`. The
// first two are the animated surface state (the table's constants until one
// lands); the model's is the actor's visibility times the geoset's alpha.
f32 ElementAlpha(const M2Surface& surf, const model::RenderModel::SurfaceAnim* anim,
                 const render_detail::RenderableView& view, const GPUGeoset& geo) {
    const f32 base = anim ? anim->alpha : surf.elementAlpha;
    return base * ModelAlpha(view, geo);
}

} // namespace

M2CombinerShading::~M2CombinerShading() {
    // Nothing here: every handle belongs to a device RenderPipeline destroys
    // first. ReleaseGpu is the ordered teardown.
}

void M2CombinerShading::Init() {
    if (initTried_)
        return;
    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev)
        return; // before InitDevice; IsAvailable stays false
    initTried_ = true;

    using namespace whiteout::flakes::Shaders;
    const gfx::GfxApi api = gfxDev->GetApi();
    auto mk = [&](gfx::ShaderStage stage, const u8* dxbc, usize dxbcN, const u8* spv, usize spvN,
                  const u8* wgsl, usize wgslN, const u8* mtl, usize mtlN) {
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
#define WDX_M2_BLOB(name)                                                                          \
    k##name, sizeof(k##name), k##name##Spv, sizeof(k##name##Spv), k##name##Wgsl,                   \
        sizeof(k##name##Wgsl), k##name##Mtl, sizeof(k##name##Mtl)

    usize k = 0;
    usize p = 0;

    // Vertex, in M2VertexShader order.
    vs_[k++] = mk(gfx::ShaderStage::Vertex, WDX_M2_BLOB(M2Vs_Diffuse_T1));
    vs_[k++] = mk(gfx::ShaderStage::Vertex, WDX_M2_BLOB(M2Vs_Diffuse_Env));
    vs_[k++] = mk(gfx::ShaderStage::Vertex, WDX_M2_BLOB(M2Vs_Diffuse_T1_T2));
    vs_[k++] = mk(gfx::ShaderStage::Vertex, WDX_M2_BLOB(M2Vs_Diffuse_T1_Env));
    vs_[k++] = mk(gfx::ShaderStage::Vertex, WDX_M2_BLOB(M2Vs_Diffuse_Env_T1));
    vs_[k++] = mk(gfx::ShaderStage::Vertex, WDX_M2_BLOB(M2Vs_Diffuse_Env_Env));
    vs_[k++] = mk(gfx::ShaderStage::Vertex, WDX_M2_BLOB(M2Vs_Diffuse_T1_Env_T1));
    vs_[k++] = mk(gfx::ShaderStage::Vertex, WDX_M2_BLOB(M2Vs_Diffuse_T1_T1));
    vs_[k++] = mk(gfx::ShaderStage::Vertex, WDX_M2_BLOB(M2Vs_Diffuse_T1_T1_T1));
    vs_[k++] = mk(gfx::ShaderStage::Vertex, WDX_M2_BLOB(M2Vs_Diffuse_EdgeFade_T1));
    vs_[k++] = mk(gfx::ShaderStage::Vertex, WDX_M2_BLOB(M2Vs_Diffuse_T2));
    vs_[k++] = mk(gfx::ShaderStage::Vertex, WDX_M2_BLOB(M2Vs_Diffuse_T1_Env_T2));
    vs_[k++] = mk(gfx::ShaderStage::Vertex, WDX_M2_BLOB(M2Vs_Diffuse_EdgeFade_T1_T2));
    vs_[k++] = mk(gfx::ShaderStage::Vertex, WDX_M2_BLOB(M2Vs_Diffuse_T1_T1_T1_T2));
    vs_[k++] = mk(gfx::ShaderStage::Vertex, WDX_M2_BLOB(M2Vs_Diffuse_EdgeFade_Env));
    vs_[k++] = mk(gfx::ShaderStage::Vertex, WDX_M2_BLOB(M2Vs_Diffuse_T1_T2_T1));
    // 12.1's vertex shaders 14 and 15. Their sources are not in the client
    // binary — only that both feed three samplers and that 14 sits beside
    // Diffuse_T1_T2_T1 in the client's shader ordering — so both borrow it
    // rather than inventing a combiner. Rows 30 and 31 are the only callers and
    // nothing in the corpus selects them; if a model ever does, this is the
    // first thing to look at.
    vs_[k++] = mk(gfx::ShaderStage::Vertex, WDX_M2_BLOB(M2Vs_Diffuse_T1_T2_T1));
    vs_[k++] = mk(gfx::ShaderStage::Vertex, WDX_M2_BLOB(M2Vs_Diffuse_T1_T2_T1));

    // Pixel, in M2PixelShader order.
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Opaque));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Mod));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Opaque_Mod));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Opaque_Mod2x));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Opaque_Mod2xNA));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Opaque_Opaque));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Mod_Mod));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Mod_Mod2x));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Mod_Add));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Mod_Mod2xNA));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Mod_AddNA));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Mod_Opaque));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Opaque_Mod2xNA_Alpha));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Opaque_AddAlpha));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Opaque_AddAlpha_Alpha));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Opaque_Mod2xNA_Alpha_Add));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Mod_AddAlpha));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Mod_AddAlpha_Alpha));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Opaque_Alpha_Alpha));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Opaque_Mod2xNA_Alpha_3s));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Opaque_AddAlpha_Wgt));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Mod_Add_Alpha));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Opaque_ModNA_Alpha));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Mod_AddAlpha_Wgt));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Opaque_Mod_Add_Wgt));
    ps_[p++] =
        mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Opaque_Mod2xNA_Alpha_UnshAlpha));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Mod_Dual_Crossfade));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Opaque_Mod2xNA_Alpha_Alpha));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Mod_Masked_Dual_Crossfade));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Opaque_Alpha));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Guild));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Guild_NoBorder));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Guild_Opaque));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Mod_Depth));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Illum));
    // Pixel shader 35 borrows the family it shares an uber-shader with, on the
    // same terms as the two vertex shaders above.
    ps_[p++] =
        mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Opaque_Mod2xNA_Alpha_Alpha));
    ps_[p++] = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Combiners_Mod_Mod_Depth));
    psDebug_ = mk(gfx::ShaderStage::Pixel, WDX_M2_BLOB(M2Ps_Debug));
#undef WDX_M2_BLOB

    // One map per draw, so the Vulkan CB ring needs room for a busy frame —
    // the same slot count the other per-draw constant buffers use.
    constexpr u32 kCbRingSlots = 4096;
    drawCb_ = gfxDev->CreateBuffer({
        .size = sizeof(M2DrawCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kCbRingSlots,
    });
    debugCb_ = gfxDev->CreateBuffer({
        .size = sizeof(core::DebugViewCbData),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kCbRingSlots,
    });
    // Written once per pass — the light is fixed and the eye moves once per
    // frame — so this needs no ring.
    passCb_ = gfxDev->CreateBuffer({
        .size = sizeof(M2PassCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });
}

void M2CombinerShading::ReleaseGpu() {
    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev)
        return;
    for (auto& [key, pso] : psos_) {
        if (pso != gfx::PipelineHandle::Invalid)
            gfxDev->Destroy(pso);
    }
    psos_.clear();
    if (passCb_ != gfx::BufferHandle::Invalid)
        gfxDev->Destroy(passCb_);
    if (drawCb_ != gfx::BufferHandle::Invalid)
        gfxDev->Destroy(drawCb_);
    if (debugCb_ != gfx::BufferHandle::Invalid)
        gfxDev->Destroy(debugCb_);
    debugCb_ = gfx::BufferHandle::Invalid;
    psDebug_ = gfx::ShaderHandle::Invalid;
    passCb_ = gfx::BufferHandle::Invalid;
    drawCb_ = gfx::BufferHandle::Invalid;
    vs_.fill(gfx::ShaderHandle::Invalid);
    ps_.fill(gfx::ShaderHandle::Invalid);
    initTried_ = false;
}

bool M2CombinerShading::IsAvailable() const {
    // The first of each is enough: all 51 are created in one pass, so a partial
    // set means the shader build itself failed.
    return vs_[0] != gfx::ShaderHandle::Invalid && ps_[0] != gfx::ShaderHandle::Invalid &&
           passCb_ != gfx::BufferHandle::Invalid && drawCb_ != gfx::BufferHandle::Invalid;
}

const M2SurfaceTable* M2CombinerShading::TableOf(const render_detail::RenderableView& view) {
    if (!view.surfaceTable || view.surfaceTable->Product() != core::ProductId::Wow)
        return nullptr;
    return static_cast<const M2SurfaceTable*>(view.surfaceTable);
}

gfx::PipelineHandle M2CombinerShading::GetOrBuildPso(const PsoKey& key) {
    if (auto it = psos_.find(key); it != psos_.end())
        return it->second;

    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev)
        return gfx::PipelineHandle::Invalid;

    // Built from the buffer's own description rather than hardcoded, but not
    // through Subset: the two UV sets bind as ONE float4 element, which no
    // per-semantic subset can express. They are 16 contiguous bytes in the
    // record, and merging them is what keeps every declared semantic index at
    // zero — see m2_combiners.slang's VSInput for why a non-zero one cannot
    // survive this toolchain.
    //
    // Order is load-bearing beyond D3D's name matching: Vulkan, WebGPU and
    // Metal derive a non-ATTR semantic's shader location from its position in
    // this array, so it has to match VSInput's field order.
    const auto& layouts = rs_.Pipeline().VertexLayouts();
    const auto attrs = layouts.Attributes(key.layoutId);
    const model::VertexAttribute* pos = nullptr;
    const model::VertexAttribute* nrm = nullptr;
    const model::VertexAttribute* uv0 = nullptr;
    const model::VertexAttribute* uv1 = nullptr;
    for (const auto& a : attrs) {
        if (a.semantic == core::VertexSemantic::Position && a.semanticIndex == 0)
            pos = &a;
        else if (a.semantic == core::VertexSemantic::Normal && a.semanticIndex == 0)
            nrm = &a;
        else if (a.semantic == core::VertexSemantic::TexCoord && a.semanticIndex == 0)
            uv0 = &a;
        else if (a.semantic == core::VertexSemantic::TexCoord && a.semanticIndex == 1)
            uv1 = &a;
    }
    // A buffer missing any of these, or with its UV sets apart, cannot feed
    // these shaders. Better no draw than one sampling undefined attributes.
    if (!pos || !nrm || !uv0 || !uv1)
        return gfx::PipelineHandle::Invalid;
    if (uv0->format != gfx::Format::R32G32_FLOAT || uv1->format != gfx::Format::R32G32_FLOAT ||
        uv1->offset != uv0->offset + 8)
        return gfx::PipelineHandle::Invalid;

    // Slot 1 is the bone stream — a `BoneVertex`, the same 8-byte record every
    // skinned WC3 geoset uploads, so the two share PackBoneVertex and the
    // palette CB layout. Always present: M2ModelAdapter emits weights for every
    // submesh (synthesising a single identity bone if a model somehow has
    // none), which is what lets the sixteen vertex shaders skin
    // unconditionally instead of doubling into skinned/unskinned permutations.
    const gfx::InputElement elements[] = {
        {"POSITION", 0, pos->format, pos->offset, 0},
        {"NORMAL", 0, nrm->format, nrm->offset, 0},
        {"TEXCOORD", 0, gfx::Format::R32G32B32A32_FLOAT, uv0->offset, 0},
        {"BLENDWEIGHT", 0, gfx::Format::R8G8B8A8_UNORM, offsetof(BoneVertex, weights), 1},
        {"BLENDINDICES", 0, gfx::Format::R8G8B8A8_UINT, offsetof(BoneVertex, indices), 1},
    };

    // Element alpha does not reach the pipeline — it only moves alphaRef, which
    // is a uniform — so 1.0 here produces the same state for every draw sharing
    // this key.
    const M2DrawState state =
        M2StateFor(static_cast<M2Blend>(key.blend), key.materialFlags, 1.0f, key.mirrored);

    gfx::GraphicsPipelineDesc desc{};
    desc.vs = vs_[key.vsIndex];
    desc.ps = key.debug ? psDebug_ : ps_[key.psIndex];
    desc.inputLayout = std::span<const gfx::InputElement>(elements);
    // The declared elements stop short of the record's end, so the true stride
    // has to be stated or the backends that bake it into the PSO walk the
    // buffer wrong.
    desc.inputSlotStrides[0] = key.stride;
    desc.inputSlotStrides[1] = sizeof(BoneVertex);
    desc.topology = gfx::PrimitiveTopology::TriangleList;
    desc.blend = state.blend;
    desc.depthStencil = state.depth;
    desc.rasterizer = state.raster;
    desc.rtvFormat = key.rtv;
    desc.dsvFormat = key.dsv;
    desc.extraRtvFormats[0] = key.extra0;
    desc.extraRtvFormats[1] = key.extra1;
    desc.extraRtvFormats[2] = key.extra2;
    desc.extraRtvCount = key.extraRtvCount;

    const auto pso = gfxDev->CreateGraphicsPipeline(desc);
    psos_.emplace(key, pso);
    return pso;
}

bool M2CombinerShading::BeginPass(const core::PassContext& ctx,
                                  const render_detail::CollectedDrawLists& lists) {
    Init();
    if (!IsAvailable())
        return false;

    passView_ = ctx.view;
    passProj_ = ctx.projection;
    passCameraPos_ = ctx.cameraPos;
    passDebug_ = ctx.debug;
    passDebugTarget_ = ctx.debugTarget;
    passLights_ = &lists.sceneLights;
    // A new pass may hand a different light pool, and the views themselves are
    // rebuilt each frame; anything cached against the old one is stale.
    lightingView_ = nullptr;

    auto* gfxDev = rs_.Pipeline().Gfx();
    if (auto* c = static_cast<M2PassCb*>(gfxDev->MapBuffer(passCb_))) {
        c->view = passView_.transpose();
        c->projection = passProj_.transpose();
        c->cameraPosWS = {passCameraPos_.x, passCameraPos_.y, passCameraPos_.z, 0.0f};
        // WowProfile carries no fog parameters yet, so density 0 makes every
        // fog mode a no-op. The modes still route correctly (m2_material.h);
        // only the term they blend toward is missing.
        c->fogParams = {0.0f, 1.0f, 0.0f, 0.0f};
        c->fogColor = {0.5f, 0.55f, 0.6f, 0.0f};
        gfxDev->UnmapBuffer(passCb_);
    }
    return true;
}

const M2LightingResult&
M2CombinerShading::LightingFor(const render_detail::RenderableView& view) {
    if (lightingView_ == &view)
        return lighting_;

    // The point the client ranks point lights against is the model's bounding
    // sphere centre; the actor's origin is what we have per view and is the
    // same point for anything built around its pivot.
    M2Lighting acc(whiteout::transform_point(Vector3f{0.0f, 0.0f, 0.0f}, view.worldTransform));

    // Scene lights first, environment second — CM2Model::SetupLighting calls
    // CM2Scene::SelectLights and only then the model's lighting callback, and
    // AddDiffuse assigns rather than accumulates. A model carrying its own
    // directional light is therefore overridden by the environment's, which is
    // what the client does.
    if (passLights_ && rs_.Settings().M2ModelLights()) {
        for (const auto& L : *passLights_) {
            if (!L.enabled)
                continue;
            M2LightInput in;
            in.positional = L.kind == model::FrameState::LightKind::Omni;
            in.positionWS = L.worldPos;
            in.directionWS = L.worldDir;
            in.ambient = {L.ambientColor.x * L.ambIntensity, L.ambientColor.y * L.ambIntensity,
                          L.ambientColor.z * L.ambIntensity};
            in.diffuse = L.diffuse;
            acc.AddLight(in);
        }
    }

    // PortraitLightingCallback, verbatim. The client's own answer to lighting
    // one model with no world around it — which is what a viewer always is.
    acc.AddAmbient(kM2PortraitAmbient);
    acc.AddDiffuse(kM2PortraitDiffuse, kM2PortraitDirection);

    lighting_ = acc.Resolve();
    lightingView_ = &view;
    return lighting_;
}

void M2CombinerShading::Draw(const render_detail::DrawItem& item, const core::PassContext& ctx) {
    if (!item.view || item.geoIdx < 0 || !item.view->geosets)
        return;
    const auto& geosets = *item.view->geosets;
    if (static_cast<usize>(item.geoIdx) >= geosets.size())
        return;
    const GPUGeoset& geo = geosets[static_cast<usize>(item.geoIdx)];

    const M2SurfaceTable* table = TableOf(*item.view);
    const M2Surface* surf = table ? table->Surface(item.key.surface) : nullptr;
    if (!surf)
        return;

    auto* gfxDev = rs_.Pipeline().Gfx();
    auto* cmd = gfxDev ? gfxDev->GetImmediateContext() : nullptr;
    if (!cmd)
        return;

    PsoKey key;
    key.rtv = rs_.Pipeline().SceneTargetFormat();
    key.dsv = rs_.Pipeline().DepthStencilFormat();
    // An MRT scene pass binds a three-attachment G-buffer and the PSO must
    // declare the same count or Vulkan and WebGPU reject the bind, even though
    // these shaders write SV_Target0 alone.
    gfx::Format extra[3] = {gfx::Format::Unknown, gfx::Format::Unknown, gfx::Format::Unknown};
    key.extraRtvCount = rs_.Pipeline().SceneExtraRtvFormats(extra);
    key.extra0 = extra[0];
    key.extra1 = extra[1];
    key.extra2 = extra[2];
    key.layoutId = geo.layoutId;
    key.stride = geo.baseStride;
    key.vsIndex = static_cast<u8>(surf->vertexShader);
    key.psIndex = static_cast<u8>(surf->pixelShader);
    // The depth half of a twin pair forces the blend opaque and nothing else —
    // SetupMaterial swaps only the blend preset and still hands the shader the
    // alpha reference the real material asked for, so an alpha-keyed surface
    // keeps punching its holes while it lays depth down. `state` below is
    // therefore built from the material's own blend, not this one.
    const bool depthTwin = item.depthFill == bls::DepthFill::Depth;
    key.blend = static_cast<u8>(depthTwin ? M2Blend::Opaque : surf->blend);
    key.materialFlags = surf->materialFlags;
    key.mirrored = item.view->mirrored;
    // A debug view swaps the combiner for the entry that runs it at fixed
    // light; the depth half of a twin pair writes no colour and keeps its own.
    key.debug = passDebug_.debugSurfaces && !depthTwin && psDebug_ != gfx::ShaderHandle::Invalid;

    const gfx::PipelineHandle pso = GetOrBuildPso(key);
    if (pso == gfx::PipelineHandle::Invalid)
        return;

    // Path A hands every geoset the actor's one palette; Path B gives each its
    // own. Which one this actor is on was decided at load, and the vertex
    // buffer's bone indices were rewritten to match — see
    // DecidePaletteLayoutAndRewrite. Checked before anything is written,
    // because the sixteen vertex shaders skin unconditionally: without both the
    // stream and the palette there is no unskinned draw to fall back to.
    gfx::BufferHandle paletteCb = geo.bonePaletteCb;
    if (item.view->skinning && item.view->skinning->UsesPerActorPalette())
        paletteCb = item.view->skinning->ActorPaletteCb();
    if (geo.boneVb == gfx::BufferHandle::Invalid || paletteCb == gfx::BufferHandle::Invalid)
        return;

    const auto* anim = SurfaceAnimFor(*item.view, item.key.surface);
    const f32 elementAlpha = ElementAlpha(*surf, anim, *item.view, geo);
    const M2DrawState state = M2StateFor(surf->blend, surf->materialFlags, elementAlpha);

    const Vector3f color = anim ? anim->color : surf->elementColor;
    const f32* weights = anim ? anim->unitWeights : surf->unitWeights;

    const M2LightingResult& lit = LightingFor(*item.view);

    if (auto* c = static_cast<M2DrawCb*>(gfxDev->MapBuffer(drawCb_))) {
        c->world = item.view->worldTransform.transpose();
        c->texMtx0 = TexMatrixFor(*item.view, surf->transformId[0]).transpose();
        c->texMtx1 = TexMatrixFor(*item.view, surf->transformId[1]).transpose();
        const Vector3f in = M2CombinerInput(surf->blend, color, geo.geosetColor);
        c->elementColor = {in.x, in.y, in.z, elementAlpha};
        c->unitWeights = {weights[0], weights[1], weights[2], weights[3]};
        // .w premultiplies, for the one blend mode whose factors expect it —
        // see M2Finish. Keyed on the blend actually bound, so the depth half of
        // a twin pair, which forces the preset opaque, writes plain colour.
        const f32 premul =
            (!depthTwin && surf->blend == M2Blend::BlendAdd) ? 1.0f : 0.0f;
        c->params = {state.alphaRef, static_cast<f32>(state.fog), state.lit ? 1.0f : 0.0f, premul};
        c->lightAmbient = {lit.ambient.x, lit.ambient.y, lit.ambient.z, 0.0f};
        c->lightDiffuse = {lit.diffuse.x, lit.diffuse.y, lit.diffuse.z, 0.0f};
        c->lightDir = {lit.directionWS.x, lit.directionWS.y, lit.directionWS.z, 0.0f};
        // The attenuation constants are authored against `.m2` yards, and both
        // the light positions and the shader's world position are in renderer
        // units — a hundred of them per yard. Converting the coefficients
        // rather than the distance keeps the shader's inner loop three madds:
        // 1/(k0 + k1·d + k2·d²) with d = s·dYards is 1/(k0 + (k1/s)·d + (k2/s²)·d²).
        // Selection is unaffected — ranking by distance is scale-invariant.
        const f32 s = (item.view->worldScale > 0.0f) ? item.view->worldScale : 1.0f;
        for (u32 L = 0; L < kM2MaxPointLights; ++L) {
            if (L < lit.pointCount) {
                const M2PointLight& p = lit.points[L];
                c->pointColor[L] = {p.diffuse.x, p.diffuse.y, p.diffuse.z, 0.0f};
                c->pointPos[L] = {p.positionWS.x, p.positionWS.y, p.positionWS.z, 1.0f};
                c->pointAtten[L] = {p.attenuation.x, p.attenuation.y / s,
                                    p.attenuation.z / (s * s), 0.0f};
            } else {
                // ComputeLocalLights' own blanking: colour zero, and a constant
                // attenuation of 1 so the reciprocal the shader takes is
                // defined without the shader having to test a count.
                c->pointColor[L] = {0.0f, 0.0f, 0.0f, 0.0f};
                c->pointPos[L] = {0.0f, 0.0f, 0.0f, 1.0f};
                c->pointAtten[L] = {1.0f, 0.0f, 0.0f, 0.0f};
            }
        }
        gfxDev->UnmapBuffer(drawCb_);
    }

    cmd->BindPipeline(pso);
    cmd->BindVertexBuffer(0, geo.Stream(core::StreamId::Base), geo.baseStride);
    cmd->BindVertexBuffer(1, geo.boneVb, sizeof(BoneVertex));
    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 2, paletteCb);
    cmd->BindIndexBuffer(geo.ib, gfx::Format::R32_UINT);
    // Both buffers in both stages: Vulkan and WebGPU build one descriptor set
    // per stage from the shared layout, so a declared binding left unbound in
    // either is a validation error rather than a harmless omission.
    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, passCb_);
    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, passCb_);
    if (key.debug) {
        if (auto* c = static_cast<core::DebugViewCbData*>(gfxDev->MapBuffer(debugCb_))) {
            *c = core::MakeDebugViewCb(passDebug_, passDebugTarget_, 0, key.psIndex, 4.0f);
            gfxDev->UnmapBuffer(debugCb_);
        }
        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 3, debugCb_);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 3, debugCb_);
    }
    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 1, drawCb_);
    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 1, drawCb_);

    // All four units, every draw — not just the ones this combiner samples.
    // Which of tex0..tex3 survive into a given pixel shader is Slang's dead-code
    // decision, and binding fewer than it kept is a descriptor the runtime never
    // writes: D3D12 draws nothing and Vulkan loses the device. Three redundant
    // binds are cheaper than depending on that being right.
    const auto& defaults = rs_.Textures().GetDefaults();
    for (u32 u = 0; u < kM2MaxTextureUnits; ++u) {
        gfx::TextureHandle tex = gfx::TextureHandle::Invalid;
        u32 wrap = assets::kSamplerWrapBitsMask;
        if (surf->textureId[u] >= 0 && item.view->textures) {
            tex = item.view->textures->Get(surf->textureId[u]);
            if (tex != gfx::TextureHandle::Invalid)
                wrap = item.view->textures->WrapFlags(surf->textureId[u]) &
                       assets::kSamplerWrapBitsMask;
        }
        if (tex == gfx::TextureHandle::Invalid)
            tex = defaults.White;
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, u, tex);
        cmd->BindSampler(gfx::ShaderStage::Pixel, u, rs_.Samplers().WrapVariant(wrap));
    }

    // G1 hook, after the PSO resolved and the CB was written, so the record
    // covers exactly the draws that were submitted.
    if (debug::DrawTraceEnabled()) {
        debug::TraceDraw d;
        d.shadingModel = static_cast<u8>(debug::TraceShadingModel::M2Combiners);
        d.blendClass = static_cast<u8>(item.key.blend);
        d.streamMask = debug::kStreamBase;
        d.surface = static_cast<i32>(item.key.surface);
        d.layer = static_cast<i32>(surf->materialLayer);
        d.matFlags = surf->materialFlags;
        d.filterMode = static_cast<i32>(surf->blend);
        d.texAnimId = surf->transformId[0];
        d.psoKey = debug::TracePsoKey({
            .vsPermute = key.vsIndex,
            .psPermute = key.psIndex,
            .matAlpha = key.blend,
            .disables = key.materialFlags,
            .vertexLayout = key.layoutId,
            .extraRtvCount = key.extraRtvCount,
        });
        for (u32 u = 0; u < kM2MaxTextureUnits && u < debug::kTraceTexSlots; ++u)
            d.texIds[u] = surf->textureId[u];
        debug::RecordUnlitDraw(d, *item.view, geo, ctx);
    }

    cmd->DrawIndexed(geo.indexCount);
}

core::SurfaceClass M2CombinerShading::Classify(const render_detail::RenderableView& view,
                                               const GPUGeoset& geo) const {
    // Reached only for a geoset with no surface range — an `.m2` whose table
    // failed to build. Drawing it opaque beats dropping it silently.
    (void)view;
    (void)geo;
    return {.visible = true, .blend = core::BlendClass::Opaque, .needsDepthFill = false};
}

core::SurfaceClass M2CombinerShading::ClassifySurface(const render_detail::RenderableView& view,
                                                      const GPUGeoset& geo, u32 surface) const {
    const M2SurfaceTable* table = TableOf(view);
    const M2Surface* s = table ? table->Surface(surface) : nullptr;
    if (!s)
        return {.visible = false};

    const f32 alpha = ElementAlpha(*s, SurfaceAnimFor(view, surface), view, geo);
    return M2ClassifySurface(*s, ModelAlpha(view, geo), alpha);
}

core::VertexNeeds M2CombinerShading::Needs(u32 surface) const {
    (void)surface;
    // All of these are already in the `.m2` 48-byte record; naming them is what
    // makes the Subset() call in GetOrBuildPso well-defined. `boneWeights` is
    // the one that does not come from that record — the draw binds the separate
    // `BoneVertex` stream for it.
    return core::VertexNeeds{
        .position = true, .normal = true, .uvSets = 2, .boneWeights = true};
}

i32 M2CombinerShading::SelectLights(bls::FrameInputs& frame, const bls::LightingContext& lighting,
                                    const Matrix44f& viewMat, const Vector3f& surfaceWS) const {
    (void)frame;
    (void)lighting;
    (void)viewMat;
    (void)surfaceWS;
    // Nothing to do here: `bls::FrameInputs` is WC3's constant block, and the
    // M2 path selects its own lights per *model* rather than per geoset —
    // CM2Lighting keeps the three nearest to the model centre, which
    // LightingFor resolves straight into the draw CB.
    return 0;
}

core::EmitMask M2CombinerShading::Emits(u32 surface, core::PassSlot pass) const {
    (void)surface;
    (void)pass;
    // Colour and depth. WowProfile is single-RTV (LinearShading() == false), so
    // there is no G-buffer to write even in a pass that declares one.
    return core::EmitMask::DefaultColor;
}

} // namespace whiteout::flakes::renderer::profiles::wow
