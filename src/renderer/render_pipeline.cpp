// ============================================================================
// RenderPipeline — extracted from RenderService.
//
// Owns the GPU device, render targets, line/BLS/tonemap pipelines, IBL probe
// state, and the per-frame submission loop. Reaches back into
// RenderService::Impl directly via friend access for scene-side state
// (actors, particles, splats, samplers, textures, settings) — same pattern
// FrameTicker / ModelLoader use (see render_service_impl.h).
// ============================================================================

#include "bls/bls_cb_layout.h"
#include "bls/bls_draw_helpers.h"
#include "bls/bls_frame.h"
#include "bls/bls_mat_params.h"
#include "bls/layer_material.h"
#include "bls/bls_program.h"
#include "bls/bls_pso_builder.h"
#include "bls/bls_pso_trace.h"
#include "bls/bls_shader_cache.h"
#include "bls/scoped_cb.h"
#include "compiled_shaders.h"
#include "constants.h"
#include "debug/debug_renderer.h"
#include "ibl/env_probe.h"
#include "ibl/split_sum.h"
#include "render_detail.h"
#include "render_pass.h"
#include "renderer/assets/asset_manager.h"
#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/assets/sampler_asset_manager.h"
#include "renderer/assets/texture_asset_manager.h"
#include "whiteout/flakes/event_data.h"
#include "renderer/corn_effects/corn_effects_gfx_backend.h"
#include "renderer/corn_effects/corn_effects_service.h"
#include "renderer/model/model_template.h"
#include "renderer/model/model_template_manager.h"
#include "renderer/perf_zone.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_pipeline_impl.h"
#include "renderer/render_service.h"
#include "renderer/render_service_impl.h"
#include "renderer/scene_manager.h"
#include "shadow/shadow_pass.h"
#include "whiteout/flakes/util/team_glow_data.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>

namespace whiteout::flakes::renderer {

using namespace ::whiteout::flakes::renderer::model;
using namespace ::whiteout::flakes::renderer::effects;

namespace {
// Drop the _SRGB qualifier from a colour format. The SD (classic WC3) path is
// gamma-space: it samples textures raw and writes gamma values, so its RTV must
// be a plain UNORM view (no linear→sRGB encode on write). Returns the format
// unchanged when it has no _SRGB variant.
gfx::Format StripSrgb(gfx::Format f) {
    switch (f) {
    case gfx::Format::R8G8B8A8_UNORM_SRGB:
        return gfx::Format::R8G8B8A8_UNORM;
    case gfx::Format::B8G8R8A8_UNORM_SRGB:
        return gfx::Format::B8G8R8A8_UNORM;
    default:
        return f;
    }
}
} // namespace
using namespace ::whiteout::flakes::renderer::assets;
using namespace ::whiteout::flakes::renderer::particle;
using namespace ::whiteout::flakes::renderer::bls;
using namespace ::whiteout::flakes::renderer::shadow;
using namespace ::whiteout::flakes::renderer::ibl;
using namespace ::whiteout::flakes::renderer::debug;
using namespace ::whiteout::flakes::renderer::render_detail;

RenderPipeline::RenderPipeline(RenderService& rs) : rs_(rs), impl_(std::make_unique<Impl>()) {}

RenderPipeline::~RenderPipeline() = default;

bool RenderPipeline::IsDeviceReady() const {
    return impl_->gfx_ != nullptr;
}
void RenderPipeline::SetPrimaryTarget(RenderTargetId id) {
    impl_->primaryTargetId_ = id;
    // Snap pipeline-wide width_/height_ to whatever the newly-primary
    // target reports. Without this, a host that creates several targets
    // before picking one primary would keep stale dimensions.
    if (auto it = impl_->targets_.find(id); it != impl_->targets_.end()) {
        impl_->width_ = it->second.width;
        impl_->height_ = it->second.height;
    }
}

gfx::IGFXDevice* RenderPipeline::Gfx() {
    return impl_->gfx_.get();
}
const gfx::IGFXDevice* RenderPipeline::Gfx() const {
    return impl_->gfx_.get();
}

gfx::Format RenderPipeline::SceneTargetFormat() const {
    // HD always renders to the HDR scene target. SD does too when the
    // host opts into SceneHdrInSd (so SD geometry/particle PSOs build for
    // the HDR rtv and additive content rolls off through the tonemap
    // instead of clipping on the LDR swap chain). See RenderViewport.
    if (impl_->frameRenderMode_ == RenderMode::HD || rs_.Settings().SceneHdrInSd())
        return kHdrSceneFormat;
    // SD mode renders directly to the swap-chain back buffer — PSO
    // rtvFormat must match its actual format. On most backends that's
    // R8G8B8A8_UNORM_SRGB (kSdSceneFormat); on WebGPU under Windows
    // Dawn-D3D12 the surface only exposes BGRA8 family, so the back
    // buffer is B8G8R8A8_UNORM_SRGB and PSOs need to be built for that.
    // Prefer the target currently being rendered (so per-viewport SD format
    // is correct even when several swap-chains with different formats
    // coexist), falling back to the primary outside a frame.
    if (impl_->gfx_) {
        const RenderTarget* t = impl_->activeTarget_;
        if (!t && impl_->primaryTargetId_ != 0) {
            auto it = impl_->targets_.find(impl_->primaryTargetId_);
            if (it != impl_->targets_.end())
                t = &it->second;
        }
        if (t && t->swap != gfx::SwapChainHandle::Invalid) {
            const gfx::Format swapFmt = impl_->gfx_->GetSwapChainFormat(t->swap);
            if (swapFmt != gfx::Format::Unknown)
                return StripSrgb(swapFmt);
        }
    }
    return kSdSceneFormat;
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

i32 RenderPipeline::Width() const {
    return impl_->activeTarget_ ? impl_->activeTarget_->width : impl_->width_;
}
i32 RenderPipeline::Height() const {
    return impl_->activeTarget_ ? impl_->activeTarget_->height : impl_->height_;
}

const Camera& RenderPipeline::FrameCamera() const {
    return impl_->activeCamera_ ? *impl_->activeCamera_ : rs_.Scene().Camera();
}

gfx::BufferHandle RenderPipeline::CbPerFrame() const {
    return impl_->cbPerFrame_;
}

RenderPipeline::ShadowResources RenderPipeline::Shadow() const {
    return {impl_->shadowPSO_, impl_->shadowPSORigid_, impl_->shadowVsCb_};
}

void RenderPipeline::Shutdown() {
    rs_.Spn().Clear();   // CPU state of the default scene (others freed on destroy)
    rs_.Splats().Clear();
    // Per-scene: clear each scene's corn emitters and release its actors' +
    // template caches' GPU resources while the device is still alive.
    rs_.ForEachCornService([](corn_effects::CornEffectsService& corn) { corn.Clear(); });
    ReleaseModelGPU();
    rs_.ForEachScene([this](SceneManager& scene) {
        scene.Templates().ReleaseAllGPU(*impl_->gfx_);
        scene.Templates().Clear();
    });
    CleanupGFX();
}

void RenderPipeline::GetFrameStats(i32& geosets, i32& textures, i32& nodes, i32& particles,
                                   i32& segments) const {
    geosets = textures = nodes = particles = segments = 0;
    for (auto& [h, mi] : rs_.Scene().Actors().All()) {
        geosets += (i32)mi->render.gpuGeosets.size();
        textures += mi->render.textures ? (i32)mi->render.textures->Size() : 0;
        nodes += mi->render.skinning.NodeCount();
        segments += mi->render.ribbons.GetTotalSegmentCount();
    }
    particles += rs_.Particles().TotalParticleCount();
}

void RenderPipeline::ReleaseModelGPU() {
    rs_.ForEachScene([this](SceneManager& scene) {
        for (auto& [h, miPtr] : scene.Actors().All())
            miPtr->ReleaseGPU(*impl_->gfx_);
    });
}

// Draw one PE2 emitter's batch. Binds the shared particle VB itself so it can be
// invoked interleaved with other producers in the unified transparent pass.
void RenderPipeline::DrawParticleEmitter(const particle::EmitterDrawList& dl,
                                         const bls::FrameInputs& frame) {
    if (dl.vertexCount <= 0)
        return;
    auto* cmd = impl_->gfx_->GetImmediateContext();
    cmd->BindVertexBuffer(0, impl_->particleServiceVB_, sizeof(Vertex));

    bls::MatParams mp = bls::FromParticleDesc(dl.material, bls::GxShaderID::SD);
    mp.disables |= bls::kDisableLighting;
    mp.diffuseColor = {1, 1, 1, 1};

    bls::RenderState rs;
    rs.shaderId = bls::GxShaderID::SD;
    rs.alphaMode = static_cast<u8>(mp.alpha);
    rs.numColors = 1;
    rs.numTexCoords = 1;
    rs.numWeights = 0;
    rs.numLights = 0;
    rs.fogEnabled = false;
    rs.depthWrite = mp.DepthWriteEnabled();
    rs.lightingEnabled = false;
    auto perm = bls::SelectPermutes(rs);

    auto req =
        bls::MakePsoRequest(impl_->blsSdProgram_, bls::VertexLayoutKind::ParticleSD, mp, perm);
    req.rtvFormat = SceneTargetFormat();
    // HD scene pass is MRT (slot 1 = linearDepth, slot 2 = normal); the SD
    // particle PSO must match the attachment count or Vulkan/WebGPU reject it.
    if (impl_->frameRenderMode_ == RenderMode::HD) {
        req.extraRtvFormats[0] = kLinearDepthFormat;
        req.extraRtvFormats[1] = kNormalBufferFormat;
        req.extraRtvCount = 2;
    }
    req.dsvFormat = impl_->depthStencilFormat_;
    auto pso = impl_->blsPsoBuilder_->GetOrBuild(req);
    if (pso == gfx::PipelineHandle::Invalid)
        return;
    cmd->BindPipeline(pso);

    if (auto vs = bls::ScopedCb<bls::SdVsCbA>(impl_->gfx_.get(), impl_->blsSdVsCb_)) {
        bls::BuildSdVsCbA(*vs, frame, mp);
    }
    if (auto ps = bls::ScopedCb<bls::SdPsCbA>(impl_->gfx_.get(), impl_->blsSdPsCb_)) {
        bls::BuildSdPsCbA(*ps, frame, mp);
    }
    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, impl_->blsSdVsCb_);
    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, impl_->blsSdPsCb_);

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

namespace {
particle::FilterMode SplatBlendModeToFilter(i32 blendMode) {

    switch (blendMode) {
    case 0:
        return particle::FilterMode::Blend;
    case 1:
        return particle::FilterMode::Additive;
    case 2:
        return particle::FilterMode::Modulate;
    case 3:
        return particle::FilterMode::Modulate2X;
    case 4:
        return particle::FilterMode::AlphaKey;
    default:
        return particle::FilterMode::Blend;
    }
}

// Front-biased day-night sun travel direction (world space, the way the light
// travels toward the scene). The raw DNC light node points wherever the asset
// authored it, which back-lights the viewer's default view; this anchors the
// sun in front of and above the model so the frontal part stays lit, sweeping
// elevation with TOD. The unit faces +X in renderer space (Max -Y — the same
// facing the default camera and view-cube use), world up is +Z, so the sun is
// toward {+X front, +Z up} with a slight +Y for three-quarter form. Shared by
// the CSM shadow pass and the geoset shading baseline so the two agree.
Vector3f ComputeSunDirWS(const dnc::DncService* dnc) {
    f32 height = 1.2f; // sun elevation above the horizon; swept by TOD below
    if (dnc) {
        constexpr f32 kPi = 3.14159265f;
        const f32 hpd = dnc->GetHoursPerDay();
        const f32 tod = dnc->GetTimeOfDay();
        const f32 theta = ((tod - hpd * 0.25f) / (hpd * 0.5f)) * kPi;
        // Low near dawn/dusk, high at noon — but always above the horizon so
        // the front never falls into shadow.
        height = 0.4f + 0.9f * std::max(0.1f, std::sin(theta));
    }
    // Direction *to* the sun, then negate for the travel direction.
    const Vector3f toSun = {1.0f, 0.35f, height};
    Vector3f travel = {-toSun.x, -toSun.y, -toSun.z};
    const f32 len = std::sqrt(travel.x * travel.x + travel.y * travel.y + travel.z * travel.z);
    if (len > 1.0e-4f) {
        const f32 inv = 1.0f / len;
        travel = {travel.x * inv, travel.y * inv, travel.z * inv};
    }
    return travel;
}
} // namespace

bool RenderPipeline::RenderSplatsBls() {
    if (!impl_->blsSdProgram_ || !impl_->blsPsoBuilder_)
        return false;
    if (rs_.Splats().Count() == 0)
        return true;

    auto* cmd = impl_->gfx_->GetImmediateContext();

    std::vector<Vertex> verts;
    std::vector<particle::SplatDrawList> drawLists;
    Matrix44f viewMat = rs_.Pipeline().FrameCamera().GetViewMatrix();
    rs_.Splats().BuildGeometry(verts, drawLists);
    if (verts.empty())
        return true;

    const i32 vertCount = (i32)verts.size();
    if (impl_->splatServiceVB_ == gfx::BufferHandle::Invalid ||
        vertCount > impl_->splatServiceVBSize_) {
        impl_->gfx_->Destroy(impl_->splatServiceVB_);
        // Doubled-and-capped growth — see particleServiceVB_ above.
        constexpr i32 kFloor   = 4096;
        constexpr i32 kCeiling = 256 * 1024;
        const i32 doubled = (std::max)(impl_->splatServiceVBSize_ * 2, kFloor);
        const i32 newSize = (std::max)(vertCount, (std::min)(doubled, kCeiling));
        gfx::BufferDesc bd;
        bd.size = (u32)(sizeof(Vertex) * newSize);
        bd.usage = gfx::BufferUsage::Vertex | gfx::BufferUsage::CpuWritable;
        bd.ringSlotsHint = 4; // mapped once per frame
        impl_->splatServiceVB_ = impl_->gfx_->CreateBuffer(bd);
        impl_->splatServiceVBSize_ = newSize;
    }
    if (void* mapped = impl_->gfx_->MapBuffer(impl_->splatServiceVB_)) {
        memcpy(mapped, verts.data(), sizeof(Vertex) * vertCount);
        impl_->gfx_->UnmapBuffer(impl_->splatServiceVB_);
    }

    cmd->BindVertexBuffer(0, impl_->splatServiceVB_, sizeof(Vertex));

    bls::FrameInputs frame;
    frame.world = Matrix44f::identity();
    frame.view = viewMat;
    const f32 aspect = (Height() > 0) ? (f32)Width() / (f32)Height() : 1.0f;
    frame.projection = rs_.Pipeline().FrameCamera().ProjectionRH(aspect);
    frame.effectTime = rs_.Scene().GetAnimationTime() * 0.001f;
    frame.numLights = 0;
    frame.viewportRect = {(f32)Width(), (f32)Height(), 0.0f, 0.0f};

    for (const auto& dl : drawLists) {
        if (dl.vertexCount <= 0)
            continue;

        particle::ParticleMaterialDesc pmd;
        pmd.filterMode = SplatBlendModeToFilter(dl.blendMode);
        pmd.unshaded = true;
        pmd.unfogged = true;
        pmd.textureId = -1;

        bls::MatParams mp = bls::FromParticleDesc(pmd, bls::GxShaderID::SD);
        mp.disables |= bls::kDisableLighting;
        mp.diffuseColor = {1, 1, 1, 1};

        bls::RenderState rs;
        rs.shaderId = bls::GxShaderID::SD;
        rs.alphaMode = static_cast<u8>(mp.alpha);
        rs.numColors = 1;
        rs.numTexCoords = 1;
        rs.numWeights = 0;
        rs.numLights = 0;
        rs.fogEnabled = false;
        rs.depthWrite = mp.DepthWriteEnabled();
        rs.lightingEnabled = false;
        auto perm = bls::SelectPermutes(rs);

        auto req =
            bls::MakePsoRequest(impl_->blsSdProgram_, bls::VertexLayoutKind::ParticleSD, mp, perm);
        req.rtvFormat = SceneTargetFormat();
        // Particles are drawn inside the main scene pass. In HD mode
        // that pass is MRT (slot 1 = linearDepth, slot 2 = normal); the
        // SD particle PSO has to match the attachment count or
        // Vulkan/WebGPU validation rejects the bind.
        if (impl_->frameRenderMode_ == RenderMode::HD) {
            req.extraRtvFormats[0] = kLinearDepthFormat;
            req.extraRtvFormats[1] = kNormalBufferFormat;
            req.extraRtvCount = 2;
        }
        req.dsvFormat = impl_->depthStencilFormat_;
        auto pso = impl_->blsPsoBuilder_->GetOrBuild(req);
        if (pso == gfx::PipelineHandle::Invalid)
            continue;
        cmd->BindPipeline(pso);

        if (auto vs = bls::ScopedCb<bls::SdVsCbA>(impl_->gfx_.get(), impl_->blsSdVsCb_)) {
            bls::BuildSdVsCbA(*vs, frame, mp);
        }
        if (auto ps = bls::ScopedCb<bls::SdPsCbA>(impl_->gfx_.get(), impl_->blsSdPsCb_)) {
            bls::BuildSdPsCbA(*ps, frame, mp);
        }
        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, impl_->blsSdVsCb_);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, impl_->blsSdPsCb_);

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

void RenderPipeline::PrepareRibbons(std::vector<RibbonDrawUnit>& out, bls::FrameInputs& outFrame) {
    if (!impl_->blsSdProgram_ || !impl_->blsPsoBuilder_)
        return;

    const f32 aspect = (Height() > 0) ? (f32)Width() / (f32)Height() : 1.0f;
    outFrame.world = Matrix44f::identity();
    outFrame.view = rs_.Pipeline().FrameCamera().GetViewMatrix();
    outFrame.projection = rs_.Pipeline().FrameCamera().ProjectionRH(aspect);
    outFrame.numLights = 0;
    outFrame.effectTime = rs_.Scene().GetAnimationTime() * 0.001f;
    outFrame.viewportRect = {(f32)Width(), (f32)Height(), 0.0f, 0.0f};

    for (auto& [_mh, _mi] : rs_.Scene().Actors().All()) {
        auto* mi = _mi.get();
        if (!mi->render.ribbons.HasEmitters() || mi->parentVisibility <= 0.02f)
            continue;
        RibbonSystem::StripResult stripResult = mi->render.ribbons.BuildStrips();
        auto& verts = stripResult.vertices;
        auto& emitterIds = stripResult.emitterIds;
        const i32 vertCount = (i32)verts.size();
        if (vertCount <= 0)
            continue;

        if (mi->render.ribbonVB == gfx::BufferHandle::Invalid ||
            vertCount > mi->render.ribbonVBSize) {
            impl_->gfx_->Destroy(mi->render.ribbonVB);
            // Doubled-and-capped growth — ribbons stream continuously as the
            // actor animates; exact-size growth churned a fresh VB every frame.
            constexpr i32 kFloor = 512;
            constexpr i32 kCeiling = 64 * 1024;
            const i32 doubled = (std::max)(mi->render.ribbonVBSize * 2, kFloor);
            const i32 newSize = (std::max)(vertCount, (std::min)(doubled, kCeiling));
            gfx::BufferDesc bd;
            bd.size = (u32)(sizeof(Vertex) * newSize);
            bd.usage = gfx::BufferUsage::Vertex | gfx::BufferUsage::CpuWritable;
            bd.ringSlotsHint = 4;
            mi->render.ribbonVB = impl_->gfx_->CreateBuffer(bd);
            mi->render.ribbonVBSize = newSize;
        }
        if (void* mapped = impl_->gfx_->MapBuffer(mi->render.ribbonVB)) {
            memcpy(mapped, verts.data(), sizeof(Vertex) * vertCount);
            impl_->gfx_->UnmapBuffer(mi->render.ribbonVB);
        }

        i32 running = 0;
        for (i32 eid : emitterIds) {
            const i32 offset = running;
            const i32 n = mi->render.ribbons.GetEmitterVertCount(eid);
            running += n;
            if (n <= 0)
                continue;
            const auto* c = mi->render.ribbons.GetConfig(eid);
            const RibbonEmitterConfig cfg = c ? *c : RibbonEmitterConfig{};
            RibbonDrawUnit u;
            u.actor = mi;
            u.filterMode = cfg.filterMode;
            u.matFlags = (cfg.twoSided ? MAT_TWO_SIDED : 0) | (cfg.unshaded ? MAT_UNSHADED : 0);
            u.textureId = cfg.textureId;
            u.count = n;
            u.offset = offset;
            u.origin = verts[offset].position; // world-space strip head
            u.priorityPlane = cfg.priorityPlane;
            out.push_back(u);
        }
    }
}

void RenderPipeline::DrawRibbonStrip(const RibbonDrawUnit& u, const bls::FrameInputs& frame) {
    if (u.count <= 0 || !u.actor)
        return;
    auto* cmd = impl_->gfx_->GetImmediateContext();
    cmd->BindVertexBuffer(0, u.actor->render.ribbonVB, sizeof(Vertex));

    bls::MatParams mp = bls::FromMdxLayer(u.filterMode, u.matFlags, bls::GxShaderID::SD);
    mp.disables |= bls::kDisableLighting;
    mp.diffuseColor = {1, 1, 1, 1};

    bls::RenderState rs = bls::MakeSdMeshRenderState(mp, 0, true, false);
    auto perm = bls::SelectPermutes(rs);
    auto req =
        bls::MakePsoRequest(impl_->blsSdProgram_, bls::VertexLayoutKind::ParticleSD, mp, perm);
    req.rtvFormat = SceneTargetFormat();
    if (impl_->frameRenderMode_ == RenderMode::HD) {
        req.extraRtvFormats[0] = kLinearDepthFormat;
        req.extraRtvFormats[1] = kNormalBufferFormat;
        req.extraRtvCount = 2;
    }
    req.dsvFormat = impl_->depthStencilFormat_;
    auto pso = impl_->blsPsoBuilder_->GetOrBuild(req);
    if (pso == gfx::PipelineHandle::Invalid)
        return;
    cmd->BindPipeline(pso);

    if (auto vs = bls::ScopedCb<bls::SdVsCbA>(impl_->gfx_.get(), impl_->blsSdVsCb_)) {
        bls::BuildSdVsCbA(*vs, frame, mp);
    }
    if (auto ps = bls::ScopedCb<bls::SdPsCbA>(impl_->gfx_.get(), impl_->blsSdPsCb_)) {
        bls::BuildSdPsCbA(*ps, frame, mp);
    }
    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, impl_->blsSdVsCb_);
    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, impl_->blsSdPsCb_);

    render_detail::BindLayerAlbedo(cmd, u.actor->render.textures.get(), u.textureId,
                                   rs_.Textures().GetDefaults().White, rs_.Samplers());
    cmd->Draw(u.count, u.offset);
}

