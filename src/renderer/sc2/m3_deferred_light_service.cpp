#include "m3_deferred_light_service.h"

#include "renderer/render_target.h"

#include "compiled_shaders.h"

#include <algorithm>

namespace whiteout::flakes::renderer::sc2 {

void M3DeferredLightService::Init(gfx::IGFXDevice& gfx, gfx::GfxApi api) {
    if (gfx_)
        return;
    gfx_ = &gfx;

    using namespace whiteout::flakes::Shaders;
    auto mk = [&](gfx::ShaderStage stage, const u8* dxbc, usize dxbcN, const u8* spv, usize spvN,
                  const u8* wgsl, usize wgslN, const u8* mtl, usize mtlN) {
        switch (api) {
        case gfx::GfxApi::Vulkan:
            return gfx_->CreateShader(stage, spv, spvN);
        case gfx::GfxApi::WebGPU:
            return gfx_->CreateShader(stage, wgsl, wgslN);
        case gfx::GfxApi::Metal:
            return gfx_->CreateShader(stage, mtl, mtlN);
        default:
            return gfx_->CreateShader(stage, dxbc, dxbcN);
        }
    };
#define WDX_M3DL_BLOB(name)                                                                        \
    k##name, sizeof(k##name), k##name##Spv, sizeof(k##name##Spv), k##name##Wgsl,                   \
        sizeof(k##name##Wgsl), k##name##Mtl, sizeof(k##name##Mtl)
    vs_ = mk(gfx::ShaderStage::Vertex, WDX_M3DL_BLOB(M3DeferredLightVS));
    ps_ = mk(gfx::ShaderStage::Pixel, WDX_M3DL_BLOB(M3DeferredLightPS));
#undef WDX_M3DL_BLOB

    // Written once per frame at most; no ring needed.
    cb_ = gfx_->CreateBuffer({
        .size = sizeof(Cb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });

    gfx::SamplerDesc sd;
    sd.minFilter = gfx::Filter::Point;
    sd.magFilter = gfx::Filter::Point;
    sd.addressU = gfx::AddressMode::Clamp;
    sd.addressV = gfx::AddressMode::Clamp;
    sd.addressW = gfx::AddressMode::Clamp;
    pointSampler_ = gfx_->CreateSampler(sd);
}

void M3DeferredLightService::Shutdown() {
    if (!gfx_)
        return;
    if (pso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(pso_);
    if (vs_ != gfx::ShaderHandle::Invalid)
        gfx_->Destroy(vs_);
    if (ps_ != gfx::ShaderHandle::Invalid)
        gfx_->Destroy(ps_);
    if (cb_ != gfx::BufferHandle::Invalid)
        gfx_->Destroy(cb_);
    if (pointSampler_ != gfx::SamplerHandle::Invalid)
        gfx_->Destroy(pointSampler_);
    pso_ = gfx::PipelineHandle::Invalid;
    vs_ = gfx::ShaderHandle::Invalid;
    ps_ = gfx::ShaderHandle::Invalid;
    cb_ = gfx::BufferHandle::Invalid;
    pointSampler_ = gfx::SamplerHandle::Invalid;
    psoHdrFmt_ = gfx::Format::Unknown;
    gfx_ = nullptr;
}

void M3DeferredLightService::EnsurePso(gfx::Format hdrFmt) {
    if (pso_ != gfx::PipelineHandle::Invalid && psoHdrFmt_ == hdrFmt)
        return;
    if (pso_ != gfx::PipelineHandle::Invalid)
        gfx_->Destroy(pso_);

    // Full-screen triangle, additive One/One onto the existing HDR colour —
    // the GTAO IBL-boost pattern. BeginRenderPassLoad attaches no depth on
    // WebGPU, so dsvFormat stays Unknown to match.
    gfx::GraphicsPipelineDesc d{};
    d.vs = vs_;
    d.ps = ps_;
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
    d.dsvFormat = gfx::Format::Unknown;
    pso_ = gfx_->CreateGraphicsPipeline(d);
    psoHdrFmt_ = hdrFmt;
}

void M3DeferredLightService::Run(gfx::IGFXCommandList* cmd, RenderTarget& target,
                                 const Matrix44f& proj, std::span<const Light> lights,
                                 gfx::Format hdrFmt) {
    if (!IsReady() || !cmd || lights.empty())
        return;
    EnsurePso(hdrFmt);
    if (pso_ == gfx::PipelineHandle::Invalid)
        return;

    const u32 count = static_cast<u32>(std::min<usize>(lights.size(), kMaxLights));
    if (auto* c = static_cast<Cb*>(gfx_->MapBuffer(cb_))) {
        const f32 p00 = proj.data[0][0];
        const f32 p11 = proj.data[1][1];
        c->unproject = {p00 != 0.0f ? 1.0f / p00 : 0.0f, p11 != 0.0f ? 1.0f / p11 : 0.0f,
                        static_cast<f32>(count), 0.0f};
        for (u32 i = 0; i < count; ++i) {
            const Light& l = lights[i];
            c->lightPos[i] = {l.posVS.x, l.posVS.y, l.posVS.z, l.attenEnd};
            c->lightColor[i] = {l.color.x, l.color.y, l.color.z, l.attenStart};
        }
        gfx_->UnmapBuffer(cb_);
    }

    cmd->BeginRenderPassLoad(target.hdrColor, gfx::TextureHandle::Invalid, 1.0f, 0);
    cmd->SetViewport({0, 0, (f32)target.width, (f32)target.height, 0, 1});
    cmd->BindPipeline(pso_);
    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, cb_);
    cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, target.linearDepth);
    cmd->BindShaderResource(gfx::ShaderStage::Pixel, 1, target.normalBuffer);
    cmd->BindShaderResource(gfx::ShaderStage::Pixel, 2, target.gbufDiffuse);
    cmd->BindSampler(gfx::ShaderStage::Pixel, 0, pointSampler_);
    cmd->Draw(3, 0);
    cmd->EndRenderPass();
}

} // namespace whiteout::flakes::renderer::sc2
