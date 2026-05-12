// ============================================================================
// RenderPipeline — extracted from RenderService.
//
// Owns the GPU device, render targets, line/BLS/tonemap pipelines, IBL probe
// state, and the per-frame submission loop. Reaches back into
// RenderService::Impl directly via friend access for scene-side state
// (actors, particles, splats, samplers, textures, settings) — same pattern
// FrameTicker / ModelLoader use (see render_service_impl.h).
// ============================================================================

#include "renderer/render_pipeline.h"
#include "renderer/render_pipeline_impl.h"
#include "renderer/perf_zone.h"
#include "renderer/render_service.h"
#include "renderer/render_service_impl.h"
#include "render_detail.h"
#include "render_pass.h"
#include "debug/debug_renderer.h"
#include "constants.h"
#include "renderer/assets/sampler_asset_manager.h"
#include "renderer/assets/texture_asset_manager.h"
#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/scene_manager.h"
#include "renderer/corn_effects/corn_effects_service.h"
#include "renderer/corn_effects/corn_effects_gfx_backend.h"
#include "renderer/model/model_template.h"
#include "renderer/model/model_template_manager.h"
#include "compiled_shaders.h"
#include "whiteout/flakes/util/team_glow_data.h"
#include "bls/bls_shader_cache.h"
#include "bls/bls_program.h"
#include "bls/bls_pso_builder.h"
#include "bls/bls_pso_trace.h"
#include "bls/bls_mat_params.h"
#include "bls/bls_cb_layout.h"
#include "bls/bls_frame.h"
#include "bls/bls_draw_helpers.h"
#include "bls/scoped_cb.h"
#include "ibl/split_sum.h"
#include "ibl/env_probe.h"
#include "shadow/shadow_pass.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>

namespace whiteout::flakes::renderer {

using namespace ::whiteout::flakes::renderer::model;
using namespace ::whiteout::flakes::renderer::effects;
using namespace ::whiteout::flakes::renderer::assets;
using namespace ::whiteout::flakes::renderer::particle;
using namespace ::whiteout::flakes::renderer::bls;
using namespace ::whiteout::flakes::renderer::shadow;
using namespace ::whiteout::flakes::renderer::ibl;
using namespace ::whiteout::flakes::renderer::debug;
using namespace ::whiteout::flakes::renderer::render_detail;

RenderPipeline::RenderPipeline(RenderService& rs)
    : rs_(rs), impl_(std::make_unique<Impl>()) {}

RenderPipeline::~RenderPipeline() = default;

bool RenderPipeline::IsDeviceReady() const               { return impl_->gfx_ != nullptr; }
void RenderPipeline::SetPrimaryTarget(RenderTargetId id) { impl_->primaryTargetId_ = id; }

gfx::IGFXDevice*       RenderPipeline::Gfx()       { return impl_->gfx_.get(); }
const gfx::IGFXDevice* RenderPipeline::Gfx() const { return impl_->gfx_.get(); }

gfx::Format RenderPipeline::SceneTargetFormat() const {
    return impl_->frameRenderMode_ == RenderMode::HD
        ? kHdrSceneFormat : kSdSceneFormat;
}

gfx::Format RenderPipeline::DepthStencilFormat() const {
    return impl_->depthStencilFormat_;
}

RenderMode RenderPipeline::FrameRenderMode() const {
    return impl_->frameRenderMode_;
}

RenderTarget* RenderPipeline::PrimaryTarget() {
    auto it = impl_->targets_.find(impl_->primaryTargetId_);
    return (it != impl_->targets_.end()) ? &it->second : nullptr;
}

i32 RenderPipeline::Width()  const { return impl_->width_;  }
i32 RenderPipeline::Height() const { return impl_->height_; }

gfx::BufferHandle RenderPipeline::CbPerFrame() const {
    return impl_->cbPerFrame_;
}

RenderPipeline::ShadowResources RenderPipeline::Shadow() const {
    return { impl_->shadowPSO_, impl_->shadowPSORigid_, impl_->shadowVsCb_ };
}

void RenderPipeline::Shutdown() {

    rs_.Spn().Clear();
    rs_.Splats().Clear();
    ReleaseModelGPU();

    rs_.Scene().Templates().ReleaseAllGPU(*impl_->gfx_);
    rs_.Scene().Templates().Clear();
    CleanupGFX();
}

void RenderPipeline::GetFrameStats(i32& geosets, i32& textures, i32& nodes,
                                   i32& particles, i32& segments) const {
    geosets = textures = nodes = particles = segments = 0;
    for (auto& [h, mi] : rs_.Scene().Actors().All()) {
        geosets  += (i32)mi->render.gpuGeosets.size();
        textures += mi->render.textures ? (i32)mi->render.textures->Size() : 0;
        nodes    += mi->render.skinning.NodeCount();
        segments += mi->render.ribbons.GetTotalSegmentCount();
    }
    particles += rs_.Particles().TotalParticleCount();
}

void RenderPipeline::ReleaseModelGPU() {
    for (auto& [h, miPtr] : rs_.Scene().Actors().All())
        miPtr->ReleaseGPU(*impl_->gfx_);
}

bool RenderPipeline::RenderParticlesBls() {

    if (!impl_->blsSdProgram_ || !impl_->blsPsoBuilder_) return false;

    auto* cmd = impl_->gfx_->GetImmediateContext();

    std::vector<Vertex> verts;
    std::vector<particle::EmitterDrawList> drawLists;
    Matrix44f viewMat;
    {
        if (rs_.Particles().EmitterCount() == 0) return true;
        viewMat = rs_.Scene().Camera().GetViewMatrix();
        rs_.Particles().BuildGeometry(viewMat, verts, drawLists);
    }
    if (verts.empty()) return true;

    std::stable_sort(drawLists.begin(), drawLists.end(),
        [](const particle::EmitterDrawList& a,
           const particle::EmitterDrawList& b) {
            if (a.priorityPlane != b.priorityPlane) return a.priorityPlane < b.priorityPlane;
            if (a.model         != b.model)         return a.model         < b.model;
            return a.emitterId < b.emitterId;
        });

    const i32 vertCount= (i32)verts.size();

    if (impl_->particleServiceVB_ == gfx::BufferHandle::Invalid || vertCount > impl_->particleServiceVBSize_) {
        impl_->gfx_->Destroy(impl_->particleServiceVB_);
        i32 newSize = (std::max)(vertCount, 4096);
        gfx::BufferDesc bd;
        bd.size          = (u32)(sizeof(Vertex) * newSize);
        bd.usage         = gfx::BufferUsage::Vertex | gfx::BufferUsage::CpuWritable;
        bd.ringSlotsHint = 4;  // mapped once per frame
        impl_->particleServiceVB_     = impl_->gfx_->CreateBuffer(bd);
        impl_->particleServiceVBSize_ = newSize;
    }
    if (void* mapped = impl_->gfx_->MapBuffer(impl_->particleServiceVB_)) {
        memcpy(mapped, verts.data(), sizeof(Vertex) * vertCount);
        impl_->gfx_->UnmapBuffer(impl_->particleServiceVB_);
    }

    cmd->BindVertexBuffer(0, impl_->particleServiceVB_, sizeof(Vertex));

    bls::FrameInputs frame;
    frame.world      = Matrix44f::identity();
    frame.view       = viewMat;
    const f32 aspect= (impl_->height_ > 0) ? (f32)impl_->width_ / (f32)impl_->height_ : 1.0f;
    frame.projection = rs_.Scene().Camera().ProjectionRH(aspect);
    frame.effectTime = rs_.Scene().GetAnimationTime() * 0.001f;
    frame.numLights  = 0;
    frame.viewportRect = { (f32)impl_->width_, (f32)impl_->height_, 0.0f, 0.0f };

    for (const auto& dl : drawLists) {
        if (dl.vertexCount <= 0) continue;

        bls::MatParams mp = bls::FromParticleDesc(dl.material, bls::GxShaderID::SD);
        mp.disables |= bls::kDisableLighting;

        mp.diffuseColor = {1, 1, 1, 1};

        bls::RenderState rs;
        rs.shaderId       = bls::GxShaderID::SD;
        rs.alphaMode      = static_cast<u8>(mp.alpha);
        rs.numColors      = 1;
        rs.numTexCoords   = 1;
        rs.numWeights     = 0;
        rs.numLights      = 0;
        rs.fogEnabled     = false;
        rs.depthWrite     = mp.DepthWriteEnabled();
        rs.lightingEnabled= false;
        auto perm = bls::SelectPermutes(rs);

        auto req = bls::MakePsoRequest(impl_->blsSdProgram_,
                                       bls::VertexLayoutKind::ParticleSD,
                                       mp, perm);
        req.rtvFormat = SceneTargetFormat();
        req.dsvFormat = impl_->depthStencilFormat_;
        auto pso = impl_->blsPsoBuilder_->GetOrBuild(req);
        if (pso == gfx::PipelineHandle::Invalid) continue;
        cmd->BindPipeline(pso);

        if (auto vs = bls::ScopedCb<bls::SdVsCbA>(impl_->gfx_.get(), impl_->blsSdVsCb_)) {
            bls::BuildSdVsCbA(*vs, frame, mp);
        }
        if (auto ps = bls::ScopedCb<bls::SdPsCbA>(impl_->gfx_.get(), impl_->blsSdPsCb_)) {
            bls::BuildSdPsCbA(*ps, frame, mp);
        }
        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, impl_->blsSdVsCb_);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel,  0, impl_->blsSdPsCb_);

        const u32 wrapFlags = 0x3;

        gfx::TextureHandle peTex = gfx::TextureHandle::Invalid;
        Actor* owner = rs_.Scene().Actors().Find(dl.model);
        const u32 ownerColor = owner ? (owner->teamColor | 0xFF000000u) : 0xFF0000FFu;
        if (dl.material.replaceableId == 1) {
            peTex = rs_.Replaceables().GetSdTeamColorTextureFor(ownerColor);
        } else if (dl.material.replaceableId == 2) {
            peTex = rs_.Replaceables().GetSdTeamGlowTextureFor(ownerColor);
        } else {
            if (owner && owner->render.textures && dl.material.textureId >= 0)
                peTex = owner->render.textures->Get(dl.material.textureId);
        }
        if (peTex != gfx::TextureHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, peTex);
        else
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, rs_.Textures().GetDefaults().White);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, rs_.Samplers().WrapVariant(wrapFlags));

        cmd->Draw(dl.vertexCount, dl.vertexOffset);
    }
    return true;
}

namespace {
particle::FilterMode SplatBlendModeToFilter(i32 blendMode) {

    switch (blendMode) {
        case 0: return particle::FilterMode::Blend;
        case 1: return particle::FilterMode::Additive;
        case 2: return particle::FilterMode::Modulate;
        case 3: return particle::FilterMode::Modulate2X;
        case 4: return particle::FilterMode::AlphaKey;
        default: return particle::FilterMode::Blend;
    }
}
}

