#pragma once

// DofService — owns the bokeh depth-of-field post-process and drives its two
// fullscreen passes per frame: a golden-angle gather (scene HDR colour + linear
// view-Z → a scratch HDR target) followed by a blit that copies the scratch
// back over the scene colour. HD-only; SD frames skip the service entirely.
//
// The gather runs the SHIPPED WC3 shader `depthoffield.bls` (GxShaderID
// DepthOfField) loaded through the BLS shader cache — same path bloom/tonemap
// take — not a hand-written variant. It pairs with the `sprite` BLS vertex
// shader + the shared fullscreen-triangle VB, and feeds the engine's
// DepthOfFieldPSPerDraw constant layout (3×float4 at b1; t0=colour, t1=depth,
// s0/s1 samplers). The blit back to the scene colour uses the shared embedded
// blit shader, exactly like PostProcessService::RunBlit.
//
// Lifecycle mirrors PostProcessService: Init once the BLS cache + sprite VB are
// up, Shutdown before the device drops, Run once per frame after GTAO/SSAO and
// before bloom + tonemap (WC3's slot: opaque → SSAO+fog → DoF → bloom). PSOs
// rebuild lazily if the HDR format changes.

#include "gfx/gfx.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer {
struct RenderTarget;
}

namespace whiteout::flakes::renderer::bls {
class BlsShaderCache;
struct BlsShader;
} // namespace whiteout::flakes::renderer::bls

namespace whiteout::flakes::renderer::dof {

struct DofParams {
    // Master enable. Off by default — the host opts in (and supplies a focal
    // distance). Mirrors WC3's per-camera GetDepthOfFieldEnabled gate.
    bool enabled = false;
    // Linear view-space distance (scene units) that stays in focus. 0 disables
    // the pass — WC3 also skips DoF when the camera's focal distance is 0.
    // Set from the host camera (WC3: CameraSetFocalDistance).
    f32 focusDistance = 0.0f;
    // Circle-of-confusion ramp: how aggressively blur grows with the depth
    // difference from the focal plane (WC3: CameraSetDepthOfFieldScale).
    f32 focusScale = 1.0f;
    // Max gather radius in pixels (WC3 default 10).
    f32 maxBlurSize = 10.0f;
    // Spiral ring spacing / sample density (WC3 default 1.0).
    f32 radiusScale = 1.0f;
    // Suppress foreground blur — only the background (far field) blurs
    // (WC3: OnlyFarField, default off).
    bool farFieldOnly = false;
};

class DofService {
public:
    DofService() = default;
    ~DofService() = default;

    DofService(const DofService&) = delete;
    DofService& operator=(const DofService&) = delete;

    // `cache` supplies the shipped `depthoffield` + `sprite` BLS shaders;
    // `spriteVb` is the shared fullscreen-triangle VB (the tonemap/bloom VB).
    void Init(gfx::IGFXDevice& gfx, gfx::GfxApi api, bls::BlsShaderCache& cache,
              gfx::BufferHandle spriteVb);
    void Shutdown();

    void SetParams(const DofParams& p) {
        params_ = p;
    }
    const DofParams& Params() const {
        return params_;
    }
    void SetEnabled(bool on) {
        params_.enabled = on;
    }

    bool IsReady() const {
        return gfx_ != nullptr && shadersReady_;
    }
    // Runs only when enabled, ready, and a usable focal distance is set.
    bool IsEnabled() const {
        return params_.enabled && params_.focusDistance > 0.0f && IsReady();
    }

    // One frame's DoF. Gathers target.hdrColor + target.linearDepth into
    // target.bloomScratchA, then blits the scratch back over target.hdrColor.
    // Both scratch and hdrColor are the HDR scene format, so the round-trip is
    // lossless. No-op if any required target slot is missing.
    void Run(gfx::IGFXCommandList* cmd, RenderTarget& target);

private:
    void EnsurePsos(gfx::Format hdrFmt);

    gfx::IGFXDevice* gfx_ = nullptr;
    gfx::GfxApi api_ = gfx::GfxApi::D3D12;

    // Gather: shipped `sprite` VS + `depthoffield` PS (BLS cache, borrowed).
    bls::BlsShader* spriteVs_ = nullptr;
    bls::BlsShader* dofPs_ = nullptr;
    // Blit: shared embedded fullscreen-triangle shaders (owned).
    gfx::ShaderHandle blitVs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle blitPs_ = gfx::ShaderHandle::Invalid;

    gfx::BufferHandle spriteVb_ = gfx::BufferHandle::Invalid; // borrowed
    gfx::PipelineHandle gatherPso_ = gfx::PipelineHandle::Invalid;
    gfx::PipelineHandle blitPso_ = gfx::PipelineHandle::Invalid;
    gfx::Format psoHdrFmt_ = gfx::Format::Unknown;

    gfx::BufferHandle cb_ = gfx::BufferHandle::Invalid;
    gfx::SamplerHandle linearSampler_ = gfx::SamplerHandle::Invalid;
    gfx::SamplerHandle pointSampler_ = gfx::SamplerHandle::Invalid;

    DofParams params_;
    bool shadersReady_ = false;
};

} // namespace whiteout::flakes::renderer::dof
