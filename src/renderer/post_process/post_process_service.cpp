#include "renderer/post_process/post_process_service.h"

#include "compiled_shaders.h"
#include "renderer/bls/bls_shader_cache.h"
#include "renderer/render_target.h"

#include <cstdio>
#include <cstring>
#include <cmath>

namespace whiteout::flakes::renderer::post_process {

namespace {

// PS slot 1 layouts mirror Wc3Shaders cb_structs.slang and the engine's
// PixelConstantsBloom / PixelConstantsBlur (see IDA 0x140436890).

struct BloomExtractCb {
    f32 threshold;
    f32 _pad[3];
};
static_assert(sizeof(BloomExtractCb) == 16, "BloomExtractCb must be 16 bytes");

struct BloomCombineCb {
    f32 _pad0_xy[2];     // cb1[0].xy — unused
    f32 intensityBloom;  // cb1[0].z
    f32 intensityScene;  // cb1[0].w
    f32 saturationBloom; // cb1[1].x
    f32 saturationScene; // cb1[1].y
    f32 _pad1_zw[2];     // cb1[1].zw — unused
};
static_assert(sizeof(BloomCombineCb) == 32, "BloomCombineCb must be 32 bytes");

struct BlurCb {
    // 15 × float4. BlurData[0] is the centre tap and carries the sample
    // count in .w; pairs BlurData[2i+1] / BlurData[2i+2] hold the
    // symmetric +/- offsets at distance (i+1). Layout matches the
    // engine's PixelConstantsBlur exactly so the shipped
    // gaussianblur.bls reads it without changes.
    f32 data[15][4];
};
static_assert(sizeof(BlurCb) == 15 * 16, "BlurCb must be 240 bytes");

constexpr i32 kBlurSampleCount = 15;
constexpr i32 kBlurPairCount = (kBlurSampleCount - 1) / 2;

// Engine's GBuffer::ComputeGaussian (IDA 0x140436c20). The exact prefactor
// is `1 / sqrt(2π·θ)` (not the usual `1/(σ√(2π))`) — but every weight is
// normalised by `totalWeights` afterwards so the prefactor cancels.
// We still mirror the engine form for bit-for-bit consistency with the
// reference numbers in the disassembly comments.
inline f32 ComputeGaussian(f32 n, f32 theta) {
    constexpr f32 kTwoPi = 6.2831855f;
    const f32 a = 1.0f / std::sqrt(kTwoPi * theta);
    const f32 denom = 2.0f * theta * theta;
    return a * std::exp(-(n * n) / denom);
}

} // namespace

void PostProcessService::Init(gfx::IGFXDevice& gfx, gfx::GfxApi api,
                              bls::BlsShaderCache& cache, gfx::BufferHandle spriteVb) {
    gfx_ = &gfx;
    api_ = api;
    spriteVb_ = spriteVb;

    // BLS shaders. `Acquire` returns nullptr if the shader bundle isn't
    // present for this backend — we treat that as "bloom unavailable" and
    // never build PSOs, so RunBloom becomes a quiet no-op rather than
    // taking the renderer down.
    spriteVs_ = cache.Acquire(gfx::ShaderStage::Vertex, "sprite");
    bloomExtractPs_ = cache.Acquire(gfx::ShaderStage::Pixel, "bloomextract");
    bloomCombinePs_ = cache.Acquire(gfx::ShaderStage::Pixel, "bloomcombine");

    // Embedded shader pick — gaussian blur + blit. Same per-backend
    // bytecode chooser the frame-capture path uses for blit.
    using namespace whiteout::flakes::Shaders;
    const u8* blurVs = kGaussianBlurVS;
    usize blurVsN = sizeof(kGaussianBlurVS);
    const u8* blurPs = kGaussianBlurPS;
    usize blurPsN = sizeof(kGaussianBlurPS);
    const u8* blitVs = kBlitVS;
    usize blitVsN = sizeof(kBlitVS);
    const u8* blitPs = kBlitPS;
    usize blitPsN = sizeof(kBlitPS);
    if (api == gfx::GfxApi::Vulkan) {
        blurVs = kGaussianBlurVSSpv;
        blurVsN = sizeof(kGaussianBlurVSSpv);
        blurPs = kGaussianBlurPSSpv;
        blurPsN = sizeof(kGaussianBlurPSSpv);
        blitVs = kBlitVSSpv;
        blitVsN = sizeof(kBlitVSSpv);
        blitPs = kBlitPSSpv;
        blitPsN = sizeof(kBlitPSSpv);
    } else if (api == gfx::GfxApi::WebGPU) {
        blurVs = kGaussianBlurVSWgsl;
        blurVsN = sizeof(kGaussianBlurVSWgsl);
        blurPs = kGaussianBlurPSWgsl;
        blurPsN = sizeof(kGaussianBlurPSWgsl);
        blitVs = kBlitVSWgsl;
        blitVsN = sizeof(kBlitVSWgsl);
        blitPs = kBlitPSWgsl;
        blitPsN = sizeof(kBlitPSWgsl);
    } else if (api == gfx::GfxApi::Metal) {
        blurVs = kGaussianBlurVSMtl;
        blurVsN = sizeof(kGaussianBlurVSMtl);
        blurPs = kGaussianBlurPSMtl;
        blurPsN = sizeof(kGaussianBlurPSMtl);
        blitVs = kBlitVSMtl;
        blitVsN = sizeof(kBlitVSMtl);
        blitPs = kBlitPSMtl;
        blitPsN = sizeof(kBlitPSMtl);
    }
    if (blurVsN > 1 && blurPsN > 1) {
        blurVs_ = gfx_->CreateShader(gfx::ShaderStage::Vertex, blurVs, blurVsN);
        blurPs_ = gfx_->CreateShader(gfx::ShaderStage::Pixel, blurPs, blurPsN);
    }
    if (blitVsN > 1 && blitPsN > 1) {
        blitVs_ = gfx_->CreateShader(gfx::ShaderStage::Vertex, blitVs, blitVsN);
        blitPs_ = gfx_->CreateShader(gfx::ShaderStage::Pixel, blitPs, blitPsN);
    }

    extractCb_ = gfx_->CreateBuffer({
        .size = sizeof(BloomExtractCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });
    combineCb_ = gfx_->CreateBuffer({
        .size = sizeof(BloomCombineCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });
    blurCb_ = gfx_->CreateBuffer({
        .size = sizeof(BlurCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });

    gfx::SamplerDesc sd;
    sd.minFilter = gfx::Filter::Linear;
    sd.magFilter = gfx::Filter::Linear;
    sd.addressU = gfx::AddressMode::Clamp;
    sd.addressV = gfx::AddressMode::Clamp;
    sd.addressW = gfx::AddressMode::Clamp;
    linearSampler_ = gfx_->CreateSampler(sd);

    auto blsOk = [](bls::BlsShader* s) { return s && !s->permuteHandles.empty(); };
    shadersReady_ = blsOk(spriteVs_) && blsOk(bloomExtractPs_) && blsOk(bloomCombinePs_) &&
                    blurVs_ != gfx::ShaderHandle::Invalid &&
                    blurPs_ != gfx::ShaderHandle::Invalid &&
                    blitVs_ != gfx::ShaderHandle::Invalid &&
                    blitPs_ != gfx::ShaderHandle::Invalid;
    if (!shadersReady_) {
        std::fprintf(
            stderr,
            "[post_process] bloom disabled — missing shader (sprite=%p extract=%p "
            "combine=%p blurVs=%llu blurPs=%llu blitVs=%llu blitPs=%llu)\n",
            (void*)spriteVs_, (void*)bloomExtractPs_, (void*)bloomCombinePs_,
            (unsigned long long)blurVs_, (unsigned long long)blurPs_,
            (unsigned long long)blitVs_, (unsigned long long)blitPs_);
    }
}

void PostProcessService::Shutdown() {
    if (!gfx_)
        return;
    if (extractPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(extractPso_);
    if (blurPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(blurPso_);
    if (combinePso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(combinePso_);
    if (blitPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(blitPso_);
    if (blurVs_ != gfx::ShaderHandle::Invalid)
        gfx_->Destroy(blurVs_);
    if (blurPs_ != gfx::ShaderHandle::Invalid)
        gfx_->Destroy(blurPs_);
    if (blitVs_ != gfx::ShaderHandle::Invalid)
        gfx_->Destroy(blitVs_);
    if (blitPs_ != gfx::ShaderHandle::Invalid)
        gfx_->Destroy(blitPs_);
    if (extractCb_ != gfx::BufferHandle::Invalid)
        gfx_->Destroy(extractCb_);
    if (combineCb_ != gfx::BufferHandle::Invalid)
        gfx_->Destroy(combineCb_);
    if (blurCb_ != gfx::BufferHandle::Invalid)
        gfx_->Destroy(blurCb_);
    if (linearSampler_ != gfx::SamplerHandle::Invalid)
        gfx_->Destroy(linearSampler_);

    extractPso_ = blurPso_ = combinePso_ = blitPso_ = gfx::PipelineHandle::Invalid;
    blurVs_ = blurPs_ = blitVs_ = blitPs_ = gfx::ShaderHandle::Invalid;
    extractCb_ = combineCb_ = blurCb_ = spriteVb_ = gfx::BufferHandle::Invalid;
    linearSampler_ = gfx::SamplerHandle::Invalid;
    spriteVs_ = bloomExtractPs_ = bloomCombinePs_ = nullptr;
    psoHdrFmt_ = gfx::Format::Unknown;
    shadersReady_ = false;
    gfx_ = nullptr;
}

void PostProcessService::EnsurePsos(gfx::Format hdrFmt) {
    if (extractPso_ != gfx::PipelineHandle::Invalid &&
        blurPso_ != gfx::PipelineHandle::Invalid &&
        combinePso_ != gfx::PipelineHandle::Invalid &&
        blitPso_ != gfx::PipelineHandle::Invalid && psoHdrFmt_ == hdrFmt)
        return;

    if (extractPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(extractPso_);
    if (blurPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(blurPso_);
    if (combinePso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(combinePso_);
    if (blitPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(blitPso_);

    // sprite VS input layout — matches the tonemap pass: 12-byte pos +
    // 8-byte uv at ATTR0 / ATTR3 (sprite VB layout).
    const gfx::InputElement spriteInput[] = {
        {"ATTR", 0, gfx::Format::R32G32B32_FLOAT, 0},
        {"ATTR", 3, gfx::Format::R32G32_FLOAT, 12},
    };

    auto build = [&](gfx::ShaderHandle vs, gfx::ShaderHandle ps,
                     const gfx::InputElement* layout, u32 layoutCount) {
        gfx::GraphicsPipelineDesc d{};
        d.vs = vs;
        d.ps = ps;
        if (layoutCount > 0)
            d.inputLayout = std::span<const gfx::InputElement>(layout, layoutCount);
        d.topology = gfx::PrimitiveTopology::TriangleList;
        d.blend.enable = false;
        d.blend.colorWrite = true;
        d.depthStencil.depthTest = false;
        d.depthStencil.depthWrite = false;
        d.rasterizer.cull = gfx::CullMode::None;
        d.rasterizer.frontCCW = true;
        d.rtvFormat = hdrFmt;
        // dsvFormat MUST match the depth format the WebGPU backend
        // auto-attaches when the caller passes Invalid for depth (see
        // webgpu_command_list.cpp's transientDepth path) — Dawn validates
        // attachment-state at SetPipeline time and rejects PSOs that
        // declare no depth when the renderpass has one. Same trick the
        // GTAO + tonemap PSOs use. Depth test+write are off so the
        // attachment is inert at runtime.
        d.dsvFormat = gfx::Format::D24_UNORM_S8_UINT;
        return gfx_->CreateGraphicsPipeline(d);
    };

    // Extract + combine share the sprite VS + 3-vertex fullscreen-tri
    // VB. Combine uses permutation index 0 (CLAMP_OUTPUT=false) so the
    // tonemap downstream still has full HDR range to grade.
    extractPso_ = build(spriteVs_->permuteHandles[0], bloomExtractPs_->permuteHandles[0],
                        spriteInput, 2);
    combinePso_ = build(spriteVs_->permuteHandles[0], bloomCombinePs_->permuteHandles[0],
                        spriteInput, 2);
    // Blur + blit use the embedded vertexId-driven fullscreen triangle
    // (no VB, no input layout) — see gaussian_blur.slang / blit.slang.
    blurPso_ = build(blurVs_, blurPs_, nullptr, 0);
    blitPso_ = build(blitVs_, blitPs_, nullptr, 0);

    psoHdrFmt_ = hdrFmt;
}

void PostProcessService::PackBloomExtractCb() {
    if (void* mapped = gfx_->MapBuffer(extractCb_)) {
        BloomExtractCb cb{};
        cb.threshold = params_.threshold;
        std::memcpy(mapped, &cb, sizeof(cb));
        gfx_->UnmapBuffer(extractCb_);
    }
}

void PostProcessService::PackBloomCombineCb() {
    if (void* mapped = gfx_->MapBuffer(combineCb_)) {
        BloomCombineCb cb{};
        cb.intensityBloom = params_.intensity;
        cb.intensityScene = params_.baseIntensity;
        cb.saturationBloom = params_.saturation;
        cb.saturationScene = params_.baseSaturation;
        std::memcpy(mapped, &cb, sizeof(cb));
        gfx_->UnmapBuffer(combineCb_);
    }
}

void PostProcessService::PackBlurCb(bool horizontal, f32 invW, f32 invH) {
    // Mirrors GBuffer::ComputeBlurConstants (IDA 0x140436890): centre tap
    // at [0], symmetric +/- pairs at [2i+1] / [2i+2] for i in 0..6 with
    // sample distance i+1. All weights renormalised so they sum to 1.
    const f32 dx = horizontal ? invW : 0.0f;
    const f32 dy = horizontal ? 0.0f : invH;
    BlurCb cb{};
    cb.data[0][0] = 0.0f;
    cb.data[0][1] = 0.0f;
    cb.data[0][2] = ComputeGaussian(0.0f, params_.blurSigma);
    cb.data[0][3] = static_cast<f32>(kBlurSampleCount);
    f32 total = cb.data[0][2];
    for (i32 i = 0; i < kBlurPairCount; ++i) {
        const f32 distance = static_cast<f32>(i + 1);
        const f32 weight = ComputeGaussian(distance, params_.blurSigma);
        const i32 idxPos = 2 * i + 1;
        const i32 idxNeg = 2 * i + 2;
        cb.data[idxPos][0] = dx * distance;
        cb.data[idxPos][1] = dy * distance;
        cb.data[idxPos][2] = weight;
        cb.data[idxPos][3] = 0.0f;
        cb.data[idxNeg][0] = -dx * distance;
        cb.data[idxNeg][1] = -dy * distance;
        cb.data[idxNeg][2] = weight;
        cb.data[idxNeg][3] = 0.0f;
        total += weight * 2.0f;
    }
    if (total > 1.0e-6f) {
        const f32 inv = 1.0f / total;
        for (i32 k = 0; k < kBlurSampleCount; ++k)
            cb.data[k][2] *= inv;
    }
    if (void* mapped = gfx_->MapBuffer(blurCb_)) {
        std::memcpy(mapped, &cb, sizeof(cb));
        gfx_->UnmapBuffer(blurCb_);
    }
}

void PostProcessService::RunBlit(gfx::IGFXCommandList* cmd, gfx::TextureHandle src,
                                 gfx::TextureHandle dst, i32 w, i32 h) {
    const f32 clearLoad[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    cmd->BeginRenderPass(dst, gfx::TextureHandle::Invalid, clearLoad, 1.0f, 0);
    cmd->SetViewport({0, 0, (f32)w, (f32)h, 0, 1});
    cmd->BindPipeline(blitPso_);
    cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, src);
    cmd->BindSampler(gfx::ShaderStage::Pixel, 0, linearSampler_);
    cmd->Draw(3, 0);
    cmd->EndRenderPass();
}

void PostProcessService::RunBloom(gfx::IGFXCommandList* cmd, const RenderTarget& target) {
    if (!IsBloomEnabled() || !cmd ||
        target.hdrColor == gfx::TextureHandle::Invalid ||
        target.bloomScratchA == gfx::TextureHandle::Invalid ||
        target.bloomScratchB == gfx::TextureHandle::Invalid ||
        spriteVb_ == gfx::BufferHandle::Invalid)
        return;

    EnsurePsos(/*hdrFmt=*/gfx::Format::R11G11B10_FLOAT);
    if (extractPso_ == gfx::PipelineHandle::Invalid ||
        blurPso_ == gfx::PipelineHandle::Invalid ||
        combinePso_ == gfx::PipelineHandle::Invalid ||
        blitPso_ == gfx::PipelineHandle::Invalid)
        return;

    const f32 w = static_cast<f32>(target.width);
    const f32 h = static_cast<f32>(target.height);
    const f32 invW = (target.width > 0) ? 1.0f / w : 0.0f;
    const f32 invH = (target.height > 0) ? 1.0f / h : 0.0f;
    const f32 clearAttach[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const u32 spriteStride = static_cast<u32>(sizeof(f32) * 5);

    PackBloomExtractCb();
    PackBloomCombineCb();

    // --- Pass 1: BloomExtract --------------------------------------------
    //   hdrColor (t0) → bloomScratchA — bright-pass with luma threshold.
    {
        cmd->BeginRenderPass(target.bloomScratchA, gfx::TextureHandle::Invalid, clearAttach,
                             1.0f, 0);
        cmd->SetViewport({0, 0, w, h, 0, 1});
        cmd->BindPipeline(extractPso_);
        cmd->BindVertexBuffer(0, spriteVb_, spriteStride);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 1, extractCb_);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, target.hdrColor);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, linearSampler_);
        cmd->Draw(3, 0);
        cmd->EndRenderPass();
    }

    // --- Pass 2: GaussianBlur horizontal ---------------------------------
    //   bloomScratchA → bloomScratchB. Embedded shader; no VB. CB at
    //   slot 0 to match gaussian_blur.slang's `register(b0)` binding.
    PackBlurCb(/*horizontal=*/true, invW, invH);
    {
        cmd->BeginRenderPass(target.bloomScratchB, gfx::TextureHandle::Invalid, clearAttach,
                             1.0f, 0);
        cmd->SetViewport({0, 0, w, h, 0, 1});
        cmd->BindPipeline(blurPso_);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, blurCb_);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, target.bloomScratchA);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, linearSampler_);
        cmd->Draw(3, 0);
        cmd->EndRenderPass();
    }

    // --- Pass 3: GaussianBlur vertical -----------------------------------
    //   bloomScratchB → bloomScratchA.
    PackBlurCb(/*horizontal=*/false, invW, invH);
    {
        cmd->BeginRenderPass(target.bloomScratchA, gfx::TextureHandle::Invalid, clearAttach,
                             1.0f, 0);
        cmd->SetViewport({0, 0, w, h, 0, 1});
        cmd->BindPipeline(blurPso_);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, blurCb_);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, target.bloomScratchB);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, linearSampler_);
        cmd->Draw(3, 0);
        cmd->EndRenderPass();
    }

    // --- Pass 4: BloomCombine --------------------------------------------
    //   hdrColor (t0) + blurred bloom in bloomScratchA (t1) → bloomScratchB.
    //   Can't read+write hdrColor in the same pass, so we land in
    //   bloomScratchB and blit back in pass 5.
    {
        cmd->BeginRenderPass(target.bloomScratchB, gfx::TextureHandle::Invalid, clearAttach,
                             1.0f, 0);
        cmd->SetViewport({0, 0, w, h, 0, 1});
        cmd->BindPipeline(combinePso_);
        cmd->BindVertexBuffer(0, spriteVb_, spriteStride);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 1, combineCb_);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, target.hdrColor);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 1, target.bloomScratchA);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, linearSampler_);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 1, linearSampler_);
        cmd->Draw(3, 0);
        cmd->EndRenderPass();
    }

    // --- Pass 5: Blit ----------------------------------------------------
    //   bloomScratchB → hdrColor — restores the composited result to
    //   hdrColor so the tonemap pass downstream reads its usual source.
    RunBlit(cmd, target.bloomScratchB, target.hdrColor, target.width, target.height);
}

} // namespace whiteout::flakes::renderer::post_process