i32 RenderPipeline::ComputeSelectedLod() const {
    i32 ov = rs_.Settings().LodOverride();
    if (ov >= 0)
        return std::clamp(ov, 0, 3);

    Vector3f camPos = rs_.Pipeline().FrameCamera().GetSource();
    f32 viewDist = std::sqrt(camPos.x * camPos.x + camPos.y * camPos.y + camPos.z * camPos.z);
    if (viewDist < 1.0f)
        return 0;

    const f32 aspect = (Height() > 0) ? (f32)Width() / (f32)Height() : 1.0f;
    Matrix44f proj = rs_.Pipeline().FrameCamera().ProjectionLH(aspect);

    f32 projM11 = proj.data[1][1];
    f32 screenPixels = projM11 / viewDist * (f32)Height() * 0.5f;
    if (screenPixels <= 0.001f)
        return 3;
    f32 deviation = 5.0f / screenPixels;

    static constexpr f32 kDeviations[4] = {0.0f, 1.0f, 2.0f, 4.0f};
    i32 selectedLOD = 4;
    do {
        --selectedLOD;
    } while (selectedLOD > 0 && deviation <= kDeviations[selectedLOD]);
    return std::clamp(selectedLOD, 0, 3);
}

bool RenderPipeline::InitDevice(gfx::GfxApi api) {

    // Preferred-device hint to the gfx layer: empty string falls back
    // to the backend's "best by VRAM / score" default. Setting it
    // here keeps the gfx layer's module-scope state in sync with
    // whatever the host loaded from .ini.
    gfx::SetPreferredDevice(rs_.Settings().PreferredDevice().c_str());

    impl_->gfx_ = gfx::CreateDevice(api, rs_.Settings().GraphicsDebug());
    if (!impl_->gfx_)
        return false;

    // Cache the device's preferred depth-stencil format once. Every
    // subsequent CreateDepthTarget / PSO dsvFormat in this file (and
    // in the BLS PSO builder, debug renderer, etc.) reads from here.
    // On AMD this lands as D32_FLOAT_S8_UINT; on NVIDIA/Intel as
    // D24_UNORM_S8_UINT.
    impl_->depthStencilFormat_ = impl_->gfx_->PreferredDepthStencilFormat();

    rs_.CreateDeviceAssetManagers(*impl_->gfx_);

    // cbPerFrame_ is ring-mapped (WriteCbPerFrame) several times per frame —
    // once per pass that needs a distinct view/projection (particles, splats,
    // corn, ribbons, scene). With multiple viewports rendered sequentially into
    // one frame that count multiplies by the viewport count, and every slot
    // must stay live until its draws retire (viewports × passes ×
    // frames-in-flight). Size the ring generously so a busy multi-viewport
    // host can't reuse a slot the GPU is still reading.
    constexpr u32 kPerFrameCbRingSlots = 256;
    impl_->cbPerFrame_ = impl_->gfx_->CreateBuffer({
        .size = sizeof(CBPerFrame),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kPerFrameCbRingSlots,
    });

    if (!CreateShaders()) {
        CleanupGFX();
        return false;
    }
    if (!CreatePipelines()) {
        CleanupGFX();
        return false;
    }

    // Optional frame capture — builds its own shaders + compute PSO. Failure
    // here isn't fatal: capture just stays unavailable (it self-diagnoses).
    impl_->capture_.Init(*impl_->gfx_, api, impl_->depthStencilFormat_);

    if (!CreateDefaultResources()) {
        CleanupGFX();
        return false;
    }

    if (!InitBlsShaders(api)) {
#if defined(__EMSCRIPTEN__)
        // Web build: JS pre-fetches the BLS shader bundles into the
        // FetchContentProvider before wf_init runs. If they're still
        // missing here something upstream went wrong — keep the gfx
        // device live so the canvas can still clear-color, but any
        // BLS-dependent draw will fail loudly.
        std::fprintf(stderr,
                     "[bls] WARN: shaders unavailable; continuing for clear-color only\n");
#else
        CleanupGFX();
        return false;
#endif
    }

    return true;
}