bool RenderPipeline::RenderSplatsBls() {
    if (!impl_->blsSdProgram_ || !impl_->blsPsoBuilder_) return false;
    if (rs_.Splats().Count() == 0)        return true;

    auto* cmd = impl_->gfx_->GetImmediateContext();

    std::vector<Vertex>                       verts;
    std::vector<particle::SplatDrawList>      drawLists;
    Matrix44f viewMat = rs_.Scene().Camera().GetViewMatrix();
    rs_.Splats().BuildGeometry(verts, drawLists);
    if (verts.empty()) return true;

    const i32 vertCount= (i32)verts.size();
    if (impl_->splatServiceVB_ == gfx::BufferHandle::Invalid || vertCount > impl_->splatServiceVBSize_) {
        impl_->gfx_->Destroy(impl_->splatServiceVB_);
        i32 newSize = (std::max)(vertCount, 4096);
        gfx::BufferDesc bd;
        bd.size          = (u32)(sizeof(Vertex) * newSize);
        bd.usage         = gfx::BufferUsage::Vertex | gfx::BufferUsage::CpuWritable;
        bd.ringSlotsHint = 4;  // mapped once per frame
        impl_->splatServiceVB_     = impl_->gfx_->CreateBuffer(bd);
        impl_->splatServiceVBSize_ = newSize;
    }
    if (void* mapped = impl_->gfx_->MapBuffer(impl_->splatServiceVB_)) {
        memcpy(mapped, verts.data(), sizeof(Vertex) * vertCount);
        impl_->gfx_->UnmapBuffer(impl_->splatServiceVB_);
    }

    cmd->BindVertexBuffer(0, impl_->splatServiceVB_, sizeof(Vertex));

    bls::FrameInputs frame;
    frame.world      = Matrix44f::identity();
    frame.view       = viewMat;
    const f32 aspect= (impl_->height_ > 0) ? (f32)impl_->width_ / (f32)impl_->height_ : 1.0f;
    frame.projection = rs_.Scene().Camera().ProjectionRH(aspect);
    frame.effectTime = rs_.Scene().GetAnimationTime() * 0.001f;
    frame.numLights  = 0;
    frame.viewportRect = { (f32)impl_->width_, (f32)impl_->height_, 0.0f, 0.0f };

    for (const auto& dl : drawLists) {
        if (dl.vertexCount <= 0) continue;

        particle::ParticleMaterialDesc pmd;
        pmd.filterMode = SplatBlendModeToFilter(dl.blendMode);
        pmd.unshaded   = true;
        pmd.unfogged   = true;
        pmd.textureId  = -1;

        bls::MatParams mp = bls::FromParticleDesc(pmd, bls::GxShaderID::SD);
        mp.disables |= bls::kDisableLighting;
        mp.diffuseColor = {1, 1, 1, 1};

        bls::RenderState rs;
        rs.shaderId        = bls::GxShaderID::SD;
        rs.alphaMode       = static_cast<u8>(mp.alpha);
        rs.numColors       = 1;
        rs.numTexCoords    = 1;
        rs.numWeights      = 0;
        rs.numLights       = 0;
        rs.fogEnabled      = false;
        rs.depthWrite      = mp.DepthWriteEnabled();
        rs.lightingEnabled = false;
        auto perm = bls::SelectPermutes(rs);

        auto req = bls::MakePsoRequest(impl_->blsSdProgram_,
                                       bls::VertexLayoutKind::ParticleSD,
                                       mp, perm);
        req.rtvFormat = SceneTargetFormat();
        req.dsvFormat = impl_->depthStencilFormat_;
        auto pso = impl_->blsPsoBuilder_->GetOrBuild(req);
        if (pso == gfx::PipelineHandle::Invalid) continue;
        cmd->BindPipeline(pso);

        if (auto vs = bls::ScopedCb<bls::SdVsCbA>(impl_->gfx_.get(), impl_->blsSdVsCb_)) {
            bls::BuildSdVsCbA(*vs, frame, mp);
        }
        if (auto ps = bls::ScopedCb<bls::SdPsCbA>(impl_->gfx_.get(), impl_->blsSdPsCb_)) {
            bls::BuildSdPsCbA(*ps, frame, mp);
        }
        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, impl_->blsSdVsCb_);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel,  0, impl_->blsSdPsCb_);

        const u32 wrapFlags = 0x3;
        if (dl.texture != gfx::TextureHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, dl.texture);
        else
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, rs_.Textures().GetDefaults().White);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, rs_.Samplers().WrapVariant(wrapFlags));

        cmd->Draw(dl.vertexCount, dl.vertexOffset);
    }
    return true;
}

void RenderPipeline::RenderCornEffects() {
    // Cornflakes-driven CornFx emitters. The backend is per-emitter and
    // built lazily on first spawn; this pass just sets the per-frame inputs
    // (cmd / view / proj / viewport / effectTime) and runs Simulate which
    // ticks every emitter — submit() emits GPU draws inline during the
    // tick, so we MUST run inside the active render pass.
    if (!impl_->blsCornFxProgram_ || !impl_->blsPsoBuilder_) return;
    auto* cmd = impl_->gfx_->GetImmediateContext();
    const f32 aspect = (impl_->height_ > 0) ? (f32)impl_->width_ / (f32)impl_->height_ : 1.0f;

    corn_effects::CornEffectsFrameInputs fi;
    fi.cmd          = cmd;
    fi.view         = rs_.Scene().Camera().ViewLH();
    fi.projection   = rs_.Scene().Camera().ProjectionLH(aspect);
    fi.viewportRect = { (f32)impl_->width_, (f32)impl_->height_, 0.0f, 0.0f };
    fi.effectTime   = rs_.Scene().GetAnimationTime() * 0.001f;
    fi.rtvFormat    = SceneTargetFormat();
    fi.dsvFormat    = impl_->depthStencilFormat_;
    rs_.CornEffects().SetFrameInputs(fi);
    rs_.CornEffects().Simulate(rs_.CornEffects().PendingDt());
}

void RenderPipeline::RenderRibbons() {
    if (!impl_->blsSdProgram_ || !impl_->blsPsoBuilder_) return;
    auto* cmd = impl_->gfx_->GetImmediateContext();

    bls::FrameInputs frame;
    frame.world        = Matrix44f::identity();
    const f32 aspect= (impl_->height_ > 0) ? (f32)impl_->width_ / (f32)impl_->height_ : 1.0f;
    frame.numLights    = 0;
    frame.viewportRect = { (f32)impl_->width_, (f32)impl_->height_, 0.0f, 0.0f };
    frame.effectTime   = rs_.Scene().GetAnimationTime() * 0.001f;

    for (auto& [_mh, _mi] : rs_.Scene().Actors().All()) {
    auto* mi = _mi.get();
    Matrix44f viewMat;
    RibbonSystem::StripResult stripResult;
    std::vector<RibbonEmitterConfig> configs;

    {
        if (!mi->render.ribbons.HasEmitters()) continue;
        if (mi->parentVisibility <= 0.02f) continue;
        viewMat = rs_.Scene().Camera().GetViewMatrix();
        stripResult = mi->render.ribbons.BuildStrips();
        for (i32 eid: stripResult.emitterIds) {
            auto* c = mi->render.ribbons.GetConfig(eid);
            configs.push_back(c ? *c : RibbonEmitterConfig{});
        }
    }

    auto& verts = stripResult.vertices;
    auto& emitterIds = stripResult.emitterIds;
    if (verts.empty()) continue;
    i32 vertCount = (i32)verts.size();

    if (mi->render.ribbonVB == gfx::BufferHandle::Invalid || vertCount > mi->render.ribbonVBSize) {
        impl_->gfx_->Destroy(mi->render.ribbonVB);
        i32 newSize = (std::max)(vertCount, 512);
        gfx::BufferDesc bd;
        bd.size          = (u32)(sizeof(Vertex) * newSize);
        bd.usage         = gfx::BufferUsage::Vertex | gfx::BufferUsage::CpuWritable;
        bd.ringSlotsHint = 4;  // per-actor, mapped once per frame
        mi->render.ribbonVB = impl_->gfx_->CreateBuffer(bd);
        mi->render.ribbonVBSize = newSize;
    }

    void* mapped = impl_->gfx_->MapBuffer(mi->render.ribbonVB);
    if (!mapped) continue;
    memcpy(mapped, verts.data(), sizeof(Vertex) * vertCount);
    impl_->gfx_->UnmapBuffer(mi->render.ribbonVB);

    cmd->BindVertexBuffer(0, mi->render.ribbonVB, sizeof(Vertex));

    frame.view       = viewMat;
    frame.projection = rs_.Scene().Camera().ProjectionRH(aspect);

    std::vector<i32> vertCounts;
    std::vector<i32> vertOffsets;
    {
        i32 running = 0;
        for (i32 eid: emitterIds) {
            vertOffsets.push_back(running);
            const i32 n = mi->render.ribbons.GetEmitterVertCount(eid);
            vertCounts.push_back(n);
            running += n;
        }
    }

    std::vector<i32> drawOrder(emitterIds.size());
    for (i32 i= 0; i < (i32)drawOrder.size(); ++i) drawOrder[i] = i;
    std::stable_sort(drawOrder.begin(), drawOrder.end(),
        [&](i32 a, i32 b) {
            const i32 pa = configs[a].priorityPlane;
            const i32 pb = configs[b].priorityPlane;
            if (pa != pb) return pa < pb;
            return emitterIds[a] < emitterIds[b];
        });

    for (i32 ei: drawOrder) {
        auto& cfg = configs[ei];
        const i32 count   = vertCounts[ei];
        const i32 offset  = vertOffsets[ei];
        if (count <= 0) continue;

        i32 matFlags = 0;
        if (cfg.twoSided) matFlags |= MAT_TWO_SIDED;
        if (cfg.unshaded) matFlags |= MAT_UNSHADED;
        bls::MatParams mp = bls::FromMdxLayer(cfg.filterMode, matFlags, bls::GxShaderID::SD);
        mp.disables |= bls::kDisableLighting;
        mp.diffuseColor = {1, 1, 1, 1};

        bls::RenderState rs = bls::MakeSdMeshRenderState(mp, 0,  true,  false);
        auto perm = bls::SelectPermutes(rs);
        auto req = bls::MakePsoRequest(impl_->blsSdProgram_,
                                       bls::VertexLayoutKind::ParticleSD,
                                       mp, perm);
        req.rtvFormat = SceneTargetFormat();
        req.dsvFormat = impl_->depthStencilFormat_;
        auto pso = impl_->blsPsoBuilder_->GetOrBuild(req);
        if (pso == gfx::PipelineHandle::Invalid) continue;
        cmd->BindPipeline(pso);

        if (auto vs = bls::ScopedCb<bls::SdVsCbA>(impl_->gfx_.get(), impl_->blsSdVsCb_)) {
            bls::BuildSdVsCbA(*vs, frame, mp);
        }
        if (auto ps = bls::ScopedCb<bls::SdPsCbA>(impl_->gfx_.get(), impl_->blsSdPsCb_)) {
            bls::BuildSdPsCbA(*ps, frame, mp);
        }
        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, impl_->blsSdVsCb_);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel,  0, impl_->blsSdPsCb_);

        render_detail::BindLayerAlbedo(cmd, mi->render.textures.get(), cfg.textureId,
                                       rs_.Textures().GetDefaults().White, rs_.Samplers());

        cmd->Draw(count, offset);
    }
    }
}

