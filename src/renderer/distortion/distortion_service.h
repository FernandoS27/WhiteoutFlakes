#pragma once

// ============================================================================
// DistortionService — Diablo III's screen-space distortion, end to end.
//
// A distortion surface does not draw colour. It writes a signed screen-space
// offset into a side buffer, and a full-screen pass afterwards bends the
// finished scene through it. The engine spells that as a post effect: the
// distortion buffer is engine texture type 19, `sub_744200` case 0 asks whether
// anything drew into it this frame, and `sub_741610` is the resolve — bind
// `g_CoreAssetTable[64]` ("v14 PostFX Distortion"), bind types 1 and 19, draw a
// full-screen quad.
//
// SHAPED UNLIKE RefractionService ON PURPOSE. WoW's refraction emitters have a
// mask shader of their own, so the service can own the whole thing. D3's do
// not: 43 of the 48 shipped distortion passes are `ps_legacy`, the same
// combiner every other D3 surface runs, and five more are `ps_distortion2tex`
// which the surface table expresses as a two-stage chain. Their geometry is
// skinned actor geometry with a bone palette. So the WRITE stays with
// D3StandardShading / D3ParticleShading and only the target moves, and this
// service owns the buffer, the clear and the resolve.
//
// One frame:
//
//   1. `BeginSceneRedirect` — as RefractionService's, and for the same reason:
//      the resolve has to SAMPLE the finished scene, and a swap-chain back
//      buffer cannot be sampled.
//   2. `BeginBuffer` — create (lazily) and clear the distortion buffer, then
//      hand it back for the caller to bind alongside the scene depth.
//   3. the caller submits its phase-3 draws.
//   4. `Run` — blit the scene into `sceneCopy_` (skipped under a redirect,
//      where it is already there) and resolve.
//
// Lifecycle mirrors RefractionService: Init once the device is up, Shutdown
// before it drops. A frame with no distortion surface allocates nothing.
// ============================================================================

#include "gfx/gfx.h"
#include "renderer/types.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::distortion {

struct DistortionParams {
    /// Master enable. On by default, like refraction's and for its reason: this
    /// is not a stylistic post effect but the only way a distortion surface is
    /// visible at all. Off, 45 shipped shaders lose the effect they carry —
    /// and, worse, the surfaces whose ONLY pass is phase 3 vanish entirely,
    /// because they must not draw into the scene either.
    bool enabled = true;

    /// The offset gain in viewport UV. `0.03` is the shipped constant, the only
    /// number in the resolve that is not a bias. Spelled here rather than
    /// included from io/d3/d3_types.h (which states it beside the encoding it
    /// belongs to) so this service stays free of the D3 headers.
    f32 strength = 0.03f;

    /// Show the distortion buffer instead of the bent scene. The engine has no
    /// such view; this is ours, and it exists because an empty buffer and a
    /// buffer full of zero offsets are the same picture through the resolve.
    ///
    /// Shown RAW, deliberately. Amplifying the recentred offset to make a weak
    /// field legible turns the half-LSB the 0.5 clear rounds to (127, not 127.5)
    /// into a visible step, and every billboard's quad then reads as a
    /// hard-edged rectangle that looks like a format mismatch and is not.
    bool debugShowBuffer = false;
};

struct DistortionFrameInputs {
    /// Where the resolved result lands. Under a redirect this is the swap-chain
    /// back buffer and NOT the texture the scene was drawn into.
    gfx::TextureHandle sceneColor = gfx::TextureHandle::Invalid;
    /// Format of the texture holding the finished scene — the redirect target
    /// when redirecting, `sceneColor` otherwise.
    gfx::Format sceneFormat = gfx::Format::Unknown;
    /// Format of `sceneColor` itself; differs only under a redirect.
    gfx::Format outputFormat = gfx::Format::Unknown;
    i32 width = 0;
    i32 height = 0;
};

class DistortionService {
public:
    DistortionService() = default;
    ~DistortionService() = default;

    DistortionService(const DistortionService&) = delete;
    DistortionService& operator=(const DistortionService&) = delete;

    void Init(gfx::IGFXDevice& gfx, gfx::GfxApi api);
    void Shutdown();

    void SetParams(const DistortionParams& p) {
        params_ = p;
    }
    const DistortionParams& Params() const {
        return params_;
    }