bool RenderPipeline::InitBlsShaders(gfx::GfxApi api) {
    if (!impl_->gfx_ || !rs_.Scene().ActiveContentProvider())
        return false;

    rs_.Replaceables().SetContentProvider(rs_.Scene().ActiveContentProvider());

    // Splat→AssetManager wiring is now applied to every scene's SplatService by
    // RenderService (CreateDeviceAssetManagers / CreateScene), not just the
    // active one — see InitSceneServices.

    // Eagerly Acquire every SPL/UBR texture + SPN child-model slot the
    // event-data SLKs reference. Desktop only: the sync provider loads
    // them immediately during the next pump, so the textures persist
    // across splat births / deaths and survive animation changes.
    //
    // Skipped on web — every Acquire there is a Hive fetch the user
    // pays for, and most SLK entries are never touched by the loaded
    // model. wf_spawn_unit calls PrefetchEventAssetsForActor instead,
    // which Acquires only the SPL/UBR/SPN/FPT slots the spawned model's
    // event objects reference.
#ifndef __EMSCRIPTEN__
    if (io::IsSplCachePopulated())
        io::PrefetchEventAssetSlots(rs_.Assets());
#endif

    rs_.EnsureDncService();
    rs_.EnsureShadowService(*impl_->gfx_);
    rs_.EnsureGtaoService(*impl_->gfx_, impl_->gfx_->GetApi());
    // DoF (EnsureDofService) is set up below alongside PostProcessService — both
    // need the shipped BLS shaders + the shared fullscreen-triangle VB, which
    // aren't created until the sprite/tonemap block further down.

    if (impl_->shadowVsCb_ == gfx::BufferHandle::Invalid) {
        impl_->shadowVsCb_ = impl_->gfx_->CreateBuffer({
            .size = sizeof(bls::HdVsCb),
            .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        });
    }

    // Graphics-debug runs load the -g2 -O0 bundles from debug_shaders/
    // (staged by the WDX_BUILD_WC3_DEBUG_SHADERS pipeline) so the bytecode
    // maps back to slang source in RenderDoc / PIX / the validation layers.
    impl_->blsShaderCache_ = std::make_unique<bls::BlsShaderCache>(
        impl_->gfx_.get(), rs_.Scene().ActiveContentProvider(), api,
        rs_.Settings().GraphicsDebug());
    impl_->blsPrograms_ = std::make_unique<bls::BlsProgramCatalog>(impl_->blsShaderCache_.get());
    impl_->blsPsoBuilder_ = std::make_unique<bls::BlsPsoBuilder>(impl_->gfx_.get());

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

    impl_->blsSdProgram_ = impl_->blsPrograms_->Load({bls::GxShaderID::SD, "SD_HighSpec", "SD"});
    impl_->blsSdOnHdProgram_ =
        impl_->blsPrograms_->Load({bls::GxShaderID::SD_on_HD, "SD_on_HD", "SD_on_HD"});
    impl_->blsHdProgram_ = impl_->blsPrograms_->Load({bls::GxShaderID::HD, "HD", "HD"});

    impl_->blsCrystalProgram_ =
        impl_->blsPrograms_->Load({bls::GxShaderID::Crystal, "HD", "Crystal"});

    // The BLS catalog name strings are the engine's wire-format shader
    // filenames on disk — preserved verbatim for interoperability with
    // shipped game data. Our own symbol names (GxShaderID::CornFx,
    // blsCornFxProgram_) are trademark-neutral.
    impl_->blsCornFxProgram_ =
        impl_->blsPrograms_->Load({bls::GxShaderID::CornFx, "PopcornFX", "PopcornFX"});

    // Hand the corn fx service the renderer's gfx + BLS resources so its
    // per-emitter CornEffectsGfxBackend can issue draws. Diffuse-texture
    // paths come from the .pkb's renderer property block and route
    // through AssetManager slots: each layer Acquires a slot at
    // prepare() time and reads its current handle via TextureOf() at
    // submit() time — the host's needs-pump drives the actual fetch +
    // decode and the slot's payload swaps in transparently.
    if (impl_->blsCornFxProgram_) {
        corn_effects::CornEffectsGfxBackend::Init pInit;
        pInit.device = impl_->gfx_.get();
        pInit.program = impl_->blsCornFxProgram_;
        pInit.psoBuilder = impl_->blsPsoBuilder_.get();
        pInit.textures = &rs_.Textures();
        pInit.samplers = &rs_.Samplers();
        pInit.assets   = &rs_.Assets();
        pInit.slotAcquire = [&rs = rs_](std::string_view path) -> std::uint32_t {
            return rs.Assets().Acquire(assets::AssetKind::Texture, path);
        };
        // Apply to every scene's corn service (current + future). The applier
        // runs immediately for existing scenes and at CreateScene() for new ones.
        rs_.SetCornBackendInitApplier(
            [pInit](corn_effects::CornEffectsService& corn) { corn.SetBackendInit(pInit); });
    }

    if (impl_->blsHdProgram_ && impl_->blsHdProgram_->vs &&
        !impl_->blsHdProgram_->vs->permuteHandles.empty()) {
        const shadow::ShadowParams& sp =
            rs_.GetShadowService() ? rs_.GetShadowService()->Params() : shadow::ShadowParams{};

        auto buildShadowPso = [&](u32 permIndex,
                                  bls::VertexLayoutKind layoutKind) -> gfx::PipelineHandle {
            if (permIndex >= impl_->blsHdProgram_->vs->permuteHandles.size())
                return gfx::PipelineHandle::Invalid;
            gfx::GraphicsPipelineDesc gpd{};
            gpd.vs = impl_->blsHdProgram_->vs->permuteHandles[permIndex];
            gpd.ps = gfx::ShaderHandle{0};
            gpd.inputLayout = bls::LayoutFor(layoutKind, impl_->gfx_->GetApi());
            gpd.topology = gfx::PrimitiveTopology::TriangleList;
            gpd.depthStencil.depthTest = true;
            gpd.depthStencil.depthWrite = true;
            gpd.depthStencil.depthCompare = gfx::CompareOp::LessEqual;
            gpd.rasterizer.cull = gfx::CullMode::Back;
            gpd.rasterizer.frontCCW = true;
            gpd.rasterizer.depthBias = sp.depthBias;
            gpd.rasterizer.slopeScaledDepthBias = sp.slopeScaledBias;
            gpd.rasterizer.depthBiasClamp = sp.depthBiasClamp;
            gpd.rtvFormat = gfx::Format::Unknown;
            gpd.dsvFormat = gfx::Format::D32_FLOAT;
            return impl_->gfx_->CreateGraphicsPipeline(gpd);
        };

        if (impl_->shadowPSO_ == gfx::PipelineHandle::Invalid) {
            impl_->shadowPSO_ = buildShadowPso(4, bls::VertexLayoutKind::MeshHDSkinnedNoTangent);
        }
        if (impl_->shadowPSORigid_ == gfx::PipelineHandle::Invalid) {
            impl_->shadowPSORigid_ = buildShadowPso(0, bls::VertexLayoutKind::ParticleSD);
        }
    }

    impl_->blsSpriteVs_ = impl_->blsShaderCache_->Acquire(gfx::ShaderStage::Vertex, "sprite");
    impl_->blsTonemapPs_ = impl_->blsShaderCache_->Acquire(gfx::ShaderStage::Pixel, "tonemap");

    if (impl_->blsSpriteVs_ && !impl_->blsSpriteVs_->permuteHandles.empty() &&
        impl_->blsTonemapPs_ && !impl_->blsTonemapPs_->permuteHandles.empty()) {
        struct TonemapVertex {
            f32 x, y, z;
            f32 u, v;
        };

        static const TonemapVertex kTonemapVerts[3] = {

            {-1.0f, 1.0f, 0.0f, 0.0f, 0.0f},
            {3.0f, 1.0f, 0.0f, 2.0f, 0.0f},
            {-1.0f, -3.0f, 0.0f, 0.0f, 2.0f},
        };
        impl_->tonemapVB_ = impl_->gfx_->CreateBuffer(
            {
                .size = sizeof(kTonemapVerts),
                .usage = gfx::BufferUsage::Vertex,
            },
            kTonemapVerts);

        // Tonemap PSO creation is deferred to RunTonemapPass (see
        // EnsureTonemapPso) — we don't know the swap-chain RTV format yet,
        // and on macOS / Metal the surface only exposes BGRA8 family, not
        // RGBA8. Building the PSO now with a hardcoded R8G8B8A8_UNORM_SRGB
        // would mismatch the BGRA8 swap-chain renderpass at BindPipeline
        // time and silently drop every present.

        impl_->tonemapPsCb_ = impl_->gfx_->CreateBuffer({
            .size = 16,
            .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        });
        gfx::SamplerDesc sd;
        sd.minFilter = gfx::Filter::Linear;
        sd.magFilter = gfx::Filter::Linear;
        sd.addressU = gfx::AddressMode::Clamp;
        sd.addressV = gfx::AddressMode::Clamp;
        sd.addressW = gfx::AddressMode::Clamp;
        impl_->tonemapSampler_ = impl_->gfx_->CreateSampler(sd);
    }

    // Post-process service (HDR bloom today, CMAA later). Reuses the
    // sprite VS + the tonemap pass's 3-vertex fullscreen-triangle VB,
    // so its Init has to come after the tonemap setup above.
    if (impl_->tonemapVB_ != gfx::BufferHandle::Invalid) {
        rs_.EnsurePostProcessService(*impl_->gfx_, impl_->gfx_->GetApi(),
                                     *impl_->blsShaderCache_, impl_->tonemapVB_);
        // Depth of field runs the shipped depthoffield.bls through the same
        // sprite VS + fullscreen-triangle VB as bloom/tonemap.
        rs_.EnsureDofService(*impl_->gfx_, impl_->gfx_->GetApi(), *impl_->blsShaderCache_,
                             impl_->tonemapVB_);
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
        .size = sizeof(bls::SdVsCbA),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kHotCbRingSlots,
    });

    impl_->blsSdPsCb_ = impl_->gfx_->CreateBuffer({
        .size = sizeof(bls::SdPsCbA),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kHotCbRingSlots,
    });
    impl_->blsHdVsCb_ = impl_->gfx_->CreateBuffer({
        .size = sizeof(bls::HdVsCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kHotCbRingSlots,
    });

    impl_->blsHdShadowCb_ = impl_->gfx_->CreateBuffer({
        .size = sizeof(bls::HdShadowCascadesCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kHotCbRingSlots,
    });

    impl_->blsHdShadowCountCb_ = impl_->gfx_->CreateBuffer({
        .size = sizeof(bls::SdOnHdShadowCascadeCountCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kHotCbRingSlots,
    });
    impl_->blsHdPsCb_ = impl_->gfx_->CreateBuffer({
        .size = sizeof(bls::HdPsCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kHotCbRingSlots,
    });
    impl_->blsSdOnHdPsCb_ = impl_->gfx_->CreateBuffer({
        .size = sizeof(bls::SdOnHdPsCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kHotCbRingSlots,
    });
    impl_->blsHdDebugVisCb_ = impl_->gfx_->CreateBuffer({
        .size = sizeof(bls::DebugVisCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kHotCbRingSlots,
    });

    rs_.Textures().RegisterOwned(kIblSplitSumLutName, ibl::CreateSplitSumLutTexture(*impl_->gfx_));

    // Initial IBL apply — RenderFrame's poll handles subsequent mode changes.
    ApplyIblMode(rs_.Settings().GetIblMode());
    rs_.Settings().ConsumeIblModeDirty();

    // Build the engine-side ImGui adapter now that the BLS cache has been
    // pointed at the device + content provider. The adapter loads imgui.bls
    // (one VS, two PS perms) through the same cache. RTV format is the
    // swapchain backbuffer format; in HD mode ImGui draws live inside the
    // tonemap pass (target.color), in SD mode inside the main scene pass
    // (also target.color) — both write through the same sRGB RTV.
    rs_.EnsureImGui(*impl_->gfx_, *impl_->blsShaderCache_, gfx::Format::R8G8B8A8_UNORM_SRGB,
                    impl_->depthStencilFormat_);

    // tonemapPSO_ is intentionally not part of the readiness check — it's
    // built lazily on first RunTonemapPass, against the actual swap-chain
    // RTV format (see comment in CreateShaders).
    const bool ok = impl_->blsSdProgram_ != nullptr && impl_->blsSdOnHdProgram_ != nullptr &&
                    impl_->blsHdProgram_ != nullptr && impl_->blsSpriteVs_ != nullptr &&
                    impl_->blsTonemapPs_ != nullptr;

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
            impl_->blsPsoTrace_->Replay(*impl_->blsPsoBuilder_, *impl_->blsPrograms_);
            impl_->blsPsoBuilder_->SetReplayMode(false);
            const auto& s = impl_->blsPsoBuilder_->Stats();
            std::fprintf(stderr,
                         "[bls] pso-trace: replay built %llu PSOs, %llu were already cached\n",
                         (unsigned long long)s.replayCacheBuilds, (unsigned long long)s.cacheHits);
        } else {
            std::fprintf(stderr, "[bls] pso-trace: no saved trace (this run will record one)\n");
        }
    }

    if (!ok) {
        std::fprintf(stderr,
                     "[bls] InitBlsShaders incomplete: "
                     "SD=%p SD_on_HD=%p HD=%p Crystal=%p CornFx=%p "
                     "spriteVs=%p tonemapPs=%p tonemapPSO=%llu\n",
                     (void*)impl_->blsSdProgram_, (void*)impl_->blsSdOnHdProgram_,
                     (void*)impl_->blsHdProgram_, (void*)impl_->blsCrystalProgram_,
                     (void*)impl_->blsCornFxProgram_, (void*)impl_->blsSpriteVs_,
                     (void*)impl_->blsTonemapPs_, (unsigned long long)impl_->tonemapPSO_);
    }
    return ok;
}

void RenderPipeline::SetEnvProbe(const std::string& relPath) {
    if (!impl_->gfx_ || !rs_.HasDeviceAssetManagers())
        return;

    rs_.Textures().ReleaseOwned(kIblFromProbeName);
    rs_.Textures().ReleaseOwned(kIblToProbeName);

    gfx::TextureHandle fromHandle = gfx::TextureHandle::Invalid;
    i32 mips = 0;
    if (!relPath.empty() && rs_.Scene().ActiveContentProvider()) {
        auto probe = ibl::LoadEnvProbe(*impl_->gfx_, *rs_.Scene().ActiveContentProvider(), relPath);
        if (probe.handle != gfx::TextureHandle::Invalid) {
            fromHandle = probe.handle;
            mips = probe.mipCount;
        }
    }
    if (fromHandle == gfx::TextureHandle::Invalid) {
        std::fprintf(stderr,
                     "[ibl] WARN: probe '%s' failed to load — using debug "
                     "procedural\n",
                     relPath.c_str());
        fromHandle = ibl::CreateDebugFacesEnvProbe(*impl_->gfx_);
        mips = ibl::kEnvProbeMipLevels;
    }
    rs_.Textures().RegisterOwned(kIblFromProbeName, fromHandle);
    impl_->iblProbeMipEnd_ = static_cast<f32>(mips - 1);

    impl_->iblDayNightLoaded_ = false;
    rs_.Textures().ReleaseOwned(kIblDayProbeName);
    rs_.Textures().ReleaseOwned(kIblNightProbeName);
}

void RenderPipeline::SetDayNightProbes(const std::string& dayPath, const std::string& nightPath) {
    if (!impl_->gfx_ || !rs_.HasDeviceAssetManagers() || !rs_.Scene().ActiveContentProvider())
        return;

    rs_.Textures().ReleaseOwned(kIblDayProbeName);
    rs_.Textures().ReleaseOwned(kIblNightProbeName);
    impl_->iblDayNightLoaded_ = false;
    impl_->iblDayMipEnd_ = 0.0f;
    impl_->iblNightMipEnd_ = 0.0f;

    auto loadProbe = [&](const std::string& path, const char* slotName) -> i32 {
        if (path.empty())
            return 0;
        auto probe = ibl::LoadEnvProbe(*impl_->gfx_, *rs_.Scene().ActiveContentProvider(), path);
        if (probe.handle == gfx::TextureHandle::Invalid) {
            std::fprintf(stderr, "[dnc] day/night IBL load failed: %s\n", path.c_str());
            return 0;
        }
        rs_.Textures().RegisterOwned(slotName, probe.handle);
        return probe.mipCount;
    };

    const i32 dayMips = loadProbe(dayPath, kIblDayProbeName);
    const i32 nightMips = loadProbe(nightPath, kIblNightProbeName);
    if (dayMips > 0 && nightMips > 0) {
        impl_->iblDayMipEnd_ = static_cast<f32>(dayMips - 1);
        impl_->iblNightMipEnd_ = static_cast<f32>(nightMips - 1);
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

        if (!impl_->iblDayNightLoaded_)
            SetEnvProbe(ibl::kPortraitIblPath);
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
    impl_->blsSdProgram_ = nullptr;
    impl_->blsSdOnHdProgram_ = nullptr;
    impl_->blsHdProgram_ = nullptr;
    impl_->blsCrystalProgram_ = nullptr;

    impl_->blsSpriteVs_ = nullptr;
    impl_->blsTonemapPs_ = nullptr;
    if (impl_->gfx_) {
        impl_->gfx_->Destroy(impl_->blsSdVsCb_);
        impl_->blsSdVsCb_ = gfx::BufferHandle::Invalid;
        impl_->gfx_->Destroy(impl_->blsSdPsCb_);
        impl_->blsSdPsCb_ = gfx::BufferHandle::Invalid;
        impl_->gfx_->Destroy(impl_->blsHdVsCb_);
        impl_->blsHdVsCb_ = gfx::BufferHandle::Invalid;
        impl_->gfx_->Destroy(impl_->blsHdShadowCb_);
        impl_->blsHdShadowCb_ = gfx::BufferHandle::Invalid;
        impl_->gfx_->Destroy(impl_->blsHdShadowCountCb_);
        impl_->blsHdShadowCountCb_ = gfx::BufferHandle::Invalid;
        if (impl_->shadowPSO_ != gfx::PipelineHandle::Invalid) {
            impl_->gfx_->Destroy(impl_->shadowPSO_);
            impl_->shadowPSO_ = gfx::PipelineHandle::Invalid;
        }
        if (impl_->shadowPSORigid_ != gfx::PipelineHandle::Invalid) {
            impl_->gfx_->Destroy(impl_->shadowPSORigid_);
            impl_->shadowPSORigid_ = gfx::PipelineHandle::Invalid;
        }
        if (impl_->shadowVsCb_ != gfx::BufferHandle::Invalid) {
            impl_->gfx_->Destroy(impl_->shadowVsCb_);
            impl_->shadowVsCb_ = gfx::BufferHandle::Invalid;
        }
        impl_->gfx_->Destroy(impl_->blsHdPsCb_);
        impl_->blsHdPsCb_ = gfx::BufferHandle::Invalid;
        impl_->gfx_->Destroy(impl_->blsSdOnHdPsCb_);
        impl_->blsSdOnHdPsCb_ = gfx::BufferHandle::Invalid;
        impl_->gfx_->Destroy(impl_->blsHdDebugVisCb_);
        impl_->blsHdDebugVisCb_ = gfx::BufferHandle::Invalid;

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
    if (impl_->blsPsoBuilder_)
        impl_->blsPsoBuilder_->SetTrace(nullptr);
    if (impl_->blsPsoTrace_)
        impl_->blsPsoTrace_->Save();
    if (impl_->blsPsoBuilder_)
        impl_->blsPsoBuilder_->Clear();
    if (impl_->blsPrograms_)
        impl_->blsPrograms_->Clear();
    if (impl_->blsShaderCache_)
        impl_->blsShaderCache_->ReleaseAll();
    impl_->blsPsoBuilder_.reset();
    impl_->blsPsoTrace_.reset();
    impl_->blsPrograms_.reset();
    impl_->blsShaderCache_.reset();
}

RenderTargetId RenderPipeline::CreateSwapChainTarget(void* nativeWindowHandle, i32 w, i32 h) {
    if (!impl_->gfx_)
        return 0;

    RenderTarget target;
    target.id = impl_->nextTargetId_++;
    target.swap = impl_->gfx_->CreateSwapChain(nativeWindowHandle, w, h);
    if (target.swap == gfx::SwapChainHandle::Invalid)
        return 0;

    target.color = impl_->gfx_->GetSwapChainBackBuffer(target.swap);
    target.colorLinear = impl_->gfx_->GetSwapChainBackBufferLinear(target.swap);
    target.hdrColor = impl_->gfx_->CreateColorTarget(w, h, kHdrSceneFormat);
    target.depth = impl_->gfx_->CreateDepthTarget(w, h, impl_->depthStencilFormat_);
    // G-buffer slot 1 + slot 2 (HD MRT pass writes linear depth + world
    // normal alongside the HDR scene colour). Always created — SD mode
    // simply doesn't bind them.
    target.linearDepth = impl_->gfx_->CreateColorTarget(w, h, kLinearDepthFormat);
    target.normalBuffer = impl_->gfx_->CreateColorTarget(w, h, kNormalBufferFormat);
    // GTAO outputs. Four R8 AO slots + one RGBA8 bent normal, full-res.
    //   Raw       — main-pass output.
    //   Denoised  — spatial-denoised, pre-temporal.
    //   aoBuffer  — current-frame final (temporal output, apply input).
    //   History   — previous-frame final; GtaoService swaps roles per
    //               frame so we never copy.
    //   BentNormal— MRT slot 1 of the main pass (RGBA8 view-space).
    target.aoBufferRaw = impl_->gfx_->CreateColorTarget(w, h, kAoBufferFormat);
    target.aoBufferDenoised = impl_->gfx_->CreateColorTarget(w, h, kAoBufferFormat);
    target.aoBuffer = impl_->gfx_->CreateColorTarget(w, h, kAoBufferFormat);
    target.aoBufferHistory = impl_->gfx_->CreateColorTarget(w, h, kAoBufferFormat);
    target.bentNormalBuffer = impl_->gfx_->CreateColorTarget(w, h, kBentNormalBufferFormat);
    target.linearDepthHistory = impl_->gfx_->CreateColorTarget(w, h, kLinearDepthFormat);
    target.bloomScratchA = impl_->gfx_->CreateColorTarget(w, h, kHdrSceneFormat);
    target.bloomScratchB = impl_->gfx_->CreateColorTarget(w, h, kHdrSceneFormat);
    target.width = w;
    target.height = h;

    RenderTargetId id = target.id;
    impl_->targets_[id] = target;

    // Mirror the new target's size into the pipeline-wide width_/height_
    // so DebugRenderer / cameras / aspect-ratio queries pick up the real
    // surface dimensions immediately. Previously only ResizePrimaryTarget
    // updated these, which meant the very first frame (before any
    // framebuffer-size event reached us) computed everything against the
    // 800x600 default.
    if (impl_->primaryTargetId_ == 0 || impl_->primaryTargetId_ == id) {
        impl_->width_ = w;
        impl_->height_ = h;
    }
    return id;
}

RenderTargetId RenderPipeline::CreateOffscreenTarget(i32 w, i32 h, gfx::Format colorFormat) {
    if (!impl_->gfx_)
        return 0;

    RenderTarget target;
    target.id = impl_->nextTargetId_++;
    target.swap = gfx::SwapChainHandle::Invalid; // headless — no presentable surface

    // Composite destination. Offscreen has no sRGB-vs-linear back-buffer pair,
    // so colorLinear mirrors color (matching ResizePrimaryTarget's no-swap path).
    target.color = impl_->gfx_->CreateColorTarget(w, h, colorFormat);
    target.colorLinear = target.color;
    // Same auxiliary G-buffer / GTAO / bloom set every target carries (HD MRT
    // and post-process bind these; SD simply doesn't).
    target.hdrColor = impl_->gfx_->CreateColorTarget(w, h, kHdrSceneFormat);
    target.depth = impl_->gfx_->CreateDepthTarget(w, h, impl_->depthStencilFormat_);
    target.linearDepth = impl_->gfx_->CreateColorTarget(w, h, kLinearDepthFormat);
    target.normalBuffer = impl_->gfx_->CreateColorTarget(w, h, kNormalBufferFormat);
    target.aoBufferRaw = impl_->gfx_->CreateColorTarget(w, h, kAoBufferFormat);
    target.aoBufferDenoised = impl_->gfx_->CreateColorTarget(w, h, kAoBufferFormat);
    target.aoBuffer = impl_->gfx_->CreateColorTarget(w, h, kAoBufferFormat);
    target.aoBufferHistory = impl_->gfx_->CreateColorTarget(w, h, kAoBufferFormat);
    target.bentNormalBuffer = impl_->gfx_->CreateColorTarget(w, h, kBentNormalBufferFormat);
    target.linearDepthHistory = impl_->gfx_->CreateColorTarget(w, h, kLinearDepthFormat);
    target.bloomScratchA = impl_->gfx_->CreateColorTarget(w, h, kHdrSceneFormat);
    target.bloomScratchB = impl_->gfx_->CreateColorTarget(w, h, kHdrSceneFormat);
    target.width = w;
    target.height = h;

    // Headless targets never become primary and don't touch pipeline-wide
    // width_/height_ — those track the presentable surface.
    RenderTargetId id = target.id;
    impl_->targets_[id] = target;
    return id;
}

void RenderPipeline::DestroyTarget(RenderTargetId id) {
    auto it = impl_->targets_.find(id);
    if (it == impl_->targets_.end() || !impl_->gfx_)
        return;
    auto& t = it->second;
    impl_->gfx_->Destroy(t.depth);
    impl_->gfx_->Destroy(t.hdrColor);
    impl_->gfx_->Destroy(t.linearDepth);
    impl_->gfx_->Destroy(t.normalBuffer);
    impl_->gfx_->Destroy(t.aoBufferRaw);
    impl_->gfx_->Destroy(t.aoBufferDenoised);
    impl_->gfx_->Destroy(t.aoBuffer);
    impl_->gfx_->Destroy(t.aoBufferHistory);
    impl_->gfx_->Destroy(t.bentNormalBuffer);
    impl_->gfx_->Destroy(t.linearDepthHistory);
    impl_->gfx_->Destroy(t.bloomScratchA);
    impl_->gfx_->Destroy(t.bloomScratchB);
    if (t.swap != gfx::SwapChainHandle::Invalid) {
        // color / colorLinear are owned by the swap-chain — released with it.
        impl_->gfx_->DestroySwapChain(t.swap);
    } else {
        // Offscreen color is ours; colorLinear aliases it, so destroy once.
        impl_->gfx_->Destroy(t.color);
    }
    if (impl_->activeTarget_ == &t)
        impl_->activeTarget_ = nullptr;
    if (impl_->primaryTargetId_ == id)
        impl_->primaryTargetId_ = 0;
    impl_->targets_.erase(it);
}

void RenderPipeline::ResizePrimaryTarget(i32 w, i32 h) {
    auto* target = PrimaryTarget();
    if (!target || !impl_->gfx_)
        return;

    auto& t = *target;
    impl_->gfx_->Destroy(t.depth);
    impl_->gfx_->Destroy(t.hdrColor);
    impl_->gfx_->Destroy(t.linearDepth);
    impl_->gfx_->Destroy(t.normalBuffer);
    impl_->gfx_->Destroy(t.aoBufferRaw);
    impl_->gfx_->Destroy(t.aoBufferDenoised);
    impl_->gfx_->Destroy(t.aoBuffer);
    impl_->gfx_->Destroy(t.aoBufferHistory);
    impl_->gfx_->Destroy(t.bentNormalBuffer);
    impl_->gfx_->Destroy(t.linearDepthHistory);
    impl_->gfx_->Destroy(t.bloomScratchA);
    impl_->gfx_->Destroy(t.bloomScratchB);

    if (t.swap != gfx::SwapChainHandle::Invalid) {
        impl_->gfx_->ResizeSwapChain(t.swap, w, h);
        t.color = impl_->gfx_->GetSwapChainBackBuffer(t.swap);
        t.colorLinear = impl_->gfx_->GetSwapChainBackBufferLinear(t.swap);
    } else {
        impl_->gfx_->Destroy(t.color);
        // Pure SD writes gamma to a UNORM target; HD (and SD-HDR) composite
        // post-tonemap to an _SRGB target (the tonemap relies on the RTV's
        // linear→sRGB encode).
        const bool sdGamma = rs_.Settings().GetRenderMode() == RenderMode::SD &&
                             !rs_.Settings().SceneHdrInSd();
        const gfx::Format offFmt =
            sdGamma ? gfx::Format::R8G8B8A8_UNORM : gfx::Format::R8G8B8A8_UNORM_SRGB;
        t.color = impl_->gfx_->CreateColorTarget(w, h, offFmt);
        t.colorLinear = t.color;
    }

    t.hdrColor = impl_->gfx_->CreateColorTarget(w, h, kHdrSceneFormat);
    t.depth = impl_->gfx_->CreateDepthTarget(w, h, impl_->depthStencilFormat_);
    t.linearDepth = impl_->gfx_->CreateColorTarget(w, h, kLinearDepthFormat);
    t.normalBuffer = impl_->gfx_->CreateColorTarget(w, h, kNormalBufferFormat);
    t.aoBufferRaw = impl_->gfx_->CreateColorTarget(w, h, kAoBufferFormat);
    t.aoBufferDenoised = impl_->gfx_->CreateColorTarget(w, h, kAoBufferFormat);
    t.aoBuffer = impl_->gfx_->CreateColorTarget(w, h, kAoBufferFormat);
    t.aoBufferHistory = impl_->gfx_->CreateColorTarget(w, h, kAoBufferFormat);
    t.bentNormalBuffer = impl_->gfx_->CreateColorTarget(w, h, kBentNormalBufferFormat);
    t.linearDepthHistory = impl_->gfx_->CreateColorTarget(w, h, kLinearDepthFormat);
    t.bloomScratchA = impl_->gfx_->CreateColorTarget(w, h, kHdrSceneFormat);
    t.bloomScratchB = impl_->gfx_->CreateColorTarget(w, h, kHdrSceneFormat);
    t.width = w;
    t.height = h;

    impl_->width_ = w;
    impl_->height_ = h;

    // History buffers were destroyed and recreated above; the GPU side
    // contains uninitialised memory. Force the temporal pass to
    // pass-through current AO on the next frame instead of EMA-blending
    // against undefined values.
    if (auto* g = rs_.GetGtaoService())
        g->ResetHistory();
}

void RenderPipeline::CleanupGFX() {

    // ImGui adapter releases its GPU resources through the device, so it
    // must go down before the device is dropped at the end of this routine.
    rs_.ShutdownImGui();

    ShutdownBlsShaders();

    if (impl_->gfx_) {

        impl_->gfx_->Destroy(impl_->lineVS_);
        impl_->gfx_->Destroy(impl_->linePS_);
        impl_->gfx_->Destroy(impl_->linePSOHdr_);
        impl_->gfx_->Destroy(impl_->linePSOSd_);
        impl_->gfx_->Destroy(impl_->tonemapPSO_);
        impl_->gfx_->Destroy(impl_->tonemapVB_);
        impl_->gfx_->Destroy(impl_->tonemapPsCb_);
        impl_->gfx_->Destroy(impl_->tonemapSampler_);
        impl_->tonemapPSO_ = gfx::PipelineHandle::Invalid;
        impl_->tonemapVB_ = gfx::BufferHandle::Invalid;
        impl_->tonemapPsCb_ = gfx::BufferHandle::Invalid;
        impl_->tonemapSampler_ = gfx::SamplerHandle::Invalid;

        impl_->capture_.Shutdown();

        impl_->gfx_->Destroy(impl_->cbPerFrame_);

        rs_.Debug().DestroyResources();

        // Tear down CornEffects FIRST — its emitters hold references
        // into the AssetManager (assets_.Release(assetSlot_) in
        // ~CornEffectsEmitter) and into the gfx device (via each
        // emitter's CornEffectsGfxBackend). ResetDeviceAssetManagers
        // below destroys the AssetManager; leaving the emitters alive
        // past that point would leave their dtors calling into a
        // destroyed AssetManager (mutex EINVAL → std::system_error →
        // std::terminate). Tested under MTL_DEBUG_LAYER=1 — the prior
        // order surfaced as a hard crash on shutdown on the Metal
        // backend; Vulkan happened to alias the freed memory in a way
        // that avoided the throw but the UAF was still there. Every scene's
        // corn service must be torn down, not just the active one.
        rs_.ForEachCornService([](corn_effects::CornEffectsService& corn) { corn.Clear(); });

        rs_.ResetDeviceAssetManagers();

        // CornEffects owns shared VB/IB/CBs — release before the device
        // dies. Service still receives backendInit on next InitDevice.
        rs_.ForEachCornService(
            [](corn_effects::CornEffectsService& corn) { corn.ReleaseGpuResources(); });

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
            impl_->gfx_->Destroy(t.linearDepth);
            impl_->gfx_->Destroy(t.normalBuffer);
            impl_->gfx_->Destroy(t.aoBufferRaw);
            impl_->gfx_->Destroy(t.aoBufferDenoised);
            impl_->gfx_->Destroy(t.aoBuffer);
            impl_->gfx_->Destroy(t.aoBufferHistory);
            impl_->gfx_->Destroy(t.bentNormalBuffer);
            impl_->gfx_->Destroy(t.linearDepthHistory);
            impl_->gfx_->Destroy(t.bloomScratchA);
            impl_->gfx_->Destroy(t.bloomScratchB);
        }
    }
    impl_->targets_.clear();
    impl_->primaryTargetId_ = 0;

    impl_->gfx_.reset();
}

bool RenderPipeline::CreateShaders() {
    using namespace whiteout::flakes::Shaders;

    // Per-backend bytecode selection:
    //   Vulkan   → SPIR-V
    //   WebGPU   → WGSL (UTF-8 source — CreateShader treats the buffer as
    //                    null-terminated text)
    //   Metal    → metallib (binary Apple Metal Library)
    //   D3D11/12 → DXBC sm_5_0
    const gfx::GfxApi api = impl_->gfx_->GetApi();
    const u8* vsBytes = kLineVS;
    usize vsSize = sizeof(kLineVS);
    const u8* psBytes = kLinePS;
    usize psSize = sizeof(kLinePS);
    if (api == gfx::GfxApi::Vulkan) {
        vsBytes = kLineVSSpv;
        vsSize = sizeof(kLineVSSpv);
        psBytes = kLinePSSpv;
        psSize = sizeof(kLinePSSpv);
    } else if (api == gfx::GfxApi::WebGPU) {
        vsBytes = kLineVSWgsl;
        vsSize = sizeof(kLineVSWgsl);
        psBytes = kLinePSWgsl;
        psSize = sizeof(kLinePSWgsl);
    } else if (api == gfx::GfxApi::Metal) {
        vsBytes = kLineVSMtl;
        vsSize = sizeof(kLineVSMtl);
        psBytes = kLinePSMtl;
        psSize = sizeof(kLinePSMtl);
    }

    impl_->lineVS_ = impl_->gfx_->CreateShader(gfx::ShaderStage::Vertex, vsBytes, vsSize);
    impl_->linePS_ = impl_->gfx_->CreateShader(gfx::ShaderStage::Pixel, psBytes, psSize);

    return impl_->lineVS_ != gfx::ShaderHandle::Invalid &&
           impl_->linePS_ != gfx::ShaderHandle::Invalid;
}

bool RenderPipeline::CreatePipelines() {
    using namespace gfx;

    InputElement lineInput[] = {
        {"POSITION", 0, Format::R32G32B32_FLOAT, 0},
        {"COLOR", 0, Format::R32G32B32A32_FLOAT, 12},
    };

    GraphicsPipelineDesc desc;
    desc.vs = impl_->lineVS_;
    desc.ps = impl_->linePS_;
    desc.inputLayout = lineInput;
    desc.topology = PrimitiveTopology::LineList;
    desc.blend.enable = false;
    desc.depthStencil = {};
    desc.rasterizer.cull = CullMode::None;
    desc.rasterizer.frontCCW = true;

    desc.dsvFormat = impl_->depthStencilFormat_;
    desc.rtvFormat = kHdrSceneFormat;
    // Lines drawn inside the HD scene pass land in the same MRT setup
    // as the BLS opaque draws (slot 1 = linearDepth, slot 2 = normal).
    // PSO must match the attachment count or Vulkan/WebGPU reject the
    // bind. The line shader doesn't output to SV_Target1/2; the gfx
    // backend's no-blend extra-slot handling masks writes to those
    // slots so the cleared G-buffer values survive intact.
    desc.extraRtvFormats[0] = kLinearDepthFormat;
    desc.extraRtvFormats[1] = kNormalBufferFormat;
    desc.extraRtvCount = 2;
    impl_->linePSOHdr_ = impl_->gfx_->CreateGraphicsPipeline(desc);

    // SD line PSO is built lazily by CurrentLinePSO against the actual
    // swap-chain RTV format (Metal-backed swap chains report BGRA8 even
    // when the rest of the engine targets RGBA8). Building it now with
    // kSdSceneFormat would mismatch BGRA8 renderpasses, fail SetPipeline,
    // and invalidate the whole command buffer.
    impl_->linePSOSd_ = PipelineHandle::Invalid;
    impl_->linePsoSdFormat_ = Format::Unknown;

    return impl_->linePSOHdr_ != PipelineHandle::Invalid;
}

gfx::PipelineHandle RenderPipeline::CurrentLinePSO() const {
    if (impl_->frameRenderMode_ == RenderMode::HD)
        return impl_->linePSOHdr_;

    const gfx::Format wantFmt = SceneTargetFormat();
    if (impl_->linePSOSd_ != gfx::PipelineHandle::Invalid &&
        impl_->linePsoSdFormat_ == wantFmt)
        return impl_->linePSOSd_;

    if (!impl_->gfx_ || impl_->lineVS_ == gfx::ShaderHandle::Invalid ||
        impl_->linePS_ == gfx::ShaderHandle::Invalid)
        return gfx::PipelineHandle::Invalid;

    if (impl_->linePSOSd_ != gfx::PipelineHandle::Invalid)
        impl_->gfx_->Destroy(impl_->linePSOSd_);

    static const gfx::InputElement lineInput[] = {
        {"POSITION", 0, gfx::Format::R32G32B32_FLOAT, 0},
        {"COLOR", 0, gfx::Format::R32G32B32A32_FLOAT, 12},
    };
    gfx::GraphicsPipelineDesc desc{};
    desc.vs = impl_->lineVS_;
    desc.ps = impl_->linePS_;
    desc.inputLayout = lineInput;
    desc.topology = gfx::PrimitiveTopology::LineList;
    desc.blend.enable = false;
    desc.depthStencil = {};
    desc.rasterizer.cull = gfx::CullMode::None;
    desc.rasterizer.frontCCW = true;
    desc.dsvFormat = impl_->depthStencilFormat_;
    desc.rtvFormat = wantFmt;
    impl_->linePSOSd_ = impl_->gfx_->CreateGraphicsPipeline(desc);
    impl_->linePsoSdFormat_ = wantFmt;
    return impl_->linePSOSd_;
}

bool RenderPipeline::CreateDefaultResources() {
    return rs_.Debug().CreateResources();
}

void RenderPipeline::RenderFrame(RenderTargetId targetId) {
    // Legacy single-viewport shim: render the scene's single camera into
    // targetId. Multi-viewport hosts call RenderViewport directly.
    Viewport vp;
    vp.target = targetId;
    vp.camera = &rs_.Scene().Camera();
    RenderViewport(vp);
}

void RenderPipeline::RenderViewport(const Viewport& vp) {
    WDX_CPU_ZONE("RenderFrame");
    // Stutter detector: at the start of each frame, snapshot the
    // PSO builder's runtime cache-miss counter. If it changes during
    // this frame, the driver synchronously compiled at least one PSO
    // mid-frame — the canonical stutter source. Print the delta with
    // the frame index so a transient hitch is greppable in stderr.
    static thread_local u64 frameCounter = 0;
    const u64 builderBuildsAtFrameStart =
        impl_->blsPsoBuilder_ ? impl_->blsPsoBuilder_->Stats().runtimeCacheBuilds : 0;

    const RenderTargetId targetId = vp.target;
    auto it = impl_->targets_.find(targetId);
    if (it == impl_->targets_.end())
        return;
    auto& target = it->second;
    if (target.color == gfx::TextureHandle::Invalid || !impl_->gfx_)
        return;
    if (target.hdrColor == gfx::TextureHandle::Invalid)
        return;

    // Publish the scene + target + camera being rendered so the pass pipeline,
    // effect services (rs_.Particles()/CornEffects()/Spn()), Width()/Height()/
    // SceneTargetFormat()/FrameCamera() resolve against THIS viewport's scene
    // rather than the default scene / primary target / scene camera. Set the
    // active scene first so the camera fallback below reads the right scene's
    // camera. The guard restores the default scene + clears the others on every
    // exit path so out-of-frame queries fall back sanely.
    rs_.SetActiveScene(vp.scene);
    impl_->activeTarget_ = &target;
    impl_->activeCamera_ = vp.camera ? vp.camera : &rs_.Scene().Camera();
    impl_->frameDrawImGui_ = vp.drawImGui;
    struct ActiveFrameGuard {
        RenderService* rs;
        const RenderTarget** target;
        const Camera** camera;
        ~ActiveFrameGuard() {
            *target = nullptr;
            *camera = nullptr;
            rs->SetActiveScene(rs->DefaultSceneId());
        }
    } activeFrameGuard{&rs_, &impl_->activeTarget_, &impl_->activeCamera_};

    // Commit any pending GPU uploads (textures + geometry the loader staged
    // since the last frame) before we start rendering this frame.
    {
        WDX_CPU_ZONE("CommitUploads");
        rs_.Loader().CommitPendingUploads();
        // CommitPendingUploads Acquires the actor's textures into
        // AssetManager slots. On desktop, pump those needs through the
        // content provider right now so the new actor's first frame
        // sees real textures instead of placeholders. No-op on
        // Emscripten — the JS host owns the drain.
        rs_.PumpAssetsViaProvider();
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
    // Render-mode flip invalidates GTAO's history (the G-buffer wasn't populated
    // during SD frames, and the previous-camera matrices map onto the
    // now-different opaque scene). Detect this PER TARGET, not globally: GTAO
    // history is per-target, so only the target whose OWN mode changed should
    // drop its history. A global reset here thrashes every target's history every
    // frame in a mixed-mode multi-viewport host — e.g. an embedded HD thumbnail
    // grid rendering while the main document is SD, which flips the global mode
    // twice per frame and would otherwise never let the thumbnails' temporal
    // denoise accumulate (HD-only shimmer).
    if (target.gtao.modeKnown && target.gtao.lastMode != impl_->frameRenderMode_)
        target.gtao.prevValid = false; // our mode changed; our last AO is stale
    target.gtao.lastMode = impl_->frameRenderMode_;
    target.gtao.modeKnown = true;
    // `useHdr` gates the HD-only deferred machinery: the 3-RTV G-buffer, GTAO
    // and bloom (all need the linearDepth/normal slots the HD opaque pass
    // populates). `sceneToHdr` is the looser question of "does the scene land
    // in the HDR target and get tonemapped?" — true for HD, and also for SD
    // when the host opts in (SceneHdrInSd). SD-HDR keeps the single-RTV forward
    // SD shading but routes it through the tonemap so additive / team-color
    // geosets roll off instead of clipping to white on the LDR swap chain.
    const bool useHdr = (impl_->frameRenderMode_ == RenderMode::HD);
    const bool sceneToHdr = useHdr || rs_.Settings().SceneHdrInSd();

    // Frame capture (off unless exporting): BeginFrame redirects the final
    // composite to an off-screen target and EndFrame (below) copies it out +
    // mirrors it onto the swap chain. Disabled is the zero-cost default —
    // BeginFrame just hands back the back buffer and EndFrame no-ops.
    // SD writes gamma straight to a UNORM target; HD composites post-tonemap to
    // the sRGB swap format. Tell the capture redirect which to build, and
    // whether its copy must re-encode to sRGB (HD) or pass gamma bytes through
    // (SD). Set before BeginFrame so EnsureResources builds the right target.
    const bool sdGamma = !sceneToHdr; // SD direct-to-LDR path (gamma-space)
    if (target.swap != gfx::SwapChainHandle::Invalid) {
        const gfx::Format swapFmt = impl_->gfx_->GetSwapChainFormat(target.swap);
        impl_->capture_.SetRedirectColorFormat(sdGamma ? StripSrgb(swapFmt) : swapFmt);
    }
    impl_->capture_.SetSrgbEncode(!sdGamma);

    const gfx::TextureHandle finalColor = impl_->capture_.BeginFrame(target);
    // SD is gamma-space: it writes gamma values that must NOT be re-encoded, so
    // it renders through a plain UNORM view. The swap chain exposes that as
    // `colorLinear`; the only non-capture swap-chain frame still points at the
    // _SRGB view (`color`), so substitute the UNORM view there. Capture and
    // offscreen targets are already UNORM (FrameCapture / kSdSceneFormat).
    gfx::TextureHandle sceneTarget;
    if (sceneToHdr) {
        sceneTarget = target.hdrColor;
    } else if (target.swap != gfx::SwapChainHandle::Invalid && finalColor == target.color) {
        sceneTarget = target.colorLinear;
    } else {
        sceneTarget = finalColor;
    }

    auto srgbByteToLinear = [](u8 b) {
        const f32 f = b / 255.0f;
        return (f <= 0.04045f) ? (f / 12.92f) : std::pow((f + 0.055f) / 1.055f, 2.4f);
    };

    auto acesInverse = [](f32 y) {
        if (y <= 0.0f)
            return 0.0f;
        if (y >= 1.0f)
            y = 0.9999f;
        const f32 A = 2.43f * y - 2.51f;
        const f32 B = 0.59f * y - 0.03f;
        const f32 C = 0.14f * y;
        if (std::abs(A) < 1e-6f) {
            return (std::abs(B) > 1e-6f) ? -C / B : 0.0f;
        }
        const f32 disc = B * B - 4.0f * A * C;
        if (disc < 0.0f)
            return 0.0f;
        const f32 r = std::sqrt(disc);
        const f32 x = (-B - r) / (2.0f * A);
        return x > 0.0f ? x : 0.0f;
    };
    const u32 bg = rs_.Settings().BackgroundColorRaw();
    const u8 rB = static_cast<u8>(bg & 0xFF);
    const u8 gB = static_cast<u8>((bg >> 8) & 0xFF);
    const u8 bB = static_cast<u8>((bg >> 16) & 0xFF);
    auto hdrClear = [&](u8 byte) {
        const f32 linTarget = srgbByteToLinear(byte);
        const f32 exposure = rs_.Settings().GetTonemapExposure();
        const f32 invExp = (exposure > 1e-6f) ? 1.0f / exposure : 1.0f;
        return acesInverse(linTarget) * invExp;
    };

    // SD writes gamma to a UNORM target (no sRGB encode on write or in the
    // capture copy), so the background clear is the gamma byte value directly —
    // it then reads back as the picked colour, matching the HD tonemap path.
    auto sdClear = [&](u8 byte) { return byte / 255.0f; };
    f32 clearColor[4] = {
        sceneToHdr ? hdrClear(rB) : sdClear(rB),
        sceneToHdr ? hdrClear(gB) : sdClear(gB),
        sceneToHdr ? hdrClear(bB) : sdClear(bB),
        1.0f,
    };

    if (rs_.GetShadowService() && rs_.GetShadowService()->IsEnabled()) {
        WDX_CPU_ZONE("ShadowPass");
        WDX_GPU_ZONE(cmd, "ShadowPass");
        Matrix44f csmCamView, csmCamProj;
        {
            const f32 aspect = (target.height > 0) ? (f32)target.width / (f32)target.height : 1.0f;
            csmCamView = rs_.Pipeline().FrameCamera().ViewLH();
            csmCamProj = rs_.Pipeline().FrameCamera().ProjectionLH(aspect);
        }

        // Shadow light direction is driven entirely by an analytic
        // TOD path so the host's time-of-day slider visibly moves the
        // shadow. The W3 DnC MDLs (DNCLordaeronUnit.mdl etc.) carry a
        // single static rotation keyframe on the FDirectSun light
        // node — the engine `SpriteGetLightDir` reads it back as a
        // fixed vector and shadows never swing with TOD in-game.
        // That's faithful to W3 but bad UX in a viewer, so we ignore
        // the DnC asset for direction (we still consume it for light
        // colour / intensity in the BLS lit pass).
        //
        // Sun arc runs along the Y axis with a constant forward
        // bias so the front of the model stays lit at every TOD
        // (Blizzard LH, Z up). Sunrise sits low-front (-Y dominant
        // with the bias), noon overhead, sunset comes back through
        // the front rather than swinging behind. Tiny +X tilt
        // prevents noon shadows from collapsing straight down; the
        // Z floor keeps shadows from inverting past sunset.
        // Same front-biased sun the geoset shading baseline uses (see
        // ComputeSunDirWS) so the cast shadow and the lit side agree.
        const Vector3f lightDirWS = ComputeSunDirWS(rs_.GetDncService());

        // Center the shadow cascade on the camera target (the host's "what
        // they're looking at"). Multi-actor scenes can have no single hero,
        // so we don't probe a focus actor here — orbital target is the most
        // useful proxy and the host can move it explicitly if needed.
        const Vector3f camTarget = rs_.Pipeline().FrameCamera().GetTarget();
        Vector3f sceneCenter = {camTarget.x, camTarget.y, camTarget.z};
        f32 sceneRadius = 150.0f;
        if (rs_.Pipeline().FrameCamera().GetMode() == Camera::Mode::Orbital) {
            sceneRadius = std::max(50.0f, rs_.Pipeline().FrameCamera().GetDistance() * 0.6f);
        }
        rs_.GetShadowService()->Update(csmCamView, csmCamProj, rs_.Pipeline().FrameCamera().GetNearZ(),
                                       rs_.Pipeline().FrameCamera().GetFarZ(), lightDirWS, sceneCenter,
                                       sceneRadius);
        shadow::ShadowPass(rs_).Run(*rs_.GetShadowService());
    }

    // G-buffer MRT bind. In HD mode the scene pass writes 3 colour
    // attachments — scene HDR (slot 0), linear view-space depth (slot 1,
    // R32F), encoded world-space normal (slot 2, RGBA8). Mirrors the
    // engine's s_worldFBHD layout. In SD mode we have only slot 0 (the
    // swap chain), and the linearDepth/normalBuffer textures are still
    // created but unused. The MRT-capable BLS pixel shaders (the
    // WC3_IS_MRT permutations) are already selected by the existing
    // `depthWrite=true` opaque path — they emit SV_Target1/2 values that
    // we now actually capture instead of silently discarding.
    if (useHdr) {
        const gfx::TextureHandle colors[3] = {sceneTarget, target.linearDepth,
                                              target.normalBuffer};
        // Slot 0: hdr clear (already in `clearColor`).
        // Slot 1: linear depth — clear to a far-distance sentinel. The
        //         buffer carries real view-space Z (HD opaque PS writes
        //         input.worldPos.z, which is view-space despite the
        //         field name — see vs_body.slang). GTAO treats anything
        //         past 1e4 as "no opaque draw landed here".
        // Slot 2: normal — clear to {0.5, 0.5, 1.0, 0} (encoded +Z up).
        f32 clearColors[3][4] = {
            {clearColor[0], clearColor[1], clearColor[2], clearColor[3]},
            {1.0e5f, 0.0f, 0.0f, 0.0f},
            {0.5f, 0.5f, 1.0f, 0.0f},
        };
        cmd->BeginRenderPass(colors, 3, target.depth, clearColors, 1.0f, 0);
    } else {
        cmd->BeginRenderPass(sceneTarget, target.depth, clearColor, 1.0f, 0);
    }
    cmd->SetViewport({0, 0, (f32)target.width, (f32)target.height, 0, 1});
    WDX_GPU_ZONE(cmd, "ScenePass");

    Matrix44f view, proj;
    {
        // GTAO must match the HD opaque scene pass's view-space
        // convention. That pass uses LH view+proj (see ComputeViewProj
        // below) and writes the LH view-space Z into linearDepth.
        // Passing RH here would have `viewPosFromUV` reconstruct
        // sign-flipped Z and invert every horizon dot product — the
        // user-visible symptom is AO appearing inverted (convex edges
        // dark, concave creases bright) and shifting with camera angle.
        view = rs_.Pipeline().FrameCamera().ViewLH();
    }
    f32 aspect = (target.height > 0) ? (f32)target.width / (f32)target.height : 1.0f;
    proj = rs_.Pipeline().FrameCamera().ProjectionLH(aspect);

    {
        render_detail::CbPerFrameDesc d;
        d.view = view;
        d.projection = proj;
        d.lightDir = render_detail::NormalizedLightDir4(kDefaultLightDir);
        d.lightColor = kGeosetLightColor;
        d.ambientColor = {kGeosetAmbientColor.x, kGeosetAmbientColor.y, kGeosetAmbientColor.z,
                          0.0f};
        render_detail::WriteCbPerFrame(impl_->gfx_.get(), impl_->cbPerFrame_, d);
    }

    if (rs_.Settings().ShowGrid()) {
        WDX_GPU_ZONE(cmd, "Grid");
        rs_.Debug().RenderGrid();
    }

    {
        WDX_CPU_ZONE("GeosetsOpaque");
        WDX_GPU_ZONE(cmd, "GeosetsOpaque");
        RenderGeosets(GeosetBucket::Opaque);
    }
    if (rs_.Settings().ShowEvents()) {
        WDX_CPU_ZONE("Splats");
        WDX_GPU_ZONE(cmd, "Splats");
        RenderSplatsBls();
    }
    {
        // Unified back-to-front transparent pass: transparent geosets, PE2
        // particles, ribbons and corn interleaved by depth (WC3's
        // IModelRenderSceneTransparent). ShowParticles/ShowRibbons gate the
        // respective producers inside.
        WDX_CPU_ZONE("Transparent");
        WDX_GPU_ZONE(cmd, "Transparent");
        RenderTransparentScene();
    }
    if (rs_.Settings().ShowCollisions())
        rs_.Debug().RenderCollisions();
    if (rs_.Settings().ShowLights())
        rs_.Debug().RenderLightMarkers();

    // SD path lands the scene directly on the swap chain backbuffer, so
    // ImGui draws stay inside this render pass. In HD mode the equivalent
    // call lives at the tail of RunTonemapPass — same RTV, just reached
    // via the tonemap composite. Compiled out when WDX_ENABLE_IMGUI=0 (web
    // build) — the forward-declared ImGuiRenderer has no complete type then.
#if WDX_ENABLE_IMGUI
    if (!sceneToHdr && impl_->frameDrawImGui_) {
        if (auto* im = rs_.ImGui()) {
            // Sync ImGui's PSO with the scene RTV format (which is the
            // swapchain backbuffer in SD mode). See the matching call in
            // RunTonemapPass for why this can't be locked in once at
            // EnsureImGui time.
            im->SetRtvFormat(SceneTargetFormat());
            WDX_GPU_ZONE(cmd, "ImGui");
            im->Render(*cmd, target.width, target.height);
        }
    }
#endif
    cmd->EndRenderPass();

    // GTAO ambient-occlusion. HD-only — needs the G-buffer slots populated
    // by the HD opaque MRT pass that just closed. Internally:
    //   1. reads target.linearDepth + target.normalBuffer as SRVs and
    //      writes scalar AO to target.aoBuffer.
    //   2. opens target.hdrColor with loadOp=Load and multiplies the AO
    //      into the existing colour via Zero-src + SrcColor-dst blend.
    if (useHdr) {
        if (auto* g = rs_.GetGtaoService()) {
            // Forward the user-facing toggle into the service each frame
            // — cheap atomic load + cheap setter, and keeps the service
            // state from drifting if the host flips the bool from
            // another thread (settings UI / .ini reload).
            const bool aoOnly = (rs_.Settings().HdDebugMode() == 9);
            g->SetEnabled(rs_.Settings().AoEnabled() || aoOnly);
            g->SetDebugAoOnly(aoOnly);
            {
                const u32 q = rs_.Settings().AoQuality();
                const u32 qMax = static_cast<u32>(gtao::Quality::Count) - 1;
                g->SetQuality(static_cast<gtao::Quality>(q > qMax ? qMax : q));
            }
            {
                gtao::GtaoParams params = g->Params();
                params.bentBoost = rs_.Settings().AoBentBoost();
                g->SetParams(params);
            }
            if (g->IsEnabled()) {
                WDX_CPU_ZONE("GTAO");
                WDX_GPU_ZONE(cmd, "GTAO");
                g->Run(cmd, target, view, proj, static_cast<u32>(frameCounter));

                // Post-process IBL bent-normal boost. Same probe-pair
                // selection logic as the BLS HD lit pass (see
                // GeosetPassHd::BindPassResources): day/night blend when
                // DnC is active, otherwise the static from/to probes.
                if (g->Params().bentBoost > 0.0f) {
                    gtao::GtaoService::IblBoostInputs in;
                    const bool useDayNight = impl_->iblDayNightLoaded_ &&
                                             rs_.GetDncService() != nullptr &&
                                             rs_.Settings().GetLightingMode() ==
                                                 LightingMode::InGame;
                    if (useDayNight) {
                        const auto blend = rs_.GetDncService()->ComputeEnvMapBlend();
                        const bool dayPrimary = blend.isDaytime;
                        in.envFromMipEnd =
                            dayPrimary ? impl_->iblDayMipEnd_ : impl_->iblNightMipEnd_;
                        in.envToMipEnd =
                            dayPrimary ? impl_->iblNightMipEnd_ : impl_->iblDayMipEnd_;
                        in.envTransitionT = blend.transitionT;
                        const auto day = rs_.Textures().GetOwned(kIblDayProbeName);
                        const auto night = rs_.Textures().GetOwned(kIblNightProbeName);
                        in.iblFrom = dayPrimary ? day : night;
                        in.iblTo = dayPrimary ? night : day;
                    } else {
                        in.envFromMipEnd = impl_->iblProbeMipEnd_;
                        in.envToMipEnd = impl_->iblProbeMipEnd_;
                        in.envTransitionT = 0.75f;
                        in.iblFrom = rs_.Textures().GetOwned(kIblFromProbeName);
                        in.iblTo = rs_.Textures().GetOwned(kIblToProbeName);
                        if (in.iblTo == gfx::TextureHandle::Invalid)
                            in.iblTo = in.iblFrom;
                    }
                    in.iblSampler = rs_.Samplers().LinearWrap();
                    if (in.iblFrom != gfx::TextureHandle::Invalid &&
                        in.iblTo != gfx::TextureHandle::Invalid) {
                        WDX_CPU_ZONE("GTAO IBL Boost");
                        WDX_GPU_ZONE(cmd, "GTAO IBL Boost");
                        g->RunIblBoost(cmd, target, in);
                    }
                }
            }
        }
    }

    // Depth of field — bokeh blur on `hdrColor` after GTAO/SSAO and before
    // bloom, exactly where WC3 runs GBuffer::ApplyDepthOfField (opaque → SSAO →
    // DoF → bloom). Forwards host-side `RenderSettings::Dof*` knobs into the
    // service; the pass self-disables unless a focal distance is set.
    if (useHdr) {
        if (auto* d = rs_.GetDofService()) {
            dof::DofParams dp = d->Params();
            dp.enabled = rs_.Settings().DofEnabled();
            dp.focusDistance = rs_.Settings().DofFocusDistance();
            dp.focusScale = rs_.Settings().DofFocusScale();
            dp.maxBlurSize = rs_.Settings().DofMaxBlurSize();
            dp.radiusScale = rs_.Settings().DofRadiusScale();
            dp.farFieldOnly = rs_.Settings().DofFarFieldOnly();
            d->SetParams(dp);
            if (d->IsEnabled()) {
                WDX_CPU_ZONE("DepthOfField");
                WDX_GPU_ZONE(cmd, "DepthOfField");
                d->Run(cmd, target);
            }
        }
    }

    // HDR post-process — bloom runs on `hdrColor` between GTAO and
    // tonemap, matching the engine ordering (GBuffer::ApplyBloom is the
    // last thing before ApplyTonemap in OnPaint). Service forwards
    // host-side `RenderSettings::Bloom*` knobs into its CB.
    if (useHdr) {
        if (auto* pp = rs_.GetPostProcessService()) {
            post_process::BloomParams bp = pp->Params();
            bp.enabled = rs_.Settings().BloomEnabled();
            bp.threshold = rs_.Settings().BloomThreshold();
            bp.intensity = rs_.Settings().BloomIntensity();
            bp.saturation = rs_.Settings().BloomSaturation();
            pp->SetParams(bp);
            if (pp->IsBloomEnabled()) {
                WDX_CPU_ZONE("Bloom");
                WDX_GPU_ZONE(cmd, "Bloom");
                pp->RunBloom(cmd, target);
            }
        }
    }

    if (sceneToHdr) {
        WDX_CPU_ZONE("Tonemap");
        WDX_GPU_ZONE(cmd, "Tonemap");
        RunTonemapPass(target, finalColor);
    }

    // Copy the off-screen composite into the readback ring and mirror it
    // onto the swap chain. No-op unless capture redirected this frame.
    {
        WDX_CPU_ZONE("FrameCapture");
        impl_->capture_.EndFrame(target);
    }

    ++frameCounter;
    if (impl_->blsPsoBuilder_) {
        const u64 builderBuildsAtFrameEnd = impl_->blsPsoBuilder_->Stats().runtimeCacheBuilds;
        const u64 delta = builderBuildsAtFrameEnd - builderBuildsAtFrameStart;
        if (delta > 0) {
            std::fprintf(stderr,
                         "[bls] frame %llu: %llu mid-frame PSO compile(s) "
                         "(every one is a synchronous driver stall — stutter source)\n",
                         (unsigned long long)frameCounter, (unsigned long long)delta);
        }
    }
}

void RenderPipeline::RunTonemapPass(const RenderTarget& target, gfx::TextureHandle dstColor) {
    if (!impl_->blsSpriteVs_ || !impl_->blsTonemapPs_ ||
        impl_->blsSpriteVs_->permuteHandles.empty() ||
        impl_->blsTonemapPs_->permuteHandles.empty())
        return;

    // Lazy-build the tonemap PSO against the actual swap-chain format. Some
    // backends only expose BGRA8 (Metal via MoltenVK, WebGPU/Dawn-D3D12),
    // others give RGBA8 — a single hardcoded rtvFormat at CreateShaders
    // time misses on the BGRA8 platforms and silently drops every present.
    const gfx::Format dstFormat = (target.swap != gfx::SwapChainHandle::Invalid)
                                      ? impl_->gfx_->GetSwapChainFormat(target.swap)
                                      : kSdSceneFormat;
    if (impl_->tonemapPSO_ == gfx::PipelineHandle::Invalid ||
        impl_->tonemapPsoFormat_ != dstFormat) {
        if (impl_->tonemapPSO_ != gfx::PipelineHandle::Invalid)
            impl_->gfx_->Destroy(impl_->tonemapPSO_);
        const gfx::InputElement spriteInput[] = {
            {"ATTR", 0, gfx::Format::R32G32B32_FLOAT, 0},
            {"ATTR", 3, gfx::Format::R32G32_FLOAT, 12},
        };
        gfx::GraphicsPipelineDesc tm;
        tm.vs = impl_->blsSpriteVs_->permuteHandles[0];
        tm.ps = impl_->blsTonemapPs_->permuteHandles[0];
        tm.inputLayout = spriteInput;
        tm.topology = gfx::PrimitiveTopology::TriangleList;
        tm.blend.enable = false;
        tm.depthStencil.depthTest = false;
        tm.depthStencil.depthWrite = false;
        tm.rasterizer.cull = gfx::CullMode::None;
        tm.rasterizer.frontCCW = true;
        tm.rtvFormat = dstFormat;
        tm.dsvFormat = impl_->depthStencilFormat_;
        impl_->tonemapPSO_ = impl_->gfx_->CreateGraphicsPipeline(tm);
        impl_->tonemapPsoFormat_ = dstFormat;
        if (impl_->tonemapPSO_ == gfx::PipelineHandle::Invalid)
            return;
    }
    auto* cmd = impl_->gfx_->GetImmediateContext();

    const f32 clearLdr[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    cmd->BeginRenderPass(dstColor, gfx::TextureHandle::Invalid, clearLdr, 1.0f, 0);
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
    cmd->BindSampler(gfx::ShaderStage::Pixel, 0, impl_->tonemapSampler_);
    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 1, impl_->tonemapPsCb_);
    cmd->Draw(3, 0);

    // Submit ImGui inside the same render pass so we don't have to open a
    // second pass with loadOp=Load. The RTV is the swap chain backbuffer
    // (sRGB), which the imgui PSO was built against. No-op if the host
    // hasn't created an ImGui context or there are no draw lists this
    // frame. Compiled out when WDX_ENABLE_IMGUI=0.
#if WDX_ENABLE_IMGUI
    if (impl_->frameDrawImGui_) if (auto* im = rs_.ImGui()) {
        // Sync ImGui's PSO format with whatever the swapchain landed on.
        // No-op when the format hasn't changed; rebuilds the PSO when it
        // has (Metal-backed swapchains return BGRA8 even when the rest of
        // the engine targets RGBA8).
        im->SetRtvFormat(dstFormat);
        WDX_GPU_ZONE(cmd, "ImGui");
        im->Render(*cmd, target.width, target.height);
    }
#endif
    cmd->EndRenderPass();
}

bool RenderPipeline::ReadbackTarget(RenderTargetId id, std::vector<u8>& outRgba, i32& width,
                                    i32& height) {
    auto it = impl_->targets_.find(id);
    if (it == impl_->targets_.end() || !impl_->gfx_)
        return false;
    auto& t = it->second;
    width = t.width;
    height = t.height;
    return impl_->gfx_->ReadbackTexture(t.color, t.width, t.height, outRgba);
}

gfx::TextureHandle RenderPipeline::GetTargetColorTexture(RenderTargetId id) const {
    auto it = impl_->targets_.find(id);
    return it != impl_->targets_.end() ? it->second.color : gfx::TextureHandle::Invalid;
}

void RenderPipeline::Present(RenderTargetId targetId) {
    auto it = impl_->targets_.find(targetId);
    if (it == impl_->targets_.end() || !impl_->gfx_)
        return;
    if (it->second.swap != gfx::SwapChainHandle::Invalid) {
        impl_->gfx_->Present(it->second.swap);
    } else {
        // Headless target: no swap chain to present, but on deferred-submit
        // backends the frame's recorded commands still need flushing to the
        // queue (Present is what submits them). Without this an off-screen
        // viewport's work never executes and a capture readback sees nothing.
        impl_->gfx_->SubmitFrame();
    }
}

// ---- Frame capture ---------------------------------------------------------
// Thin forwarders — the implementation lives in FrameCapture (frame_capture.h).

void RenderPipeline::EnableFrameCapture(bool enable) {
    impl_->capture_.SetEnabled(enable);
}

bool RenderPipeline::IsFrameCaptureEnabled() const {
    return impl_->capture_.IsEnabled();
}

i32 RenderPipeline::FrameCaptureRingSize() const {
    return impl_->capture_.RingSize();
}

i32 RenderPipeline::LastCapturedSlot() const {
    return impl_->capture_.LastSlot();
}

bool RenderPipeline::DownloadCaptureSlot(i32 slot, std::vector<u8>& outRgba, i32& width,
                                         i32& height) {
    return impl_->capture_.DownloadSlot(slot, outRgba, width, height);
}

class GeosetPassBls : public BlsGeosetPass<GeosetPassBls> {
public:
    using BlsGeosetPass::BlsGeosetPass;

    bool IsAvailable() const {
        return rs_.Pipeline().impl_->blsSdProgram_ && rs_.Pipeline().impl_->blsPsoBuilder_;
    }

    void ComputeViewProj(Matrix44f& view, Matrix44f& proj) const {
        view = rs_.Pipeline().FrameCamera().GetViewMatrix();
        const f32 aspect = (rs_.Pipeline().Height() > 0)
                               ? static_cast<f32>(rs_.Pipeline().Width()) /
                                     static_cast<f32>(rs_.Pipeline().Height())
                               : 1.0f;
        proj = rs_.Pipeline().FrameCamera().ProjectionRH(aspect);
    }

    void BindPassResources(gfx::IGFXCommandList*, bls::FrameInputs&) const {}

    bls::BaselineLights Baseline(const Matrix44f& view) const {

        if (auto* dnc = rs_.GetDncService();
            dnc && dnc->HasAsset() && rs_.Settings().GetLightingMode() == LightingMode::InGame) {
            const auto sample = dnc->SampleNow();
            if (sample.valid) {
                // Light *from* the front-biased sun (toward-light = -travel),
                // keeping the DNC's day/night ambient+diffuse colour. Uses the
                // same sun as the shadow pass so the lit side and the cast
                // shadow agree, and the model's front stays illuminated.
                const Vector3f sunWS = ComputeSunDirWS(dnc);
                const Vector3f dirVS = whiteout::transform_normal(
                    Vector3f{-sunWS.x, -sunWS.y, -sunWS.z}, view);
                return {sample.ambient, sample.diffuse, dirVS};
            }
        }

        return {
            {kGeosetAmbientColor.x, kGeosetAmbientColor.y, kGeosetAmbientColor.z},
            {kGeosetLightColor.x, kGeosetLightColor.y, kGeosetLightColor.z},
            {0.0f, 0.0f, 1.0f},
        };
    }

    // ---- List-driven submission (RunLists dispatches here) ----------------
    // DrawOpaqueItem / DrawTransparentItem name *what* to draw; EmitLayers is
    // the single cohesive submission path (one geoset, one depth-fill mode);
    // DrawLayer issues the PSO + draw for one resolved layer.

    void DrawOpaqueItem(const render_detail::OpaqueItem& item, bls::FrameInputs& frame,
                        const Matrix44f& viewMat, gfx::IGFXCommandList* cmd,
                        const bls::BaselineLights& baseline) {
        const auto& view_ = *item.view;
        EmitLayers(view_, (*view_.geosets)[item.geoIdx], bls::DepthFill::None, frame, viewMat, cmd,
                   baseline);
    }

    void DrawTransparentItem(const render_detail::TransparentItem& item, bls::FrameInputs& frame,
                             const Matrix44f& viewMat, gfx::IGFXCommandList* cmd,
                             const bls::BaselineLights& baseline) {
        const auto& view_ = *item.view;
        EmitLayers(view_, (*view_.geosets)[item.geoIdx], item.depthFill, frame, viewMat, cmd,
                   baseline);
    }

    // Draw a geoset's visible layers (in order) under `depthFill`. The per-layer
    // fade promotion + depth-only prepass live entirely in ResolveLayerMaterial —
    // there is no special-case prepass loop here.
    void EmitLayers(const render_detail::RenderableView& view_, const GPUGeoset& geo,
                    bls::DepthFill depthFill, bls::FrameInputs& frame, const Matrix44f& viewMat,
                    gfx::IGFXCommandList* cmd, const bls::BaselineLights& baseline) {
        const GPUMaterial* mat = nullptr;
        if (geo.materialId >= 0 && geo.materialId < (i32)view_.materials->size())
            mat = &(*view_.materials)[geo.materialId];

        const f32 geoAlpha = geo.geosetAlpha * view_.parentVisibility;
        if (geoAlpha <= 0.0f)
            return;

        i32 numLayers = mat ? (i32)mat->cpu.layers.size() : 0;
        if (numLayers <= 0)
            numLayers = 1;

        // Pick the bone palette CB the actor owns: per-actor on Path A (on the
        // SkinningSystem), per-geoset on Path B. Without this the SD path saw
        // only `geo.bonePaletteCb` (Invalid for Path A) and rendered bind pose.
        gfx::BufferHandle paletteCb = geo.bonePaletteCb;
        if (view_.skinning && view_.skinning->UsesPerActorPalette())
            paletteCb = view_.skinning->ActorPaletteCb();
        const bool hasBones = render_detail::BindSdMeshGeometry(cmd, geo, paletteCb);
        const auto layout =
            hasBones ? bls::VertexLayoutKind::ParticleSDSkinned : bls::VertexLayoutKind::ParticleSD;

        const i32 lightCount = bls::BuildLightPalette(frame, *view_.activeLights, viewMat, baseline,
                                                      rs_.Settings().GetLightingMode());

        for (i32 li = 0; li < numLayers; ++li) {
            const render_detail::UnpackedLayer layer = render_detail::UnpackLayer(mat, li);
            const f32 combinedAlpha = geoAlpha * layer.alpha;
            if (combinedAlpha < 0.004f)
                continue;

            const bls::MatParams base =
                bls::FromMdxLayer(layer.filterMode, layer.flags, bls::GxShaderID::SD);
            const Vector4f color = {geo.geosetColor.x, geo.geosetColor.y, geo.geosetColor.z,
                                    combinedAlpha};
            // isOpaqueFading only feeds the Color (HD) path; SD passes None.
            const bool isOpaqueFading =
                combinedAlpha < 0.99f && bls::FilterToGxAlpha(layer.filterMode) < bls::GxMatAlpha::Blend;
            const bls::MatParams mp =
                bls::ResolveLayerMaterial(depthFill, base, isOpaqueFading, color);

            const bool unlit = (mp.disables & bls::kDisableLighting) != 0;
            const i32 activeN = unlit ? 0 : lightCount;
            DrawLayer(view_, geo, layer, mp, activeN, unlit, hasBones, layout, frame, cmd);
        }
    }

    void DrawLayer(const render_detail::RenderableView& view_, const GPUGeoset& geo,
                   const render_detail::UnpackedLayer& layer, const bls::MatParams& matParams,
                   i32 activeN, bool unlit, bool hasBones, bls::VertexLayoutKind layout,
                   bls::FrameInputs& frame, gfx::IGFXCommandList* cmd) {
        auto* impl = rs_.Pipeline().impl_.get();
        frame.numLights = activeN;
        render_detail::ApplyTexAnimPaletteToFrame(frame, view_.texAnimPalette,
                                                  layer.textureAnimationId);

        const auto rsLocal = bls::MakeSdMeshRenderState(matParams, activeN, unlit, hasBones);
        const auto permLocal = bls::SelectPermutes(rsLocal);
        auto reqLocal = bls::MakePsoRequest(impl->blsSdProgram_, layout, matParams, permLocal);

        reqLocal.rtvFormat = rs_.Pipeline().SceneTargetFormat();
        // HD mode binds a 3-RT G-buffer; the SD-on-HD PSO must match the
        // attachment count even though it only writes SV_Target0.
        if (impl->frameRenderMode_ == RenderMode::HD) {
            reqLocal.extraRtvFormats[0] = RenderPipeline::kLinearDepthFormat;
            reqLocal.extraRtvFormats[1] = RenderPipeline::kNormalBufferFormat;
            reqLocal.extraRtvCount = 2;
        }
        reqLocal.dsvFormat = impl->depthStencilFormat_;
        auto pso = impl->blsPsoBuilder_->GetOrBuild(reqLocal);
        if (pso == gfx::PipelineHandle::Invalid)
            return;
        cmd->BindPipeline(pso);
        cmd->BindVertexBuffer(0, render_detail::PickSlot0Vb(geo, layer.coordId), sizeof(Vertex));

        frame.world = view_.worldTransform;
        if (auto vs = bls::ScopedCb<bls::SdVsCbA>(rs_.Pipeline().Gfx(), impl->blsSdVsCb_))
            bls::BuildSdVsCbA(*vs, frame, matParams);
        if (auto ps = bls::ScopedCb<bls::SdPsCbA>(rs_.Pipeline().Gfx(), impl->blsSdPsCb_))
            bls::BuildSdPsCbA(*ps, frame, matParams);
        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, impl->blsSdVsCb_);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, impl->blsSdPsCb_);

        render_detail::BindLayerAlbedo(cmd, view_.textures, layer.textureId,
                                       rs_.Textures().GetDefaults().White, rs_.Samplers());
        cmd->DrawIndexed(geo.indexCount);
    }
};

bool RenderPipeline::RenderGeosetsBls(GeosetBucket bucket) {
    return GeosetPassBls{rs_, bucket}.RunLists();
}

class GeosetPassHd : public BlsGeosetPass<GeosetPassHd> {
public:
    using BlsGeosetPass::BlsGeosetPass;

    bool IsAvailable() const {
        return rs_.Pipeline().impl_->blsHdProgram_ && rs_.Pipeline().impl_->blsPsoBuilder_;
    }

    void ComputeViewProj(Matrix44f& view, Matrix44f& proj) const {

        const f32 aspect = (rs_.Pipeline().Height() > 0)
                               ? static_cast<f32>(rs_.Pipeline().Width()) /
                                     static_cast<f32>(rs_.Pipeline().Height())
                               : 1.0f;
        view = rs_.Pipeline().FrameCamera().ViewLH();
        proj = rs_.Pipeline().FrameCamera().ProjectionLH(aspect);
    }

    void BindPassResources(gfx::IGFXCommandList* cmd, bls::FrameInputs& frame) {

        const bool useDayNight = rs_.Pipeline().impl_->iblDayNightLoaded_ &&
                                 rs_.GetDncService() != nullptr &&
                                 rs_.Settings().GetLightingMode() == LightingMode::InGame;
        if (useDayNight) {
            const auto blend = rs_.GetDncService()->ComputeEnvMapBlend();

            const bool dayPrimary = blend.isDaytime;
            frame.envFromMipEnd = dayPrimary ? rs_.Pipeline().impl_->iblDayMipEnd_
                                             : rs_.Pipeline().impl_->iblNightMipEnd_;
            frame.envToMipEnd = dayPrimary ? rs_.Pipeline().impl_->iblNightMipEnd_
                                           : rs_.Pipeline().impl_->iblDayMipEnd_;
            frame.envTransitionT = blend.transitionT;
        } else {
            frame.envFromMipEnd = rs_.Pipeline().impl_->iblProbeMipEnd_;
            frame.envToMipEnd = rs_.Pipeline().impl_->iblProbeMipEnd_;
            frame.envTransitionT = 0.75f;
        }

        const gfx::SamplerHandle linWrap = rs_.Samplers().LinearWrap();
        cmd->BindSampler(gfx::ShaderStage::Pixel, 1, linWrap);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 2, linWrap);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 3, linWrap);
        // The HD pixel shader also samples s_teamColor (s4) and the IBL
        // probes / BRDF LUT at s13..s15. They were never bound explicitly
        // — d3d11/d3d12 forgive the missing slots silently, but Vulkan
        // fires VUID-vkCmdDrawIndexed-None-08114 the first time a draw
        // touches the unbound descriptor.
        cmd->BindSampler(gfx::ShaderStage::Pixel, 4, linWrap);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 13, linWrap);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 14, linWrap);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 15, linWrap);

        gfx::TextureHandle from = gfx::TextureHandle::Invalid;
        gfx::TextureHandle to = gfx::TextureHandle::Invalid;
        if (useDayNight) {
            const auto day = rs_.Textures().GetOwned(RenderPipeline::kIblDayProbeName);
            const auto night = rs_.Textures().GetOwned(RenderPipeline::kIblNightProbeName);
            const auto blend = rs_.GetDncService()->ComputeEnvMapBlend();
            from = blend.isDaytime ? day : night;
            to = blend.isDaytime ? night : day;
        } else {
            from = rs_.Textures().GetOwned(RenderPipeline::kIblFromProbeName);
            to = rs_.Textures().GetOwned(RenderPipeline::kIblToProbeName);
            if (to == gfx::TextureHandle::Invalid)
                to = from;
        }
        if (from != gfx::TextureHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 13, from);
        if (to != gfx::TextureHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 14, to);
        const gfx::TextureHandle lut = rs_.Textures().GetOwned(RenderPipeline::kIblSplitSumLutName);
        if (lut != gfx::TextureHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 15, lut);

        if (auto* shadowSvc = rs_.GetShadowService()) {
            // The HD opaque PS samples each cascade with `SampleCmpLevelZero`
            // which REQUIRES a hardware comparison sampler. Without binding
            // one, WebGPU falls back to its default-Always comparison
            // sampler (no shadows) and D3D12/Vulkan are undefined (the
            // user-visible symptom is "everything darker when shadows are
            // on"). Bind the LessEqual comparison sampler at the same slots
            // as the depth targets.
            const gfx::SamplerHandle cmpSmp = rs_.Samplers().ShadowComparison();
            for (i32 c = 0; c < 3; ++c) {
                const gfx::TextureHandle sh = shadowSvc->depthTarget(c);
                if (sh != gfx::TextureHandle::Invalid) {
                    cmd->BindShaderResource(gfx::ShaderStage::Pixel, 10 + static_cast<u32>(c), sh);
                }
                if (cmpSmp != gfx::SamplerHandle::Invalid)
                    cmd->BindSampler(gfx::ShaderStage::Pixel, 10 + static_cast<u32>(c), cmpSmp);
            }
        }

        // Hoist pass-constant CBs out of the per-draw lambda. The HD draw
        // path used to write + bind HdShadowCascadesCb, HdDebugVisCb, and
        // SdOnHdShadowCascadeCountCb on EVERY draw, even though their
        // contents are constant across the entire pass (cascade VPs,
        // global debug mode, cascade count). On Firefox each ScopedCb
        // costs ~50µs IPC; with 60 draws/frame this was ~9 ms wasted.
        // Now: write+bind once per pass, the inner loop only writes the
        // genuinely per-draw HdVsCb and HdPsCb.
        if (rs_.GetShadowService() &&
            rs_.Pipeline().impl_->blsHdShadowCb_ != gfx::BufferHandle::Invalid) {
            if (auto sc = bls::ScopedCb<bls::HdShadowCascadesCb>(
                    rs_.Pipeline().Gfx(), rs_.Pipeline().impl_->blsHdShadowCb_)) {
                rs_.GetShadowService()->FillVsCb(*sc);
            }
            cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 1,
                                    rs_.Pipeline().impl_->blsHdShadowCb_);
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
            cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 1,
                                    rs_.Pipeline().impl_->blsHdShadowCountCb_);
        }
        if (rs_.Pipeline().impl_->blsHdDebugVisCb_ != gfx::BufferHandle::Invalid) {
            if (auto dbg = bls::ScopedCb<bls::DebugVisCb>(
                    rs_.Pipeline().Gfx(), rs_.Pipeline().impl_->blsHdDebugVisCb_)) {
                const i32 dbgMode = rs_.Settings().HdDebugMode();
                u32 enabled = 0;
                i32 psMode = dbgMode;
                Vector3f overrideA = {0, 0, 0};
                Vector3f overrideO = {0, 0, 0};
                if (dbgMode >= 5 && dbgMode <= 7) {
                    enabled = 1;
                    psMode = 0;
                    overrideA = (dbgMode == 5)   ? Vector3f{1, 1, 1}
                                : (dbgMode == 6) ? Vector3f{0.5f, 0.5f, 0.5f}
                                                 : Vector3f{0, 0, 0};
                } else if (dbgMode == 8) {
                    // ORM stub from texture_asset_manager (NeutralOrm 0x0000FFFFu):
                    // AO=1, Roughness=1, Metallic=0. Shader reads .yz for
                    // (roughness, metalness); .x feeds crystal refractMask.
                    enabled = 2;
                    psMode = 0;
                    overrideO = {1.0f, 1.0f, 0.0f};
                } else if (dbgMode == 9) {
                    // "AO Only" — BLS state stays at default; the GTAO
                    // apply pass overwrites hdrColor with the AO factor
                    // (see GtaoService::SetDebugAoOnly below).
                    psMode = 0;
                }
                dbg->enabledShaders = enabled;
                const u32 modeBits = static_cast<u32>(psMode);
                std::memcpy(&dbg->debugMode, &modeBits, sizeof(f32));
                dbg->_p0[0] = dbg->_p0[1] = 0.0f;
                dbg->overrideAlbedo = overrideA;
                dbg->_p1 = 0.0f;
                dbg->overrideOrm = overrideO;
                dbg->_p2 = 0.0f;
            }
            cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 3,
                                    rs_.Pipeline().impl_->blsHdDebugVisCb_);
        }
    }

    bls::BaselineLights Baseline(const Matrix44f& view) const {

        if (auto* dnc = rs_.GetDncService();
            dnc && dnc->HasAsset() && rs_.Settings().GetLightingMode() == LightingMode::InGame) {
            const auto sample = dnc->SampleNow();
            if (sample.valid) {
                // Front-biased sun (matches the shadow pass + SD baseline);
                // keep the DNC day/night ambient+diffuse colour.
                const Vector3f sunWS = ComputeSunDirWS(dnc);
                const Vector3f dirVS = whiteout::transform_normal(
                    Vector3f{-sunWS.x, -sunWS.y, -sunWS.z}, view);
                return {sample.ambient, sample.diffuse, dirVS};
            }
        }

        return {
            {kHdBaselineAmbientColor.x, kHdBaselineAmbientColor.y, kHdBaselineAmbientColor.z},
            {kHdBaselineLightColor.x, kHdBaselineLightColor.y, kHdBaselineLightColor.z},
            {0.0f, 0.0f, -1.0f},
        };
    }

    void DrawOpaqueItem(const render_detail::OpaqueItem& item, bls::FrameInputs& frame,
                        const Matrix44f& viewMat, gfx::IGFXCommandList* cmd,
                        const bls::BaselineLights& baseline) {
        const auto& view_ = *item.view;
        EmitLayersHd(view_, (*view_.geosets)[item.geoIdx], frame, viewMat, cmd, baseline);
    }

    void DrawTransparentItem(const render_detail::TransparentItem& item, bls::FrameInputs& frame,
                             const Matrix44f& viewMat, gfx::IGFXCommandList* cmd,
                             const bls::BaselineLights& baseline) {
        const auto& view_ = *item.view;
        EmitLayersHd(view_, (*view_.geosets)[item.geoIdx], frame, viewMat, cmd, baseline);
    }

    // The HD submission, kept intact (HD/SD-on-HD/Crystal program selection,
    // fresnel, normal/ORM/emissive + team-colour maps, shadows, MRT G-buffer,
    // and the per-layer fading-opaque depth prepass) — now fed one geoset at a
    // time by RunLists instead of the legacy bucket loop.
    void EmitLayersHd(const render_detail::RenderableView& view_, const GPUGeoset& geo,
                      bls::FrameInputs& frame, const Matrix44f& viewMat, gfx::IGFXCommandList* cmd,
                      const bls::BaselineLights& baseline) {
        const i32 lightCountForGeoset = bls::BuildLightPalette(
            frame, *view_.activeLights, viewMat, baseline, rs_.Settings().GetLightingMode());

        const GPUMaterial* mat = nullptr;
        const i32 matId = geo.materialId;
        if (matId >= 0 && matId < (i32)view_.materials->size())
            mat = &(*view_.materials)[matId];

        const f32 geoAlpha = geo.geosetAlpha * view_.parentVisibility;

        if (geoAlpha <= 0.0f)
            return;

        i32 numLayers = mat ? (i32)mat->cpu.layers.size() : 0;
        if (numLayers <= 0)
            numLayers = 1;

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
            (geo.boneVb != gfx::BufferHandle::Invalid) && (paletteCb != gfx::BufferHandle::Invalid);
        if (hasBones) {
            const u32 boneSlot = hasTangents ? 2u : 1u;
            cmd->BindVertexBuffer(boneSlot, geo.boneVb, sizeof(BoneVertex));
            cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 3, paletteCb);
        }

        struct LayerJob {
            render_detail::UnpackedLayer layer;
            bls::MatParams mp;
            const bls::BlsProgram* program = nullptr;
            bls::GxShaderID programShaderId = bls::GxShaderID::SD_on_HD;
            i32 activeN = 0;
            bool unlit = false;
            bool isOpaqueFading = false;
            bool valid = false;
        };
        std::vector<LayerJob> jobs(numLayers);

        for (i32 li = 0; li < numLayers; ++li) {
            jobs[li].layer = render_detail::UnpackLayer(mat, li);
            const auto& layer = jobs[li].layer;
            f32 combinedAlpha = geoAlpha * layer.alpha;
            if (combinedAlpha < 0.004f)
                continue;
            const bool isOpaqueFading =
                combinedAlpha < 0.99f && layer.filterMode <= FILTER_TRANSPARENT;
            i32 effectiveFilter = layer.filterMode;
            if (isOpaqueFading)
                effectiveFilter = FILTER_BLEND;

            const bool isCrystalMaterial =
                (layer.shaderId == 24) && rs_.Pipeline().impl_->blsCrystalProgram_ != nullptr;
            const bool isHdMaterial =
                isCrystalMaterial || layer.shaderId == 1 || layer.shaderId == 24;
            const bls::GxShaderID programShaderId = isCrystalMaterial ? bls::GxShaderID::Crystal
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
                mp.diffuseColor = {geo.geosetColor.x, geo.geosetColor.y, geo.geosetColor.z,
                                   combinedAlpha};
            }
            mp.emissiveGain = layer.emissiveGain;
            mp.fresnelTeamColor = layer.fresnelTeamColor;
            mp.fresnelOpacity = layer.fresnelOpacity;
            mp.fresnelColor = layer.fresnelColor;

            const bool unlit = (mp.disables & bls::kDisableLighting) != 0;
            const i32 activeN = unlit ? 0 : lightCountForGeoset;

            jobs[li].mp = mp;
            jobs[li].program = program;
            jobs[li].programShaderId = programShaderId;
            jobs[li].activeN = activeN;
            jobs[li].unlit = unlit;
            jobs[li].isOpaqueFading = isOpaqueFading;
            jobs[li].valid = true;
        }

        auto issueHdDraw = [&](const LayerJob& job, const bls::MatParams& matParams) {
            const auto& layer = job.layer;
            const bool unlit = job.unlit;
            const i32 activeN = job.activeN;
            const bls::GxShaderID programShaderId = job.programShaderId;
            const bls::BlsProgram* program = job.program;
            frame.numLights = activeN;
            render_detail::ApplyTexAnimPaletteToFrame(frame, view_.texAnimPalette,
                                                      layer.textureAnimationId);
            {
                bls::RenderState rs;
                rs.shaderId = programShaderId;
                rs.alphaMode = static_cast<u8>(matParams.alpha);
                rs.numColors = 0;
                rs.numTexCoords = 1;

                rs.numTangents = hasTangents ? 1 : 0;

                rs.numWeights = hasBones ? 4 : 0;
                rs.numLights = static_cast<u8>(activeN);
                rs.fogEnabled = false;
                rs.depthWrite = matParams.DepthWriteEnabled();
                rs.lightingEnabled = !unlit && activeN > 0;
                rs.prepass = false;
                rs.shadows = rs_.GetShadowService() && rs_.GetShadowService()->IsEnabled();

                rs.teamColor =
                    (layer.teamColorMapId == kHdTeamColorActive) || (layer.teamColorMapId >= 0);
                const i32 dbgMode = rs_.Settings().HdDebugMode();
                const bool debugActive = (dbgMode > 0);
                rs.debugShader = debugActive;
                auto perm = bls::SelectPermutes(rs);

                bls::PsoRequest req{};
                req.program = program;
                req.vsIndex = perm.vs;
                req.psIndex = perm.ps;
                req.material = matParams;

                if (hasBones) {
                    req.layout = hasTangents ? bls::VertexLayoutKind::MeshHDSkinned
                                             : bls::VertexLayoutKind::MeshHDSkinnedNoTangent;
                } else {
                    req.layout = hasTangents ? bls::VertexLayoutKind::MeshHDTangent
                                             : bls::VertexLayoutKind::ParticleSD;
                }
                req.topology = gfx::PrimitiveTopology::TriangleList;
                req.rtvFormat = RenderPipeline::kHdrSceneFormat;
                // Match the HD G-buffer render pass: slot 1 = linear
                // depth, slot 2 = encoded normal. Picked up by the
                // backend's multi-RTV PSO build path. The HD opaque
                // shader's WC3_IS_MRT permutation writes SV_Target1/2,
                // so enable extra-slot writes (transparent / SD / line
                // PSOs leave the flag at the default `false`).
                req.extraRtvFormats[0] = RenderPipeline::kLinearDepthFormat;
                req.extraRtvFormats[1] = RenderPipeline::kNormalBufferFormat;
                req.extraRtvCount = 2;
                // Only the colour-writing, depth-writing materials
                // (= the WC3_IS_MRT permutation) actually emit
                // SV_Target1/2. Depth-prepass materials have
                // DepthWriteEnabled=true but ColorWriteEnabled=false —
                // their shader outputs nothing at all and WebGPU
                // rejects a PSO whose extras have writeMask != 0 with
                // no matching fragment output.
                req.extraColorWrite =
                    matParams.DepthWriteEnabled() && matParams.ColorWriteEnabled();
                req.dsvFormat = rs_.Pipeline().impl_->depthStencilFormat_;
                req.lhClipSpace = true;
                auto pso = rs_.Pipeline().impl_->blsPsoBuilder_->GetOrBuild(req);
                if (pso == gfx::PipelineHandle::Invalid)
                    return;
                cmd->BindPipeline(pso);

                cmd->BindVertexBuffer(0, render_detail::PickSlot0Vb(geo, layer.coordId),
                                      sizeof(Vertex));

                frame.world = view_.worldTransform;

                if (auto vs = bls::ScopedCb<bls::HdVsCb>(rs_.Pipeline().Gfx(),
                                                         rs_.Pipeline().impl_->blsHdVsCb_)) {
                    bls::BuildHdVsCb(*vs, frame, matParams);
                }
                cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 2,
                                        rs_.Pipeline().impl_->blsHdVsCb_);

                // Pass-constant CBs (HdShadowCascadesCb @ VS slot 1,
                // SdOnHdShadowCascadeCountCb @ PS slot 1, DebugVisCb @
                // PS slot 3) are bound once in BindPassResources — they
                // don't vary per draw.

                if (program == rs_.Pipeline().impl_->blsHdProgram_ ||
                    program == rs_.Pipeline().impl_->blsCrystalProgram_) {
                    if (auto ps = bls::ScopedCb<bls::HdPsCb>(rs_.Pipeline().Gfx(),
                                                             rs_.Pipeline().impl_->blsHdPsCb_)) {
                        bls::BuildHdPsCb(*ps, frame, matParams);
                    }
                    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 2,
                                            rs_.Pipeline().impl_->blsHdPsCb_);
                } else {
                    if (auto ps = bls::ScopedCb<bls::SdOnHdPsCb>(
                            rs_.Pipeline().Gfx(), rs_.Pipeline().impl_->blsSdOnHdPsCb_)) {
                        bls::BuildSdOnHdPsCb(*ps, frame, matParams);
                    }
                    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 2,
                                            rs_.Pipeline().impl_->blsSdOnHdPsCb_);
                }

                auto bindMaterialTex = [&](u32 slot, i32 texId, gfx::TextureHandle fallback,
                                           u32* outWrap) {
                    if (texId >= 0 && view_.textures) {
                        const gfx::TextureHandle h = view_.textures->Get(texId);
                        if (h != gfx::TextureHandle::Invalid) {
                            cmd->BindShaderResource(gfx::ShaderStage::Pixel, slot, h);
                            if (outWrap)
                                *outWrap = view_.textures->WrapFlags(texId) & kSamplerWrapBitsMask;
                            return true;
                        }
                    }
                    cmd->BindShaderResource(gfx::ShaderStage::Pixel, slot, fallback);
                    return false;
                };

                const auto& defs = rs_.Textures().GetDefaults();
                u32 wrapFlags = kSamplerWrapBitsMask;
                bindMaterialTex(0, layer.textureId, defs.White, &wrapFlags);
                bindMaterialTex(1, layer.normalMapId, defs.FlatNormal, nullptr);
                bindMaterialTex(2, layer.ormMapId, defs.NeutralOrm, nullptr);
                bindMaterialTex(3, layer.emissiveMapId, defs.Black, nullptr);

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

        for (i32 li = 0; li < numLayers; ++li) {
            if (!jobs[li].valid || !jobs[li].isOpaqueFading)
                continue;
            bls::MatParams prepass = jobs[li].mp;
            prepass.diffuseColor = {1.0f, 1.0f, 1.0f, 1.0f};
            prepass.disables &= ~bls::kDisableDepthWrite;
            prepass.disables |= bls::kDisableBit8;
            issueHdDraw(jobs[li], prepass);
        }

        for (i32 li = 0; li < numLayers; ++li) {
            if (!jobs[li].valid)
                continue;
            issueHdDraw(jobs[li], jobs[li].mp);
        }
    }
};