i32 RenderPipeline::ComputeSelectedLod() const {
    i32 ov = rs_.Settings().LodOverride();
    if (ov >= 0) return std::clamp(ov, 0, 3);

    Vector3f camPos = rs_.Scene().Camera().GetSource();
    f32 viewDist = std::sqrt(camPos.x*camPos.x + camPos.y*camPos.y + camPos.z*camPos.z);
    if (viewDist < 1.0f) return 0;

    const f32 aspect= (impl_->height_ > 0) ? (f32)impl_->width_ / (f32)impl_->height_ : 1.0f;
    Matrix44f proj = rs_.Scene().Camera().ProjectionLH(aspect);

    f32 projM11 = proj.data[1][1];
    f32 screenPixels = projM11 / viewDist * (f32)impl_->height_ * 0.5f;
    if (screenPixels <= 0.001f) return 3;
    f32 deviation = 5.0f / screenPixels;

    static constexpr f32 kDeviations[4] = {0.0f, 1.0f, 2.0f, 4.0f};
    i32 selectedLOD = 4;
    do { --selectedLOD; }
    while (selectedLOD > 0 && deviation <= kDeviations[selectedLOD]);
    return std::clamp(selectedLOD, 0, 3);
}

bool RenderPipeline::InitDevice(gfx::GfxApi api) {

    // Preferred-device hint to the gfx layer: empty string falls back
    // to the backend's "best by VRAM / score" default. Setting it
    // here keeps the gfx layer's module-scope state in sync with
    // whatever the host loaded from .ini.
    gfx::SetPreferredDevice(rs_.Settings().PreferredDevice().c_str());

    impl_->gfx_ = gfx::CreateDevice(api, rs_.Settings().GraphicsDebug());
    if (!impl_->gfx_) return false;

    // Cache the device's preferred depth-stencil format once. Every
    // subsequent CreateDepthTarget / PSO dsvFormat in this file (and
    // in the BLS PSO builder, debug renderer, etc.) reads from here.
    // On AMD this lands as D32_FLOAT_S8_UINT; on NVIDIA/Intel as
    // D24_UNORM_S8_UINT.
    impl_->depthStencilFormat_ = impl_->gfx_->PreferredDepthStencilFormat();

    rs_.CreateDeviceAssetManagers(*impl_->gfx_);

    impl_->cbPerFrame_ = impl_->gfx_->CreateBuffer({
        .size  = sizeof(CBPerFrame),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });

    if (!CreateShaders())          { CleanupGFX(); return false; }
    if (!CreatePipelines())        { CleanupGFX(); return false; }
    if (!CreateDefaultResources()) { CleanupGFX(); return false; }

    if (!InitBlsShaders(api)) { CleanupGFX(); return false; }

    return true;
}

