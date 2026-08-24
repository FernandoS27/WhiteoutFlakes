#pragma once

// ============================================================================
// RefractionService — WoW's refraction particles, end to end.
//
// A refraction emitter does not draw colour. It draws a scalar distortion field
// into a side buffer, and a full-screen pass afterwards bends the finished scene
// through it. The client gives that its own M2 pass and its own `RefractionBuffer`
// (Engine/Source/ShaderEffect/RefractionBuffer.cpp); this is the same two steps:
//
//   1. `RunMask`  — the emitters' quads into `mask_`, depth-tested against the
//                   scene's depth but not writing it.
//   2. `RunApply` — blit the scene into `sceneCopy_`, then read
//                   (sceneCopy_, mask_) and write the distorted result back
//                   over the scene colour.
//
// Both use `refraction.slang`; the derivation of every constant is in
// M2_REFRACTION_DESIGN.md. Lifecycle mirrors DofService: Init once the device
// is up, Shutdown before it drops, Run once per frame between the transparent
// scene and the tonemap. The two textures are owned here and re-created lazily
// when the frame's size or scene format changes, so a frame with no refraction
// emitter allocates nothing at all.
// ============================================================================

#include "gfx/gfx.h"
#include "renderer/particle/particle_service.h"
#include "renderer/types.h"
#include "whiteout/flakes/types.h"

#include <vector>

namespace whiteout::flakes::renderer::refraction {

struct RefractionParams {
    /// Master enable. On by default, unlike DoF: refraction is not a stylistic
    /// post-effect but the only way a refraction emitter is visible at all —
    /// off means 366 shipped models silently lose an effect they carry.
    bool enabled = true;

    /// Index-of-refraction ratio. 0 selects the client's own default for its
    /// `RefractRatio` CVar, 1/1.33 (`SetRefractionRatio` @0x100e99340).
    f32 ratio = 0.0f;

    /// Multiplier on the screen-space offset the refracted ray produces. 1 is
    /// the client; exposed because the client's magnitude depends on its own
    /// clip-space convention and this is the one knob that compensates.
    f32 strength = 1.0f;

    /// The retail pixel shader's `* 4.0` on the triple-product mask.
    f32 maskGain = 4.0f;

    /// How far a refraction particle stays visible, in WoW yards. The client's
    /// `1 - saturate(viewZ * 0.01)` is exactly 100.
    f32 fadeDistanceYards = 100.0f;

    /// Show the distortion buffer instead of the distorted scene — the client's
    /// `showRefractionBuffer` console command (@0x1019383f0). Height in red,
    /// coverage in green.
    bool debugShowMask = false;
};

/// Everything about the frame the mask pass needs that is not the geometry.
struct RefractionFrameInputs {
    Matrix44f view = Matrix44f::identity();
    Matrix44f projection = Matrix44f::identity();

    /// Renderer units per WoW yard, so the distance fade stays in yards.
    f32 worldScale = 1.0f;

    /// Where the refracted result lands. In redirect mode (see
    /// BeginSceneRedirect) this is the swap-chain back buffer and NOT the
    /// texture the scene was drawn into.
    gfx::TextureHandle sceneColor = gfx::TextureHandle::Invalid;
    /// Format of the texture holding the finished scene — the redirect target
    /// when redirecting, `sceneColor` otherwise.
    gfx::Format sceneFormat = gfx::Format::Unknown;
    /// Format of `sceneColor` itself. Differs from `sceneFormat` only in
    /// redirect mode, where the scene is an offscreen UNORM and the output is
    /// whichever of RGBA8/BGRA8 the swap chain handed out.
    gfx::Format outputFormat = gfx::Format::Unknown;
    gfx::TextureHandle depth = gfx::TextureHandle::Invalid;
    gfx::Format depthFormat = gfx::Format::D24_UNORM_S8_UINT;

    i32 width = 0;
    i32 height = 0;

    /// One entry per draw in the geometry's `draws`, already resolved by the
    /// caller — the service knows nothing about actors or texture scopes, the
    /// same split DrawParticleEmitter uses.
    std::vector<gfx::TextureHandle> drawTextures;
    gfx::SamplerHandle wrapSampler = gfx::SamplerHandle::Invalid;
};

class RefractionService {
public:
    RefractionService() = default;
    ~RefractionService() = default;