bool RenderPipeline::RenderGeosetsHd(GeosetBucket bucket) {
    return GeosetPassHd{rs_, bucket}.RunLists();
}

void RenderPipeline::RenderGeosets(GeosetBucket bucket) {
    if (impl_->frameRenderMode_ == RenderMode::HD) {
        RenderGeosetsHd(bucket);
    } else {
        RenderGeosetsBls(bucket);
    }
}

// Unified transparent pass — WC3's IModelRenderSceneTransparent. Prepares each
// transparent producer (geosets via the mode's GeosetPass, PE2 particles), then
// interleaves their per-unit draws back-to-front by (priorityPlane, distance).
// Ribbons + corn join here in a later step; for now they stay separate passes.
template <class GeosetPass>
void RenderPipeline::RenderTransparentSceneT() {
    auto* cmd = impl_->gfx_->GetImmediateContext();
    const Vector3f camPos = rs_.Pipeline().FrameCamera().GetSource();
    auto sqDistTo = [&](const Vector3f& p) {
        const Vector3f d = {p.x - camPos.x, p.y - camPos.y, p.z - camPos.z};
        return d.x * d.x + d.y * d.y + d.z * d.z;
    };

    // --- Geosets: prepare the pass (also does the pass-global binds once) ---
    GeosetPass pass(rs_, GeosetBucket::Transparent);
    render_detail::CollectedDrawLists geo;
    bls::FrameInputs geoFrame;
    Matrix44f geoView;
    bls::BaselineLights geoBaseline;
    const bool haveGeo = pass.PrepareInterleaved(geo, geoFrame, geoView, geoBaseline);

    // --- PE2 particles: build geometry into the shared VB ---
    std::vector<particle::EmitterDrawList> partDraws;
    bls::FrameInputs partFrame;
    bool haveParticles = false;
    if (rs_.Settings().ShowParticles() && rs_.Particles().EmitterCount() > 0) {
        std::vector<Vertex> verts;
        const Matrix44f viewMat = rs_.Pipeline().FrameCamera().GetViewMatrix();
        rs_.Particles().BuildGeometry(viewMat, verts, partDraws);
        const i32 vertCount = (i32)verts.size();
        if (vertCount > 0) {
            if (impl_->particleServiceVB_ == gfx::BufferHandle::Invalid ||
                vertCount > impl_->particleServiceVBSize_) {
                impl_->gfx_->Destroy(impl_->particleServiceVB_);
                constexpr i32 kFloor = 4096;
                constexpr i32 kCeiling = 512 * 1024;
                const i32 doubled = (std::max)(impl_->particleServiceVBSize_ * 2, kFloor);
                const i32 newSize = (std::max)(vertCount, (std::min)(doubled, kCeiling));
                gfx::BufferDesc bd;
                bd.size = (u32)(sizeof(Vertex) * newSize);
                bd.usage = gfx::BufferUsage::Vertex | gfx::BufferUsage::CpuWritable;
                bd.ringSlotsHint = 4;
                impl_->particleServiceVB_ = impl_->gfx_->CreateBuffer(bd);
                impl_->particleServiceVBSize_ = newSize;
            }
            if (void* mapped = impl_->gfx_->MapBuffer(impl_->particleServiceVB_)) {
                memcpy(mapped, verts.data(), sizeof(Vertex) * vertCount);
                impl_->gfx_->UnmapBuffer(impl_->particleServiceVB_);
            }
            partFrame.world = Matrix44f::identity();
            partFrame.view = viewMat;
            const f32 aspect = (Height() > 0) ? (f32)Width() / (f32)Height() : 1.0f;
            partFrame.projection = rs_.Pipeline().FrameCamera().ProjectionRH(aspect);
            partFrame.effectTime = rs_.Scene().GetAnimationTime() * 0.001f;
            partFrame.numLights = 0;
            partFrame.viewportRect = {(f32)Width(), (f32)Height(), 0.0f, 0.0f};
            haveParticles = true;
        }
    }

    // --- Ribbons: build each actor's strips into its per-actor VB ---
    std::vector<RibbonDrawUnit> ribbonUnits;
    bls::FrameInputs ribbonFrame;
    if (rs_.Settings().ShowRibbons())
        PrepareRibbons(ribbonUnits, ribbonFrame);

    // --- Corn (PopcornFX): tick + consolidate into the shared buffers ---
    std::vector<corn_effects::CornDrawUnit> cornUnits;
    if (rs_.Settings().ShowParticles()) {
        const f32 aspect = (Height() > 0) ? (f32)Width() / (f32)Height() : 1.0f;
        corn_effects::CornEffectsFrameInputs fi;
        fi.cmd = cmd;
        fi.view = rs_.Pipeline().FrameCamera().ViewLH();
        fi.projection = rs_.Pipeline().FrameCamera().ProjectionLH(aspect);
        fi.viewportRect = {(f32)Width(), (f32)Height(), 0.0f, 0.0f};
        fi.effectTime = rs_.Scene().GetAnimationTime() * 0.001f;
        fi.rtvFormat = SceneTargetFormat();
        fi.dsvFormat = impl_->depthStencilFormat_;
        if (impl_->frameRenderMode_ == RenderMode::HD) {
            fi.extraRtvFormats[0] = kLinearDepthFormat;
            fi.extraRtvFormats[1] = kNormalBufferFormat;
            fi.extraRtvCount = 2;
        }
        rs_.CornEffects().SetFrameInputs(fi);
        rs_.CornEffects().Simulate(rs_.CornEffects().PendingDt());
        rs_.CornEffects().PrepareInterleavedDraws(cornUnits);
    }

    // --- Collect + sort entries (priorityPlane asc, distance back-to-front) ---
    std::vector<render_detail::TransparentDraw> entries;
    entries.reserve((haveGeo ? geo.lists.transparent.size() : 0) + partDraws.size() +
                    ribbonUnits.size() + cornUnits.size());
    if (haveGeo)
        for (u32 i = 0; i < geo.lists.transparent.size(); ++i)
            entries.push_back({geo.lists.transparent[i].sqDist,
                               geo.lists.transparent[i].priorityPlane,
                               render_detail::TransparentKind::Geoset, i});
    if (haveParticles)
        for (u32 i = 0; i < partDraws.size(); ++i)
            entries.push_back({sqDistTo(partDraws[i].worldOrigin), partDraws[i].priorityPlane,
                               render_detail::TransparentKind::Particle, i});
    for (u32 i = 0; i < ribbonUnits.size(); ++i)
        entries.push_back({sqDistTo(ribbonUnits[i].origin), ribbonUnits[i].priorityPlane,
                           render_detail::TransparentKind::Ribbon, i});
    for (u32 i = 0; i < cornUnits.size(); ++i) {
        Vector3f origin = {0, 0, 0};
        if (Actor* owner = rs_.Scene().Actors().Find(cornUnits[i].model))
            origin = whiteout::transform_point({0, 0, 0}, owner->worldTransform);
        entries.push_back(
            {sqDistTo(origin), cornUnits[i].priorityPlane, render_detail::TransparentKind::Corn, i});
    }
    if (entries.empty())
        return;
    std::sort(entries.begin(), entries.end(), render_detail::TransparentDrawOrder);

    // --- Dispatch in sorted order ---
    for (const auto& e : entries) {
        switch (e.kind) {
        case render_detail::TransparentKind::Geoset:
            pass.DrawTransparentItem(geo.lists.transparent[e.unit], geoFrame, geoView, cmd,
                                     geoBaseline);
            break;
        case render_detail::TransparentKind::Particle:
            DrawParticleEmitter(partDraws[e.unit], partFrame);
            break;
        case render_detail::TransparentKind::Ribbon:
            DrawRibbonStrip(ribbonUnits[e.unit], ribbonFrame);
            break;
        case render_detail::TransparentKind::Corn:
            rs_.CornEffects().DrawCornSlice(cornUnits[e.unit].slice);
            break;
        }
    }
}

void RenderPipeline::RenderTransparentScene() {
    if (impl_->frameRenderMode_ == RenderMode::HD)
        RenderTransparentSceneT<GeosetPassHd>();
    else
        RenderTransparentSceneT<GeosetPassBls>();
}

} // namespace whiteout::flakes::renderer