bool RenderPipeline::InitBlsShaders(gfx::GfxApi api) {
    if (!impl_->gfx_ || !rs_.Scene().ActiveContentProvider()) return false;

    rs_.Replaceables().SetContentProvider(rs_.Scene().ActiveContentProvider());

    rs_.Splats().Configure(impl_->gfx_.get(), &rs_.Textures(),
                            rs_.Scene().ActiveContentProvider());

    rs_.EnsureDncService();
    rs_.EnsureShadowService(*impl_->gfx_);

    if (impl_->shadowVsCb_ == gfx::BufferHandle::Invalid) {
        impl_->shadowVsCb_ = impl_->gfx_->CreateBuffer({
            .size  = sizeof(bls::HdVsCb),
            .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        });
    }

    impl_->blsShaderCache_ = std::make_unique<bls::BlsShaderCache>(
        impl_->gfx_.get(), rs_.Scene().ActiveContentProvider(), api);
    impl_->blsPrograms_    = std::make_unique<bls::BlsProgramCatalog>(impl_->blsShaderCache_.get());
    impl_->blsPsoBuilder_  = std::make_unique<bls::BlsPsoBuilder>(impl_->gfx_.get());

    // PSO pre-warm trace: load keys saved by previous runs, attach the
    // recorder so every cache-missed PSO during this run gets logged.
    // Trace file lives next to the host-provided gfx pipeline cache
    // (or empty/in-memory-only if the host didn't set one).
    {
        std::filesystem::path tracePath;
        const auto& pipelineCachePath = gfx::GetPipelineCachePath();
        if (!pipelineCachePath.empty()) {
            tracePath = pipelineCachePath.parent_path() / "pso_trace.bin";
        }
        impl_->blsPsoTrace_ = std::make_unique<bls::BlsPsoTrace>(tracePath);
        impl_->blsPsoBuilder_->SetTrace(impl_->blsPsoTrace_.get());
    }

    impl_->blsSdProgram_ = impl_->blsPrograms_->Load({
        bls::GxShaderID::SD, "SD_HighSpec", "SD"
    });
    impl_->blsSdOnHdProgram_ = impl_->blsPrograms_->Load({
        bls::GxShaderID::SD_on_HD, "SD_on_HD", "SD_on_HD"
    });
    impl_->blsHdProgram_ = impl_->blsPrograms_->Load({
        bls::GxShaderID::HD, "HD", "HD"
    });

    impl_->blsCrystalProgram_ = impl_->blsPrograms_->Load({
        bls::GxShaderID::Crystal, "HD", "Crystal"
    });

    // The BLS catalog name strings are the engine's wire-format shader
    // filenames on disk — preserved verbatim for interoperability with
    // shipped game data. Our own symbol names (GxShaderID::CornFx,
    // blsCornFxProgram_) are trademark-neutral.
    impl_->blsCornFxProgram_ = impl_->blsPrograms_->Load({
        bls::GxShaderID::CornFx, "PopcornFX", "PopcornFX"
    });

    // Hand the corn fx service the renderer's gfx + BLS resources so its
    // per-emitter CornEffectsGfxBackend can issue draws. The texture resolver
    // uses TextureAssetManager's path-based lookup; corn fx diffuse paths
    // come from the .pkb's renderer property block.
    if (impl_->blsCornFxProgram_) {
        corn_effects::CornEffectsGfxBackend::Init pInit;
        pInit.device     = impl_->gfx_.get();
        pInit.program    = impl_->blsCornFxProgram_;
        pInit.psoBuilder = impl_->blsPsoBuilder_.get();
        pInit.textures   = &rs_.Textures();
        pInit.samplers   = &rs_.Samplers();
        // Resolve diffuse paths to gfx::TextureHandle by loading them on
        // demand through the active content provider. CornEffects .pkb assets
        // reference textures (DDS/TGA/BLP) that live outside the MDX's
        // own texture list, so a pure shared-cache lookup misses — the
        // service-level loader caches the result under the normalised
        // path key so subsequent spawns hit instead of re-decoding.
        pInit.resolver = [&rs = rs_](std::string_view path) -> gfx::TextureHandle {
            return rs.LoadCornEffectsTexture(path);
        };
        rs_.CornEffects().SetBackendInit(pInit);
    }

    if (impl_->blsHdProgram_ && impl_->blsHdProgram_->vs &&
        !impl_->blsHdProgram_->vs->permuteHandles.empty()) {
        const shadow::ShadowParams& sp = rs_.GetShadowService()
            ? rs_.GetShadowService()->Params()
            : shadow::ShadowParams{};

        auto buildShadowPso = [&](u32 permIndex,
                                   bls::VertexLayoutKind layoutKind)
            -> gfx::PipelineHandle {
            if (permIndex >= impl_->blsHdProgram_->vs->permuteHandles.size())
                return gfx::PipelineHandle::Invalid;
            gfx::GraphicsPipelineDesc gpd{};
            gpd.vs                              = impl_->blsHdProgram_->vs->permuteHandles[permIndex];
            gpd.ps                              = gfx::ShaderHandle{0};
            gpd.inputLayout                     = bls::LayoutFor(layoutKind);
            gpd.topology                        = gfx::PrimitiveTopology::TriangleList;
            gpd.depthStencil.depthTest          = true;
            gpd.depthStencil.depthWrite         = true;
            gpd.depthStencil.depthCompare       = gfx::CompareOp::LessEqual;
            gpd.rasterizer.cull                 = gfx::CullMode::Back;
            gpd.rasterizer.frontCCW             = true;
            gpd.rasterizer.depthBias            = sp.depthBias;
            gpd.rasterizer.slopeScaledDepthBias = sp.slopeScaledBias;
            gpd.rasterizer.depthBiasClamp       = sp.depthBiasClamp;
            gpd.rtvFormat                       = gfx::Format::Unknown;
            gpd.dsvFormat                       = gfx::Format::D32_FLOAT;
            return impl_->gfx_->CreateGraphicsPipeline(gpd);
        };

        if (impl_->shadowPSO_ == gfx::PipelineHandle::Invalid) {
            impl_->shadowPSO_ = buildShadowPso(
                 4,
                bls::VertexLayoutKind::MeshHDSkinnedNoTangent);
        }
        if (impl_->shadowPSORigid_ == gfx::PipelineHandle::Invalid) {
            impl_->shadowPSORigid_ = buildShadowPso(
                 0,
                bls::VertexLayoutKind::ParticleSD);
        }
    }

    impl_->blsSpriteVs_  = impl_->blsShaderCache_->Acquire(gfx::ShaderStage::Vertex, "sprite");
    impl_->blsTonemapPs_ = impl_->blsShaderCache_->Acquire(gfx::ShaderStage::Pixel,  "tonemap");

    if (impl_->blsSpriteVs_ && !impl_->blsSpriteVs_->permuteHandles.empty()
        && impl_->blsTonemapPs_ && !impl_->blsTonemapPs_->permuteHandles.empty())
    {
        struct TonemapVertex {
            f32 x, y, z;
            f32 u, v;
        };

        static const TonemapVertex kTonemapVerts[3] = {

            { -1.0f,  1.0f, 0.0f,         0.0f, 0.0f },
            {  3.0f,  1.0f, 0.0f,         2.0f, 0.0f },
            { -1.0f, -3.0f, 0.0f,         0.0f, 2.0f },
        };
        impl_->tonemapVB_ = impl_->gfx_->CreateBuffer({
            .size  = sizeof(kTonemapVerts),
            .usage = gfx::BufferUsage::Vertex,
        }, kTonemapVerts);

        const gfx::InputElement spriteInput[] = {
            {"ATTR", 0, gfx::Format::R32G32B32_FLOAT, 0},
            {"ATTR", 3, gfx::Format::R32G32_FLOAT,    12},
        };
        gfx::GraphicsPipelineDesc tm;
        tm.vs              = impl_->blsSpriteVs_->permuteHandles[0];
        tm.ps              = impl_->blsTonemapPs_->permuteHandles[0];
        tm.inputLayout     = spriteInput;
        tm.topology        = gfx::PrimitiveTopology::TriangleList;
        tm.blend.enable    = false;
        tm.depthStencil.depthTest  = false;
        tm.depthStencil.depthWrite = false;
        tm.rasterizer.cull     = gfx::CullMode::None;
        tm.rasterizer.frontCCW = true;
        tm.rtvFormat       = gfx::Format::R8G8B8A8_UNORM_SRGB;
        tm.dsvFormat       = impl_->depthStencilFormat_;
        impl_->tonemapPSO_ = impl_->gfx_->CreateGraphicsPipeline(tm);

        impl_->tonemapPsCb_ = impl_->gfx_->CreateBuffer({
            .size  = 16,
            .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        });
        gfx::SamplerDesc sd;
        sd.minFilter = gfx::Filter::Linear;
        sd.magFilter = gfx::Filter::Linear;
        sd.addressU  = gfx::AddressMode::Clamp;
        sd.addressV  = gfx::AddressMode::Clamp;
        sd.addressW  = gfx::AddressMode::Clamp;
        impl_->tonemapSampler_ = impl_->gfx_->CreateSampler(sd);
    }

    // All BLS frame-uniform CBs get mapped once per draw call. With
    // ~2000+ draws/frame on PE1-heavy scenes (BreeForge Birth) the
    // default 256-slot ring wraps mid-frame and corrupts earlier
    // descriptor writes (visible as light flicker / shading glitches
    // on Vulkan). Request a large ring on every hot CB; the slot
    // stride is small (each CB ~256-1KB), so total memory stays
    // reasonable (~32 MB across all hot CBs at 4096 slots).
    constexpr u32 kHotCbRingSlots = 4096;
    impl_->blsSdVsCb_ = impl_->gfx_->CreateBuffer({
        .size  = sizeof(bls::SdVsCbA),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kHotCbRingSlots,
    });

    impl_->blsSdPsCb_ = impl_->gfx_->CreateBuffer({
        .size  = sizeof(bls::SdPsCbA),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kHotCbRingSlots,
    });
    impl_->blsHdVsCb_ = impl_->gfx_->CreateBuffer({
        .size  = sizeof(bls::HdVsCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kHotCbRingSlots,
    });

    impl_->blsHdShadowCb_ = impl_->gfx_->CreateBuffer({
        .size  = sizeof(bls::HdShadowCascadesCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kHotCbRingSlots,
    });

    impl_->blsHdShadowCountCb_ = impl_->gfx_->CreateBuffer({
        .size  = sizeof(bls::SdOnHdShadowCascadeCountCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kHotCbRingSlots,
    });
    impl_->blsHdPsCb_ = impl_->gfx_->CreateBuffer({
        .size  = sizeof(bls::HdPsCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kHotCbRingSlots,
    });
    impl_->blsSdOnHdPsCb_ = impl_->gfx_->CreateBuffer({
        .size  = sizeof(bls::SdOnHdPsCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kHotCbRingSlots,
    });
    impl_->blsHdDebugVisCb_ = impl_->gfx_->CreateBuffer({
        .size  = sizeof(bls::DebugVisCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kHotCbRingSlots,
    });

    rs_.Textures().RegisterOwned(kIblSplitSumLutName, ibl::CreateSplitSumLutTexture(*impl_->gfx_));

    // Initial IBL apply — RenderFrame's poll handles subsequent mode changes.
    ApplyIblMode(rs_.Settings().GetIblMode());
    rs_.Settings().ConsumeIblModeDirty();

    const bool ok =
           impl_->blsSdProgram_     != nullptr
        && impl_->blsSdOnHdProgram_ != nullptr
        && impl_->blsHdProgram_     != nullptr
        && impl_->blsSpriteVs_      != nullptr
        && impl_->blsTonemapPs_     != nullptr
        && impl_->tonemapPSO_       != gfx::PipelineHandle::Invalid;

    // Replay the saved PSO trace now that every BLS program is loaded
    // and the shadow / tonemap PSOs are built. This pre-warms the
    // pipeline cache with every key the previous run actually touched
    // before any draw fires, so the first frame doesn't stutter on
    // first-encounter PSO compiles. Entries whose program index or
    // permute index don't resolve (e.g. shaders recompiled with a
    // different perm count) silently no-op.
    if (ok && impl_->blsPsoTrace_ && impl_->blsPsoBuilder_) {
        const auto n = impl_->blsPsoTrace_->EntryCount();
        if (n > 0) {
            std::fprintf(stderr, "[bls] pso-trace: replaying %zu PSOs\n", n);
            impl_->blsPsoBuilder_->SetReplayMode(true);
            impl_->blsPsoTrace_->Replay(*impl_->blsPsoBuilder_,
                                         *impl_->blsPrograms_);
            impl_->blsPsoBuilder_->SetReplayMode(false);
            const auto& s = impl_->blsPsoBuilder_->Stats();
            std::fprintf(stderr,
                "[bls] pso-trace: replay built %llu PSOs, %llu were already cached\n",
                (unsigned long long)s.replayCacheBuilds,
                (unsigned long long)s.cacheHits);
        } else {
            std::fprintf(stderr,
                "[bls] pso-trace: no saved trace (this run will record one)\n");
        }
    }

    if (!ok) {
        std::fprintf(stderr,
            "[bls] InitBlsShaders incomplete: "
            "SD=%p SD_on_HD=%p HD=%p Crystal=%p CornFx=%p "
            "spriteVs=%p tonemapPs=%p tonemapPSO=%llu\n",
            (void*)impl_->blsSdProgram_,
            (void*)impl_->blsSdOnHdProgram_,
            (void*)impl_->blsHdProgram_,
            (void*)impl_->blsCrystalProgram_,
            (void*)impl_->blsCornFxProgram_,
            (void*)impl_->blsSpriteVs_,
            (void*)impl_->blsTonemapPs_,
            (unsigned long long)impl_->tonemapPSO_);
    }
    return ok;
}

void RenderPipeline::SetEnvProbe(const std::string& relPath) {
    if (!impl_->gfx_ || !rs_.HasDeviceAssetManagers()) return;

    rs_.Textures().ReleaseOwned(kIblFromProbeName);
    rs_.Textures().ReleaseOwned(kIblToProbeName);

    gfx::TextureHandle fromHandle = gfx::TextureHandle::Invalid;
    i32 mips = 0;
    if (!relPath.empty() && rs_.Scene().ActiveContentProvider()) {
        auto probe = ibl::LoadEnvProbe(*impl_->gfx_, *rs_.Scene().ActiveContentProvider(), relPath);
        if (probe.handle != gfx::TextureHandle::Invalid) {
            fromHandle = probe.handle;
            mips       = probe.mipCount;
        }
    }
    if (fromHandle == gfx::TextureHandle::Invalid) {
        std::fprintf(stderr,
                     "[ibl] WARN: probe '%s' failed to load — using debug "
                     "procedural\n",
                     relPath.c_str());
        fromHandle = ibl::CreateDebugFacesEnvProbe(*impl_->gfx_);
        mips       = ibl::kEnvProbeMipLevels;
    }
    rs_.Textures().RegisterOwned(kIblFromProbeName, fromHandle);
    impl_->iblProbeMipEnd_ = static_cast<f32>(mips - 1);

    impl_->iblDayNightLoaded_ = false;
    rs_.Textures().ReleaseOwned(kIblDayProbeName);
    rs_.Textures().ReleaseOwned(kIblNightProbeName);
}

void RenderPipeline::SetDayNightProbes(const std::string& dayPath,
                                       const std::string& nightPath) {
    if (!impl_->gfx_ || !rs_.HasDeviceAssetManagers() || !rs_.Scene().ActiveContentProvider()) return;

    rs_.Textures().ReleaseOwned(kIblDayProbeName);
    rs_.Textures().ReleaseOwned(kIblNightProbeName);
    impl_->iblDayNightLoaded_ = false;
    impl_->iblDayMipEnd_      = 0.0f;
    impl_->iblNightMipEnd_    = 0.0f;

    auto loadProbe = [&](const std::string& path,
                         const char* slotName) -> i32 {
        if (path.empty()) return 0;
        auto probe = ibl::LoadEnvProbe(*impl_->gfx_, *rs_.Scene().ActiveContentProvider(), path);
        if (probe.handle == gfx::TextureHandle::Invalid) {
            std::fprintf(stderr, "[dnc] day/night IBL load failed: %s\n", path.c_str());
            return 0;
        }
        rs_.Textures().RegisterOwned(slotName, probe.handle);
        return probe.mipCount;
    };

    const i32 dayMips   = loadProbe(dayPath,   kIblDayProbeName);
    const i32 nightMips = loadProbe(nightPath, kIblNightProbeName);
    if (dayMips > 0 && nightMips > 0) {
        impl_->iblDayMipEnd_      = static_cast<f32>(dayMips - 1);
        impl_->iblNightMipEnd_    = static_cast<f32>(nightMips - 1);
        impl_->iblDayNightLoaded_ = true;
    } else {

        rs_.Textures().ReleaseOwned(kIblDayProbeName);
        rs_.Textures().ReleaseOwned(kIblNightProbeName);
    }
}

void RenderPipeline::ApplyIblMode(IblMode mode) {
    switch (mode) {
        case IblMode::DayNight:
            SetDayNightProbes(ibl::kDayIblPath, ibl::kNightIblPath);

            if (!impl_->iblDayNightLoaded_) SetEnvProbe(ibl::kPortraitIblPath);
            return;
        case IblMode::Dungeon:
            SetEnvProbe(ibl::kDungeonIblPath);
            return;
        case IblMode::Sunset:
            SetEnvProbe(ibl::kSunsetIblPath);
            return;
        case IblMode::Portrait:
            break;
    }
    SetEnvProbe(ibl::kPortraitIblPath);
}

void RenderPipeline::ShutdownBlsShaders() {
    impl_->blsSdProgram_      = nullptr;
    impl_->blsSdOnHdProgram_  = nullptr;
    impl_->blsHdProgram_      = nullptr;
    impl_->blsCrystalProgram_ = nullptr;

    impl_->blsSpriteVs_      = nullptr;
    impl_->blsTonemapPs_     = nullptr;
    if (impl_->gfx_) {
        impl_->gfx_->Destroy(impl_->blsSdVsCb_);     impl_->blsSdVsCb_     = gfx::BufferHandle::Invalid;
        impl_->gfx_->Destroy(impl_->blsSdPsCb_);     impl_->blsSdPsCb_     = gfx::BufferHandle::Invalid;
        impl_->gfx_->Destroy(impl_->blsHdVsCb_);     impl_->blsHdVsCb_     = gfx::BufferHandle::Invalid;
        impl_->gfx_->Destroy(impl_->blsHdShadowCb_); impl_->blsHdShadowCb_ = gfx::BufferHandle::Invalid;
        impl_->gfx_->Destroy(impl_->blsHdShadowCountCb_); impl_->blsHdShadowCountCb_ = gfx::BufferHandle::Invalid;
        if (impl_->shadowPSO_  != gfx::PipelineHandle::Invalid) {
            impl_->gfx_->Destroy(impl_->shadowPSO_);  impl_->shadowPSO_  = gfx::PipelineHandle::Invalid;
        }
        if (impl_->shadowPSORigid_ != gfx::PipelineHandle::Invalid) {
            impl_->gfx_->Destroy(impl_->shadowPSORigid_);
            impl_->shadowPSORigid_ = gfx::PipelineHandle::Invalid;
        }
        if (impl_->shadowVsCb_ != gfx::BufferHandle::Invalid) {
            impl_->gfx_->Destroy(impl_->shadowVsCb_); impl_->shadowVsCb_ = gfx::BufferHandle::Invalid;
        }
        impl_->gfx_->Destroy(impl_->blsHdPsCb_);     impl_->blsHdPsCb_     = gfx::BufferHandle::Invalid;
        impl_->gfx_->Destroy(impl_->blsSdOnHdPsCb_); impl_->blsSdOnHdPsCb_ = gfx::BufferHandle::Invalid;
        impl_->gfx_->Destroy(impl_->blsHdDebugVisCb_); impl_->blsHdDebugVisCb_ = gfx::BufferHandle::Invalid;

        if (rs_.HasDeviceAssetManagers()) {
            rs_.Textures().ReleaseOwned(kIblFromProbeName);
            rs_.Textures().ReleaseOwned(kIblToProbeName);
            rs_.Textures().ReleaseOwned(kIblDayProbeName);
            rs_.Textures().ReleaseOwned(kIblNightProbeName);
            rs_.Textures().ReleaseOwned(kIblSplitSumLutName);
        }
        impl_->iblDayNightLoaded_ = false;
    }
    // Flush the PSO trace before tearing the builder down — Save()
    // is a no-op if nothing changed this run. Detach first so the
    // builder's Clear() can't append more keys mid-shutdown.
    if (impl_->blsPsoBuilder_)  impl_->blsPsoBuilder_->SetTrace(nullptr);
    if (impl_->blsPsoTrace_)    impl_->blsPsoTrace_->Save();
    if (impl_->blsPsoBuilder_)  impl_->blsPsoBuilder_->Clear();
    if (impl_->blsPrograms_)    impl_->blsPrograms_->Clear();
    if (impl_->blsShaderCache_) impl_->blsShaderCache_->ReleaseAll();
    impl_->blsPsoBuilder_.reset();
    impl_->blsPsoTrace_.reset();
    impl_->blsPrograms_.reset();
    impl_->blsShaderCache_.reset();
}

RenderTargetId RenderPipeline::CreateSwapChainTarget(void* nativeWindowHandle, i32 w, i32 h) {
    if (!impl_->gfx_) return 0;

    RenderTarget target;
    target.id   = impl_->nextTargetId_++;
    target.swap  = impl_->gfx_->CreateSwapChain(nativeWindowHandle, w, h);
    if (target.swap == gfx::SwapChainHandle::Invalid) return 0;

    target.color       = impl_->gfx_->GetSwapChainBackBuffer(target.swap);
    target.colorLinear = impl_->gfx_->GetSwapChainBackBufferLinear(target.swap);
    target.hdrColor    = impl_->gfx_->CreateColorTarget(w, h, kHdrSceneFormat);
    target.depth       = impl_->gfx_->CreateDepthTarget(w, h, impl_->depthStencilFormat_);
    target.width       = w;
    target.height      = h;

    RenderTargetId id = target.id;
    impl_->targets_[id] = target;
    return id;
}

void RenderPipeline::ResizePrimaryTarget(i32 w, i32 h) {
    auto* target = PrimaryTarget();
    if (!target || !impl_->gfx_) return;

    auto& t = *target;
    impl_->gfx_->Destroy(t.depth);
    impl_->gfx_->Destroy(t.hdrColor);

    if (t.swap != gfx::SwapChainHandle::Invalid) {
        impl_->gfx_->ResizeSwapChain(t.swap, w, h);
        t.color       = impl_->gfx_->GetSwapChainBackBuffer(t.swap);
        t.colorLinear = impl_->gfx_->GetSwapChainBackBufferLinear(t.swap);
    } else {
        impl_->gfx_->Destroy(t.color);
        t.color       = impl_->gfx_->CreateColorTarget(w, h, gfx::Format::R8G8B8A8_UNORM_SRGB);
        t.colorLinear = t.color;
    }

    t.hdrColor = impl_->gfx_->CreateColorTarget(w, h, kHdrSceneFormat);
    t.depth    = impl_->gfx_->CreateDepthTarget(w, h, impl_->depthStencilFormat_);
    t.width    = w;
    t.height   = h;

    impl_->width_  = w;
    impl_->height_ = h;
}

void RenderPipeline::CleanupGFX() {

    ShutdownBlsShaders();

    if (impl_->gfx_) {

        impl_->gfx_->Destroy(impl_->lineVS_);  impl_->gfx_->Destroy(impl_->linePS_);
        impl_->gfx_->Destroy(impl_->linePSOHdr_);
        impl_->gfx_->Destroy(impl_->linePSOSd_);
        impl_->gfx_->Destroy(impl_->tonemapPSO_);
        impl_->gfx_->Destroy(impl_->tonemapVB_);
        impl_->gfx_->Destroy(impl_->tonemapPsCb_);
        impl_->gfx_->Destroy(impl_->tonemapSampler_);
        impl_->tonemapPSO_     = gfx::PipelineHandle::Invalid;
        impl_->tonemapVB_      = gfx::BufferHandle::Invalid;
        impl_->tonemapPsCb_    = gfx::BufferHandle::Invalid;
        impl_->tonemapSampler_ = gfx::SamplerHandle::Invalid;

        impl_->gfx_->Destroy(impl_->cbPerFrame_);

        rs_.Debug().DestroyResources();

        rs_.ResetDeviceAssetManagers();

        impl_->gfx_->Destroy(impl_->particleServiceVB_);
        impl_->particleServiceVB_ = gfx::BufferHandle::Invalid;
        impl_->particleServiceVBSize_ = 0;

        impl_->gfx_->Destroy(impl_->splatServiceVB_);
        impl_->splatServiceVB_ = gfx::BufferHandle::Invalid;
        impl_->splatServiceVBSize_ = 0;

        for (auto& [id, t] : impl_->targets_) {
            if (t.swap != gfx::SwapChainHandle::Invalid)
                impl_->gfx_->DestroySwapChain(t.swap);
            else
                impl_->gfx_->Destroy(t.color);
            impl_->gfx_->Destroy(t.hdrColor);
            impl_->gfx_->Destroy(t.depth);
        }
    }
    impl_->targets_.clear();
    impl_->primaryTargetId_ = 0;

    impl_->gfx_.reset();
}

bool RenderPipeline::CreateShaders() {
    using namespace whiteout::flakes::Shaders;

    // Vulkan consumes the SPIR-V variant emitted alongside DXBC; D3D11
    // and D3D12 keep using the DXBC blob (sm_5_0 is accepted by both).
    const bool vk = impl_->gfx_->GetApi() == gfx::GfxApi::Vulkan;
    const u8*  vsBytes = vk ? kLineVSSpv : kLineVS;
    usize      vsSize  = vk ? sizeof(kLineVSSpv) : sizeof(kLineVS);
    const u8*  psBytes = vk ? kLinePSSpv : kLinePS;
    usize      psSize  = vk ? sizeof(kLinePSSpv) : sizeof(kLinePS);

    impl_->lineVS_ = impl_->gfx_->CreateShader(gfx::ShaderStage::Vertex, vsBytes, vsSize);
    impl_->linePS_ = impl_->gfx_->CreateShader(gfx::ShaderStage::Pixel,  psBytes, psSize);

    return impl_->lineVS_ != gfx::ShaderHandle::Invalid
        && impl_->linePS_ != gfx::ShaderHandle::Invalid;
}

bool RenderPipeline::CreatePipelines() {
    using namespace gfx;

    InputElement lineInput[] = {
        {"POSITION", 0, Format::R32G32B32_FLOAT,    0},
        {"COLOR",    0, Format::R32G32B32A32_FLOAT, 12},
    };

    GraphicsPipelineDesc desc;
    desc.vs          = impl_->lineVS_;
    desc.ps          = impl_->linePS_;
    desc.inputLayout = lineInput;
    desc.topology    = PrimitiveTopology::LineList;
    desc.blend.enable = false;
    desc.depthStencil = {};
    desc.rasterizer.cull     = CullMode::None;
    desc.rasterizer.frontCCW = true;

    desc.dsvFormat = impl_->depthStencilFormat_;
    desc.rtvFormat = kHdrSceneFormat;
    impl_->linePSOHdr_    = impl_->gfx_->CreateGraphicsPipeline(desc);

    desc.rtvFormat = kSdSceneFormat;
    impl_->linePSOSd_     = impl_->gfx_->CreateGraphicsPipeline(desc);

    return impl_->linePSOHdr_ != PipelineHandle::Invalid
        && impl_->linePSOSd_  != PipelineHandle::Invalid;
}

gfx::PipelineHandle RenderPipeline::CurrentLinePSO() const {
    return impl_->frameRenderMode_ == RenderMode::HD ? impl_->linePSOHdr_ : impl_->linePSOSd_;
}

bool RenderPipeline::CreateDefaultResources() {
    return rs_.Debug().CreateResources();
}

void RenderPipeline::RenderFrame(RenderTargetId targetId) {
    WDX_CPU_ZONE("RenderFrame");
    // Stutter detector: at the start of each frame, snapshot the
    // PSO builder's runtime cache-miss counter. If it changes during
    // this frame, the driver synchronously compiled at least one PSO
    // mid-frame — the canonical stutter source. Print the delta with
    // the frame index so a transient hitch is greppable in stderr.
    static thread_local u64 frameCounter      = 0;
    const u64 builderBuildsAtFrameStart = impl_->blsPsoBuilder_
        ? impl_->blsPsoBuilder_->Stats().runtimeCacheBuilds : 0;

    auto it = impl_->targets_.find(targetId);
    if (it == impl_->targets_.end()) return;
    auto& target = it->second;
    if (target.color    == gfx::TextureHandle::Invalid || !impl_->gfx_) return;
    if (target.hdrColor == gfx::TextureHandle::Invalid)         return;

    // Commit any pending GPU uploads (textures + geometry the loader staged
    // since the last frame) before we start rendering this frame.
    {
        WDX_CPU_ZONE("CommitUploads");
        rs_.Loader().CommitPendingUploads();
    }

    // Apply any pending settings changes before drawing.
    if (rs_.Settings().ConsumeIblModeDirty())
        ApplyIblMode(rs_.Settings().GetIblMode());

    auto* cmd = impl_->gfx_->GetImmediateContext();

    // Snapshot the render mode for the duration of this frame. The
    // host may flip Settings().SetRenderMode() from another thread
    // (test_main.cpp does this once per model load), and every
    // per-frame decision below must agree on the value — otherwise
    // the scene pass attaches the SRGB swap chain while CurrentLinePSO
    // hands out the HDR-rtv line PSO. See `Impl::frameRenderMode_`.
    impl_->frameRenderMode_ = rs_.Settings().GetRenderMode();
    const bool useHdr = (impl_->frameRenderMode_ == RenderMode::HD);
    const gfx::TextureHandle sceneTarget = useHdr ? target.hdrColor : target.color;

    auto srgbByteToLinear = [](u8 b) {
        const f32 f = b / 255.0f;
        return (f <= 0.04045f) ? (f / 12.92f)
                               : std::pow((f + 0.055f) / 1.055f, 2.4f);
    };

    auto acesInverse = [](f32 y) {
        if (y <= 0.0f) return 0.0f;
        if (y >= 1.0f) y = 0.9999f;
        const f32 A = 2.43f * y - 2.51f;
        const f32 B = 0.59f * y - 0.03f;
        const f32 C = 0.14f * y;
        if (std::abs(A) < 1e-6f) {
            return (std::abs(B) > 1e-6f) ? -C / B : 0.0f;
        }
        const f32 disc = B * B - 4.0f * A * C;
        if (disc < 0.0f) return 0.0f;
        const f32 r = std::sqrt(disc);
        const f32 x = (-B - r) / (2.0f * A);
        return x > 0.0f ? x : 0.0f;
    };
    const u32 bg = rs_.Settings().BackgroundColorRaw();
    const u8  rB = static_cast<u8>(bg        & 0xFF);
    const u8  gB = static_cast<u8>((bg >> 8) & 0xFF);
    const u8  bB = static_cast<u8>((bg >> 16) & 0xFF);
    auto hdrClear = [&](u8 byte) {
        const f32 linTarget = srgbByteToLinear(byte);
        const f32 exposure  = rs_.Settings().GetTonemapExposure();
        const f32 invExp    = (exposure > 1e-6f) ? 1.0f / exposure : 1.0f;
        return acesInverse(linTarget) * invExp;
    };

    auto sdClear = [&](u8 byte) {
        return srgbByteToLinear(byte);
    };
    f32 clearColor[4] = {
        useHdr ? hdrClear(rB) : sdClear(rB),
        useHdr ? hdrClear(gB) : sdClear(gB),
        useHdr ? hdrClear(bB) : sdClear(bB),
        1.0f,
    };

    if (rs_.GetShadowService() && rs_.GetShadowService()->IsEnabled()) {
        WDX_CPU_ZONE("ShadowPass");
        WDX_GPU_ZONE(cmd, "ShadowPass");
        Matrix44f csmCamView, csmCamProj;
        {
            const f32 aspect= (target.height > 0)
                ? (f32)target.width / (f32)target.height : 1.0f;
            csmCamView = rs_.Scene().Camera().ViewLH();
            csmCamProj = rs_.Scene().Camera().ProjectionLH(aspect);
        }

        Vector3f lightDirWS = { -0.3f, 0.5f, -0.7f };
        if (auto* dnc = rs_.GetDncService(); dnc && dnc->HasAsset()) {
            const auto sample = dnc->SampleNow();
            if (sample.valid) lightDirWS = sample.worldDir;
        }

        // Center the shadow cascade on the camera target (the host's "what
        // they're looking at"). Multi-actor scenes can have no single hero,
        // so we don't probe a focus actor here — orbital target is the most
        // useful proxy and the host can move it explicitly if needed.
        const Vector3f camTarget = rs_.Scene().Camera().GetTarget();
        Vector3f sceneCenter = { camTarget.x, camTarget.y, camTarget.z };
        f32      sceneRadius = 150.0f;
        if (rs_.Scene().Camera().GetMode() == Camera::Mode::Orbital) {
            sceneRadius = std::max(50.0f, rs_.Scene().Camera().GetDistance() * 0.6f);
        }
        rs_.GetShadowService()->Update(csmCamView, csmCamProj,
                               rs_.Scene().Camera().GetNearZ(),
                               rs_.Scene().Camera().GetFarZ(),
                               lightDirWS, sceneCenter, sceneRadius);
        shadow::ShadowPass(rs_).Run(*rs_.GetShadowService());
    }

    cmd->BeginRenderPass(sceneTarget, target.depth, clearColor, 1.0f, 0);
    cmd->SetViewport({0, 0, (f32)target.width, (f32)target.height, 0, 1});
    WDX_GPU_ZONE(cmd, "ScenePass");

    Matrix44f view, proj;
    {
        view = rs_.Scene().Camera().GetViewMatrix();
    }
    f32 aspect = (target.height > 0) ? (f32)target.width / (f32)target.height : 1.0f;
    proj = rs_.Scene().Camera().ProjectionRH(aspect);

    {
        render_detail::CbPerFrameDesc d;
        d.view         = view;
        d.projection   = proj;
        d.lightDir     = render_detail::NormalizedLightDir4(kDefaultLightDir);
        d.lightColor   = kGeosetLightColor;
        d.ambientColor = {kGeosetAmbientColor.x, kGeosetAmbientColor.y, kGeosetAmbientColor.z, 0.0f};
        render_detail::WriteCbPerFrame(impl_->gfx_.get(), impl_->cbPerFrame_, d);
    }

    if (rs_.Settings().ShowGrid()) {
        WDX_GPU_ZONE(cmd, "Grid");
        rs_.Debug().RenderGrid();
    }

    { WDX_CPU_ZONE("GeosetsOpaque");
      WDX_GPU_ZONE(cmd, "GeosetsOpaque");
      RenderGeosets(GeosetBucket::Opaque); }
    if (rs_.Settings().ShowEvents()) {
      WDX_CPU_ZONE("Splats");
      WDX_GPU_ZONE(cmd, "Splats");
      RenderSplatsBls();
    }
    { WDX_CPU_ZONE("GeosetsTransparent");
      WDX_GPU_ZONE(cmd, "GeosetsTransparent");
      RenderGeosets(GeosetBucket::Transparent); }
    if (rs_.Settings().ShowParticles()) {
      WDX_CPU_ZONE("Particles");
      WDX_GPU_ZONE(cmd, "Particles");
      RenderParticlesBls();
    }
    if (rs_.Settings().ShowParticles()) {
      WDX_CPU_ZONE("CornEffects");
      WDX_GPU_ZONE(cmd, "CornEffects");
      RenderCornEffects();
    }
    if (rs_.Settings().ShowRibbons()) {
      WDX_CPU_ZONE("Ribbons");
      WDX_GPU_ZONE(cmd, "Ribbons");
      RenderRibbons();
    }
    if (rs_.Settings().ShowCollisions()) rs_.Debug().RenderCollisions();
    if (rs_.Settings().ShowLights())     rs_.Debug().RenderLightMarkers();
    rs_.Debug().RenderViewCube();
    cmd->EndRenderPass();

    if (useHdr) {
        WDX_CPU_ZONE("Tonemap");
        WDX_GPU_ZONE(cmd, "Tonemap");
        RunTonemapPass(target);
    }

    ++frameCounter;
    if (impl_->blsPsoBuilder_) {
        const u64 builderBuildsAtFrameEnd = impl_->blsPsoBuilder_->Stats().runtimeCacheBuilds;
        const u64 delta = builderBuildsAtFrameEnd - builderBuildsAtFrameStart;
        if (delta > 0) {
            std::fprintf(stderr,
                "[bls] frame %llu: %llu mid-frame PSO compile(s) "
                "(every one is a synchronous driver stall — stutter source)\n",
                (unsigned long long)frameCounter,
                (unsigned long long)delta);
        }
    }
}

void RenderPipeline::RunTonemapPass(const RenderTarget& target) {
    if (impl_->tonemapPSO_ == gfx::PipelineHandle::Invalid) return;
    auto* cmd = impl_->gfx_->GetImmediateContext();

    const f32 clearLdr[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    cmd->BeginRenderPass(target.color, gfx::TextureHandle::Invalid,
                         clearLdr, 1.0f, 0);
    cmd->SetViewport({0, 0, (f32)target.width, (f32)target.height, 0, 1});

    if (impl_->tonemapPsCb_ != gfx::BufferHandle::Invalid) {
        if (void* mapped = impl_->gfx_->MapBuffer(impl_->tonemapPsCb_)) {
            f32 cb[4] = {rs_.Settings().GetTonemapExposure(), 0.0f, 0.0f, 0.0f};
            std::memcpy(mapped, cb, sizeof(cb));
            impl_->gfx_->UnmapBuffer(impl_->tonemapPsCb_);
        }
    }

    cmd->BindPipeline(impl_->tonemapPSO_);
    cmd->BindVertexBuffer(0, impl_->tonemapVB_, sizeof(f32) * 5);
    cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, target.hdrColor);
    cmd->BindSampler       (gfx::ShaderStage::Pixel, 0, impl_->tonemapSampler_);
    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 1, impl_->tonemapPsCb_);
    cmd->Draw(3, 0);
    cmd->EndRenderPass();
}

void RenderPipeline::Present(RenderTargetId targetId) {
    auto it = impl_->targets_.find(targetId);
    if (it != impl_->targets_.end() && it->second.swap != gfx::SwapChainHandle::Invalid)
        impl_->gfx_->Present(it->second.swap);
}

class GeosetPassBls : public BlsGeosetPass<GeosetPassBls> {
public:
    using BlsGeosetPass::BlsGeosetPass;

    bool IsAvailable() const {
        return rs_.Pipeline().impl_->blsSdProgram_ && rs_.Pipeline().impl_->blsPsoBuilder_;
    }

    void ComputeViewProj(Matrix44f& view, Matrix44f& proj) const {
        view = rs_.Scene().Camera().GetViewMatrix();
        const f32 aspect= (rs_.Pipeline().Height() > 0)
            ? static_cast<f32>(rs_.Pipeline().Width()) / static_cast<f32>(rs_.Pipeline().Height()) : 1.0f;
        proj = rs_.Scene().Camera().ProjectionRH(aspect);
    }

    void BindPassResources(gfx::IGFXCommandList*, bls::FrameInputs&) const {

    }

    bls::BaselineLights Baseline(const Matrix44f& view) const {

        if (auto* dnc = rs_.GetDncService();
            dnc && dnc->HasAsset() &&
            rs_.Settings().GetLightingMode() == LightingMode::InGame) {
            const auto sample = dnc->SampleNow();
            if (sample.valid) {

                const Vector3f dirVS = whiteout::transform_normal(
                    Vector3f{ -sample.worldDir.x, -sample.worldDir.y, -sample.worldDir.z },
                    view);
                return { sample.ambient, sample.diffuse, dirVS };
            }
        }

        return {
                    { kGeosetAmbientColor.x, kGeosetAmbientColor.y, kGeosetAmbientColor.z },
                    { kGeosetLightColor.x,   kGeosetLightColor.y,   kGeosetLightColor.z },
              { 0.0f, 0.0f, 1.0f },
        };
    }

    void DrawGeoset(const render_detail::GeosetRef& ref,
                    bls::FrameInputs&               frame,
                    const Matrix44f&                 ,
                    gfx::IGFXCommandList*           cmd,
                    i32                             lightCountForGeoset) {
        const auto& view_ = *ref.view;
        const auto& geo   = (*view_.geosets)[ref.idx];

        const GPUMaterial* mat = nullptr;
        const i32 matId= geo.materialId;
        if (matId >= 0 && matId < (i32)view_.materials->size())
            mat = &(*view_.materials)[matId];

        const f32 geoAlpha= geo.geosetAlpha * view_.parentVisibility;

        if (geoAlpha <= 0.0f) return;

        i32 numLayers = mat ? (i32)mat->cpu.layers.size() : 0;
        if (numLayers <= 0) numLayers = 1;

        // Pick the bone palette CB the actor actually owns: per-actor
        // on Path A (allocated on the SkinningSystem), per-geoset on
        // Path B (the original layout). Without this branch the SD
        // path looked at `geo.bonePaletteCb` only — which is Invalid
        // for Path A actors — and incorrectly fell back to the non-
        // skinned VS permutation, rendering at bind pose. Caught on
        // Oldblood_hero.mdx via RenderDoc.
        gfx::BufferHandle paletteCb = geo.bonePaletteCb;
        if (view_.skinning && view_.skinning->UsesPerActorPalette()) {
            paletteCb = view_.skinning->ActorPaletteCb();
        }
        const bool hasBones = render_detail::BindSdMeshGeometry(cmd, geo, paletteCb);

        const auto layout = hasBones
            ? bls::VertexLayoutKind::ParticleSDSkinned
            : bls::VertexLayoutKind::ParticleSD;

        struct LayerJob {
            render_detail::UnpackedLayer layer;
            bls::MatParams               mp;
            i32                          activeN = 0;
            bool                         unlit   = false;
            bool                         isOpaqueFading = false;
            bool                         valid   = false;
        };
        std::vector<LayerJob> jobs(numLayers);

        for (i32 li= 0; li < numLayers; ++li) {
            jobs[li].layer = render_detail::UnpackLayer(mat, li);
            const auto& layer = jobs[li].layer;
            const f32 combinedAlpha = geoAlpha * layer.alpha;
            if (combinedAlpha < 0.004f) continue;

            const bool isOpaqueFading =
                combinedAlpha < 0.99f && layer.filterMode <= FILTER_TRANSPARENT;
            i32 effectiveFilter = layer.filterMode;
            if (isOpaqueFading)
                effectiveFilter = FILTER_BLEND;

            bls::MatParams mp = bls::FromMdxLayer(effectiveFilter, layer.flags, bls::GxShaderID::SD);
            if (mp.alpha == bls::GxMatAlpha::Modulate) {
                mp.diffuseColor = {combinedAlpha, 1, 1, 1};
            } else {
                mp.diffuseColor = {geo.geosetColor.x, geo.geosetColor.y, geo.geosetColor.z, combinedAlpha};
            }
            const bool unlit = (mp.disables & bls::kDisableLighting) != 0;
            const i32 activeN = unlit ? 0 : lightCountForGeoset;

            jobs[li].mp = mp;
            jobs[li].activeN = activeN;
            jobs[li].unlit   = unlit;
            jobs[li].isOpaqueFading = isOpaqueFading;
            jobs[li].valid   = true;
        }

        auto issueDraw = [&](const LayerJob& job, const bls::MatParams& matParams) {
            frame.numLights = job.activeN;
            render_detail::ApplyTexAnimPaletteToFrame(frame, view_.texAnimPalette, job.layer.textureAnimationId);

            const auto rsLocal   = bls::MakeSdMeshRenderState(matParams, job.activeN, job.unlit, hasBones);
            const auto permLocal = bls::SelectPermutes(rsLocal);
            auto reqLocal        = bls::MakePsoRequest(rs_.Pipeline().impl_->blsSdProgram_,
                                                       layout,
                                                       matParams, permLocal);

            reqLocal.rtvFormat = rs_.Pipeline().SceneTargetFormat();
            reqLocal.dsvFormat = rs_.Pipeline().impl_->depthStencilFormat_;
            auto pso = rs_.Pipeline().impl_->blsPsoBuilder_->GetOrBuild(reqLocal);
            if (pso == gfx::PipelineHandle::Invalid) return;
            cmd->BindPipeline(pso);

            cmd->BindVertexBuffer(0,
                render_detail::PickSlot0Vb(geo, job.layer.coordId),
                sizeof(Vertex));

            frame.world = view_.worldTransform;

            if (auto vs = bls::ScopedCb<bls::SdVsCbA>(rs_.Pipeline().Gfx(), rs_.Pipeline().impl_->blsSdVsCb_)) {
                bls::BuildSdVsCbA(*vs, frame, matParams);
            }
            if (auto ps = bls::ScopedCb<bls::SdPsCbA>(rs_.Pipeline().Gfx(), rs_.Pipeline().impl_->blsSdPsCb_)) {
                bls::BuildSdPsCbA(*ps, frame, matParams);
            }
            cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, rs_.Pipeline().impl_->blsSdVsCb_);
            cmd->BindConstantBuffer(gfx::ShaderStage::Pixel,  0, rs_.Pipeline().impl_->blsSdPsCb_);

            render_detail::BindLayerAlbedo(cmd, view_.textures, job.layer.textureId,
                                           rs_.Textures().GetDefaults().White,
                                           rs_.Samplers());

            cmd->DrawIndexed(geo.indexCount);
        };

        for (i32 li= 0; li < numLayers; ++li) {
            if (!jobs[li].valid || !jobs[li].isOpaqueFading) continue;
            bls::MatParams prepass = jobs[li].mp;
            prepass.diffuseColor = {1.0f, 1.0f, 1.0f, 1.0f};
            prepass.disables &= ~bls::kDisableDepthWrite;
            prepass.disables |=  bls::kDisableBit8;
            issueDraw(jobs[li], prepass);
        }

        for (i32 li= 0; li < numLayers; ++li) {
            if (!jobs[li].valid) continue;
            issueDraw(jobs[li], jobs[li].mp);
        }
    }
};

bool RenderPipeline::RenderGeosetsBls(GeosetBucket bucket) {
    return GeosetPassBls{rs_, bucket}.Run();
}

class GeosetPassHd : public BlsGeosetPass<GeosetPassHd> {
public:
    using BlsGeosetPass::BlsGeosetPass;

    bool IsAvailable() const {
        return rs_.Pipeline().impl_->blsHdProgram_ && rs_.Pipeline().impl_->blsPsoBuilder_;
    }

    void ComputeViewProj(Matrix44f& view, Matrix44f& proj) const {

        const f32 aspect= (rs_.Pipeline().Height() > 0)
            ? static_cast<f32>(rs_.Pipeline().Width()) / static_cast<f32>(rs_.Pipeline().Height()) : 1.0f;
        view = rs_.Scene().Camera().ViewLH();
        proj = rs_.Scene().Camera().ProjectionLH(aspect);
    }

    void BindPassResources(gfx::IGFXCommandList* cmd, bls::FrameInputs& frame) {

        const bool useDayNight = rs_.Pipeline().impl_->iblDayNightLoaded_
                              && rs_.GetDncService() != nullptr
                              && rs_.Settings().GetLightingMode() == LightingMode::InGame;
        if (useDayNight) {
            const auto blend = rs_.GetDncService()->ComputeEnvMapBlend();

            const bool dayPrimary = blend.isDaytime;
            frame.envFromMipEnd  = dayPrimary ? rs_.Pipeline().impl_->iblDayMipEnd_   : rs_.Pipeline().impl_->iblNightMipEnd_;
            frame.envToMipEnd    = dayPrimary ? rs_.Pipeline().impl_->iblNightMipEnd_ : rs_.Pipeline().impl_->iblDayMipEnd_;
            frame.envTransitionT = blend.transitionT;
        } else {
            frame.envFromMipEnd  = rs_.Pipeline().impl_->iblProbeMipEnd_;
            frame.envToMipEnd    = rs_.Pipeline().impl_->iblProbeMipEnd_;
            frame.envTransitionT = 0.75f;
        }

        const gfx::SamplerHandle linWrap = rs_.Samplers().LinearWrap();
        cmd->BindSampler(gfx::ShaderStage::Pixel, 1,  linWrap);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 2,  linWrap);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 3,  linWrap);
        // The HD pixel shader also samples s_teamColor (s4) and the IBL
        // probes / BRDF LUT at s13..s15. They were never bound explicitly
        // — d3d11/d3d12 forgive the missing slots silently, but Vulkan
        // fires VUID-vkCmdDrawIndexed-None-08114 the first time a draw
        // touches the unbound descriptor.
        cmd->BindSampler(gfx::ShaderStage::Pixel, 4,  linWrap);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 13, linWrap);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 14, linWrap);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 15, linWrap);

        gfx::TextureHandle from = gfx::TextureHandle::Invalid;
        gfx::TextureHandle to   = gfx::TextureHandle::Invalid;
        if (useDayNight) {
            const auto day   = rs_.Textures().GetOwned(RenderPipeline::kIblDayProbeName);
            const auto night = rs_.Textures().GetOwned(RenderPipeline::kIblNightProbeName);
            const auto blend = rs_.GetDncService()->ComputeEnvMapBlend();
            from = blend.isDaytime ? day   : night;
            to   = blend.isDaytime ? night : day;
        } else {
            from = rs_.Textures().GetOwned(RenderPipeline::kIblFromProbeName);
            to   = rs_.Textures().GetOwned(RenderPipeline::kIblToProbeName);
            if (to == gfx::TextureHandle::Invalid) to = from;
        }
        if (from != gfx::TextureHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 13, from);
        if (to   != gfx::TextureHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 14, to);
        const gfx::TextureHandle lut = rs_.Textures().GetOwned(
            RenderPipeline::kIblSplitSumLutName);
        if (lut != gfx::TextureHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 15, lut);

        if (auto* shadowSvc = rs_.GetShadowService()) {
            for (i32 c= 0; c < 3; ++c) {
                const gfx::TextureHandle sh = shadowSvc->depthTarget(c);
                if (sh != gfx::TextureHandle::Invalid) {
                    cmd->BindShaderResource(gfx::ShaderStage::Pixel,
                                            10 + static_cast<u32>(c), sh);
                }
            }
        }
    }

    bls::BaselineLights Baseline(const Matrix44f& view) const {

        if (auto* dnc = rs_.GetDncService();
            dnc && dnc->HasAsset() &&
            rs_.Settings().GetLightingMode() == LightingMode::InGame) {
            const auto sample = dnc->SampleNow();
            if (sample.valid) {
                const Vector3f dirVS = whiteout::transform_normal(
                    Vector3f{ -sample.worldDir.x, -sample.worldDir.y, -sample.worldDir.z },
                    view);
                return { sample.ambient, sample.diffuse, dirVS };
            }
        }

        return {
                    { kHdBaselineAmbientColor.x, kHdBaselineAmbientColor.y, kHdBaselineAmbientColor.z },
                    { kHdBaselineLightColor.x,   kHdBaselineLightColor.y,   kHdBaselineLightColor.z },
              { 0.0f, 0.0f, -1.0f },
        };
    }

    void DrawGeoset(const render_detail::GeosetRef& ref,
                    bls::FrameInputs&               frame,
                    const Matrix44f&                 ,
                    gfx::IGFXCommandList*           cmd,
                    i32                             lightCountForGeoset) {
        const auto& view_ = *ref.view;
        const auto& geo   = (*view_.geosets)[ref.idx];

        const GPUMaterial* mat = nullptr;
        const i32 matId= geo.materialId;
        if (matId >= 0 && matId < (i32)view_.materials->size())
            mat = &(*view_.materials)[matId];

        const f32 geoAlpha= geo.geosetAlpha * view_.parentVisibility;

        if (geoAlpha <= 0.0f) return;

        i32 numLayers = mat ? (i32)mat->cpu.layers.size() : 0;
        if (numLayers <= 0) numLayers = 1;

        cmd->BindIndexBuffer(geo.ib, gfx::Format::R32_UINT);

        const bool hasTangents = (geo.tangentVb != gfx::BufferHandle::Invalid);
        if (hasTangents)
            cmd->BindVertexBuffer(1, geo.tangentVb, sizeof(Vector4f));

        // Bone palette CB: prefer the per-actor handle (Path A) when
        // the actor's SkinningSystem owns it; otherwise fall back to
        // the per-geoset CB (Path B). The vertex buffer's boneIdx
        // values were rewritten at load time to match whichever path
        // this actor sits on, so the shader code doesn't change.
        gfx::BufferHandle paletteCb = geo.bonePaletteCb;
        if (view_.skinning && view_.skinning->UsesPerActorPalette()) {
            paletteCb = view_.skinning->ActorPaletteCb();
        }
        const bool hasBones =
            (geo.boneVb != gfx::BufferHandle::Invalid) &&
            (paletteCb  != gfx::BufferHandle::Invalid);
        if (hasBones) {
            const u32 boneSlot = hasTangents ? 2u : 1u;
            cmd->BindVertexBuffer(boneSlot, geo.boneVb, sizeof(BoneVertex));
            cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 3, paletteCb);
        }

        struct LayerJob {
            render_detail::UnpackedLayer layer;
            bls::MatParams               mp;
            const bls::BlsProgram*       program = nullptr;
            bls::GxShaderID              programShaderId = bls::GxShaderID::SD_on_HD;
            i32                          activeN = 0;
            bool                         unlit   = false;
            bool                         isOpaqueFading = false;
            bool                         valid   = false;
        };
        std::vector<LayerJob> jobs(numLayers);

        for (i32 li= 0; li < numLayers; ++li) {
            jobs[li].layer = render_detail::UnpackLayer(mat, li);
            const auto& layer = jobs[li].layer;
            f32 combinedAlpha = geoAlpha * layer.alpha;
            if (combinedAlpha < 0.004f) continue;
            const bool isOpaqueFading =
                combinedAlpha < 0.99f && layer.filterMode <= FILTER_TRANSPARENT;
            i32 effectiveFilter = layer.filterMode;
            if (isOpaqueFading)
                effectiveFilter = FILTER_BLEND;

            const bool isCrystalMaterial = (layer.shaderId == 24)
                                        && rs_.Pipeline().impl_->blsCrystalProgram_ != nullptr;
            const bool isHdMaterial =
                isCrystalMaterial
                || layer.shaderId == 1
                || layer.shaderId == 24;
            const bls::GxShaderID programShaderId =
                isCrystalMaterial ? bls::GxShaderID::Crystal
                : isHdMaterial    ? bls::GxShaderID::HD
                                  : bls::GxShaderID::SD_on_HD;
            const bls::BlsProgram* program =
                isCrystalMaterial ? rs_.Pipeline().impl_->blsCrystalProgram_
                : isHdMaterial    ? rs_.Pipeline().impl_->blsHdProgram_
                                  : rs_.Pipeline().impl_->blsSdOnHdProgram_;

            bls::MatParams mp = bls::FromMdxLayer(effectiveFilter, layer.flags, programShaderId);
            if (mp.alpha == bls::GxMatAlpha::Modulate) {
                mp.diffuseColor = {combinedAlpha, 1, 1, 1};
            } else {
                mp.diffuseColor = {geo.geosetColor.x, geo.geosetColor.y, geo.geosetColor.z, combinedAlpha};
            }
            mp.emissiveGain     = layer.emissiveGain;
            mp.fresnelTeamColor = layer.fresnelTeamColor;
            mp.fresnelOpacity   = layer.fresnelOpacity;
            mp.fresnelColor     = layer.fresnelColor;

            const bool unlit   = (mp.disables & bls::kDisableLighting) != 0;
            const i32 activeN = unlit ? 0 : lightCountForGeoset;

            jobs[li].mp = mp;
            jobs[li].program = program;
            jobs[li].programShaderId = programShaderId;
            jobs[li].activeN = activeN;
            jobs[li].unlit   = unlit;
            jobs[li].isOpaqueFading = isOpaqueFading;
            jobs[li].valid   = true;
        }

        auto issueHdDraw = [&](const LayerJob& job, const bls::MatParams& matParams) {
            const auto& layer = job.layer;
            const bool unlit  = job.unlit;
            const i32 activeN = job.activeN;
            const bls::GxShaderID programShaderId = job.programShaderId;
            const bls::BlsProgram* program = job.program;
            frame.numLights = activeN;
            render_detail::ApplyTexAnimPaletteToFrame(frame, view_.texAnimPalette, layer.textureAnimationId);
            {
            bls::RenderState rs;
            rs.shaderId       = programShaderId;
            rs.alphaMode      = static_cast<u8>(matParams.alpha);
            rs.numColors      = 0;
            rs.numTexCoords   = 1;

            rs.numTangents    = hasTangents ? 1 : 0;

            rs.numWeights     = hasBones ? 4 : 0;
            rs.numLights      = static_cast<u8>(activeN);
            rs.fogEnabled     = false;
            rs.depthWrite     = matParams.DepthWriteEnabled();
            rs.lightingEnabled= !unlit && activeN > 0;
            rs.prepass        = false;
            rs.shadows        = rs_.GetShadowService() && rs_.GetShadowService()->IsEnabled();

            rs.teamColor      = (layer.teamColorMapId == kHdTeamColorActive)
                             || (layer.teamColorMapId >= 0);
            const i32 dbgMode     = rs_.Settings().HdDebugMode();
            const bool debugActive = (dbgMode > 0);
            rs.debugShader = debugActive;
            auto perm = bls::SelectPermutes(rs);

            bls::PsoRequest req{};
            req.program   = program;
            req.vsIndex   = perm.vs;
            req.psIndex   = perm.ps;
            req.material  = matParams;

            if (hasBones) {
                req.layout = hasTangents
                    ? bls::VertexLayoutKind::MeshHDSkinned
                    : bls::VertexLayoutKind::MeshHDSkinnedNoTangent;
            } else {
                req.layout = hasTangents
                    ? bls::VertexLayoutKind::MeshHDTangent
                    : bls::VertexLayoutKind::ParticleSD;
            }
            req.topology  = gfx::PrimitiveTopology::TriangleList;
            req.rtvFormat = RenderPipeline::kHdrSceneFormat;
            req.dsvFormat = rs_.Pipeline().impl_->depthStencilFormat_;
            req.lhClipSpace = true;
            auto pso = rs_.Pipeline().impl_->blsPsoBuilder_->GetOrBuild(req);
            if (pso == gfx::PipelineHandle::Invalid) return;
            cmd->BindPipeline(pso);

            cmd->BindVertexBuffer(0,
                render_detail::PickSlot0Vb(geo, layer.coordId),
                sizeof(Vertex));

            frame.world = view_.worldTransform;

            if (auto vs = bls::ScopedCb<bls::HdVsCb>(rs_.Pipeline().Gfx(), rs_.Pipeline().impl_->blsHdVsCb_)) {
                bls::BuildHdVsCb(*vs, frame, matParams);
            }
            cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 2, rs_.Pipeline().impl_->blsHdVsCb_);

            if (rs_.GetShadowService() && rs_.Pipeline().impl_->blsHdShadowCb_ != gfx::BufferHandle::Invalid) {
                if (auto sc = bls::ScopedCb<bls::HdShadowCascadesCb>(rs_.Pipeline().Gfx(),
                                                                      rs_.Pipeline().impl_->blsHdShadowCb_)) {
                    rs_.GetShadowService()->FillVsCb(*sc);
                }
                cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 1, rs_.Pipeline().impl_->blsHdShadowCb_);
            }

            if (program == rs_.Pipeline().impl_->blsHdProgram_ ||
                program == rs_.Pipeline().impl_->blsCrystalProgram_) {
                if (auto ps = bls::ScopedCb<bls::HdPsCb>(rs_.Pipeline().Gfx(), rs_.Pipeline().impl_->blsHdPsCb_)) {
                    bls::BuildHdPsCb(*ps, frame, matParams);
                }
                cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 2, rs_.Pipeline().impl_->blsHdPsCb_);

                if (auto dbg = bls::ScopedCb<bls::DebugVisCb>(rs_.Pipeline().Gfx(), rs_.Pipeline().impl_->blsHdDebugVisCb_)) {

                    u32 enabled   = 0;
                    i32      psMode    = dbgMode;
                    Vector3f overrideA = {0, 0, 0};
                    if (dbgMode >= 5 && dbgMode <= 7) {
                        enabled = 1;
                        psMode  = 0;
                        overrideA = (dbgMode == 5) ? Vector3f{1, 1, 1}
                                   : (dbgMode == 6) ? Vector3f{0.5f, 0.5f, 0.5f}
                                                    : Vector3f{0, 0, 0};
                    }
                    dbg->enabledShaders = enabled;

                    const u32 modeBits = static_cast<u32>(psMode);
                    std::memcpy(&dbg->debugMode, &modeBits, sizeof(f32));
                    dbg->_p0[0] = dbg->_p0[1] = 0.0f;
                    dbg->overrideAlbedo = overrideA; dbg->_p1 = 0.0f;
                    dbg->overrideOrm    = {0, 0, 0}; dbg->_p2 = 0.0f;
                }
                cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 3, rs_.Pipeline().impl_->blsHdDebugVisCb_);
            } else {
                if (auto ps = bls::ScopedCb<bls::SdOnHdPsCb>(rs_.Pipeline().Gfx(), rs_.Pipeline().impl_->blsSdOnHdPsCb_)) {
                    bls::BuildSdOnHdPsCb(*ps, frame, matParams);
                }
                cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 2, rs_.Pipeline().impl_->blsSdOnHdPsCb_);
            }

            if (rs_.Pipeline().impl_->blsHdShadowCountCb_ != gfx::BufferHandle::Invalid) {
                if (auto cnt = bls::ScopedCb<bls::SdOnHdShadowCascadeCountCb>(
                        rs_.Pipeline().Gfx(), rs_.Pipeline().impl_->blsHdShadowCountCb_)) {
                    const i32 n = (rs_.GetShadowService() && rs_.GetShadowService()->IsEnabled())
                                      ? rs_.GetShadowService()->cascadeCount()
                                      : 0;
                    const u32 bits = static_cast<u32>(n);
                    std::memcpy(&cnt->numCascades, &bits, sizeof(f32));
                    cnt->_pad[0] = cnt->_pad[1] = cnt->_pad[2] = 0.0f;
                }
                cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 1, rs_.Pipeline().impl_->blsHdShadowCountCb_);
            }

            auto bindMaterialTex = [&](u32 slot, i32 texId,
                                        gfx::TextureHandle fallback,
                                        u32* outWrap) {
                if (texId >= 0 && view_.textures) {
                    const gfx::TextureHandle h = view_.textures->Get(texId);
                    if (h != gfx::TextureHandle::Invalid) {
                        cmd->BindShaderResource(gfx::ShaderStage::Pixel, slot, h);
                        if (outWrap) *outWrap = view_.textures->WrapFlags(texId) & kSamplerWrapBitsMask;
                        return true;
                    }
                }
                cmd->BindShaderResource(gfx::ShaderStage::Pixel, slot, fallback);
                return false;
            };

            const auto& defs = rs_.Textures().GetDefaults();
            u32 wrapFlags = kSamplerWrapBitsMask;
            bindMaterialTex(0, layer.textureId,      defs.White,      &wrapFlags);
            bindMaterialTex(1, layer.normalMapId,    defs.FlatNormal, nullptr);
            bindMaterialTex(2, layer.ormMapId,       defs.NeutralOrm, nullptr);
            bindMaterialTex(3, layer.emissiveMapId,  defs.Black,      nullptr);

            if (layer.teamColorMapId == kHdTeamColorActive) {
                const u32 ownerRgba = view_.teamColor | 0xFF000000u;
                cmd->BindShaderResource(gfx::ShaderStage::Pixel, 4,
                                        rs_.Replaceables().GetHdSwatchTextureFor(ownerRgba));
            } else if (layer.teamColorMapId >= 0) {
                bindMaterialTex(4, layer.teamColorMapId, defs.Black, nullptr);
            } else {
                cmd->BindShaderResource(gfx::ShaderStage::Pixel, 4, defs.Black);
            }
            cmd->BindSampler(gfx::ShaderStage::Pixel, 0, rs_.Samplers().WrapVariant(wrapFlags));

            cmd->DrawIndexed(geo.indexCount);
            }
        };

        for (i32 li= 0; li < numLayers; ++li) {
            if (!jobs[li].valid || !jobs[li].isOpaqueFading) continue;
            bls::MatParams prepass = jobs[li].mp;
            prepass.diffuseColor = {1.0f, 1.0f, 1.0f, 1.0f};
            prepass.disables &= ~bls::kDisableDepthWrite;
            prepass.disables |=  bls::kDisableBit8;
            issueHdDraw(jobs[li], prepass);
        }

        for (i32 li= 0; li < numLayers; ++li) {
            if (!jobs[li].valid) continue;
            issueHdDraw(jobs[li], jobs[li].mp);
        }
    }
};

bool RenderPipeline::RenderGeosetsHd(GeosetBucket bucket) {
    return GeosetPassHd{rs_, bucket}.Run();
}

void RenderPipeline::RenderGeosets(GeosetBucket bucket) {

    if (impl_->frameRenderMode_ == RenderMode::HD) {
        RenderGeosetsHd(bucket);
    } else {
        RenderGeosetsBls(bucket);
    }
}

}
