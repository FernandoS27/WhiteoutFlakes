#include "renderer/gtao/gtao_service.h"

#include "compiled_shaders.h"
#include "renderer/render_target.h"

#include <cstdio>
#include <cstring>

namespace whiteout::flakes::renderer::gtao {

namespace {

// CB layout — must match the Slang GtaoCB struct in
// src/renderer/shaders/gtao.slang exactly.
struct GtaoCb {
    Matrix44f view;
    Matrix44f proj;
    Matrix44f invProj;
    f32 viewportXY[2];
    f32 viewportInvXY[2];
    f32 params[4]; // radius, intensity, falloff, _unused
    u32 miscXY[4]; // frameIndex, debugMode, _pad, _pad
};
static_assert(sizeof(GtaoCb) == 64 * 3 + 16 + 16 + 16,
              "GtaoCb size must match shader expectation");

// Per-backend bytecode selection for the GTAO entry points.
struct ShaderBlob {
    const u8* data;
    usize size;
};

struct ShaderBlobs {
    ShaderBlob vs;
    ShaderBlob mainPs[3]; // [Low, Medium, High] — indexed by Quality.
    ShaderBlob denoisePs;
    ShaderBlob applyPs;
};

ShaderBlobs PickBlobs(gfx::GfxApi api) {
    using namespace whiteout::flakes::Shaders;
#define BLOB(SYM) ShaderBlob{ (SYM), sizeof(SYM) }
    ShaderBlobs b{};
    b.vs = BLOB(kGtaoVS);
    b.mainPs[0] = BLOB(kGtaoMainLowPS);
    b.mainPs[1] = BLOB(kGtaoMainPS);
    b.mainPs[2] = BLOB(kGtaoMainHighPS);
    b.denoisePs = BLOB(kGtaoDenoisePS);
    b.applyPs = BLOB(kGtaoApplyPS);
    if (api == gfx::GfxApi::Vulkan) {
        b.vs = BLOB(kGtaoVSSpv);
        b.mainPs[0] = BLOB(kGtaoMainLowPSSpv);
        b.mainPs[1] = BLOB(kGtaoMainPSSpv);
        b.mainPs[2] = BLOB(kGtaoMainHighPSSpv);
        b.denoisePs = BLOB(kGtaoDenoisePSSpv);
        b.applyPs = BLOB(kGtaoApplyPSSpv);
    } else if (api == gfx::GfxApi::WebGPU) {
        b.vs = BLOB(kGtaoVSWgsl);
        b.mainPs[0] = BLOB(kGtaoMainLowPSWgsl);
        b.mainPs[1] = BLOB(kGtaoMainPSWgsl);
        b.mainPs[2] = BLOB(kGtaoMainHighPSWgsl);
        b.denoisePs = BLOB(kGtaoDenoisePSWgsl);
        b.applyPs = BLOB(kGtaoApplyPSWgsl);
    } else if (api == gfx::GfxApi::Metal) {
        b.vs = BLOB(kGtaoVSMtl);
        b.mainPs[0] = BLOB(kGtaoMainLowPSMtl);
        b.mainPs[1] = BLOB(kGtaoMainPSMtl);
        b.mainPs[2] = BLOB(kGtaoMainHighPSMtl);
        b.denoisePs = BLOB(kGtaoDenoisePSMtl);
        b.applyPs = BLOB(kGtaoApplyPSMtl);
    }
    return b;
#undef BLOB
}

Matrix44f InvertProjLH(const Matrix44f& proj) {
    // Standard 4x4 inverse. The projection matrices we feed in are nearly
    // diagonal-dominant; a generic inverse is fine and avoids a hand-rolled
    // analytical form per coordinate convention. Adapted from the Cramer's
    // rule body in many graphics math libs.
    const f32* m = &proj.data[0][0];
    f32 inv[16];

    inv[0]  =  m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
               m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4]  = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
               m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8]  =  m[4] * m[9]  * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
               m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9]  * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
               m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1]  = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
               m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5]  =  m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
               m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9]  = -m[0] * m[9]  * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
               m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] =  m[0] * m[9]  * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
               m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2]  =  m[1] * m[6]  * m[15] - m[1] * m[7]  * m[14] - m[5] * m[2] * m[15] +
               m[5] * m[3] * m[14] + m[13] * m[2] * m[7]  - m[13] * m[3] * m[6];
    inv[6]  = -m[0] * m[6]  * m[15] + m[0] * m[7]  * m[14] + m[4] * m[2] * m[15] -
               m[4] * m[3] * m[14] - m[12] * m[2] * m[7]  + m[12] * m[3] * m[6];
    inv[10] =  m[0] * m[5]  * m[15] - m[0] * m[7]  * m[13] - m[4] * m[1] * m[15] +
               m[4] * m[3] * m[13] + m[12] * m[1] * m[7]  - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5]  * m[14] + m[0] * m[6]  * m[13] + m[4] * m[1] * m[14] -
               m[4] * m[2] * m[13] - m[12] * m[1] * m[6]  + m[12] * m[2] * m[5];
    inv[3]  = -m[1] * m[6]  * m[11] + m[1] * m[7]  * m[10] + m[5] * m[2] * m[11] -
               m[5] * m[3] * m[10] - m[9] * m[2] * m[7]   + m[9] * m[3] * m[6];
    inv[7]  =  m[0] * m[6]  * m[11] - m[0] * m[7]  * m[10] - m[4] * m[2] * m[11] +
               m[4] * m[3] * m[10] + m[8] * m[2] * m[7]   - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5]  * m[11] + m[0] * m[7]  * m[9]  + m[4] * m[1] * m[11] -
               m[4] * m[3] * m[9]  - m[8] * m[1] * m[7]   + m[8] * m[3] * m[5];
    inv[15] =  m[0] * m[5]  * m[10] - m[0] * m[6]  * m[9]  - m[4] * m[1] * m[10] +
               m[4] * m[2] * m[9]  + m[8] * m[1] * m[6]   - m[8] * m[2] * m[5];

    f32 det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f) {
        // Degenerate — return identity so the shader doesn't NaN.
        return Matrix44f::identity();
    }
    f32 invDet = 1.0f / det;
    Matrix44f r{};
    for (i32 i = 0; i < 16; ++i)
        (&r.data[0][0])[i] = inv[i] * invDet;
    return r;
}

} // namespace

