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
    Matrix44f invView;
    Matrix44f prevViewProj;
    f32 viewportXY[2];
    f32 viewportInvXY[2];
    f32 params[4]; // radius, intensity, falloff, temporalAlpha (1 = no history)
    f32 ibl[4];    // envFromMipEnd, envToMipEnd, envTransitionT, boostStrength
    u32 miscXY[4]; // frameIndex, debugMode, _pad, _pad
};
static_assert(sizeof(GtaoCb) == 64 * 5 + 16 + 16 + 16 + 16,
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
    ShaderBlob temporalPs;
    ShaderBlob iblBoostPs;
    ShaderBlob depthCopyPs;
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
    b.temporalPs = BLOB(kGtaoTemporalPS);
    b.iblBoostPs = BLOB(kGtaoIblBoostPS);
    b.depthCopyPs = BLOB(kGtaoDepthCopyPS);
    b.applyPs = BLOB(kGtaoApplyPS);
    if (api == gfx::GfxApi::Vulkan) {
        b.vs = BLOB(kGtaoVSSpv);
        b.mainPs[0] = BLOB(kGtaoMainLowPSSpv);
        b.mainPs[1] = BLOB(kGtaoMainPSSpv);
        b.mainPs[2] = BLOB(kGtaoMainHighPSSpv);
        b.denoisePs = BLOB(kGtaoDenoisePSSpv);
        b.temporalPs = BLOB(kGtaoTemporalPSSpv);
        b.iblBoostPs = BLOB(kGtaoIblBoostPSSpv);
        b.depthCopyPs = BLOB(kGtaoDepthCopyPSSpv);
        b.applyPs = BLOB(kGtaoApplyPSSpv);
    } else if (api == gfx::GfxApi::WebGPU) {
        b.vs = BLOB(kGtaoVSWgsl);
        b.mainPs[0] = BLOB(kGtaoMainLowPSWgsl);
        b.mainPs[1] = BLOB(kGtaoMainPSWgsl);
        b.mainPs[2] = BLOB(kGtaoMainHighPSWgsl);
        b.denoisePs = BLOB(kGtaoDenoisePSWgsl);
        b.temporalPs = BLOB(kGtaoTemporalPSWgsl);
        b.iblBoostPs = BLOB(kGtaoIblBoostPSWgsl);
        b.depthCopyPs = BLOB(kGtaoDepthCopyPSWgsl);
        b.applyPs = BLOB(kGtaoApplyPSWgsl);
    } else if (api == gfx::GfxApi::Metal) {
        b.vs = BLOB(kGtaoVSMtl);
        b.mainPs[0] = BLOB(kGtaoMainLowPSMtl);
        b.mainPs[1] = BLOB(kGtaoMainPSMtl);
        b.mainPs[2] = BLOB(kGtaoMainHighPSMtl);
        b.denoisePs = BLOB(kGtaoDenoisePSMtl);
        b.temporalPs = BLOB(kGtaoTemporalPSMtl);
        b.iblBoostPs = BLOB(kGtaoIblBoostPSMtl);
        b.depthCopyPs = BLOB(kGtaoDepthCopyPSMtl);
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
        !blobOk(blobs.mainPs[2]) || !blobOk(blobs.denoisePs) || !blobOk(blobs.temporalPs) ||
        !blobOk(blobs.iblBoostPs) || !blobOk(blobs.depthCopyPs) || !blobOk(blobs.applyPs)) {
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
    temporalPs_ = gfx_->CreateShader(gfx::ShaderStage::Pixel, blobs.temporalPs.data,
                                     blobs.temporalPs.size);
    iblBoostPs_ = gfx_->CreateShader(gfx::ShaderStage::Pixel, blobs.iblBoostPs.data,
                                     blobs.iblBoostPs.size);
    depthCopyPs_ = gfx_->CreateShader(gfx::ShaderStage::Pixel, blobs.depthCopyPs.data,
                                      blobs.depthCopyPs.size);
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
                    (temporalPs_ != gfx::ShaderHandle::Invalid) &&
                    (iblBoostPs_ != gfx::ShaderHandle::Invalid) &&
                    (depthCopyPs_ != gfx::ShaderHandle::Invalid) &&
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
    if (temporalPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(temporalPso_);
    if (iblBoostPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(iblBoostPso_);
    if (depthCopyPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(depthCopyPso_);
    if (applyPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(applyPso_);
    if (applyPsoDebug_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(applyPsoDebug_);
    if (vs_ != gfx::ShaderHandle::Invalid)
        gfx_->Destroy(vs_);
    if (denoisePs_ != gfx::ShaderHandle::Invalid)
        gfx_->Destroy(denoisePs_);
    if (temporalPs_ != gfx::ShaderHandle::Invalid)
        gfx_->Destroy(temporalPs_);
    if (iblBoostPs_ != gfx::ShaderHandle::Invalid)
        gfx_->Destroy(iblBoostPs_);
    if (depthCopyPs_ != gfx::ShaderHandle::Invalid)
        gfx_->Destroy(depthCopyPs_);
    if (applyPs_ != gfx::ShaderHandle::Invalid)
        gfx_->Destroy(applyPs_);
    if (cb_ != gfx::BufferHandle::Invalid)
        gfx_->Destroy(cb_);
    if (pointSampler_ != gfx::SamplerHandle::Invalid)
        gfx_->Destroy(pointSampler_);
    vs_ = denoisePs_ = temporalPs_ = iblBoostPs_ = depthCopyPs_ = applyPs_ =
        gfx::ShaderHandle::Invalid;
    for (u32 q = 0; q < static_cast<u32>(Quality::Count); ++q) {
        mainPs_[q] = gfx::ShaderHandle::Invalid;
        mainPso_[q] = gfx::PipelineHandle::Invalid;
    }
    denoisePso_ = temporalPso_ = iblBoostPso_ = depthCopyPso_ = applyPso_ = applyPsoDebug_ =
        gfx::PipelineHandle::Invalid;
    prevValid_ = false;
    frameCounter_ = 0;
    lastAoOutput_ = gfx::TextureHandle::Invalid;
    cb_ = gfx::BufferHandle::Invalid;
    pointSampler_ = gfx::SamplerHandle::Invalid;
    psoAoFmt_ = psoHdrFmt_ = psoLinearDepthFmt_ = gfx::Format::Unknown;
    shadersReady_ = false;
    gfx_ = nullptr;
}

void GtaoService::EnsurePsos(gfx::Format aoFmt, gfx::Format hdrFmt,
                             gfx::Format linearDepthFmt) {
    bool mainOk = true;
    for (u32 q = 0; q < static_cast<u32>(Quality::Count); ++q)
        mainOk = mainOk && (mainPso_[q] != gfx::PipelineHandle::Invalid);
    if (mainOk && psoAoFmt_ == aoFmt && denoisePso_ != gfx::PipelineHandle::Invalid &&
        temporalPso_ != gfx::PipelineHandle::Invalid &&
        iblBoostPso_ != gfx::PipelineHandle::Invalid &&
        depthCopyPso_ != gfx::PipelineHandle::Invalid &&
        applyPso_ != gfx::PipelineHandle::Invalid &&
        applyPsoDebug_ != gfx::PipelineHandle::Invalid && psoHdrFmt_ == hdrFmt &&
        psoLinearDepthFmt_ == linearDepthFmt)
        return;

    for (u32 q = 0; q < static_cast<u32>(Quality::Count); ++q) {
        if (mainPso_[q] != gfx::PipelineHandle::Invalid)
            gfx_->Destroy(mainPso_[q]);
    }
    if (denoisePso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(denoisePso_);
    if (temporalPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(temporalPso_);
    if (iblBoostPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(iblBoostPso_);
    if (depthCopyPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(depthCopyPso_);
    if (applyPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(applyPso_);
    if (applyPsoDebug_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(applyPsoDebug_);

    // Main pass: full-screen triangle (no VB), writes scalar AO at slot 0
    // and the view-space bent normal at slot 1, depth test off, no blend.
    // One PSO per quality preset. The bent-normal format is fixed (RGBA8
    // UNORM); aoFmt drives slot 0.
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
        d.extraRtvFormats[0] = gfx::Format::R8G8B8A8_UNORM;
        d.extraRtvCount = 1;
        // dsvFormat matches the depth-stencil format the WebGPU backend
        // auto-attaches when the caller passes Invalid for depth (see
        // webgpu_command_list.cpp's transientDepth path). Depth test +
        // write are off, so the attachment is a no-op at runtime; the
        // format only exists to satisfy strict attachment-state match.
        d.dsvFormat = gfx::Format::D24_UNORM_S8_UINT;
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
        // dsvFormat matches the depth-stencil format the WebGPU backend
        // auto-attaches when the caller passes Invalid for depth (see
        // webgpu_command_list.cpp's transientDepth path). Depth test +
        // write are off, so the attachment is a no-op at runtime; the
        // format only exists to satisfy strict attachment-state match.
        d.dsvFormat = gfx::Format::D24_UNORM_S8_UINT;
        denoisePso_ = gfx_->CreateGraphicsPipeline(d);
    }

    // Temporal pass: same FSQ topology, reads aoBufferDenoised (current
    // spatial output) + aoBufferHistory + linearDepth, writes the
    // temporally-stabilised scalar AO.
    {
        gfx::GraphicsPipelineDesc d{};
        d.vs = vs_;
        d.ps = temporalPs_;
        d.topology = gfx::PrimitiveTopology::TriangleList;
        d.blend.enable = false;
        d.blend.colorWrite = true;
        d.depthStencil.depthTest = false;
        d.depthStencil.depthWrite = false;
        d.rasterizer.cull = gfx::CullMode::None;
        d.rasterizer.frontCCW = true;
        d.rtvFormat = aoFmt;
        // dsvFormat matches the depth-stencil format the WebGPU backend
        // auto-attaches when the caller passes Invalid for depth (see
        // webgpu_command_list.cpp's transientDepth path). Depth test +
        // write are off, so the attachment is a no-op at runtime; the
        // format only exists to satisfy strict attachment-state match.
        d.dsvFormat = gfx::Format::D24_UNORM_S8_UINT;
        temporalPso_ = gfx_->CreateGraphicsPipeline(d);
    }

    // Depth-copy pass: a single-tap blit that writes the current frame's
    // linearDepth into linearDepthHistory at the tail of the GTAO
    // pipeline. Next frame's temporal pass reads it for disocclusion
    // rejection. Rasterised at the full-res viewport; rtv format = the
    // linearDepth texture format (R32F).
    {
        gfx::GraphicsPipelineDesc d{};
        d.vs = vs_;
        d.ps = depthCopyPs_;
        d.topology = gfx::PrimitiveTopology::TriangleList;
        d.blend.enable = false;
        d.blend.colorWrite = true;
        d.depthStencil.depthTest = false;
        d.depthStencil.depthWrite = false;
        d.rasterizer.cull = gfx::CullMode::None;
        d.rasterizer.frontCCW = true;
        d.rtvFormat = linearDepthFmt;
        // Depth-copy uses BeginRenderPass (DOES auto-attach transient
        // depth on WebGPU), so the dsvFormat matches the same default.
        d.dsvFormat = gfx::Format::D24_UNORM_S8_UINT;
        depthCopyPso_ = gfx_->CreateGraphicsPipeline(d);
        psoLinearDepthFmt_ = linearDepthFmt;
    }

    // IBL bent-normal boost pass: full-screen triangle, additively
    // blends `iblDiffuse(bentN) * (1-ao) * strength` into hdrColor.
    //   dst.rgb = One*src + One*dst = src + dst
    {
        gfx::GraphicsPipelineDesc d{};
        d.vs = vs_;
        d.ps = iblBoostPs_;
        d.topology = gfx::PrimitiveTopology::TriangleList;
        d.blend.enable = true;
        d.blend.srcColor = gfx::BlendFactor::One;
        d.blend.dstColor = gfx::BlendFactor::One;
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
        // Boost runs via BeginRenderPassLoad (no auto-attach depth on
        // WebGPU); leave dsvFormat Unknown to match the load pass.
        d.dsvFormat = gfx::Format::Unknown;
        iblBoostPso_ = gfx_->CreateGraphicsPipeline(d);
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
        // Apply runs via BeginRenderPassLoad, which DOES NOT auto-attach
        // a transient depth on WebGPU (only BeginRenderPass does). Keep
        // dsvFormat at Unknown so the PSO matches the depth-less load
        // pass.
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
        // Apply runs via BeginRenderPassLoad, which DOES NOT auto-attach
        // a transient depth on WebGPU (only BeginRenderPass does). Keep
        // dsvFormat at Unknown so the PSO matches the depth-less load
        // pass.
        d.dsvFormat = gfx::Format::Unknown;
        applyPsoDebug_ = gfx_->CreateGraphicsPipeline(d);
    }
}

void GtaoService::Run(gfx::IGFXCommandList* cmd, const RenderTarget& target,
                      const Matrix44f& view, const Matrix44f& proj, u32 frameIndex) {
    if (!IsEnabled() || !cmd || target.aoBuffer == gfx::TextureHandle::Invalid ||
        target.aoBufferRaw == gfx::TextureHandle::Invalid ||
        target.aoBufferDenoised == gfx::TextureHandle::Invalid ||
        target.aoBufferHistory == gfx::TextureHandle::Invalid ||
        target.bentNormalBuffer == gfx::TextureHandle::Invalid ||
        target.linearDepth == gfx::TextureHandle::Invalid ||
        target.linearDepthHistory == gfx::TextureHandle::Invalid ||
        target.normalBuffer == gfx::TextureHandle::Invalid ||
        target.hdrColor == gfx::TextureHandle::Invalid)
        return;

    EnsurePsos(/*aoFmt=*/gfx::Format::R8_UNORM, /*hdrFmt=*/gfx::Format::R11G11B10_FLOAT,
               /*linearDepthFmt=*/gfx::Format::R32_FLOAT);
    const u32 qIdx = static_cast<u32>(quality_);
    if (qIdx >= static_cast<u32>(Quality::Count) ||
        mainPso_[qIdx] == gfx::PipelineHandle::Invalid ||
        denoisePso_ == gfx::PipelineHandle::Invalid ||
        temporalPso_ == gfx::PipelineHandle::Invalid ||
        depthCopyPso_ == gfx::PipelineHandle::Invalid ||
        applyPso_ == gfx::PipelineHandle::Invalid)
        return;

    // Temporal ping-pong. Each frame we alternate which physical texture
    // is "current frame's output" vs. "history". The apply pass reads
    // whichever was the write target this frame.
    const bool       evenFrame    = (frameCounter_ & 1u) == 0u;
    const gfx::TextureHandle curAo   = evenFrame ? target.aoBuffer : target.aoBufferHistory;
    const gfx::TextureHandle histAo  = evenFrame ? target.aoBufferHistory : target.aoBuffer;

    // Build the world→clip transform for THIS frame. Stored after Run
    // for next frame's temporal sample; the saved copy from last frame
    // feeds the shader this frame.
    const Matrix44f curViewProj   = view * proj;
    const Matrix44f curInvView    = Matrix44f::inverse(view);
    const Matrix44f sampleViewProj = prevValid_ ? prevViewProj_ : curViewProj;
    const f32       temporalAlpha = prevValid_ ? 0.1f : 1.0f;

    if (void* mapped = gfx_->MapBuffer(cb_)) {
        GtaoCb cb{};
        cb.view = view;
        cb.proj = proj;
        cb.invProj = InvertProjLH(proj);
        cb.invView = curInvView;
        cb.prevViewProj = sampleViewProj;
        cb.viewportXY[0] = static_cast<f32>(target.width);
        cb.viewportXY[1] = static_cast<f32>(target.height);
        cb.viewportInvXY[0] = (target.width > 0) ? 1.0f / target.width : 0.0f;
        cb.viewportInvXY[1] = (target.height > 0) ? 1.0f / target.height : 0.0f;
        cb.params[0] = params_.radius;
        cb.params[1] = params_.intensity;
        cb.params[2] = params_.falloff;
        cb.params[3] = temporalAlpha;
        cb.ibl[0] = 0.0f;
        cb.ibl[1] = 0.0f;
        cb.ibl[2] = 0.0f;
        cb.ibl[3] = 0.0f;
        cb.miscXY[0] = frameIndex;
        cb.miscXY[1] = params_.debugMode;
        cb.miscXY[2] = 0;
        cb.miscXY[3] = 0;
        std::memcpy(mapped, &cb, sizeof(cb));
        gfx_->UnmapBuffer(cb_);
    }

    // --- Main pass: linearDepth + normalBuffer → aoBufferRaw + bentNormal ---
    {
        const gfx::TextureHandle colors[2] = {target.aoBufferRaw, target.bentNormalBuffer};
        const f32 clears[2][4] = {
            {1.0f, 0.0f, 0.0f, 0.0f}, // AO = 1 (fully unoccluded sentinel)
            {0.5f, 0.5f, 1.0f, 1.0f}, // bent normal = encoded +Z (camera-forward)
        };
        cmd->BeginRenderPass(colors, 2, target.depth, clears, 1.0f, 0);
        cmd->SetViewport({0, 0, (f32)target.width, (f32)target.height, 0, 1});
        cmd->BindPipeline(mainPso_[qIdx]);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, cb_);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, target.linearDepth);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 1, target.normalBuffer);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, pointSampler_);
        cmd->Draw(3, 0);
        cmd->EndRenderPass();
    }

    // --- Denoise pass: aoBufferRaw + linearDepth → aoBufferDenoised ---
    {
        const f32 clearAo[4] = {1.0f, 0.0f, 0.0f, 0.0f};
        cmd->BeginRenderPass(target.aoBufferDenoised, target.depth, clearAo, 1.0f, 0);
        cmd->SetViewport({0, 0, (f32)target.width, (f32)target.height, 0, 1});
        cmd->BindPipeline(denoisePso_);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, cb_);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, target.linearDepth);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 2, target.aoBufferRaw);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, pointSampler_);
        cmd->Draw(3, 0);
        cmd->EndRenderPass();
    }

    // --- Temporal pass: aoBufferDenoised + aoBufferHistory + linearDepth(+History) → curAo ---
    // t8=linearDepthHistory is sampled at prevUv for disocclusion
    // rejection — the temporal pass treats a depth mismatch as a hard
    // history-reject, which suppresses AO ghosts at moving silhouettes.
    {
        const f32 clearAo[4] = {1.0f, 0.0f, 0.0f, 0.0f};
        cmd->BeginRenderPass(curAo, target.depth, clearAo, 1.0f, 0);
        cmd->SetViewport({0, 0, (f32)target.width, (f32)target.height, 0, 1});
        cmd->BindPipeline(temporalPso_);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, cb_);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, target.linearDepth);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 2, target.aoBufferDenoised);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 4, histAo);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 8, target.linearDepthHistory);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, pointSampler_);
        cmd->Draw(3, 0);
        cmd->EndRenderPass();
    }

    // --- Apply pass: hdrColor (load) ← hdrColor * curAo (via blend) ---
    {
        cmd->BeginRenderPassLoad(target.hdrColor, gfx::TextureHandle::Invalid, 1.0f, 0);
        cmd->SetViewport({0, 0, (f32)target.width, (f32)target.height, 0, 1});
        cmd->BindPipeline(debugAoOnly_ ? applyPsoDebug_ : applyPso_);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, cb_);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 3, curAo);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, pointSampler_);
        cmd->Draw(3, 0);
        cmd->EndRenderPass();
    }

    // --- Depth copy: linearDepth → linearDepthHistory ---
    // Last in the pipeline so next frame's temporal pass has prev
    // linearDepth to compare against. RunIblBoost runs after this; it
    // touches hdrColor, not linearDepthHistory, so the order is safe.
    {
        const f32 clearDepth[4] = {1.0e5f, 0.0f, 0.0f, 0.0f};
        cmd->BeginRenderPass(target.linearDepthHistory, target.depth, clearDepth, 1.0f, 0);
        cmd->SetViewport({0, 0, (f32)target.width, (f32)target.height, 0, 1});
        cmd->BindPipeline(depthCopyPso_);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, cb_);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, target.linearDepth);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, pointSampler_);
        cmd->Draw(3, 0);
        cmd->EndRenderPass();
    }

    // Stash this frame's transform so next frame's temporal pass can
    // reproject. Flip the ping-pong index and arm the history bit.
    prevViewProj_ = curViewProj;
    prevValid_ = true;
    lastAoOutput_ = curAo;
    ++frameCounter_;
}

