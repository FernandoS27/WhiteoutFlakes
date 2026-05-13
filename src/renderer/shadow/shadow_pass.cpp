#include "shadow_pass.h"

#include "renderer/bls/bls_cb_layout.h"
#include "renderer/bls/scoped_cb.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/render_model.h"
#include "renderer/render_detail.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "renderer/types.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::shadow {

using namespace ::whiteout::flakes::renderer::model;
using namespace ::whiteout::flakes::renderer::animation;
using namespace ::whiteout::flakes::renderer::assets;
using namespace ::whiteout::flakes::renderer::render_detail;

namespace {

void BuildShadowVsCb(bls::HdVsCb& out, const Matrix44f& worldTransform,
                     const Matrix44f& cascadeVP) {
    std::memset(&out, 0, sizeof(out));
    out.world = worldTransform;
    out.worldView = worldTransform;
    out.worldViewProj = worldTransform * cascadeVP;
    out.misc = {0.0f, 1.0f, 0.0f, 0.0f};
    out.diffuseColor = {1.0f, 1.0f, 1.0f, 1.0f};
    out.texMtx0 = {};
    out.texMtx1 = {};
}

} // namespace

bool ShadowPass::Run(ShadowService& service) {
    if (!service.IsEnabled())
        return false;

    auto* gfx = rs_.Pipeline().Gfx();
    if (!gfx)
        return false;
    auto* cmd = gfx->GetImmediateContext();
    if (!cmd)
        return false;

    const auto shadow = rs_.Pipeline().Shadow();
    const gfx::PipelineHandle psoSkinned = shadow.psoSkinned;
    const gfx::PipelineHandle psoRigid = shadow.psoRigid;
    const gfx::BufferHandle vsCb = shadow.vsCb;
    const bool anyPso =
        (psoSkinned != gfx::PipelineHandle::Invalid || psoRigid != gfx::PipelineHandle::Invalid) &&
        vsCb != gfx::BufferHandle::Invalid;

    bool any = false;
    for (i32 c = 0; c < service.cascadeCount(); ++c) {
        const gfx::TextureHandle dst = service.depthTarget(c);
        if (dst == gfx::TextureHandle::Invalid)
            continue;

        cmd->BeginRenderPass(gfx::TextureHandle::Invalid, dst, nullptr, 1.0f, 0);
        const f32 res = static_cast<f32>(service.Params().cascadeResolution);
        cmd->SetViewport({0.0f, 0.0f, res, res, 0.0f, 1.0f});

        if (anyPso) {
            const Matrix44f& cascadeVP = service.cascadeVP(c);

            const i32 selectedLod = rs_.Pipeline().ComputeSelectedLod();

            gfx::PipelineHandle currentPso = gfx::PipelineHandle::Invalid;

            for (auto& [h, mi] : rs_.Scene().Actors().All()) {
                if (!mi)
                    continue;
                if (mi->IsChild())
                    continue;
                if (mi->parentVisibility <= 0.02f)
                    continue;

                const i32 modelLod = mi->render.hasLods ? selectedLod : 0;

                for (auto& geo : mi->render.gpuGeosets) {
                    if (geo.unskinnedVb == gfx::BufferHandle::Invalid)
                        continue;
                    if (geo.ib == gfx::BufferHandle::Invalid)
                        continue;
                    if (geo.indexCount == 0)
                        continue;

                    if (!GeosetPassesLod(geo.lod, modelLod))
                        continue;

                    const f32 geoAlpha = geo.geosetAlpha * mi->parentVisibility;
                    if (geoAlpha <= 0.0f)
                        continue;

                    // Pick the bone palette CB the actor actually owns —
                    // per-actor on Path A, per-geoset on Path B. Same
                    // pattern as the scene draw paths (see DrawGeoset
                    // in render_pipeline.cpp). Without this branch the
                    // shadow pass uses the non-skinned PSO for every
                    // Path A actor, drawing shadows at bind pose.
                    gfx::BufferHandle paletteCb = geo.bonePaletteCb;
                    if (mi->render.skinning.UsesPerActorPalette()) {
                        paletteCb = mi->render.skinning.ActorPaletteCb();
                    }
                    const bool hasBones = geo.boneVb != gfx::BufferHandle::Invalid &&
                                          paletteCb != gfx::BufferHandle::Invalid;
                    const gfx::PipelineHandle pso = hasBones ? psoSkinned : psoRigid;
                    if (pso == gfx::PipelineHandle::Invalid)
                        continue;

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
                        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 3, paletteCb);
                        cmd->BindVertexBuffer(1, geo.boneVb, sizeof(BoneVertex));
                    }

                    cmd->DrawIndexed(static_cast<u32>(geo.indexCount), 0, 0);
                }
            }
        }

        cmd->EndRenderPass();
        any = true;
    }
    return any;
}

} // namespace whiteout::flakes::renderer::shadow