    bool IsReady() const {
        return gfx_ != nullptr && shadersReady_;
    }
    bool IsEnabled() const {
        return params_.enabled && IsReady();
    }

    /// @brief The buffer's format.
    ///
    /// UNORM and not float, unlike refraction's mask: this buffer holds a
    /// SIGNED PAIR encoded around 0.5 and the resolve reads it back with a
    /// single `* 2 - 1`, so eight bits per axis is the encoding the shipped
    /// programs were authored against. Refraction's is float because its apply
    /// takes a central DIFFERENCE, which quantises to nothing at 8 bits; this
    /// one reads the value itself.
    static constexpr gfx::Format kBufferFormat = gfx::Format::R8G8B8A8_UNORM;

    /// @brief Claim the scene for this frame; returns the texture it must be
    ///        drawn into instead of the caller's own target, or Invalid.
    gfx::TextureHandle BeginSceneRedirect(i32 w, i32 h, gfx::Format sceneFormat);

    /// @brief Open the frame's distortion buffer, cleared to neutral.
    ///
    /// The clear is (0.5, 0.5, 0.5, 0) and it is forced by the encoding, not
    /// chosen: the resolve reads `(rg * 2 - 1) * strength`, so a pixel nothing
    /// drew must read back exactly 0.5 or the whole screen shifts. The alpha is
    /// 0 because the write blends SRCALPHA / INVSRCALPHA over it — every one of
    /// the 48 shipped distortion passes does — which makes an untouched pixel
    /// keep the neutral and a covered one take the surface's edge term.
    ///
    /// Returns Invalid when the service cannot take the frame; the caller then
    /// skips its distortion draws entirely.
    gfx::TextureHandle BeginBuffer(gfx::IGFXCommandList* cmd, i32 w, i32 h,
                                   gfx::Format sceneFormat);

    /// @brief The buffer opened by the last @ref BeginBuffer, or Invalid.
    gfx::TextureHandle Buffer() const {
        return buffer_;
    }

    /// @brief Tell the service something actually drew, so @ref Run knows the
    ///        buffer is worth resolving.
    void MarkWritten() {
        written_ = true;
    }

    /// @brief The resolve. No-op when unavailable, or when nothing wrote —
    ///        except under a redirect, which always presents (even with the
    ///        effect switched off) because nothing else puts the frame on
    ///        screen.
    void Run(gfx::IGFXCommandList* cmd, const DistortionFrameInputs& frame);

private:
    bool EnsureTargets(i32 w, i32 h, gfx::Format sceneFormat);
    void EnsurePsos(gfx::Format sceneFormat, gfx::Format outputFormat);

    gfx::IGFXDevice* gfx_ = nullptr;
    gfx::GfxApi api_ = gfx::GfxApi::D3D12;

    gfx::ShaderHandle applyVs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle applyPs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle blitVs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle blitPs_ = gfx::ShaderHandle::Invalid;

    gfx::PipelineHandle applyPso_ = gfx::PipelineHandle::Invalid;
    gfx::PipelineHandle blitPso_ = gfx::PipelineHandle::Invalid;
    /// The same blit, but writing the OUTPUT format: what presents a redirected
    /// frame that had no distortion to resolve.
    gfx::PipelineHandle presentPso_ = gfx::PipelineHandle::Invalid;
    gfx::Format psoSceneFmt_ = gfx::Format::Unknown;
    gfx::Format psoOutputFmt_ = gfx::Format::Unknown;

    gfx::BufferHandle applyCb_ = gfx::BufferHandle::Invalid;

    gfx::TextureHandle buffer_ = gfx::TextureHandle::Invalid;
    gfx::TextureHandle sceneCopy_ = gfx::TextureHandle::Invalid;
    i32 targetW_ = 0;
    i32 targetH_ = 0;
    gfx::Format targetSceneFmt_ = gfx::Format::Unknown;

    gfx::SamplerHandle clampSampler_ = gfx::SamplerHandle::Invalid;

    DistortionParams params_;
    bool shadersReady_ = false;
    /// Set by BeginSceneRedirect, cleared by the Run that consumes it.
    bool redirected_ = false;
    /// Set by MarkWritten, cleared by Run. Without it a frame whose only
    /// distortion surface is off-screen would still pay for the resolve.
    bool written_ = false;
};

} // namespace whiteout::flakes::renderer::distortion
