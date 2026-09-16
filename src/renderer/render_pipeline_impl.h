#pragma once

#include "core/render_detail.h"
#include "core/render_profile.h"
#include "core/vertex_layout.h"
#include "frame_capture.h"
#include "render_pipeline.h"
#include "renderer/particle/output/multitex_particle_service.h"
#include "renderer/particle/particle_service.h"
#include "shading/shading_registry.h"
#include "shading/unlit_shading.h"
#include "profiles/wc3/wc3_debug_programs.h"
#include "renderer/mesh_overlay/mesh_overlay_renderer.h"
#if WDX_ENABLE_M2
#include "renderer/profiles/wow/m2_shading.h"
#endif
#if WDX_ENABLE_M3
#include "renderer/profiles/sc2_heroes/m3_standard_shading.h"
#endif
#if WDX_ENABLE_D3
#include "renderer/profiles/diablo3/d3_particle_shading.h"
#include "renderer/profiles/diablo3/d3_standard_shading.h"
#endif

#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace whiteout::flakes::renderer::bls {
class BlsShaderCache;
class BlsProgramCatalog;
class BlsPsoBuilder;
class BlsPsoTrace;
struct BlsProgram;
struct BlsShader;
} // namespace whiteout::flakes::renderer::bls

namespace whiteout::flakes::renderer {

class Camera;

struct RenderPipeline::Impl {
    // ---- GFX device + targets ----
    std::unique_ptr<gfx::IGFXDevice> gfx_;
    std::unordered_map<RenderTargetId, RenderTarget> targets_;
    RenderTargetId nextTargetId_ = 1;
    RenderTargetId primaryTargetId_ = 0;

    // ---- Display surface size ----
    // Fallback dimensions used outside a frame (host queries before the first
    // RenderFrame, resize bookkeeping). During a frame, Width()/Height() resolve
    // against activeTarget_ instead so every pass sizes to the target actually
    // being rendered rather than the primary swap-chain.
    i32 width_ = 800;
    i32 height_ = 600;

    // The render target currently being rendered by RenderFrame/RenderViewport,
    // or nullptr outside a frame. Set at the top of RenderFrame, cleared at the
    // end. Multi-viewport renders sequentially, so a single active pointer is
    // sufficient: Width()/Height()/SceneTargetFormat() read it to stay
    // per-viewport-correct without threading the target through every pass.
    const RenderTarget* activeTarget_ = nullptr;

    // The camera the in-flight frame renders from. Published alongside
    // activeTarget_ at the top of RenderFrame and read by every pass through
    // RenderPipeline::FrameCamera(). Today it always points at the scene's
    // single camera; once viewports own a camera (Phase 3) RenderViewport sets
    // it to the viewport's active camera so each viewport renders from its own.
    const Camera* activeCamera_ = nullptr;

    // Whether the in-flight viewport composites the host ImGui draw data.
    // Published from Viewport::drawImGui at the top of RenderViewport; the
    // scene-pass / tonemap ImGui blocks gate on it so off-screen viewports
    // (thumbnails) don't redraw the UI on top of their scene.
    bool frameDrawImGui_ = true;

    RenderMode frameRenderMode_ = RenderMode::SD;
    // The profile the in-flight frame is running, latched beside the mode and
    // for the same reason. After P5 the profile — not the mode — is what says
    // whether the scene pass is MRT, what format it lands in, and what colour
    // space it shades in; RenderMode only picks *which* WC3 profile. A frame
    // that asked the mode instead would get the right answer for WC3 and the
    // wrong one for any profile selected by product.
    //
    // Null until the first RenderViewport latches it. Queries that can run
    // before any frame (PSO warm-up) fall back to the mode, which is what they
    // read before this existed.
    const core::IRenderProfile* frameProfile_ = nullptr;
    // The frame's debug view, latched beside the profile and handed to every
    // model through PassContext.
    core::DebugFrame frameDebug_;
    core::DebugTargetInfo frameDebugTarget_;
    // The WC3 debug pixel programs; created lazily on the first debug draw.
    std::unique_ptr<profiles::wc3::Wc3DebugPrograms> wc3DebugPrograms_;
    std::unique_ptr<mesh_overlay::MeshOverlayRenderer> meshOverlay_;

