#pragma once

// GtaoService — owns the GTAO ambient-occlusion post-process state and
// drives its two passes per frame (compute AO into target.aoBuffer,
// then modulate target.hdrColor in place via load-op + multiplicative
// blend). HD-only; SD frames skip the service entirely.
//
// Lifecycle mirrors ShadowService: Init at gfx-device creation, Shutdown
// before the device drops, Run once per frame between the HD opaque
// pass's EndRenderPass and the tonemap pass. Resources (shaders, PSOs,
// CB, sampler) live for the lifetime of the device; they're rebuilt
// lazily when the AO format or HDR format the PSO was matched against
// changes (e.g. swap-chain reformat).

#include "gfx/gfx.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer {
struct RenderTarget;
}

namespace whiteout::flakes::renderer::gtao {

// Slice/step quality presets. Values index the per-PSO shader handle
// arrays in GtaoService; keep contiguous and starting at 0.
enum class Quality : u32 {
    Low = 0,    // 2 slices × 2 steps  — cheapest, visible banding.
    Medium = 1, // 4 slices × 4 steps  — default, matches V1.
    High = 2,   // 8 slices × 8 steps  — smoothest, ~4× cost of medium.
    Count = 3,
};

struct GtaoParams {
    bool enabled = true;
    // World-space sample radius in scene units. Larger → bigger creases,
    // more cross-pixel coherence in the result.
    f32 radius = 1.5f;
    // Post-integral power. 1.0 = linear; >1 darkens AO; <1 lightens.
    f32 intensity = 1.5f;
    // Falloff distance multiplier — samples past `radius * falloff`
    // contribute zero occlusion. Tunes how aggressively far samples
    // are dropped to avoid haloing.
    f32 falloff = 1.0f;
    // Debug visualisation mode passed to the apply pass:
    //   0 — multiply AO into HDR (production)
    //   1 — write vec3(ao) — replaces HDR colour with the AO factor
    //   2 — write vec3(1-ao) — shows occlusion as brightness
    u32 debugMode = 0;
};

class GtaoService {
public:
    GtaoService() = default;
    ~GtaoService() = default;

    GtaoService(const GtaoService&) = delete;
    GtaoService& operator=(const GtaoService&) = delete;

    // gfx + api give us the device handle and the active backend so the
    // right embedded shader bytecode variant is selected. Safe to call
    // with api=Unknown — Init becomes a no-op and IsReady() stays false.
    void Init(gfx::IGFXDevice& gfx, gfx::GfxApi api);
    void Shutdown();

    void SetParams(const GtaoParams& p) {
        params_ = p;
    }
    const GtaoParams& Params() const {
        return params_;
    }
    void SetEnabled(bool on) {
        params_.enabled = on;
    }
    bool IsEnabled() const {
        return params_.enabled && IsReady();
    }

    void SetQuality(Quality q) {
        quality_ = q;
    }
    Quality GetQuality() const {
        return quality_;
    }

    // "AO Only" debug view — apply pass writes the AO factor directly to
    // hdrColor instead of modulating the scene colour through a multiplicative
    // blend. Driven from the host's HdDebugMode toggle.
    void SetDebugAoOnly(bool on) {
        debugAoOnly_ = on;
    }
    bool IsReady() const {
        return gfx_ != nullptr && shadersReady_;
    }

    // One frame's worth of GTAO. Runs:
    //   1. Main pass: reads target.linearDepth + target.normalBuffer SRVs,
    //      writes the AO factor to target.aoBuffer.
    //   2. Apply pass: opens target.hdrColor with loadOp=Load, reads
    //      target.aoBuffer, multiplies into the HDR colour in-place via
    //      a DST*SRC blend.
    //
    // `view` / `proj` come from the same camera that built the HD opaque
    // pass — needed for view-space reconstruction. `frameIndex` rotates
    // the per-pixel jitter so the noise pattern decorrelates over time.
    void Run(gfx::IGFXCommandList* cmd, const RenderTarget& target, const Matrix44f& view,
             const Matrix44f& proj, u32 frameIndex);

private:
    void EnsurePsos(gfx::Format aoFmt, gfx::Format hdrFmt);

    gfx::IGFXDevice* gfx_ = nullptr;
    gfx::GfxApi api_ = gfx::GfxApi::D3D12;

    gfx::ShaderHandle vs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle mainPs_[static_cast<u32>(Quality::Count)] = {
        gfx::ShaderHandle::Invalid, gfx::ShaderHandle::Invalid, gfx::ShaderHandle::Invalid};
    gfx::ShaderHandle denoisePs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle applyPs_ = gfx::ShaderHandle::Invalid;

    gfx::PipelineHandle mainPso_[static_cast<u32>(Quality::Count)] = {
        gfx::PipelineHandle::Invalid, gfx::PipelineHandle::Invalid,
        gfx::PipelineHandle::Invalid};
    gfx::PipelineHandle denoisePso_ = gfx::PipelineHandle::Invalid;
    gfx::PipelineHandle applyPso_ = gfx::PipelineHandle::Invalid;
    gfx::PipelineHandle applyPsoDebug_ = gfx::PipelineHandle::Invalid;
    gfx::Format psoAoFmt_ = gfx::Format::Unknown;
    gfx::Format psoHdrFmt_ = gfx::Format::Unknown;

    Quality quality_ = Quality::Medium;
    bool debugAoOnly_ = false;

    gfx::BufferHandle cb_ = gfx::BufferHandle::Invalid;
    gfx::SamplerHandle pointSampler_ = gfx::SamplerHandle::Invalid;

    GtaoParams params_;
    bool shadersReady_ = false;
};

} // namespace whiteout::flakes::renderer::gtao
