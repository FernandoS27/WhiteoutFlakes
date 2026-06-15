#include "renderer/dof/dof_service.h"

#include "compiled_shaders.h"
#include "renderer/bls/bls_shader_cache.h"
#include "renderer/render_target.h"

#include <cstdio>
#include <cstring>

namespace whiteout::flakes::renderer::dof {

namespace {

// Pixel-stage constant buffer — byte-identical to the engine's
// DepthOfFieldPSPerDraw (Wc3Shaders cb_structs.slang). Three float4 slots; the
// shipped depthoffield.bls reads cb1[0].zw, cb1[1].xyzw and cb1[2].w.
struct DofCb {
    f32 c0[4]; // [_, _, invResolution.x, invResolution.y]
    f32 c1[4]; // [maxBlurSize, radiusScale, focusDistance, focusScale]
    f32 c2[4]; // [_, _, _, farFieldOnly]
};
static_assert(sizeof(DofCb) == 48, "DofCb must match DepthOfFieldPSPerDraw (3 float4)");

// Shared embedded fullscreen-triangle blit (vertexId-driven, no VB) — the same
// shader PostProcessService::RunBlit uses to copy a scratch RT back over hdr.
struct ShaderBlob {
    const u8* data = nullptr;
    usize size = 0;
};

void PickBlitBlobs(gfx::GfxApi api, ShaderBlob& vs, ShaderBlob& ps) {
    using namespace whiteout::flakes::Shaders;
#define BLOB(SYM) ShaderBlob{(SYM), sizeof(SYM)}
    vs = BLOB(kBlitVS);
    ps = BLOB(kBlitPS);
    if (api == gfx::GfxApi::Vulkan) {
        vs = BLOB(kBlitVSSpv);
        ps = BLOB(kBlitPSSpv);
    } else if (api == gfx::GfxApi::WebGPU) {
        vs = BLOB(kBlitVSWgsl);
        ps = BLOB(kBlitPSWgsl);
    } else if (api == gfx::GfxApi::Metal) {
        vs = BLOB(kBlitVSMtl);
        ps = BLOB(kBlitPSMtl);
    }
#undef BLOB
}

constexpr gfx::Format kHdrSceneFormat = gfx::Format::R11G11B10_FLOAT;

inline bool BlsOk(const bls::BlsShader* s) {
    return s && !s->permuteHandles.empty();
}

} // namespace

void DofService::Init(gfx::IGFXDevice& gfx, gfx::GfxApi api, bls::BlsShaderCache& cache,
                      gfx::BufferHandle spriteVb) {
    gfx_ = &gfx;
    api_ = api;
    spriteVb_ = spriteVb;

    // Shipped WC3 shaders. Acquire returns nullptr if the bundle isn't packed
    // for this backend — we treat that as "DoF unavailable" and never build
    // PSOs, so Run becomes a quiet no-op rather than taking the renderer down.
    spriteVs_ = cache.Acquire(gfx::ShaderStage::Vertex, "sprite");
    dofPs_ = cache.Acquire(gfx::ShaderStage::Pixel, "depthoffield");

    // Embedded blit (same per-backend bytecode chooser the bloom blit uses).
    ShaderBlob blitVs, blitPs;
    PickBlitBlobs(api, blitVs, blitPs);
    if (blitVs.size > 1 && blitPs.size > 1) {
        blitVs_ = gfx_->CreateShader(gfx::ShaderStage::Vertex, blitVs.data, blitVs.size);
        blitPs_ = gfx_->CreateShader(gfx::ShaderStage::Pixel, blitPs.data, blitPs.size);
    }

    cb_ = gfx_->CreateBuffer({
        .size = sizeof(DofCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });

    // Linear-clamp for colour taps, point-clamp for depth (never interpolate
    // across a depth discontinuity).
    {
        gfx::SamplerDesc sd;
        sd.minFilter = gfx::Filter::Linear;
        sd.magFilter = gfx::Filter::Linear;
        sd.addressU = gfx::AddressMode::Clamp;
        sd.addressV = gfx::AddressMode::Clamp;
        sd.addressW = gfx::AddressMode::Clamp;
        linearSampler_ = gfx_->CreateSampler(sd);
        sd.minFilter = gfx::Filter::Point;
        sd.magFilter = gfx::Filter::Point;
        pointSampler_ = gfx_->CreateSampler(sd);
    }

    shadersReady_ = BlsOk(spriteVs_) && BlsOk(dofPs_) &&
                    blitVs_ != gfx::ShaderHandle::Invalid &&
                    blitPs_ != gfx::ShaderHandle::Invalid &&
                    spriteVb_ != gfx::BufferHandle::Invalid;
    if (!shadersReady_) {
        std::fprintf(stderr,
                     "[dof] disabled — missing shader (sprite=%p depthoffield=%p blitVs=%llu "
                     "blitPs=%llu spriteVb=%llu)\n",
                     (void*)spriteVs_, (void*)dofPs_, (unsigned long long)blitVs_,
                     (unsigned long long)blitPs_, (unsigned long long)spriteVb_);
    }
}

void DofService::Shutdown() {
    if (!gfx_)
        return;
    if (gatherPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(gatherPso_);
    if (blitPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(blitPso_);
    if (blitVs_ != gfx::ShaderHandle::Invalid)
        gfx_->Destroy(blitVs_);
    if (blitPs_ != gfx::ShaderHandle::Invalid)
        gfx_->Destroy(blitPs_);
    if (cb_ != gfx::BufferHandle::Invalid)
        gfx_->Destroy(cb_);
    if (linearSampler_ != gfx::SamplerHandle::Invalid)
        gfx_->Destroy(linearSampler_);
    if (pointSampler_ != gfx::SamplerHandle::Invalid)
        gfx_->Destroy(pointSampler_);
    gatherPso_ = blitPso_ = gfx::PipelineHandle::Invalid;
    blitVs_ = blitPs_ = gfx::ShaderHandle::Invalid;
    cb_ = spriteVb_ = gfx::BufferHandle::Invalid;
    linearSampler_ = pointSampler_ = gfx::SamplerHandle::Invalid;
    spriteVs_ = dofPs_ = nullptr;
    psoHdrFmt_ = gfx::Format::Unknown;
    shadersReady_ = false;
    gfx_ = nullptr;
}

void DofService::EnsurePsos(gfx::Format hdrFmt) {
    if (gatherPso_ != gfx::PipelineHandle::Invalid && blitPso_ != gfx::PipelineHandle::Invalid &&
        psoHdrFmt_ == hdrFmt)
        return;
    if (gatherPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(gatherPso_);
    if (blitPso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(blitPso_);

    // sprite VS input layout — 12-byte pos at ATTR0 + 8-byte uv at ATTR3, the
    // shared fullscreen-triangle VB layout (same as bloom extract/combine).
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
        // dsvFormat must match the depth the WebGPU backend auto-attaches when
        // the pass declares no depth (Dawn validates attachment state at
        // SetPipeline) — same trick bloom/GTAO/tonemap use. Depth is inert here.
        d.dsvFormat = gfx::Format::D24_UNORM_S8_UINT;
        return gfx_->CreateGraphicsPipeline(d);
    };

    // Gather: shipped sprite VS + depthoffield PS, sprite VB layout.
    gatherPso_ = build(spriteVs_->permuteHandles[0], dofPs_->permuteHandles[0], spriteInput, 2);
    // Blit: embedded vertexId-driven fullscreen triangle, no VB / layout.
    blitPso_ = build(blitVs_, blitPs_, nullptr, 0);
    psoHdrFmt_ = hdrFmt;
}

void DofService::Run(gfx::IGFXCommandList* cmd, RenderTarget& target) {
    if (!IsEnabled() || !cmd || target.hdrColor == gfx::TextureHandle::Invalid ||
        target.bloomScratchA == gfx::TextureHandle::Invalid ||
        target.linearDepth == gfx::TextureHandle::Invalid)
        return;

    EnsurePsos(kHdrSceneFormat);
    if (gatherPso_ == gfx::PipelineHandle::Invalid || blitPso_ == gfx::PipelineHandle::Invalid)
        return;

    if (void* mapped = gfx_->MapBuffer(cb_)) {
        DofCb cb{};
        cb.c0[2] = (target.width > 0) ? 1.0f / target.width : 0.0f;
        cb.c0[3] = (target.height > 0) ? 1.0f / target.height : 0.0f;
        cb.c1[0] = params_.maxBlurSize;
        cb.c1[1] = params_.radiusScale;
        cb.c1[2] = params_.focusDistance;
        cb.c1[3] = params_.focusScale;
        cb.c2[3] = params_.farFieldOnly ? 1.0f : 0.0f;
        std::memcpy(mapped, &cb, sizeof(cb));
        gfx_->UnmapBuffer(cb_);
    }

    const f32 clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const u32 spriteStride = static_cast<u32>(sizeof(f32) * 5);

    // --- Gather pass: hdrColor (t0) + linearDepth (t1) → bloomScratchA ---
    //   CB at slot 1 (register b1), colour sampler s0, depth sampler s1 —
    //   matches depthoffield.bls's DepthOfFieldPSPerDraw / t0,t1 / s0,s1.
    {
        cmd->BeginRenderPass(target.bloomScratchA, gfx::TextureHandle::Invalid, clear, 1.0f, 0);
        cmd->SetViewport({0, 0, (f32)target.width, (f32)target.height, 0, 1});
        cmd->BindPipeline(gatherPso_);
        cmd->BindVertexBuffer(0, spriteVb_, spriteStride);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 1, cb_);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, target.hdrColor);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 1, target.linearDepth);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, linearSampler_);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 1, pointSampler_);
        cmd->Draw(3, 0);
        cmd->EndRenderPass();
    }

    // --- Blit pass: bloomScratchA → hdrColor (in-place round-trip) ---
    {
        cmd->BeginRenderPass(target.hdrColor, gfx::TextureHandle::Invalid, clear, 1.0f, 0);
        cmd->SetViewport({0, 0, (f32)target.width, (f32)target.height, 0, 1});
        cmd->BindPipeline(blitPso_);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, target.bloomScratchA);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, linearSampler_);
        cmd->Draw(3, 0);
        cmd->EndRenderPass();
    }
}

} // namespace whiteout::flakes::renderer::dof
