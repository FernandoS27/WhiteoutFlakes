#include "shadow_pass.h"

#include "renderer/bls/bls_cb_layout.h"
#include "renderer/bls/scoped_cb.h"
#include "renderer/debug/draw_trace_hooks.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/render_model.h"
#include "renderer/render_detail.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "renderer/types.h"
#include "whiteout/flakes/types.h"

#include <algorithm>
#include <vector>

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

    // Sorted handles, not map order. This is the second hash-ordered draw path
    // (the scene's is BuildDrawLists): the loop below carries a currentPso
    // state tracker whose bind/skip behaviour follows whatever order it gets,
    // and G1 records a ShadowMap slot per cascade. Collected once and reused
    // across cascades so the three passes agree with each other too.
    std::vector<u32> topLevel;
    topLevel.reserve(rs_.Scene().Actors().All().size());
    for (auto& [h, mi] : rs_.Scene().Actors().All()) {
        if (mi && !mi->IsChild())
            topLevel.push_back(h);
    }
    std::sort(topLevel.begin(), topLevel.end());

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

            for (u32 h : topLevel) {
                auto* mi = rs_.Scene().Actors().Find(h);
                if (!mi)
                    continue;
                if (mi->parentVisibility <= 0.02f)
                    continue;

                const i32 modelLod = mi->render.hasLods ? selectedLod : 0;

                // Hoist the vsCb write + bind out of the geoset loop —
                // worldViewProj is constant across all geosets of this
                // actor in this cascade. Was: write+bind per geoset
                // (~30 × 3 cascades × N actors = 90+ queue.WriteBuffer
                // calls per frame on Firefox = >5 ms of IPC). Now:
                // write+bind once per actor per cascade. Done lazily on
                // first valid geoset so empty actors don't pay the cost.
                bool actorCbBound = false;

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

                    if (!actorCbBound) {
                        if (auto vs = bls::ScopedCb<bls::HdVsCb>(gfx, vsCb)) {
                            BuildShadowVsCb(*vs, mi->worldTransform, cascadeVP);
                        }
                        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 2, vsCb);
                        actorCbBound = true;
                    }

                    cmd->BindIndexBuffer(geo.ib, gfx::Format::R32_UINT);
                    cmd->BindVertexBuffer(0, geo.unskinnedVb, sizeof(Vertex));

                    if (hasBones) {
                        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 3, paletteCb);
                        cmd->BindVertexBuffer(1, geo.boneVb, sizeof(BoneVertex));
                    }

                    // G1 hook. The cascade loop is a second hash-ordered draw
                    // path (REFACTOR_PLAN §1.1 #7) with a currentPso state
                    // tracker whose behaviour follows that order, so it needs
                    // its own record — otherwise P0.8's fix here has no gate.
                    if (debug::DrawTraceEnabled()) {
                        debug::TraceDraw d;
                        d.passSlot = static_cast<u8>(debug::TracePassSlot::ShadowMap);
                        d.shadingModel = static_cast<u8>(debug::TraceShadingModel::None);
                        d.sortOrder = c; // cascade index
                        d.actor.rootActor =
                            debug::TraceRootOrdinal(rs_.Scene().Actors().All(), mi->handle);
                        d.actor.role = static_cast<u8>(mi->role);
                        d.submesh = static_cast<i32>(&geo - mi->render.gpuGeosets.data());
                        d.surface = geo.materialId;
                        d.lod = geo.lod;
                        d.indexCount = geo.indexCount;
                        d.vertexCount = geo.vertexCount;
                        d.streamMask =
                            debug::kStreamBase | (hasBones ? debug::kStreamBone : 0);
                        d.palettePath =
                            hasBones ? (mi->render.skinning.UsesPerActorPalette() ? 1 : 2) : 0;
                        d.psoKey = hasBones ? 2u : 1u;
                        debug::DrawTraceRecorder::Instance().Record(d);
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
