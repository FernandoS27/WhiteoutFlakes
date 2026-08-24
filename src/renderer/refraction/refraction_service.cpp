#include "renderer/refraction/refraction_service.h"

#include "compiled_shaders.h"

#include <cstdio>
#include <cstring>

namespace whiteout::flakes::renderer::refraction {

namespace {

// The interleaved vertex the mask pass draws. Built here rather than reusing
// the renderer's 48-byte `Vertex` because this is the one stream in the engine
// that carries three UV sets — the client's own CGxVertexPCT3, minus its packed
// colour.
struct MaskVertex {
    Vector3f position;
    Vector4f color;
    // The sprite UV and layer 1 share one attribute, and layer 2 rides TANGENT:
    // slang mangles a numbered semantic on a vertex INPUT (`TEXCOORD1` is
    // emitted as index 10), so every element here has to name a digit-free
    // semantic. See refraction_mask.slang's header.
    Vector2f uv0;
    Vector2f uv1;
    Vector2f uv2;
};
static_assert(sizeof(MaskVertex) == 52, "MaskVertex must stay tightly packed");

struct MaskVsCb {
    Matrix44f world;
    Matrix44f view;
    Matrix44f projection;
};

struct MaskPsCb {
    f32 params[4];
};

struct ApplyCb {
    f32 texelAndProj[4];
    f32 params[4];
};

// The client's `RefractRatio` CVar default: 1/1.33, water against air
// (`SetRefractionRatio` @0x100e99340 substitutes exactly this when the CVar is
// zero).
constexpr f32 kDefaultRatio = 0.75187969f;

// The constant the retail particle vertex shader folds into vertex colour
// (`oColor0 = float4(0.5,0.5,0.5,1) * vColor`). The refraction pixel shader,
// unlike the ordinary particle ones, never multiplies it back.
constexpr f32 kColorScale = 0.5f;

// The distortion field. FLOAT, and that is load-bearing rather than a quality
// choice: the apply pass reads this buffer as a HEIGHT and takes its central
// difference, and over a smooth blob the per-texel difference is a fraction of
// an 8-bit LSB — an R8G8B8A8_UNORM buffer quantises the whole gradient to zero
// and refracts nothing at all. The client's own target is `R16F`
// (`RefractionBuffer::Validate` @0x100e98c90 passes EGxTexFormat 13); this is
// the nearest format the gfx layer exposes, and the three extra channels pay
// for themselves by carrying the coverage the debug view shows.
constexpr gfx::Format kMaskFormat = gfx::Format::R16G16B16A16_FLOAT;

Matrix44f Transposed(const Matrix44f& m) {
    Matrix44f o;
    for (i32 r = 0; r < 4; ++r)
        for (i32 c = 0; c < 4; ++c)
            o.data[r][c] = m.data[c][r];
    return o;
}

void SetBlendFor(gfx::BlendDesc& b, particle::FilterMode mode) {
    b.enable = true;
    b.opColor = gfx::BlendOp::Add;
    b.opAlpha = gfx::BlendOp::Add;
    switch (mode) {
    case particle::FilterMode::Additive:
        // 397 of the 439 shipped refraction emitters. Overlapping quads
        // accumulate height, which is the shape the remap expects.
        b.srcColor = gfx::BlendFactor::SrcAlpha;
        b.dstColor = gfx::BlendFactor::One;
        b.srcAlpha = gfx::BlendFactor::SrcAlpha;
        b.dstAlpha = gfx::BlendFactor::One;
        break;
    case particle::FilterMode::Modulate:
        b.srcColor = gfx::BlendFactor::Zero;
        b.dstColor = gfx::BlendFactor::SrcColor;
        b.srcAlpha = gfx::BlendFactor::Zero;
        b.dstAlpha = gfx::BlendFactor::SrcAlpha;
        break;
    case particle::FilterMode::Modulate2X:
        b.srcColor = gfx::BlendFactor::DstColor;
        b.dstColor = gfx::BlendFactor::SrcColor;
        b.srcAlpha = gfx::BlendFactor::DstAlpha;
        b.dstAlpha = gfx::BlendFactor::SrcAlpha;
        break;
    case particle::FilterMode::Blend:
    case particle::FilterMode::AlphaKey:
    default:
        b.srcColor = gfx::BlendFactor::SrcAlpha;
        b.dstColor = gfx::BlendFactor::InvSrcAlpha;
        b.srcAlpha = gfx::BlendFactor::SrcAlpha;
        b.dstAlpha = gfx::BlendFactor::InvSrcAlpha;
        break;
    }
}

} // namespace

void RefractionService::Init(gfx::IGFXDevice& gfx, gfx::GfxApi api) {
    gfx_ = &gfx;
    api_ = api;

    using namespace whiteout::flakes::Shaders;
    const u8* maskVs = kRefractParticleVS;
    usize maskVsN = sizeof(kRefractParticleVS);
    const u8* maskPs = kRefractParticlePS;
    usize maskPsN = sizeof(kRefractParticlePS);
    const u8* applyVs = kRefractApplyVS;
    usize applyVsN = sizeof(kRefractApplyVS);
    const u8* applyPs = kRefractApplyPS;
    usize applyPsN = sizeof(kRefractApplyPS);
    const u8* blitVs = kBlitVS;
    usize blitVsN = sizeof(kBlitVS);
    const u8* blitPs = kBlitPS;
    usize blitPsN = sizeof(kBlitPS);
    if (api == gfx::GfxApi::Vulkan) {
        maskVs = kRefractParticleVSSpv;
        maskVsN = sizeof(kRefractParticleVSSpv);
        maskPs = kRefractParticlePSSpv;
        maskPsN = sizeof(kRefractParticlePSSpv);
        applyVs = kRefractApplyVSSpv;
        applyVsN = sizeof(kRefractApplyVSSpv);
        applyPs = kRefractApplyPSSpv;
        applyPsN = sizeof(kRefractApplyPSSpv);
        blitVs = kBlitVSSpv;
        blitVsN = sizeof(kBlitVSSpv);
        blitPs = kBlitPSSpv;
        blitPsN = sizeof(kBlitPSSpv);
    } else if (api == gfx::GfxApi::WebGPU) {
        maskVs = kRefractParticleVSWgsl;
        maskVsN = sizeof(kRefractParticleVSWgsl);
        maskPs = kRefractParticlePSWgsl;
        maskPsN = sizeof(kRefractParticlePSWgsl);
        applyVs = kRefractApplyVSWgsl;
        applyVsN = sizeof(kRefractApplyVSWgsl);
        applyPs = kRefractApplyPSWgsl;
        applyPsN = sizeof(kRefractApplyPSWgsl);
        blitVs = kBlitVSWgsl;
        blitVsN = sizeof(kBlitVSWgsl);
        blitPs = kBlitPSWgsl;
        blitPsN = sizeof(kBlitPSWgsl);
    } else if (api == gfx::GfxApi::Metal) {
        maskVs = kRefractParticleVSMtl;
        maskVsN = sizeof(kRefractParticleVSMtl);
        maskPs = kRefractParticlePSMtl;
        maskPsN = sizeof(kRefractParticlePSMtl);
        applyVs = kRefractApplyVSMtl;
        applyVsN = sizeof(kRefractApplyVSMtl);
        applyPs = kRefractApplyPSMtl;
        applyPsN = sizeof(kRefractApplyPSMtl);
        blitVs = kBlitVSMtl;
        blitVsN = sizeof(kBlitVSMtl);
        blitPs = kBlitPSMtl;
        blitPsN = sizeof(kBlitPSMtl);
    }

    if (maskVsN > 1 && maskPsN > 1) {
        maskVs_ = gfx_->CreateShader(gfx::ShaderStage::Vertex, maskVs, maskVsN);
        maskPs_ = gfx_->CreateShader(gfx::ShaderStage::Pixel, maskPs, maskPsN);
    }
    if (applyVsN > 1 && applyPsN > 1) {
        applyVs_ = gfx_->CreateShader(gfx::ShaderStage::Vertex, applyVs, applyVsN);
        applyPs_ = gfx_->CreateShader(gfx::ShaderStage::Pixel, applyPs, applyPsN);
    }
    if (blitVsN > 1 && blitPsN > 1) {
        blitVs_ = gfx_->CreateShader(gfx::ShaderStage::Vertex, blitVs, blitVsN);
        blitPs_ = gfx_->CreateShader(gfx::ShaderStage::Pixel, blitPs, blitPsN);
    }

    maskVsCb_ = gfx_->CreateBuffer({
        .size = sizeof(MaskVsCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });
    maskPsCb_ = gfx_->CreateBuffer({
        .size = sizeof(MaskPsCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });
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

    shadersReady_ = maskVs_ != gfx::ShaderHandle::Invalid &&
                    maskPs_ != gfx::ShaderHandle::Invalid &&
                    applyVs_ != gfx::ShaderHandle::Invalid &&
                    applyPs_ != gfx::ShaderHandle::Invalid &&
                    blitVs_ != gfx::ShaderHandle::Invalid &&
                    blitPs_ != gfx::ShaderHandle::Invalid;
    if (!shadersReady_) {
        std::fprintf(stderr,
                     "[refraction] disabled — missing shader (maskVs=%llu maskPs=%llu "
                     "applyVs=%llu applyPs=%llu blitVs=%llu blitPs=%llu)\n",
                     (unsigned long long)maskVs_, (unsigned long long)maskPs_,
                     (unsigned long long)applyVs_, (unsigned long long)applyPs_,
                     (unsigned long long)blitVs_, (unsigned long long)blitPs_);
    }
}

void RefractionService::Shutdown() {
    if (!gfx_)
        return;
    for (auto& p : maskPso_) {
        if (p != gfx::PipelineHandle::Invalid)
            gfx_->Destroy(p);
        p = gfx::PipelineHandle::Invalid;
    }
    if (applyPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(applyPso_);
    if (blitPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(blitPso_);
    for (gfx::ShaderHandle* s : {&maskVs_, &maskPs_, &applyVs_, &applyPs_, &blitVs_, &blitPs_}) {
        if (*s != gfx::ShaderHandle::Invalid)
            gfx_->Destroy(*s);
        *s = gfx::ShaderHandle::Invalid;
    }
    for (gfx::BufferHandle* b : {&vb_, &maskVsCb_, &maskPsCb_, &applyCb_}) {
        if (*b != gfx::BufferHandle::Invalid)
            gfx_->Destroy(*b);
        *b = gfx::BufferHandle::Invalid;
    }
    for (gfx::TextureHandle* t : {&mask_, &sceneCopy_}) {
        if (*t != gfx::TextureHandle::Invalid)
            gfx_->Destroy(*t);
        *t = gfx::TextureHandle::Invalid;
    }
    if (clampSampler_ != gfx::SamplerHandle::Invalid)
        gfx_->Destroy(clampSampler_);
    clampSampler_ = gfx::SamplerHandle::Invalid;

    applyPso_ = blitPso_ = gfx::PipelineHandle::Invalid;
    psoSceneFmt_ = psoOutputFmt_ = psoDepthFmt_ = gfx::Format::Unknown;
    targetSceneFmt_ = gfx::Format::Unknown;
    redirected_ = false;
    targetW_ = targetH_ = 0;
    vbCapacity_ = 0;
    shadersReady_ = false;
    gfx_ = nullptr;
}

bool RefractionService::EnsureTargets(i32 w, i32 h, gfx::Format sceneFormat) {
    if (w <= 0 || h <= 0)
        return false;
    if (mask_ != gfx::TextureHandle::Invalid && targetW_ == w && targetH_ == h &&
        targetSceneFmt_ == sceneFormat)
        return true;

    if (mask_ != gfx::TextureHandle::Invalid)
        gfx_->Destroy(mask_);
    if (sceneCopy_ != gfx::TextureHandle::Invalid)
        gfx_->Destroy(sceneCopy_);

    mask_ = gfx_->CreateColorTarget(w, h, kMaskFormat);
    // Matched to whatever the frame's scene colour actually is: SD writes gamma
    // bytes to a UNORM target and HD writes an HDR float one, and the copy has
    // to round-trip either without re-encoding.
    sceneCopy_ = gfx_->CreateColorTarget(w, h, sceneFormat);
    targetW_ = w;
    targetH_ = h;
    targetSceneFmt_ = sceneFormat;
    return mask_ != gfx::TextureHandle::Invalid && sceneCopy_ != gfx::TextureHandle::Invalid;
}

void RefractionService::EnsurePsos(gfx::Format sceneFormat, gfx::Format outputFormat,
                                   gfx::Format depthFormat) {
    if (psoSceneFmt_ == sceneFormat && psoOutputFmt_ == outputFormat &&
        psoDepthFmt_ == depthFormat && applyPso_ != gfx::PipelineHandle::Invalid)
        return;

    for (auto& p : maskPso_) {
        if (p != gfx::PipelineHandle::Invalid)
            gfx_->Destroy(p);
        p = gfx::PipelineHandle::Invalid;
    }
    if (applyPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(applyPso_);
    if (blitPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(blitPso_);
    applyPso_ = blitPso_ = gfx::PipelineHandle::Invalid;

    static const gfx::InputElement kMaskLayout[] = {
        {"POSITION", 0, gfx::Format::R32G32B32_FLOAT, offsetof(MaskVertex, position)},
        {"COLOR", 0, gfx::Format::R32G32B32A32_FLOAT, offsetof(MaskVertex, color)},
        {"TEXCOORD", 0, gfx::Format::R32G32B32A32_FLOAT, offsetof(MaskVertex, uv0)},
        {"TANGENT", 0, gfx::Format::R32G32_FLOAT, offsetof(MaskVertex, uv2)},
    };

    for (usize i = 0; i < kBlendModes; ++i) {
        gfx::GraphicsPipelineDesc d{};
        d.vs = maskVs_;
        d.ps = maskPs_;
        d.inputLayout = kMaskLayout;
        d.inputSlotStrides[0] = sizeof(MaskVertex);
        d.topology = gfx::PrimitiveTopology::TriangleList;
        SetBlendFor(d.blend, static_cast<particle::FilterMode>(i));
        // Depth-tested against the finished scene, never written: the mask is a
        // side buffer, and the depth it reads belongs to the scene pass.
        d.depthStencil.depthTest = true;
        d.depthStencil.depthWrite = false;
        d.depthStencil.depthCompare = gfx::CompareOp::LessEqual;
        d.rasterizer.cull = gfx::CullMode::None;
        d.rtvFormat = kMaskFormat;
        d.dsvFormat = depthFormat;
        maskPso_[i] = gfx_->CreateGraphicsPipeline(d);
    }

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
    // The snapshot blit writes the scene copy; the apply pass writes whatever
    // the caller handed as its output. Those are the same texture normally and
    // different ones under a scene redirect, where the output is a BGRA8 or
    // RGBA8 back buffer and the copy is the SD gamma UNORM the scene rendered
    // into. One rtvFormat for both would fail PSO creation on the BGRA8
    // machines and silently drop the pass.
    blitPso_ = fullscreen(blitVs_, blitPs_, sceneFormat);
    applyPso_ = fullscreen(applyVs_, applyPs_, outputFormat);

    psoSceneFmt_ = sceneFormat;
    psoOutputFmt_ = outputFormat;
    psoDepthFmt_ = depthFormat;
}

gfx::TextureHandle RefractionService::BeginSceneRedirect(i32 w, i32 h, gfx::Format sceneFormat) {
    if (!IsEnabled() || !EnsureTargets(w, h, sceneFormat))
        return gfx::TextureHandle::Invalid;
    redirected_ = true;
    return sceneCopy_;
}

bool RefractionService::UploadVertices(const particle::MultiTexGeometry& geo) {
    const i32 count = static_cast<i32>(geo.vertices.size());
    if (count <= 0)
        return false;
    // The two arrays are built in lockstep by the geometry builder; a mismatch
    // means an emitter reached the refraction path without its UV sets, and
    // drawing it would read whatever the previous frame left behind.
    if (geo.extraUV.size() != geo.vertices.size())
        return false;

    if (vb_ == gfx::BufferHandle::Invalid || count > vbCapacity_) {
        if (vb_ != gfx::BufferHandle::Invalid)
            gfx_->Destroy(vb_);
        constexpr i32 kFloor = 1024;
        const i32 newSize = (count > kFloor) ? count : kFloor;
        gfx::BufferDesc bd;
        bd.size = static_cast<u64>(sizeof(MaskVertex)) * static_cast<u64>(newSize);
        bd.usage = gfx::BufferUsage::Vertex | gfx::BufferUsage::CpuWritable;
        bd.ringSlotsHint = 4;
        vb_ = gfx_->CreateBuffer(bd);
        vbCapacity_ = newSize;
    }
    if (vb_ == gfx::BufferHandle::Invalid)
        return false;

    void* mapped = gfx_->MapBuffer(vb_);
    if (!mapped)
        return false;
    auto* dst = static_cast<MaskVertex*>(mapped);
    for (i32 i = 0; i < count; ++i) {
        const Vertex& v = geo.vertices[static_cast<usize>(i)];
        const Vector4f& extra = geo.extraUV[static_cast<usize>(i)];
        dst[i].position = v.position;
        dst[i].color = v.color;
        dst[i].uv0 = v.uv;
        dst[i].uv1 = {extra.x, extra.y};
        dst[i].uv2 = {extra.z, extra.w};
    }
    gfx_->UnmapBuffer(vb_);
    return true;
}

void RefractionService::Run(gfx::IGFXCommandList* cmd, const RefractionFrameInputs& frame,
                            const particle::MultiTexGeometry& geo) {
    // A redirected frame runs even with nothing to refract: the scene is in
    // `sceneCopy_` and this pass is the only thing that puts it on screen.
    const bool redirected = redirected_;
    redirected_ = false;
    if (!cmd || !IsEnabled() || (geo.draws.empty() && !redirected))
        return;
    if (frame.sceneColor == gfx::TextureHandle::Invalid || frame.width <= 0 || frame.height <= 0)
        return;
    if (!EnsureTargets(frame.width, frame.height, frame.sceneFormat))
        return;
    const gfx::Format outputFormat = (frame.outputFormat != gfx::Format::Unknown)
                                         ? frame.outputFormat
                                         : frame.sceneFormat;
    EnsurePsos(frame.sceneFormat, outputFormat, frame.depthFormat);
    // Every PSO, not just the two full-screen ones: a mask pipeline that failed
    // to build leaves an empty distortion buffer, and an empty buffer refracts
    // nothing — which looks exactly like a working feature on a model that has
    // no refraction emitter at all. It has to be loud instead.
    if (applyPso_ == gfx::PipelineHandle::Invalid || blitPso_ == gfx::PipelineHandle::Invalid ||
        maskPso_[0] == gfx::PipelineHandle::Invalid) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                         "[refraction] no pass — pipeline build failed "
                         "(apply=%llu blit=%llu mask=%llu)\n",
                         (unsigned long long)applyPso_, (unsigned long long)blitPso_,
                         (unsigned long long)maskPso_[0]);
        }
        return;
    }
    const bool haveGeometry = UploadVertices(geo);
    if (!haveGeometry && !redirected)
        return;

    const f32 w = static_cast<f32>(frame.width);
    const f32 h = static_cast<f32>(frame.height);

    // ---- Constants -------------------------------------------------------
    {
        if (void* p = gfx_->MapBuffer(maskVsCb_)) {
            auto* cb = static_cast<MaskVsCb*>(p);
            cb->world = Matrix44f::identity();
            cb->view = Transposed(frame.view);
            cb->projection = Transposed(frame.projection);
            gfx_->UnmapBuffer(maskVsCb_);
        }
        if (void* p = gfx_->MapBuffer(maskPsCb_)) {
            auto* cb = static_cast<MaskPsCb*>(p);
            const f32 fadeUnits =
                params_.fadeDistanceYards * ((frame.worldScale > 0.0f) ? frame.worldScale : 1.0f);
            cb->params[0] = (fadeUnits > 0.0f) ? (1.0f / fadeUnits) : 0.0f;
            cb->params[1] = kColorScale;
            cb->params[2] = params_.maskGain;
            cb->params[3] = 0.0f;
            gfx_->UnmapBuffer(maskPsCb_);
        }
    }

    // ---- Pass 1: the distortion mask -------------------------------------
    //
    // Two render passes rather than one: the colour has to start cleared and
    // the depth has to survive, and only the load-op variant can keep depth.
    {
        const f32 clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        cmd->BeginRenderPass(mask_, gfx::TextureHandle::Invalid, clear, 1.0f, 0);
        cmd->EndRenderPass();
    }
    if (haveGeometry) {
        cmd->BeginRenderPassLoad(mask_, frame.depth, 1.0f, 0, /*loadDepth=*/true);
        cmd->SetViewport({0, 0, w, h, 0, 1});
        for (usize i = 0; i < geo.draws.size(); ++i) {
            const particle::EmitterDrawList& dl = geo.draws[i];
            if (dl.vertexCount <= 0)
                continue;
            const usize blend = static_cast<usize>(dl.material.filterMode);
            const gfx::PipelineHandle pso = maskPso_[(blend < kBlendModes) ? blend : 0];
            if (pso == gfx::PipelineHandle::Invalid)
                continue;
            // Pipeline FIRST, then every root binding, and both re-issued per
            // draw. BindPipeline re-sets the root signature on D3D12, which
            // discards constant buffers, textures and samplers bound before it
            // — hoisting the two CBs out of this loop leaves the vertex shader
            // reading an unbound matrix and the quads collapse to a point.
            cmd->BindPipeline(pso);
            cmd->BindVertexBuffer(0, vb_, sizeof(MaskVertex));
            cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, maskVsCb_);
            // Slot 1, not 0: the mask shaders pin their two constant buffers to
            // b0 (vertex) and b1 (pixel), because slang assigns registers per
            // module rather than per stage and the two would otherwise collide.
            cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 1, maskPsCb_);
            const gfx::TextureHandle tex =
                (i < frame.drawTextures.size()) ? frame.drawTextures[i] : gfx::TextureHandle::Invalid;
            if (tex != gfx::TextureHandle::Invalid)
                cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, tex);
            if (frame.wrapSampler != gfx::SamplerHandle::Invalid)
                cmd->BindSampler(gfx::ShaderStage::Pixel, 0, frame.wrapSampler);
            cmd->Draw(static_cast<u32>(dl.vertexCount), static_cast<u32>(dl.vertexOffset));
        }
        cmd->EndRenderPass();
    }

    // ---- Pass 2: snapshot the scene --------------------------------------
    //
    // Skipped entirely under a scene redirect: the scene was rendered straight
    // into `sceneCopy_`, so there is nothing to copy and the back buffer this
    // would have sampled could not be sampled anyway.
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

    // ---- Pass 3: bend the scene through the mask -------------------------
    {
        if (void* p = gfx_->MapBuffer(applyCb_)) {
            auto* cb = static_cast<ApplyCb*>(p);
            cb->texelAndProj[0] = 1.0f / w;
            cb->texelAndProj[1] = 1.0f / h;
            // The client feeds the projection's own (m00, m11) here — the two
            // terms that turn a view-space direction into clip space.
            cb->texelAndProj[2] = frame.projection.data[0][0];
            cb->texelAndProj[3] = frame.projection.data[1][1];
            cb->params[0] = (params_.ratio > 0.0f) ? params_.ratio : kDefaultRatio;
            cb->params[1] = params_.strength;
            cb->params[2] = params_.debugShowMask ? 1.0f : 0.0f;
            cb->params[3] = 0.0f;
            gfx_->UnmapBuffer(applyCb_);
        }
        const f32 clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        cmd->BeginRenderPass(frame.sceneColor, gfx::TextureHandle::Invalid, clear, 1.0f, 0);
        cmd->SetViewport({0, 0, w, h, 0, 1});
        cmd->BindPipeline(applyPso_);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, applyCb_);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, sceneCopy_);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 1, mask_);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, clampSampler_);
        cmd->Draw(3, 0);
        cmd->EndRenderPass();
    }
}

} // namespace whiteout::flakes::renderer::refraction