    // Shading models, long-lived so they can hold per-model caches and so
    // P8/P9/P10 have somewhere to register their ids. Held by base pointer to
    // keep the WC3 profile headers out of this one.
    shading::ShadingRegistry shadingModels_;
    std::unique_ptr<shading::IShadingModel> wc3SdShading_;
    std::unique_ptr<shading::IShadingModel> wc3HdShading_;
    // Product-neutral, so it is held by concrete type: CleanupGFX calls its
    // ReleaseGpu, which is not on the interface (nothing else owns GPU objects
    // outside the BLS caches).
    std::unique_ptr<shading::UnlitShading> unlitShading_;
#if WDX_ENABLE_M2
    // Concrete for the same reason: CleanupGFX calls its ReleaseGpu, which is
    // not on IShadingModel.
    std::unique_ptr<profiles::wow::M2CombinerShading> m2Shading_;
#endif
#if WDX_ENABLE_M3
    // Concrete for the same reason again.
    std::unique_ptr<profiles::sc2_heroes::M3StandardShading> m3Shading_;
#endif
#if WDX_ENABLE_D3
    std::unique_ptr<profiles::diablo3::D3StandardShading> d3Shading_;
    // Not an IShadingModel: a particle is not a surface and never enters the
    // draw-item pipeline. It owns its shaders for the same reason d3Shading_
    // does and is driven straight from DrawParticleEmitter.
    std::unique_ptr<profiles::diablo3::D3ParticleShading> d3Particles_;
#endif

    // The two WC3 frames, declared. ValidateProfile runs once when they are
    // built, so a declaration that contradicts itself fails at init rather
    // than at whichever frame first reads a target nobody wrote.
    std::unique_ptr<core::IRenderProfile> wc3SdProfile_;
    std::unique_ptr<core::IRenderProfile> wc3HdProfile_;
#if WDX_ENABLE_M2
    // Selected by the scene's ProductId rather than by RenderMode — see
    // ActiveProfile. Null in a build without the format.
    std::unique_ptr<core::IRenderProfile> wowProfile_;
#endif
#if WDX_ENABLE_M3
    std::unique_ptr<core::IRenderProfile> sc2HeroesProfile_;
#endif
#if WDX_ENABLE_D3
    std::unique_ptr<core::IRenderProfile> d3Profile_;
#endif

    // Interned MeshBuffer layouts. Holds no GPU objects, so it outlives
    // CleanupGFX and needs no teardown hook.
    core::VertexLayoutCache vertexLayouts_;

    // Cached at InitDevice time via Gfx()->PreferredDepthStencilFormat().
    // Renderer-wide source of truth for the depth-target format and
    // every PSO's dsvFormat — see RenderPipeline::DepthStencilFormat().
    gfx::Format depthStencilFormat_ = gfx::Format::D24_UNORM_S8_UINT;

