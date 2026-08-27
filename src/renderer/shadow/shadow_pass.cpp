#include "shadow_pass.h"

#include "renderer/bls/bls_cb_layout.h"
#include "renderer/bls/bls_frame.h"
#include "renderer/bls/bls_mat_params.h"
#include "renderer/bls/scoped_cb.h"
#include "renderer/core/surface_table.h"
#include "renderer/debug/draw_trace_hooks.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/render_model.h"
#include "renderer/core/render_detail.h"
#include "renderer/profiles/wc3/wc3_classify.h"
#include "renderer/profiles/wc3/wc3_surface_table.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "renderer/types.h"
#include "whiteout/flakes/types.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace whiteout::flakes::renderer::shadow {

using namespace ::whiteout::flakes::renderer::model;
using namespace ::whiteout::flakes::renderer::animation;
using namespace ::whiteout::flakes::renderer::assets;
using namespace ::whiteout::flakes::renderer::render_detail;

namespace {

namespace wc3 = ::whiteout::flakes::renderer::profiles::wc3;

// What the caster contributes for one geoset: which layer's alpha defines its
// silhouette, and whether that silhouette needs the alpha test to be right.
// `cast = false` drops the geoset from every cascade — an additive glow plane
// is not an occluder, and neither is a geoset whose layers have all faded out.
struct CasterClass {
    bool cast = true;
    bool alphaTest = false;
    i32 layerIndex = -1;
};

CasterClass ClassifyCaster(const RenderableView& view, const wc3::Wc3SurfaceTable* table,
                           const GPUGeoset& geo) {
    // No WC3 table: a foreign product's actor (M2 / M3 / D3), whose layers this
    // pass cannot read. Cast the whole geoset, as it always did.
    if (!table)
        return {};

    const wc3::GeosetClass gc = wc3::ClassifyGeoset(view, geo);
    if (!gc.visible || !gc.opaqueFilter)
        return {.cast = false};

    const wc3::UnpackedLayer layer =
        wc3::Wc3SurfaceTable::Layer(table->Material(geo.materialId), gc.firstVisibleLayer);
    return {.cast = true,
            .alphaTest = bls::FilterToGxAlpha(layer.filterMode) == bls::GxMatAlpha::AlphaKey,
            .layerIndex = gc.firstVisibleLayer};
}

// The caster's VS constants. `misc.w` is `underWater`, and it has to stay 0:
// the depth-prepass PS opens with `discard` on a negative worldPos.w, which the
// VS computes as (worldZ - clipHeight) * underWater, so any other value would
// cut the cascade at the world floor.
void BuildShadowVsCb(bls::HdVsCb& out, const Matrix44f& worldTransform,
                     const Matrix44f& cascadeVP, f32 alpha, const bls::ShaderTexMtx& texMtx) {
    std::memset(&out, 0, sizeof(out));
    out.world = worldTransform;
    out.worldView = worldTransform;
    out.worldViewProj = worldTransform * cascadeVP;
    out.misc = {0.0f, 1.0f, 0.0f, 0.0f};
    // The alpha the lit pass tests is albedo.a × this, so the caster has to
    // carry the geoset's combined alpha or a fading unit's cut-out moves.
    out.diffuseColor = {1.0f, 1.0f, 1.0f, alpha};
    out.texMtx0 = texMtx;
    out.texMtx1 = bls::IdentityTexMtx();
}

// The three registers ps/ps_depth_prepass.slang actually reads. Everything else
// in HdPsCb belongs to shading, which this permutation compiles out. Nothing
// sets `cloakAmount` on a WC3 layer — leaving it 0 selects the uncloaked alpha
// branch, which is the one the lit pass takes for the same reason.
void BuildShadowPsCb(bls::HdPsCb& out, const wc3::UnpackedLayer& layer) {
    std::memset(&out, 0, sizeof(out));
    out.alphaRef = bls::kAlphaKeyRef;
    out.fresnelColor = {0.0f, 0.0f, 0.0f, layer.fresnelOpacity};
}

bls::ShaderTexMtx LayerTexMtx(const RenderableView& view, i32 textureAnimationId) {
    const auto* palette = view.texAnimPalette;
    if (!palette || textureAnimationId < 0 ||
        textureAnimationId >= static_cast<i32>(palette->size()))
        return bls::IdentityTexMtx();
    const auto& e = (*palette)[textureAnimationId];
    bls::ShaderTexMtx m;
    m.rows[0] = {e.row0[0], e.row0[1], e.row0[2], e.row0[3]};
    m.rows[1] = {e.row1[0], e.row1[1], e.row1[2], e.row1[3]};
    return m;
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
    const gfx::BufferHandle vsCb = shadow.vsCb;
    const gfx::BufferHandle psCb = shadow.psCb;
    const bool anyPso = (shadow.psoSkinned != gfx::PipelineHandle::Invalid ||
                         shadow.psoRigid != gfx::PipelineHandle::Invalid) &&
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

    const auto& defaultTex = rs_.Textures().GetDefaults();

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

            // The vsCb write is the expensive half of the bind pair — on
            // Firefox it is a queue.WriteBuffer IPC round trip, and the pass
            // issues one draw per geoset per cascade. It used to be hoisted to
            // once per actor, which an alpha-tested caster can no longer do:
            // the cut-out needs that layer's UV matrix and combined alpha, and
            // those vary within an actor. So write on change instead of on a
            // fixed schedule — an actor whose geosets all resolve to the same
            // constants still pays exactly one write, as before.
            bls::HdVsCb lastVs{};
            bool haveVs = false;

            for (u32 h : topLevel) {
                auto* mi = rs_.Scene().Actors().Find(h);
                if (!mi)
                    continue;
                if (mi->parentVisibility <= 0.02f)
                    continue;

                const i32 modelLod = mi->render.hasLods ? selectedLod : 0;

                RenderableView view;
                FillRenderableView(view, *mi, rs_.Scene().Actors().All());
                // Guarded rather than cast blind: SurfaceTableCast asserts on a
                // product mismatch, and a scene can hold M2 / M3 / D3 actors
                // beside the WC3 ones.
                const wc3::Wc3SurfaceTable* table =
                    (view.surfaceTable && view.surfaceTable->Product() == core::ProductId::Wc3)
                        ? core::SurfaceTableCast<wc3::Wc3SurfaceTable>(view.surfaceTable)
                        : nullptr;

                for (auto& geo : mi->render.gpuGeosets) {
                    if (geo.unskinnedVb == gfx::BufferHandle::Invalid)
                        continue;
                    if (geo.ib == gfx::BufferHandle::Invalid)
                        continue;
                    if (geo.indexCount == 0)
                        continue;

                    if (!GeosetPassesLod(geo.lod, modelLod) || geo.hidden)
                        continue;

                    const f32 geoAlpha = geo.geosetAlpha * mi->parentVisibility;
                    if (geoAlpha <= 0.0f)
                        continue;

                    const CasterClass caster = ClassifyCaster(view, table, geo);
                    if (!caster.cast)
                        continue;

                    const wc3::UnpackedLayer layer = wc3::Wc3SurfaceTable::Layer(
                        table ? table->Material(geo.materialId) : nullptr, caster.layerIndex);

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

                    // An alpha-tested caster falls back to the plain depth PSO
                    // rather than dropping the draw: a solid shadow is wrong,
                    // but no shadow at all is worse, and the fallback only
                    // happens on a bundle without the prepass permutations.
                    gfx::PipelineHandle pso =
                        hasBones ? shadow.psoSkinned : shadow.psoRigid;
                    bool alphaTest = caster.alphaTest;
                    if (alphaTest) {
                        const gfx::PipelineHandle alphaPso =
                            hasBones ? shadow.psoSkinnedAlphaTest : shadow.psoRigidAlphaTest;
                        if (alphaPso != gfx::PipelineHandle::Invalid && psCb != gfx::BufferHandle::Invalid)
                            pso = alphaPso;
                        else
                            alphaTest = false;
                    }
                    if (pso == gfx::PipelineHandle::Invalid)
                        continue;

                    if (pso != currentPso) {
                        cmd->BindPipeline(pso);
                        currentPso = pso;
                    }

                    bls::HdVsCb vsData;
                    BuildShadowVsCb(vsData, mi->worldTransform, cascadeVP,
                                    alphaTest ? geoAlpha * layer.alpha : 1.0f,
                                    alphaTest ? LayerTexMtx(view, layer.textureAnimationId)
                                              : bls::IdentityTexMtx());
                    if (!haveVs || std::memcmp(&vsData, &lastVs, sizeof(vsData)) != 0) {
                        if (auto vs = bls::ScopedCb<bls::HdVsCb>(gfx, vsCb)) {
                            *vs = vsData;
                        }
                        lastVs = vsData;
                        haveVs = true;
                    }
                    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 2, vsCb);

                    if (alphaTest) {
                        if (auto ps = bls::ScopedCb<bls::HdPsCb>(gfx, psCb)) {
                            BuildShadowPsCb(*ps, layer);
                        }
                        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 2, psCb);
                        // t0 + its sampler carry the cut-out; t1 only feeds the
                        // fresnel term, which is inert at fresnelOpacity 0 —
                        // bound anyway because the permutation declares it.
                        BindLayerAlbedo(cmd, view.textures, layer.textureId, defaultTex.White,
                                        rs_.Samplers(), 0);
                        BindLayerAlbedo(cmd, view.textures, layer.normalMapId,
                                        defaultTex.FlatNormal, rs_.Samplers(), 1);
                    }

                    cmd->BindIndexBuffer(geo.ib, gfx::Format::R32_UINT);
                    // coordId picks between the two interleaved copies, so an
                    // alpha-keyed layer authored against UV1 tests the UV set
                    // the lit pass sampled.
                    cmd->BindVertexBuffer(0, PickSlot0Vb(geo, alphaTest ? layer.coordId : 0),
                                          sizeof(Vertex));

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
                        d.layer = caster.layerIndex;
                        d.lod = geo.lod;
                        d.indexCount = geo.indexCount;
                        d.vertexCount = geo.vertexCount;
                        d.streamMask =
                            debug::kStreamBase | (hasBones ? debug::kStreamBone : 0);
                        d.palettePath =
                            hasBones ? (mi->render.skinning.UsesPerActorPalette() ? 1 : 2) : 0;
                        // The caster's material decision, which is what the
                        // alpha-test split turns on.
                        d.blendClass = static_cast<u8>(bls::FilterToGxAlpha(layer.filterMode));
                        d.filterMode = layer.filterMode;
                        d.texIds[0] = alphaTest ? layer.textureId : -1;
                        d.texAnimId = alphaTest ? layer.textureAnimationId : -1;
                        d.combinedAlpha = vsData.diffuseColor.w;
                        d.psoKey = (hasBones ? 2u : 1u) | (alphaTest ? 4u : 0u);
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
