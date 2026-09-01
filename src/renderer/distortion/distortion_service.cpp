#include "renderer/distortion/distortion_service.h"

#include "compiled_shaders.h"

#include <cstdio>

namespace whiteout::flakes::renderer::distortion {

namespace {

struct ApplyCb {
    f32 params[4];
};

} // namespace

void DistortionService::Init(gfx::IGFXDevice& gfx, gfx::GfxApi api) {
    gfx_ = &gfx;
    api_ = api;

    using namespace whiteout::flakes::Shaders;
    const u8* applyVs = kD3DistortionApplyVS;
    usize applyVsN = sizeof(kD3DistortionApplyVS);
    const u8* applyPs = kD3DistortionApplyPS;
    usize applyPsN = sizeof(kD3DistortionApplyPS);
    const u8* blitVs = kBlitVS;
    usize blitVsN = sizeof(kBlitVS);
    const u8* blitPs = kBlitPS;
    usize blitPsN = sizeof(kBlitPS);
    if (api == gfx::GfxApi::Vulkan) {
        applyVs = kD3DistortionApplyVSSpv;
        applyVsN = sizeof(kD3DistortionApplyVSSpv);
        applyPs = kD3DistortionApplyPSSpv;
        applyPsN = sizeof(kD3DistortionApplyPSSpv);
        blitVs = kBlitVSSpv;
        blitVsN = sizeof(kBlitVSSpv);
        blitPs = kBlitPSSpv;
        blitPsN = sizeof(kBlitPSSpv);
    } else if (api == gfx::GfxApi::WebGPU) {
        applyVs = kD3DistortionApplyVSWgsl;
        applyVsN = sizeof(kD3DistortionApplyVSWgsl);
        applyPs = kD3DistortionApplyPSWgsl;
        applyPsN = sizeof(kD3DistortionApplyPSWgsl);
        blitVs = kBlitVSWgsl;
        blitVsN = sizeof(kBlitVSWgsl);
        blitPs = kBlitPSWgsl;
        blitPsN = sizeof(kBlitPSWgsl);
    } else if (api == gfx::GfxApi::Metal) {
        applyVs = kD3DistortionApplyVSMtl;
        applyVsN = sizeof(kD3DistortionApplyVSMtl);
        applyPs = kD3DistortionApplyPSMtl;
        applyPsN = sizeof(kD3DistortionApplyPSMtl);
        blitVs = kBlitVSMtl;
        blitVsN = sizeof(kBlitVSMtl);
        blitPs = kBlitPSMtl;
        blitPsN = sizeof(kBlitPSMtl);
    }

    if (applyVsN > 1 && applyPsN > 1) {
        applyVs_ = gfx_->CreateShader(gfx::ShaderStage::Vertex, applyVs, applyVsN);
        applyPs_ = gfx_->CreateShader(gfx::ShaderStage::Pixel, applyPs, applyPsN);
    }
    if (blitVsN > 1 && blitPsN > 1) {
        blitVs_ = gfx_->CreateShader(gfx::ShaderStage::Vertex, blitVs, blitVsN);
        blitPs_ = gfx_->CreateShader(gfx::ShaderStage::Pixel, blitPs, blitPsN);
    }

    applyCb_ = gfx_->CreateBuffer({
        .size = sizeof(ApplyCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });

    gfx::SamplerDesc sd;
    sd.minFilter = gfx::Filter::Linear;
    sd.magFilter = gfx::Filter::Linear;
    sd.addressU = gfx::AddressMode::Clamp;
    sd.addressV = gfx::AddressMode::Clamp;
    sd.addressW = gfx::AddressMode::Clamp;
    clampSampler_ = gfx_->CreateSampler(sd);

    shadersReady_ =
        applyVs_ != gfx::ShaderHandle::Invalid && applyPs_ != gfx::ShaderHandle::Invalid &&
        blitVs_ != gfx::ShaderHandle::Invalid && blitPs_ != gfx::ShaderHandle::Invalid;
    if (!shadersReady_) {
        std::fprintf(stderr,
                     "[d3 distortion] disabled — missing shader (applyVs=%llu applyPs=%llu "
                     "blitVs=%llu blitPs=%llu)\n",
                     (unsigned long long)applyVs_, (unsigned long long)applyPs_,
                     (unsigned long long)blitVs_, (unsigned long long)blitPs_);
    }
}

void DistortionService::Shutdown() {
    if (!gfx_)
        return;
    if (applyPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(applyPso_);
    if (blitPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(blitPso_);
    if (presentPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(presentPso_);
    for (gfx::ShaderHandle* s : {&applyVs_, &applyPs_, &blitVs_, &blitPs_}) {
        if (*s != gfx::ShaderHandle::Invalid)
            gfx_->Destroy(*s);
        *s = gfx::ShaderHandle::Invalid;
    }
    if (applyCb_ != gfx::BufferHandle::Invalid)
        gfx_->Destroy(applyCb_);
    applyCb_ = gfx::BufferHandle::Invalid;
    for (gfx::TextureHandle* t : {&buffer_, &sceneCopy_}) {
        if (*t != gfx::TextureHandle::Invalid)
            gfx_->Destroy(*t);
        *t = gfx::TextureHandle::Invalid;
    }
    if (clampSampler_ != gfx::SamplerHandle::Invalid)
        gfx_->Destroy(clampSampler_);
    clampSampler_ = gfx::SamplerHandle::Invalid;

    applyPso_ = blitPso_ = presentPso_ = gfx::PipelineHandle::Invalid;
    psoSceneFmt_ = psoOutputFmt_ = gfx::Format::Unknown;
    targetSceneFmt_ = gfx::Format::Unknown;
    redirected_ = false;
    written_ = false;
    targetW_ = targetH_ = 0;
    shadersReady_ = false;
    gfx_ = nullptr;
}

bool DistortionService::EnsureTargets(i32 w, i32 h, gfx::Format sceneFormat) {
    if (!gfx_ || w <= 0 || h <= 0)
        return false;
    if (buffer_ != gfx::TextureHandle::Invalid && targetW_ == w && targetH_ == h &&
        targetSceneFmt_ == sceneFormat)
        return true;

    if (buffer_ != gfx::TextureHandle::Invalid)
        gfx_->Destroy(buffer_);
    if (sceneCopy_ != gfx::TextureHandle::Invalid)
        gfx_->Destroy(sceneCopy_);

    buffer_ = gfx_->CreateColorTarget(w, h, kBufferFormat);
    // Matched to whatever the frame's scene colour actually is: SD writes gamma
    // bytes to a UNORM target and HD writes an HDR float one, and the copy has
    // to round-trip either without re-encoding.
    sceneCopy_ = gfx_->CreateColorTarget(w, h, sceneFormat);
    targetW_ = w;
    targetH_ = h;
    targetSceneFmt_ = sceneFormat;
    return buffer_ != gfx::TextureHandle::Invalid && sceneCopy_ != gfx::TextureHandle::Invalid;
}

void DistortionService::EnsurePsos(gfx::Format sceneFormat, gfx::Format outputFormat) {
    if (psoSceneFmt_ == sceneFormat && psoOutputFmt_ == outputFormat &&
        applyPso_ != gfx::PipelineHandle::Invalid)
        return;

    if (applyPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(applyPso_);
    if (blitPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(blitPso_);
    if (presentPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(presentPso_);
    applyPso_ = blitPso_ = presentPso_ = gfx::PipelineHandle::Invalid;

    auto fullscreen = [&](gfx::ShaderHandle vs, gfx::ShaderHandle ps, gfx::Format rtv) {
        gfx::GraphicsPipelineDesc d{};
        d.vs = vs;
        d.ps = ps;
        d.topology = gfx::PrimitiveTopology::TriangleList;
        d.blend.enable = false;
        d.depthStencil.depthTest = false;
        d.depthStencil.depthWrite = false;
        d.rasterizer.cull = gfx::CullMode::None;
        d.rasterizer.frontCCW = true;
        d.rtvFormat = rtv;
        d.dsvFormat = gfx::Format::D24_UNORM_S8_UINT;
        return gfx_->CreateGraphicsPipeline(d);
    };
    // Two formats, for RefractionService's reason: the snapshot writes the scene
    // copy and the resolve writes the caller's output, and under a redirect
    // those are a gamma UNORM and a swap-chain BGRA8/RGBA8. One format for both
    // fails PSO creation on half the machines and silently drops the pass.
    blitPso_ = fullscreen(blitVs_, blitPs_, sceneFormat);
    presentPso_ = fullscreen(blitVs_, blitPs_, outputFormat);
    applyPso_ = fullscreen(applyVs_, applyPs_, outputFormat);

    psoSceneFmt_ = sceneFormat;
    psoOutputFmt_ = outputFormat;
}

gfx::TextureHandle DistortionService::BeginSceneRedirect(i32 w, i32 h, gfx::Format sceneFormat) {
    if (!IsEnabled() || !EnsureTargets(w, h, sceneFormat))
        return gfx::TextureHandle::Invalid;
    redirected_ = true;
    return sceneCopy_;
}

gfx::TextureHandle DistortionService::BeginBuffer(gfx::IGFXCommandList* cmd, i32 w, i32 h,
                                                 gfx::Format sceneFormat) {
    if (!cmd || !IsEnabled() || !EnsureTargets(w, h, sceneFormat))
        return gfx::TextureHandle::Invalid;
    // The neutral, and it is arithmetic rather than taste: the resolve reads
    // `(rg * 2 - 1) * strength`, so 0.5 is zero offset and anything else shifts
    // every pixel nothing drew on. Alpha 0 so the SRCALPHA / INVSRCALPHA write
    // every shipped distortion pass uses leaves an untouched pixel neutral.
    const f32 clear[4] = {0.5f, 0.5f, 0.5f, 0.0f};
    cmd->BeginRenderPass(buffer_, gfx::TextureHandle::Invalid, clear, 1.0f, 0);
    cmd->EndRenderPass();
    written_ = false;
    return buffer_;
}

void DistortionService::Run(gfx::IGFXCommandList* cmd, const DistortionFrameInputs& frame) {
    // A redirected frame runs even with nothing to distort: the scene is in
    // `sceneCopy_` and this pass is the only thing that puts it on screen.
    const bool redirected = redirected_;
    const bool written = written_;
    redirected_ = false;
    written_ = false;
    // `IsEnabled()` is deliberately NOT part of the gate for a redirected
    // frame: the scene went into `sceneCopy_` and this pass is the only thing
    // that puts it on screen, so switching the effect off between the redirect
    // and here must present the scene rather than drop the frame.
    if (!cmd || !IsReady() || (!written && !redirected))
        return;
    if (frame.sceneColor == gfx::TextureHandle::Invalid || frame.width <= 0 || frame.height <= 0)
        return;
    if (!EnsureTargets(frame.width, frame.height, frame.sceneFormat))
        return;
    const gfx::Format outputFormat = (frame.outputFormat != gfx::Format::Unknown)
                                         ? frame.outputFormat
                                         : frame.sceneFormat;
    EnsurePsos(frame.sceneFormat, outputFormat);
    if (applyPso_ == gfx::PipelineHandle::Invalid || blitPso_ == gfx::PipelineHandle::Invalid ||
        presentPso_ == gfx::PipelineHandle::Invalid) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                         "[d3 distortion] no pass — pipeline build failed (apply=%llu blit=%llu "
                         "present=%llu)\n",
                         (unsigned long long)applyPso_, (unsigned long long)blitPso_,
                         (unsigned long long)presentPso_);
        }
        return;
    }

    const f32 w = static_cast<f32>(frame.width);
    const f32 h = static_cast<f32>(frame.height);

    // ---- Snapshot the scene ---------------------------------------------
    //
    // Skipped under a redirect: the scene was drawn straight into `sceneCopy_`,
    // so there is nothing to copy and the back buffer this would have sampled
    // could not be sampled anyway.
    if (!redirected) {
        const f32 clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        cmd->BeginRenderPass(sceneCopy_, gfx::TextureHandle::Invalid, clear, 1.0f, 0);
        cmd->SetViewport({0, 0, w, h, 0, 1});
        cmd->BindPipeline(blitPso_);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, frame.sceneColor);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, clampSampler_);
        cmd->Draw(3, 0);
        cmd->EndRenderPass();
    }

    // ---- Bend the scene through the buffer -------------------------------
    //
    // ...or, when NOTHING wrote to it, put the scene back untouched. The engine
    // makes exactly this call: `sub_744200` case 0 asks `sub_7424C0(0)` whether
    // any distortion geometry drew this frame and skips the whole post effect
    // when it did not.
    //
    // Here it is not an optimisation but a correctness requirement, and the
    // difference is a redirect: a frame with no distortion still has to be
    // presented out of `sceneCopy_`, and running the resolve over a buffer this
    // frame never cleared reads whatever the texture was created with. Zero,
    // through `(rg * 2 - 1) * strength`, is a UNIFORM -0.03 shift -- the whole
    // screen slides three percent sideways on a model that carries no
    // distortion at all. That is what it did.
    {
        const bool resolve = params_.enabled && (written || params_.debugShowBuffer);
        if (void* p = gfx_->MapBuffer(applyCb_)) {
            auto* cb = static_cast<ApplyCb*>(p);
            cb->params[0] = params_.strength;
            cb->params[1] = params_.debugShowBuffer ? 1.0f : 0.0f;
            cb->params[2] = 0.0f;
            cb->params[3] = 0.0f;
            gfx_->UnmapBuffer(applyCb_);
        }
        const f32 clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        cmd->BeginRenderPass(frame.sceneColor, gfx::TextureHandle::Invalid, clear, 1.0f, 0);
        cmd->SetViewport({0, 0, w, h, 0, 1});
        if (!resolve) {
            // The straight copy. Identity: same full-screen triangle, same
            // clamped linear sampler, texel centres onto texel centres. Its own
            // PSO because it writes the OUTPUT format here, while `blitPso_`
            // writes the scene copy's — under a redirect those are a gamma
            // UNORM and whichever of RGBA8/BGRA8 the swap chain handed out.
            cmd->BindPipeline(presentPso_);
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, sceneCopy_);
            cmd->BindSampler(gfx::ShaderStage::Pixel, 0, clampSampler_);
        } else {
            cmd->BindPipeline(applyPso_);
            cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, applyCb_);
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, sceneCopy_);
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 1, buffer_);
            cmd->BindSampler(gfx::ShaderStage::Pixel, 0, clampSampler_);
        }
        cmd->Draw(3, 0);
        cmd->EndRenderPass();
    }
}

} // namespace whiteout::flakes::renderer::distortion