    RefractionService(const RefractionService&) = delete;
    RefractionService& operator=(const RefractionService&) = delete;

    void Init(gfx::IGFXDevice& gfx, gfx::GfxApi api);
    void Shutdown();

    void SetParams(const RefractionParams& p) {
        params_ = p;
    }
    const RefractionParams& Params() const {
        return params_;
    }
    void SetEnabled(bool on) {
        params_.enabled = on;
    }

    bool IsReady() const {
        return gfx_ != nullptr && shadersReady_;
    }
    bool IsEnabled() const {
        return params_.enabled && IsReady();
    }

    /// Claim the scene for this frame and hand back the texture it must be
    /// drawn into instead of the caller's own target.
    ///
    /// Refraction has to SAMPLE the finished scene, and a swap-chain back
    /// buffer cannot be sampled — it is created render-target-only on every
    /// backend, so binding it returns the null descriptor and the apply pass
    /// paints the whole screen black. The fix is the client's own: it renders
    /// the world, copies it to `s_pBackBufferCopyTexture` and reads that.
    /// Here the copy is free — the scene is drawn straight into the texture
    /// the apply pass was going to read anyway, and the apply pass IS the blit
    /// back onto the back buffer.
    ///
    /// The caller must then run `Run` unconditionally for the frame, even with
    /// no refraction geometry: nothing else puts the scene on screen.
    /// Returns Invalid when the service cannot take the frame, in which case
    /// the caller keeps its own target and nothing is redirected.
    gfx::TextureHandle BeginSceneRedirect(i32 w, i32 h, gfx::Format sceneFormat);

    /// One frame's refraction. No-op when disabled, unavailable, or handed no
    /// geometry — in particular this is what makes a scene with no refraction
    /// emitter cost nothing, including the two textures. The one exception is
    /// a redirected frame, which always presents.
    void Run(gfx::IGFXCommandList* cmd, const RefractionFrameInputs& frame,
             const particle::MultiTexGeometry& geo);

private:
    bool EnsureTargets(i32 w, i32 h, gfx::Format sceneFormat);
    void EnsurePsos(gfx::Format sceneFormat, gfx::Format outputFormat, gfx::Format depthFormat);
    bool UploadVertices(const particle::MultiTexGeometry& geo);

    gfx::IGFXDevice* gfx_ = nullptr;
    gfx::GfxApi api_ = gfx::GfxApi::D3D12;

    gfx::ShaderHandle maskVs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle maskPs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle applyVs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle applyPs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle blitVs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle blitPs_ = gfx::ShaderHandle::Invalid;

    // One mask PSO per blend class the shipped emitters use. Built lazily and
    // keyed by (blend, scene format, depth format) — the two formats change
    // when a differently-configured viewport renders.
    static constexpr usize kBlendModes = 5;
    gfx::PipelineHandle maskPso_[kBlendModes] = {};
    gfx::PipelineHandle applyPso_ = gfx::PipelineHandle::Invalid;
    gfx::PipelineHandle blitPso_ = gfx::PipelineHandle::Invalid;
    gfx::Format psoSceneFmt_ = gfx::Format::Unknown;
    gfx::Format psoOutputFmt_ = gfx::Format::Unknown;
    gfx::Format psoDepthFmt_ = gfx::Format::Unknown;

    gfx::BufferHandle vb_ = gfx::BufferHandle::Invalid;
    i32 vbCapacity_ = 0;
    gfx::BufferHandle maskVsCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle maskPsCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle applyCb_ = gfx::BufferHandle::Invalid;

    gfx::TextureHandle mask_ = gfx::TextureHandle::Invalid;
    gfx::TextureHandle sceneCopy_ = gfx::TextureHandle::Invalid;
    i32 targetW_ = 0;
    i32 targetH_ = 0;
    gfx::Format targetSceneFmt_ = gfx::Format::Unknown;

    gfx::SamplerHandle clampSampler_ = gfx::SamplerHandle::Invalid;

    RefractionParams params_;
    bool shadersReady_ = false;
    /// Set by BeginSceneRedirect, cleared by the Run that consumes it. Tells
    /// Run that `sceneCopy_` already holds the scene, so the snapshot blit is
    /// skipped and the apply pass is also what presents.
    bool redirected_ = false;
};

} // namespace whiteout::flakes::renderer::refraction