    // ---- Line / debug pipelines ----
    gfx::ShaderHandle lineVS_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle linePS_ = gfx::ShaderHandle::Invalid;
    gfx::PipelineHandle linePSOHdr_ = gfx::PipelineHandle::Invalid;
    // Sc2Heroes binds a fourth scene attachment (the M3 albedo/spec sidecar)
    // that linePSOHdr_'s two extras do not cover. Built on first use, since no
    // other profile declares the slot — see LinePSO.
    gfx::PipelineHandle linePSOGbuf_ = gfx::PipelineHandle::Invalid;
    gfx::PipelineHandle linePSOSd_ = gfx::PipelineHandle::Invalid;
    // RTV format the SD line PSO was built against. Tracked so CurrentLinePSO
    // can rebuild it when the swap-chain format isn't the hardcoded RGBA8_SRGB
    // (macOS Metal / WebGPU-Dawn surfaces only expose BGRA8 — without the
    // rebuild, SetPipeline mismatches the renderpass on every debug draw and
    // invalidates the whole CommandBuffer, leaving the user with a magenta
    // screen). linePSOHdr_ doesn't need this because the HDR scene target is
    // a fixed R11G11B10_FLOAT offscreen.
    gfx::Format linePsoSdFormat_ = gfx::Format::Unknown;
    // Same two pipelines with the depth test off, for markers that have to be
    // visible through the model they annotate — see CurrentOverlayLinePSO.
    gfx::PipelineHandle overlayLinePSOHdr_ = gfx::PipelineHandle::Invalid;
    gfx::PipelineHandle overlayLinePSOGbuf_ = gfx::PipelineHandle::Invalid;
    gfx::PipelineHandle overlayLinePSOSd_ = gfx::PipelineHandle::Invalid;
    gfx::Format overlayLinePsoSdFormat_ = gfx::Format::Unknown;
    // One tonemap PSO per destination RTV format. Kept per format rather than
    // rebuilt on change because a host that renders thumbnails alternates
    // formats WITHIN a frame — an offscreen cell then the sRGB swap chain —
    // and destroying the PSO the previous viewport is still using is a
    // use-after-free the driver crashes on. Formats seen per run: two.
    std::vector<std::pair<gfx::Format, gfx::PipelineHandle>> tonemapPSOs_;
    // What stands in for the tonemap under a debug channel view: a straight
    // copy, so the channel reaches the screen as computed. Per format for the
    // tonemap's reason.
    gfx::ShaderHandle debugCopyVs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle debugCopyPs_ = gfx::ShaderHandle::Invalid;
    std::vector<std::pair<gfx::Format, gfx::PipelineHandle>> debugCopyPSOs_;
    gfx::BufferHandle cbPerFrame_ = gfx::BufferHandle::Invalid;

    // ---- Particle / splat VBs ----
    gfx::BufferHandle particleServiceVB_ = gfx::BufferHandle::Invalid;
    i32 particleServiceVBSize_ = 0;
    // This frame's refraction emitters. Built where every other particle's
    // geometry is built — inside the transparent scene — but drawn later, by
    // the Refraction pass, into a buffer of its own. Cleared at the top of
    // every viewport so a frame that skips the transparent scene cannot draw
    // the previous frame's distortion.
    particle::MultiTexGeometry refractionGeo_;
    // This frame's multi-texture emitters. Same three-UV stream as refraction,
    // but drawn where every other particle is — the emitter is ordinary colour,
    // it just takes three layers to make it. Cleared alongside the above.
    particle::MultiTexGeometry multiTexGeo_;
    // This frame's Diablo III texcoords, index-parallel with the ORDINARY
    // particle stream rather than a stream of its own — a D3 quad is an ordinary
    // billboard everywhere but its four texture coordinates. Interleaved into
    // the D3 program's own vertex by D3ParticleShading::BeginFrame.
    particle::D3VertexStream d3UvGeo_;
    std::unique_ptr<particle::MultiTexParticleService> multiTexParticles_;
    Matrix44f refractionView_ = Matrix44f::identity();
    Matrix44f refractionProjection_ = Matrix44f::identity();
    // Set for a frame whose scene was redirected into the refraction service's
    // own texture because the real destination — a swap-chain back buffer —
    // cannot be sampled. Holds that destination, which the Refraction pass then
    // writes; Invalid means the scene went where it always goes.
    gfx::TextureHandle refractionMirrorDst_ = gfx::TextureHandle::Invalid;
    gfx::Format refractionMirrorFmt_ = gfx::Format::Unknown;
    // The same redirect for Diablo III's distortion resolve, which has the same
    // problem: it samples the finished scene and a back buffer cannot be
    // sampled. Never both — one scene, one profile, one owner.
    gfx::TextureHandle distortionMirrorDst_ = gfx::TextureHandle::Invalid;
    gfx::Format distortionMirrorFmt_ = gfx::Format::Unknown;
    // This frame's transparent-scene draw lists, kept past the function that
    // builds them: the Distortion pass runs after the scene render pass closes
    // and re-submits `lists.distortion` into another target, so the `views`
    // these DrawItems point at have to outlive RenderTransparentScene.
    render_detail::CollectedDrawLists transparentLists_;
    // This frame's Diablo III distortion emitters, held back from the
    // transparent queue. Their vertices are already in the shared particle VB,
    // so only the draw list has to survive to the Distortion pass.
    std::vector<particle::EmitterDrawList> distortionParticles_;
    // The frame inputs those draws were prepared with — the same `partFrame`
    // the transparent queue used, since the geometry is the same geometry.
    bls::FrameInputs distortionParticleFrame_{};
    gfx::BufferHandle splatServiceVB_ = gfx::BufferHandle::Invalid;
    i32 splatServiceVBSize_ = 0;