void GtaoService::Init(gfx::IGFXDevice& gfx, gfx::GfxApi api) {
    gfx_ = &gfx;
    api_ = api;

    const ShaderBlobs blobs = PickBlobs(api);
    auto blobOk = [](const ShaderBlob& b) { return b.size > 1 && b.data != nullptr; };
    if (!blobOk(blobs.vs) || !blobOk(blobs.mainPs[0]) || !blobOk(blobs.mainPs[1]) ||
        !blobOk(blobs.mainPs[2]) || !blobOk(blobs.denoisePs) || !blobOk(blobs.applyPs)) {
        // Embed pipeline stubs unsupported backends with a single byte.
        std::fprintf(stderr, "[gtao] no shader bytecode for backend %d, disabling GTAO\n",
                     static_cast<i32>(api));
        shadersReady_ = false;
        return;
    }

    vs_ = gfx_->CreateShader(gfx::ShaderStage::Vertex, blobs.vs.data, blobs.vs.size);
    for (u32 q = 0; q < static_cast<u32>(Quality::Count); ++q) {
        mainPs_[q] = gfx_->CreateShader(gfx::ShaderStage::Pixel, blobs.mainPs[q].data,
                                        blobs.mainPs[q].size);
    }
    denoisePs_ =
        gfx_->CreateShader(gfx::ShaderStage::Pixel, blobs.denoisePs.data, blobs.denoisePs.size);
    applyPs_ =
        gfx_->CreateShader(gfx::ShaderStage::Pixel, blobs.applyPs.data, blobs.applyPs.size);

    cb_ = gfx_->CreateBuffer({
        .size = sizeof(GtaoCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });

    gfx::SamplerDesc sd;
    sd.minFilter = gfx::Filter::Point;
    sd.magFilter = gfx::Filter::Point;
    sd.addressU = gfx::AddressMode::Clamp;
    sd.addressV = gfx::AddressMode::Clamp;
    sd.addressW = gfx::AddressMode::Clamp;
    pointSampler_ = gfx_->CreateSampler(sd);

    bool mainOk = true;
    for (u32 q = 0; q < static_cast<u32>(Quality::Count); ++q)
        mainOk = mainOk && (mainPs_[q] != gfx::ShaderHandle::Invalid);
    shadersReady_ = (vs_ != gfx::ShaderHandle::Invalid) && mainOk &&
                    (denoisePs_ != gfx::ShaderHandle::Invalid) &&
                    (applyPs_ != gfx::ShaderHandle::Invalid);
}

void GtaoService::Shutdown() {
    if (!gfx_)
        return;
    for (u32 q = 0; q < static_cast<u32>(Quality::Count); ++q) {
        if (mainPso_[q] != gfx::PipelineHandle::Invalid)
            gfx_->Destroy(mainPso_[q]);
        if (mainPs_[q] != gfx::ShaderHandle::Invalid)
            gfx_->Destroy(mainPs_[q]);
    }
    if (denoisePso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(denoisePso_);
    if (applyPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(applyPso_);
    if (applyPsoDebug_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(applyPsoDebug_);
    if (vs_ != gfx::ShaderHandle::Invalid)
        gfx_->Destroy(vs_);
    if (denoisePs_ != gfx::ShaderHandle::Invalid)
        gfx_->Destroy(denoisePs_);
    if (applyPs_ != gfx::ShaderHandle::Invalid)
        gfx_->Destroy(applyPs_);
    if (cb_ != gfx::BufferHandle::Invalid)
        gfx_->Destroy(cb_);
    if (pointSampler_ != gfx::SamplerHandle::Invalid)
        gfx_->Destroy(pointSampler_);
    vs_ = denoisePs_ = applyPs_ = gfx::ShaderHandle::Invalid;
    for (u32 q = 0; q < static_cast<u32>(Quality::Count); ++q) {
        mainPs_[q] = gfx::ShaderHandle::Invalid;
        mainPso_[q] = gfx::PipelineHandle::Invalid;
    }
    denoisePso_ = applyPso_ = applyPsoDebug_ = gfx::PipelineHandle::Invalid;
    cb_ = gfx::BufferHandle::Invalid;
    pointSampler_ = gfx::SamplerHandle::Invalid;
    psoAoFmt_ = psoHdrFmt_ = gfx::Format::Unknown;
    shadersReady_ = false;
    gfx_ = nullptr;
}

void GtaoService::EnsurePsos(gfx::Format aoFmt, gfx::Format hdrFmt) {
    bool mainOk = true;
    for (u32 q = 0; q < static_cast<u32>(Quality::Count); ++q)
        mainOk = mainOk && (mainPso_[q] != gfx::PipelineHandle::Invalid);
    if (mainOk && psoAoFmt_ == aoFmt && denoisePso_ != gfx::PipelineHandle::Invalid &&
        applyPso_ != gfx::PipelineHandle::Invalid &&
        applyPsoDebug_ != gfx::PipelineHandle::Invalid && psoHdrFmt_ == hdrFmt)
        return;

    for (u32 q = 0; q < static_cast<u32>(Quality::Count); ++q) {
        if (mainPso_[q] != gfx::PipelineHandle::Invalid)
            gfx_->Destroy(mainPso_[q]);
    }
    if (denoisePso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(denoisePso_);
    if (applyPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(applyPso_);
    if (applyPsoDebug_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(applyPsoDebug_);

    // Main pass: full-screen triangle (no VB), writes scalar AO to aoBuffer,
    // depth test off (post-process), no blend. One PSO per quality preset.
    for (u32 q = 0; q < static_cast<u32>(Quality::Count); ++q) {
        gfx::GraphicsPipelineDesc d{};
        d.vs = vs_;
        d.ps = mainPs_[q];
        d.topology = gfx::PrimitiveTopology::TriangleList;
        d.blend.enable = false;
        d.blend.colorWrite = true;
        d.depthStencil.depthTest = false;
        d.depthStencil.depthWrite = false;
        d.rasterizer.cull = gfx::CullMode::None;
        d.rasterizer.frontCCW = true;
        d.rtvFormat = aoFmt;
        d.dsvFormat = gfx::Format::Unknown;
        mainPso_[q] = gfx_->CreateGraphicsPipeline(d);
    }
    psoAoFmt_ = aoFmt;

    // Denoise pass: same FSQ topology, reads aoBufferRaw + linearDepth,
    // writes the denoised scalar AO. No blend; depth test off.
    {
        gfx::GraphicsPipelineDesc d{};
        d.vs = vs_;
        d.ps = denoisePs_;
        d.topology = gfx::PrimitiveTopology::TriangleList;
        d.blend.enable = false;
        d.blend.colorWrite = true;
        d.depthStencil.depthTest = false;
        d.depthStencil.depthWrite = false;
        d.rasterizer.cull = gfx::CullMode::None;
        d.rasterizer.frontCCW = true;
        d.rtvFormat = aoFmt;
        d.dsvFormat = gfx::Format::Unknown;
        denoisePso_ = gfx_->CreateGraphicsPipeline(d);
    }

    // Apply pass: full-screen triangle, writes hdrColor with multiplicative
    // blend: result = src * dst. src is the shader's AO factor; dst is the
    // existing HDR colour preserved by loadOp=Load. Equation reads
    //   dst.rgb = SrcFactor*src + DstFactor*dst
    //           = Zero*src + SrcColor*dst
    //           = src.rgb * dst.rgb
    {
        gfx::GraphicsPipelineDesc d{};
        d.vs = vs_;
        d.ps = applyPs_;
        d.topology = gfx::PrimitiveTopology::TriangleList;
        d.blend.enable = true;
        d.blend.srcColor = gfx::BlendFactor::Zero;
        d.blend.dstColor = gfx::BlendFactor::SrcColor;
        d.blend.opColor = gfx::BlendOp::Add;
        d.blend.srcAlpha = gfx::BlendFactor::One;
        d.blend.dstAlpha = gfx::BlendFactor::Zero;
        d.blend.opAlpha = gfx::BlendOp::Add;
        d.blend.colorWrite = true;
        d.depthStencil.depthTest = false;
        d.depthStencil.depthWrite = false;
        d.rasterizer.cull = gfx::CullMode::None;
        d.rasterizer.frontCCW = true;
        d.rtvFormat = hdrFmt;
        d.dsvFormat = gfx::Format::Unknown;
        applyPso_ = gfx_->CreateGraphicsPipeline(d);
        psoHdrFmt_ = hdrFmt;
    }

    // Debug "AO Only" apply: same FSQ as the apply pass, but with blend
    // disabled so the shader's float4(ao,ao,ao,1) lands directly on
    // hdrColor instead of modulating the existing scene.
    {
        gfx::GraphicsPipelineDesc d{};
        d.vs = vs_;
        d.ps = applyPs_;
        d.topology = gfx::PrimitiveTopology::TriangleList;
        d.blend.enable = false;
        d.blend.colorWrite = true;
        d.depthStencil.depthTest = false;
        d.depthStencil.depthWrite = false;
        d.rasterizer.cull = gfx::CullMode::None;
        d.rasterizer.frontCCW = true;
        d.rtvFormat = hdrFmt;
        d.dsvFormat = gfx::Format::Unknown;
        applyPsoDebug_ = gfx_->CreateGraphicsPipeline(d);
    }
}

void GtaoService::Run(gfx::IGFXCommandList* cmd, const RenderTarget& target,
                      const Matrix44f& view, const Matrix44f& proj, u32 frameIndex) {
    if (!IsEnabled() || !cmd || target.aoBuffer == gfx::TextureHandle::Invalid ||
        target.aoBufferRaw == gfx::TextureHandle::Invalid ||
        target.linearDepth == gfx::TextureHandle::Invalid ||
        target.normalBuffer == gfx::TextureHandle::Invalid ||
        target.hdrColor == gfx::TextureHandle::Invalid)
        return;

    EnsurePsos(/*aoFmt=*/gfx::Format::R8_UNORM, /*hdrFmt=*/gfx::Format::R11G11B10_FLOAT);
    const u32 qIdx = static_cast<u32>(quality_);
    if (qIdx >= static_cast<u32>(Quality::Count) ||
        mainPso_[qIdx] == gfx::PipelineHandle::Invalid ||
        denoisePso_ == gfx::PipelineHandle::Invalid ||
        applyPso_ == gfx::PipelineHandle::Invalid)
        return;

    // Update the GTAO CB. CPU-writable; pack a fresh value every frame.
    if (void* mapped = gfx_->MapBuffer(cb_)) {
        GtaoCb cb{};
        cb.view = view;
        cb.proj = proj;
        cb.invProj = InvertProjLH(proj);
        cb.viewportXY[0] = static_cast<f32>(target.width);
        cb.viewportXY[1] = static_cast<f32>(target.height);
        cb.viewportInvXY[0] = (target.width > 0) ? 1.0f / target.width : 0.0f;
        cb.viewportInvXY[1] = (target.height > 0) ? 1.0f / target.height : 0.0f;
        cb.params[0] = params_.radius;
        cb.params[1] = params_.intensity;
        cb.params[2] = params_.falloff;
        cb.params[3] = 0.0f;
        cb.miscXY[0] = frameIndex;
        cb.miscXY[1] = params_.debugMode;
        cb.miscXY[2] = 0;
        cb.miscXY[3] = 0;
        std::memcpy(mapped, &cb, sizeof(cb));
        gfx_->UnmapBuffer(cb_);
    }

    // --- Main pass: linearDepth + normalBuffer → aoBufferRaw ---
    {
        const f32 clearAo[4] = {1.0f, 0.0f, 0.0f, 0.0f};
        cmd->BeginRenderPass(target.aoBufferRaw, gfx::TextureHandle::Invalid, clearAo, 1.0f, 0);
        cmd->SetViewport({0, 0, (f32)target.width, (f32)target.height, 0, 1});
        cmd->BindPipeline(mainPso_[qIdx]);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, cb_);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, target.linearDepth);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 1, target.normalBuffer);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, pointSampler_);
        cmd->Draw(3, 0);
        cmd->EndRenderPass();
    }

    // --- Denoise pass: aoBufferRaw + linearDepth → aoBuffer ---
    // 5×5 cross-bilateral filter. Slang per-entry register allocation
    // packs g_linearDepth at t0 (declared 1st) and g_aoNoisy at t2 in
    // the denoise PS (it's the 3rd texture declaration; only the two
    // sampled ones survive the emit, but declaration order is kept).
    // Vulkan uses the explicit vk::binding(18, 1) hint → host slot 2.
    {
        const f32 clearAo[4] = {1.0f, 0.0f, 0.0f, 0.0f};
        cmd->BeginRenderPass(target.aoBuffer, gfx::TextureHandle::Invalid, clearAo, 1.0f, 0);
        cmd->SetViewport({0, 0, (f32)target.width, (f32)target.height, 0, 1});
        cmd->BindPipeline(denoisePso_);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, cb_);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, target.linearDepth);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 2, target.aoBufferRaw);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, pointSampler_);
        cmd->Draw(3, 0);
        cmd->EndRenderPass();
    }

    // --- Apply pass: hdrColor (load) ← hdrColor * aoBuffer (via blend) ---
    // Debug "AO Only" routes through the no-blend PSO so the AO factor
    // overwrites the scene colour wholesale instead of modulating it.
    {
        cmd->BeginRenderPassLoad(target.hdrColor, gfx::TextureHandle::Invalid, 1.0f, 0);
        cmd->SetViewport({0, 0, (f32)target.width, (f32)target.height, 0, 1});
        cmd->BindPipeline(debugAoOnly_ ? applyPsoDebug_ : applyPso_);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, cb_);
        // Slang preserves declaration order when assigning HLSL registers
        // even when only a subset is used: g_aoBuffer is declared 4th in
        // gtao.slang and lands at register t3 in the apply PS emit. For
        // Vulkan the explicit vk::binding(17, 1) hint maps slot 1 to the
        // same texture. Bind both so D3D/Vulkan/WGSL converge.
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 1, target.aoBuffer);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 3, target.aoBuffer);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, pointSampler_);
        cmd->Draw(3, 0);
        cmd->EndRenderPass();
    }
}

} // namespace whiteout::flakes::renderer::gtao
