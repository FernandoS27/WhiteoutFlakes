#pragma once

// PostProcessService — owns the HD post-process pipeline that runs between
// the GTAO / IBL boost block and the tonemap pass. Today: HDR bloom (4
// fullscreen draws + 1 blit-back). Phase 3 will add CMAA here once the gfx
// layer grows UAV/Storage texture support; until then this service is just
// the bloom driver.
//
// Lifecycle mirrors GtaoService: Init at device-creation time, Shutdown
// before the device drops, RunBloom once per HD frame between IBL boost
// and tonemap. Resources (BLS shader handles, PSOs, CBs, sampler) live for
// the lifetime of the device; the PSOs are rebuilt lazily if the HDR
// format changes (e.g. swap-chain reformat).

#include "gfx/gfx.h"
#include "whiteout/flakes/types.h"

#include <cstdint>

namespace whiteout::flakes::renderer {
struct RenderTarget;
}

namespace whiteout::flakes::renderer::bls {
class BlsShaderCache;
struct BlsShader;
} // namespace whiteout::flakes::renderer::bls

namespace whiteout::flakes::renderer::post_process {

struct BloomParams {
    bool enabled = false;
    // Match the engine's RegisterBloom defaults (see IDA 0x1418b0200 /
    // BL_BLOOMTHRESH_D etc.). These reproduce the Reforged-HD look.
    f32 threshold = 1.0f;        // Bright-pass luma cutoff.
    f32 intensity = 1.25f;       // Gain on the blurred layer in the combine.
    f32 saturation = 1.0f;       // Saturation on the blurred layer.
    f32 baseIntensity = 1.0f;    // Gain on the scene layer in the combine.
    f32 baseSaturation = 1.0f;   // Saturation on the scene layer.
    f32 blurSigma = 4.0f;        // Gaussian σ for the separable blur.
};

class PostProcessService {
public:
    PostProcessService() = default;
    ~PostProcessService() = default;

    PostProcessService(const PostProcessService&) = delete;
    PostProcessService& operator=(const PostProcessService&) = delete;

    // gfx + api give us the device handle and the active backend so the
    // built-in blit shader's per-backend bytecode variant is selected.
    // `cache` supplies the three BLS PS shaders (bloomextract /
    // gaussianblur / bloomcombine) and the sprite VS via `Acquire`.
    // `spriteVb` is the 3-vertex fullscreen-triangle VB the bloom passes
    // bind (re-uses the buffer the tonemap pass already owns).
    void Init(gfx::IGFXDevice& gfx, gfx::GfxApi api, bls::BlsShaderCache& cache,
              gfx::BufferHandle spriteVb);
    void Shutdown();

    void SetParams(const BloomParams& p) {
        params_ = p;
    }
    const BloomParams& Params() const {
        return params_;
    }
    void SetEnabled(bool on) {
        params_.enabled = on;
    }
    bool IsBloomEnabled() const {
        return params_.enabled && IsReady();
    }
    bool IsReady() const {
        return gfx_ != nullptr && shadersReady_;
    }

    // One frame's HDR bloom on `target.hdrColor`:
    //   1. BloomExtract: hdrColor → bloomScratchA   (bright-pass).
    //   2. GaussianBlur H: bloomScratchA → bloomScratchB.
    //   3. GaussianBlur V: bloomScratchB → bloomScratchA.
    //   4. BloomCombine: hdrColor + bloomScratchA → bloomScratchB.
    //   5. Blit: bloomScratchB → hdrColor (so the tonemap downstream
    //      reads the composited result from its usual source).
    // No-op when disabled, when the HDR target is missing, or when the
    // shader/PSO state isn't ready yet.
    void RunBloom(gfx::IGFXCommandList* cmd, const RenderTarget& target);

private:
    void EnsurePsos(gfx::Format hdrFmt);
    void PackBloomExtractCb();
    void PackBloomCombineCb();
    void PackBlurCb(bool horizontal, f32 invW, f32 invH);
    void RunBlit(gfx::IGFXCommandList* cmd, gfx::TextureHandle src,
                 gfx::TextureHandle dst, i32 w, i32 h);

    gfx::IGFXDevice* gfx_ = nullptr;
    gfx::GfxApi api_ = gfx::GfxApi::D3D12;

    // BLS-side shader handles (owned by `cache`, ref-counted via Acquire).
    // Sprite VS + bloom extract / combine use the BLS bundles; the blur
    // pass is an embedded slang shader (kGaussianBlur* below) because
    // Wc3Shaders never authored a blur source, so the engine's shipped
    // gaussianblur.bls isn't staged into the runtime shader dir.
    bls::BlsShader* spriteVs_ = nullptr;
    bls::BlsShader* bloomExtractPs_ = nullptr;
    bls::BlsShader* bloomCombinePs_ = nullptr;

    // Renderer-internal embedded shaders:
    //   blur — drives the separable Gaussian (vertex-id fullscreen tri,
    //          no VB / input layout).
    //   blit — final scratch → hdrColor copy. Reuses the same
    //          `blit.slang` variants the frame-capture path loads.
    gfx::ShaderHandle blurVs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle blurPs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle blitVs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle blitPs_ = gfx::ShaderHandle::Invalid;

    gfx::PipelineHandle extractPso_ = gfx::PipelineHandle::Invalid;
    gfx::PipelineHandle blurPso_ = gfx::PipelineHandle::Invalid;
    gfx::PipelineHandle combinePso_ = gfx::PipelineHandle::Invalid;
    gfx::PipelineHandle blitPso_ = gfx::PipelineHandle::Invalid;
    gfx::Format psoHdrFmt_ = gfx::Format::Unknown;

    // PS slot 1 CBs (matches the engine + Wc3Shaders bindings).
    gfx::BufferHandle extractCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle combineCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle blurCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle spriteVb_ = gfx::BufferHandle::Invalid;
    gfx::SamplerHandle linearSampler_ = gfx::SamplerHandle::Invalid;

    BloomParams params_;
    bool shadersReady_ = false;
};

} // namespace whiteout::flakes::renderer::post_process