    // ---- BLS pipeline ----
    std::unique_ptr<bls::BlsShaderCache> blsShaderCache_;
    std::unique_ptr<bls::BlsProgramCatalog> blsPrograms_;
    std::unique_ptr<bls::BlsPsoBuilder> blsPsoBuilder_;
    // Pre-warm trace: records PsoRequest keys built this run, replays
    // them on the next run before the first draw. Sits behind the
    // builder; ctor of the trace loads any saved keys from disk, the
    // renderer calls Replay() once all BLS programs are loaded, and
    // the builder forwards every cache miss into Record(). See
    // RenderPipeline::InitBlsShaders.
    std::unique_ptr<bls::BlsPsoTrace> blsPsoTrace_;
    const bls::BlsProgram* blsSdProgram_ = nullptr;
    const bls::BlsProgram* blsSdOnHdProgram_ = nullptr;
    const bls::BlsProgram* blsHdProgram_ = nullptr;
    const bls::BlsProgram* blsCrystalProgram_ = nullptr;
    const bls::BlsProgram* blsCornFxProgram_ = nullptr;

    gfx::BufferHandle blsSdVsCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle blsSdPsCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle blsHdVsCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle blsHdPsCb_ = gfx::BufferHandle::Invalid;
    // 3.0.0 HD pass banks: VS b1 (blight rect) and PS b1 (cascades + cluster
    // grid), written once per pass.
    gfx::BufferHandle blsHdVsBlightCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle blsHdClusteredCb_ = gfx::BufferHandle::Invalid;
    // The clustered light set, PS t16 / t17 / t18. Grown on demand; the
    // capacities are element counts.
    gfx::BufferHandle blsHdLightsSb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle blsHdLightIndicesSb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle blsHdClustersSb_ = gfx::BufferHandle::Invalid;
    u32 blsHdLightsCapacity_ = 0;
    u32 blsHdLightIndicesCapacity_ = 0;
    u32 blsHdClustersCapacity_ = 0;

    // ---- Shadow ----
    gfx::PipelineHandle shadowPSO_ = gfx::PipelineHandle::Invalid;
    gfx::PipelineHandle shadowPSORigid_ = gfx::PipelineHandle::Invalid;
    gfx::PipelineHandle shadowPSOAlpha_ = gfx::PipelineHandle::Invalid;
    gfx::PipelineHandle shadowPSORigidAlpha_ = gfx::PipelineHandle::Invalid;
    // Scene clock (ms) at the last point-shadow allocation, -1 before the first.
    i32 pointShadowClockMs_ = -1;
    gfx::BufferHandle shadowVsCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle shadowPsCb_ = gfx::BufferHandle::Invalid;

    // ---- IBL probe state (mip counts + load state; mode lives in settings_) ----
    // Full-chain mip COUNT of each probe, log2(max(w,h)) + 1 — what the engine
    // uploads to PS cb2[25] (CGxTex+0x48), not the last mip index.
    f32 iblProbeMipCount_ = 0.0f;
    f32 iblDayMipCount_ = 0.0f;
    f32 iblNightMipCount_ = 0.0f;
    bool iblDayNightLoaded_ = false;
    // The single probe came out of the content, not CreateDebugFacesEnvProbe.
    bool iblProbeFromContent_ = false;

    // ---- Tonemap GPU resources (exposure lives in settings_) ----
    bls::BlsShader* blsSpriteVs_ = nullptr;
    bls::BlsShader* blsTonemapPs_ = nullptr;
    gfx::BufferHandle tonemapVB_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle tonemapPsCb_ = gfx::BufferHandle::Invalid;
    gfx::SamplerHandle tonemapSampler_ = gfx::SamplerHandle::Invalid;

    // ---- Optional frame capture (PNG / GIF export) ----
    // Self-contained; off and zero-cost unless enabled. See frame_capture.h.
    FrameCapture capture_;
};

} // namespace whiteout::flakes::renderer