void GtaoService::RunIblBoost(gfx::IGFXCommandList* cmd, const RenderTarget& target,
                              const IblBoostInputs& in) {
    if (!IsEnabled() || !cmd || params_.bentBoost <= 0.0f ||
        lastAoOutput_ == gfx::TextureHandle::Invalid ||
        target.bentNormalBuffer == gfx::TextureHandle::Invalid ||
        target.linearDepth == gfx::TextureHandle::Invalid ||
        target.hdrColor == gfx::TextureHandle::Invalid ||
        in.iblFrom == gfx::TextureHandle::Invalid || in.iblTo == gfx::TextureHandle::Invalid ||
        in.iblSampler == gfx::SamplerHandle::Invalid ||
        iblBoostPso_ == gfx::PipelineHandle::Invalid)
        return;

    // Repack the CB with the IBL probe state for this call. The rest of
    // the struct still holds whatever Run() left in it this frame —
    // view/proj/viewport are still valid for the same scene.
    if (void* mapped = gfx_->MapBuffer(cb_)) {
        GtaoCb cb{};
        std::memcpy(&cb, mapped, sizeof(cb));
        cb.ibl[0] = in.envFromMipEnd;
        cb.ibl[1] = in.envToMipEnd;
        cb.ibl[2] = in.envTransitionT;
        cb.ibl[3] = params_.bentBoost;
        std::memcpy(mapped, &cb, sizeof(cb));
        gfx_->UnmapBuffer(cb_);
    }

    cmd->BeginRenderPassLoad(target.hdrColor, gfx::TextureHandle::Invalid, 1.0f, 0);
    cmd->SetViewport({0, 0, (f32)target.width, (f32)target.height, 0, 1});
    cmd->BindPipeline(iblBoostPso_);
    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, cb_);
    cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, target.linearDepth);
    cmd->BindShaderResource(gfx::ShaderStage::Pixel, 3, lastAoOutput_);
    cmd->BindShaderResource(gfx::ShaderStage::Pixel, 5, target.bentNormalBuffer);
    cmd->BindShaderResource(gfx::ShaderStage::Pixel, 6, in.iblFrom);
    cmd->BindShaderResource(gfx::ShaderStage::Pixel, 7, in.iblTo);
    cmd->BindSampler(gfx::ShaderStage::Pixel, 0, pointSampler_);
    cmd->BindSampler(gfx::ShaderStage::Pixel, 1, in.iblSampler);
    cmd->Draw(3, 0);
    cmd->EndRenderPass();
}

} // namespace whiteout::flakes::renderer::gtao
