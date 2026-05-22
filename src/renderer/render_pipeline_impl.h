#pragma once

#include "frame_capture.h"
#include "render_pipeline.h"

#include <unordered_map>

namespace whiteout::flakes::renderer::bls {
class BlsShaderCache;
class BlsProgramCatalog;
class BlsPsoBuilder;
class BlsPsoTrace;
struct BlsProgram;
struct BlsShader;
} // namespace whiteout::flakes::renderer::bls

namespace whiteout::flakes::renderer {

struct RenderPipeline::Impl {
    // ---- GFX device + targets ----
    std::unique_ptr<gfx::IGFXDevice> gfx_;
    std::unordered_map<RenderTargetId, RenderTarget> targets_;
    RenderTargetId nextTargetId_ = 1;
    RenderTargetId primaryTargetId_ = 0;

    // ---- Display surface size ----
    i32 width_ = 800;
    i32 height_ = 600;

    RenderMode frameRenderMode_ = RenderMode::SD;

    // Cached at InitDevice time via Gfx()->PreferredDepthStencilFormat().
    // Renderer-wide source of truth for the depth-target format and
    // every PSO's dsvFormat — see RenderPipeline::DepthStencilFormat().
    gfx::Format depthStencilFormat_ = gfx::Format::D24_UNORM_S8_UINT;

    // ---- Line / debug pipelines ----
    gfx::ShaderHandle lineVS_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle linePS_ = gfx::ShaderHandle::Invalid;
    gfx::PipelineHandle linePSOHdr_ = gfx::PipelineHandle::Invalid;
    gfx::PipelineHandle linePSOSd_ = gfx::PipelineHandle::Invalid;
    gfx::PipelineHandle tonemapPSO_ = gfx::PipelineHandle::Invalid;
    gfx::BufferHandle cbPerFrame_ = gfx::BufferHandle::Invalid;

    // ---- Particle / splat VBs ----
    gfx::BufferHandle particleServiceVB_ = gfx::BufferHandle::Invalid;
    i32 particleServiceVBSize_ = 0;
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
    gfx::BufferHandle blsHdShadowCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle blsHdShadowCountCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle blsSdOnHdPsCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle blsHdDebugVisCb_ = gfx::BufferHandle::Invalid;

    // ---- Shadow ----
    gfx::PipelineHandle shadowPSO_ = gfx::PipelineHandle::Invalid;
    gfx::PipelineHandle shadowPSORigid_ = gfx::PipelineHandle::Invalid;
    gfx::BufferHandle shadowVsCb_ = gfx::BufferHandle::Invalid;

    // ---- IBL probe state (mip extents + load state; mode lives in settings_) ----
    f32 iblProbeMipEnd_ = 0.0f;
    f32 iblDayMipEnd_ = 0.0f;
    f32 iblNightMipEnd_ = 0.0f;
    bool iblDayNightLoaded_ = false;

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
