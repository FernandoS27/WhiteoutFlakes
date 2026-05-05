#include "shadow_pass.h"

#include "common_types.h"
#include "renderer/render_service.h"
#include "renderer/render_service_internal.h"
#include "renderer/scene_manager.h"
#include "renderer/model_instance.h"
#include "renderer/render_model.h"
#include "renderer/bls/bls_cb_layout.h"
#include "renderer/bls/scoped_cb.h"
#include "renderer/types.h"

namespace WhiteoutDex::shadow {

namespace {

void BuildShadowVsCb(bls::HdVsCb&     out,
                     const Matrix44f& worldTransform,
                     const Matrix44f& cascadeVP) {
    std::memset(&out, 0, sizeof(out));
    out.world         = worldTransform;
    out.worldView     = worldTransform;
    out.worldViewProj = worldTransform * cascadeVP;
    out.misc          = { 0.0f, 1.0f, 0.0f, 0.0f };
    out.diffuseColor  = { 1.0f, 1.0f, 1.0f, 1.0f };
    out.texMtx0       = {};
    out.texMtx1       = {};
}

}

bool ShadowPass::Run(ShadowService& service) {
    if (!service.IsEnabled()) return false;

    auto* gfx = rs_.GetGfxDevice();
    if (!gfx) return false;
    auto* cmd = gfx->GetImmediateContext();
    if (!cmd) return false;

    const gfx::PipelineHandle psoSkinned = rs_.shadowPSO_;
    const gfx::PipelineHandle psoRigid   = rs_.shadowPSORigid_;
    const gfx::BufferHandle   vsCb       = rs_.shadowVsCb_;
    const bool                anyPso =
        (psoSkinned != gfx::PipelineHandle::Invalid ||
         psoRigid   != gfx::PipelineHandle::Invalid)
        && vsCb != gfx::BufferHandle::Invalid;

    bool any = false;
    for (i32 c = 0; c < service.cascadeCount(); ++c) {
        const gfx::TextureHandle dst = service.depthTarget(c);
        if (dst == gfx::TextureHandle::Invalid) continue;

        cmd->BeginRenderPass(         gfx::TextureHandle::Invalid,
                                       dst,
                                nullptr,
                                1.0f,
                                0);
        const f32 res = static_cast<f32>(service.Params().cascadeResolution);
        cmd->SetViewport({0.0f, 0.0f, res, res, 0.0f, 1.0f});

        if (anyPso) {
            const Matrix44f& cascadeVP = service.cascadeVP(c);

            const i32 selectedLod = rs_.ComputeSelectedLod();

            gfx::PipelineHandle currentPso = gfx::PipelineHandle::Invalid;

            std::lock_guard<std::mutex> lock(rs_.dataMutex_);
            for (auto& [h, mi] : rs_.scene_->Actors().All()) {
                if (!mi)               continue;
                if (mi->isPE1Child)    continue;
                if (mi->parentVisibility <= 0.02f) continue;

                const i32 modelLod = mi->render.hasLods ? selectedLod : 0;

                for (auto& geo : mi->render.gpuGeosets) {
                    if (geo.unskinnedVb == gfx::BufferHandle::Invalid) continue;
                    if (geo.ib          == gfx::BufferHandle::Invalid) continue;
                    if (geo.indexCount  == 0)                          continue;

                    if (!RenderService::GeosetPassesLod(geo.lod, modelLod))
                        continue;

                    const f32 geoAlpha =
                        geo.geosetAlpha * mi->parentVisibility;
                    if (geoAlpha <= 0.0f) continue;

                    const bool hasBones =
                        geo.boneVb        != gfx::BufferHandle::Invalid &&
                        geo.bonePaletteCb != gfx::BufferHandle::Invalid;
                    const gfx::PipelineHandle pso =
                        hasBones ? psoSkinned : psoRigid;
                    if (pso == gfx::PipelineHandle::Invalid) continue;

                    if (pso != currentPso) {
                        cmd->BindPipeline(pso);
                        currentPso = pso;
                    }

                    if (auto vs = bls::ScopedCb<bls::HdVsCb>(gfx, vsCb)) {
                        BuildShadowVsCb(*vs, mi->worldTransform, cascadeVP);
                    }
                    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 2, vsCb);

                    cmd->BindIndexBuffer(geo.ib, gfx::Format::R32_UINT);
                    cmd->BindVertexBuffer(0, geo.unskinnedVb, sizeof(Vertex));

                    if (hasBones) {
                        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex,
                                                3, geo.bonePaletteCb);
                        cmd->BindVertexBuffer(1, geo.boneVb, sizeof(BoneVertex));
                    }

                    cmd->DrawIndexed(static_cast<u32>(geo.indexCount),
                                         0,
                                         0);
                }
            }
        }

        cmd->EndRenderPass();
        any = true;
    }
    return any;
}

}
