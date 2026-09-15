#pragma once

// GeosetPassBls — relocated verbatim from render_pipeline.cpp (P1). The class body is
// unchanged; only its file moved. It stays a header-only class because
// RenderPipeline instantiates it directly and it is a friend of RenderPipeline
// (see render_pipeline.h), which is what gives it `impl_` access.

#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/assets/sampler_asset_manager.h"
#include "renderer/assets/texture_asset_manager.h"
#include "renderer/core/surface_pass_base.h"
#include "renderer/debug/draw_trace_hooks.h"
#include "renderer/profiles/wc3/wc3_sun.h"
#include "renderer/profiles/wc3/wc3_surface_table.h"
#include "renderer/shading/shading_model.h"
#include "renderer/shadow/shadow_service.h"

namespace whiteout::flakes::renderer {

// The using-directives the class body relied on while it lived in
// render_pipeline.cpp. Kept so the body itself is byte-identical to what moved.
using namespace ::whiteout::flakes::renderer::model;
using namespace ::whiteout::flakes::renderer::assets;
using namespace ::whiteout::flakes::renderer::bls;
using namespace ::whiteout::flakes::renderer::render_detail;
using profiles::wc3::ComputeSunDirWS;
using profiles::wc3::UnpackedLayer;
using profiles::wc3::Wc3SurfaceTable;

class GeosetPassBls : public BlsGeosetPass<GeosetPassBls> {
public:
    using BlsGeosetPass::BlsGeosetPass;

    bool IsAvailable() const {
        return rs_.Pipeline().impl_->blsSdProgram_ && rs_.Pipeline().impl_->blsPsoBuilder_;
    }

    void ComputeViewProj(Matrix44f& view, Matrix44f& proj) const {
        view = rs_.Pipeline().FrameCamera().GetViewMatrix();
        const f32 aspect = (rs_.Pipeline().Height() > 0)
                               ? static_cast<f32>(rs_.Pipeline().Width()) /
                                     static_cast<f32>(rs_.Pipeline().Height())
                               : 1.0f;
        proj = rs_.Pipeline().FrameCamera().ProjectionRH(aspect);
    }

    void BindPassResources(gfx::IGFXCommandList*, bls::FrameInputs&,
                           const bls::LightingContext&) const {}

    bls::BaselineLights Baseline(const Matrix44f& view) const {

        if (auto* dnc = rs_.GetDncService();
            dnc && dnc->HasAsset() && rs_.Settings().GetLightingMode() == LightingMode::InGame) {
            const auto sample = dnc->SampleNow();
            if (sample.valid) {
                // Light *from* the front-biased sun (toward-light = -travel),
                // keeping the DNC's day/night ambient+diffuse colour. Uses the
                // same sun as the shadow pass so the lit side and the cast
                // shadow agree, and the model's front stays illuminated.
                const Vector3f sunWS = ComputeSunDirWS(dnc);
                const Vector3f dirVS = whiteout::transform_normal(
                    Vector3f{-sunWS.x, -sunWS.y, -sunWS.z}, view);
                return {.ambient = sample.ambient,
                        .diffuse = sample.diffuse,
                        .ambientColor = sample.ambientColor,
                        .dirToSourceVS = dirVS};
            }
        }

        return {
            .ambient = {kGeosetAmbientColor.x, kGeosetAmbientColor.y, kGeosetAmbientColor.z},
            .diffuse = {kGeosetLightColor.x, kGeosetLightColor.y, kGeosetLightColor.z},
            .dirToSourceVS = {0.0f, 0.0f, 1.0f},
        };
    }

    // ---- List-driven submission (RunLists dispatches here) ----------------
    // DrawOpaqueItem / DrawTransparentItem name *what* to draw; EmitLayers is
    // the single cohesive submission path (one geoset, one depth-fill mode);
    // DrawLayer issues the PSO + draw for one resolved layer.

    void DrawOpaqueItem(const render_detail::DrawItem& item, bls::FrameInputs& frame,
                        const Matrix44f& viewMat, gfx::IGFXCommandList* cmd,
                        const bls::LightingContext& lighting) {
        const auto& view_ = *item.view;
        EmitLayers(view_, (*view_.geosets)[item.geoIdx], bls::DepthFill::None, frame, viewMat, cmd,
                   lighting);
    }

    void DrawTransparentItem(const render_detail::DrawItem& item, bls::FrameInputs& frame,
                             const Matrix44f& viewMat, gfx::IGFXCommandList* cmd,
                             const bls::LightingContext& lighting) {
        const auto& view_ = *item.view;
        EmitLayers(view_, (*view_.geosets)[item.geoIdx], item.depthFill, frame, viewMat, cmd,
                   lighting);
    }

    // Draw a geoset's visible layers (in order) under `depthFill`. The per-layer
    // fade promotion + depth-only prepass live entirely in ResolveLayerMaterial —
    // there is no special-case prepass loop here.
    void EmitLayers(const render_detail::RenderableView& view_, const GPUGeoset& geo,
                    bls::DepthFill depthFill, bls::FrameInputs& frame, const Matrix44f& viewMat,
                    gfx::IGFXCommandList* cmd, const bls::LightingContext& lighting) {
        const auto* table = core::SurfaceTableCast<Wc3SurfaceTable>(view_.surfaceTable);
        const GPUMaterial* mat = table ? table->Material(geo.materialId) : nullptr;

        const f32 geoAlpha = geo.geosetAlpha * view_.parentVisibility;
        if (geoAlpha <= 0.0f)
            return;

        i32 numLayers = mat ? (i32)mat->cpu.layers.size() : 0;
        if (numLayers <= 0)
            numLayers = 1;

        // Pick the bone palette CB the actor owns: per-actor on Path A (on the
        // SkinningSystem), per-geoset on Path B. Without this the SD path saw
        // only `geo.bonePaletteCb` (Invalid for Path A) and rendered bind pose.
        gfx::BufferHandle paletteCb = geo.bonePaletteCb;
        if (view_.skinning && view_.skinning->UsesPerActorPalette())
            paletteCb = view_.skinning->ActorPaletteCb();
        const bool hasBones = render_detail::BindSdMeshGeometry(cmd, geo, paletteCb);
        const auto layout =
            hasBones ? bls::VertexLayoutKind::ParticleSDSkinned : bls::VertexLayoutKind::ParticleSD;

        const i32 lightCount = owner_->SelectLights(
            frame, lighting, viewMat, render_detail::GeosetCentroidWS(view_, geo));

        for (i32 li = 0; li < numLayers; ++li) {
            const UnpackedLayer layer = Wc3SurfaceTable::Layer(mat, li);
            const f32 combinedAlpha = geoAlpha * layer.alpha;
            if (combinedAlpha < 0.004f)
                continue;

            const bls::MatParams base =
                bls::FromMdxLayer(layer.filterMode, layer.flags, bls::GxShaderID::SD);
            const Vector4f color = {geo.geosetColor.x, geo.geosetColor.y, geo.geosetColor.z,
                                    combinedAlpha};
            // Opaque/alpha-key layer with sub-full opacity → promoted to a blend
            // (ResolveLayerMaterial); an alpha-key layer keeps its cutoff ref.
            const bool isOpaqueFading =
                combinedAlpha < 0.99f && bls::FilterToGxAlpha(layer.filterMode) < bls::GxMatAlpha::Blend;
            const bls::MatParams mp =
                bls::ResolveLayerMaterial(depthFill, base, isOpaqueFading, color);

            const bool unlit = (mp.disables & bls::kDisableLighting) != 0;
            const i32 activeN = unlit ? 0 : lightCount;
            DrawLayer(view_, geo, layer, li, combinedAlpha, mp, activeN, unlit, hasBones, layout,
                      frame, cmd);
        }
    }

    void DrawLayer(const render_detail::RenderableView& view_, const GPUGeoset& geo,
                   const UnpackedLayer& layer, i32 layerIndex, f32 combinedAlpha,
                   const bls::MatParams& matParams, i32 activeN, bool unlit, bool hasBones,
                   bls::VertexLayoutKind layout, bls::FrameInputs& frame,
                   gfx::IGFXCommandList* cmd) {
        auto* impl = rs_.Pipeline().impl_.get();
        frame.numLights = activeN;
        render_detail::ApplyTexAnimPaletteToFrame(frame, view_.texAnimPalette,
                                                  layer.textureAnimationId);

        const auto rsLocal = bls::MakeSdMeshRenderState(matParams, activeN, unlit, hasBones,
                                                        bls::DrawFogMode(frame.fog, matParams));
        const auto permLocal = bls::SelectPermutes(rsLocal);
        auto reqLocal = bls::MakePsoRequest(impl->blsSdProgram_, layout, matParams, permLocal);

        reqLocal.rtvFormat = rs_.Pipeline().SceneTargetFormat();
        // An MRT scene pass binds a 3-RT G-buffer; the SD-on-HD PSO must match
        // the attachment count even though it only writes SV_Target0.
        reqLocal.extraRtvCount = rs_.Pipeline().SceneExtraRtvFormats(reqLocal.extraRtvFormats);
        reqLocal.dsvFormat = impl->depthStencilFormat_;
        auto pso = impl->blsPsoBuilder_->GetOrBuild(reqLocal);
        if (pso == gfx::PipelineHandle::Invalid)
            return;
        cmd->BindPipeline(pso);
        cmd->BindVertexBuffer(0, render_detail::PickSlot0Vb(geo, layer.coordId), sizeof(Vertex));

        frame.world = view_.worldTransform;
        if (auto vs = bls::ScopedCb<bls::SdVsCbA>(rs_.Pipeline().Gfx(), impl->blsSdVsCb_))
            bls::BuildSdVsCbA(*vs, frame, matParams);
        if (auto ps = bls::ScopedCb<bls::SdPsCbA>(rs_.Pipeline().Gfx(), impl->blsSdPsCb_))
            bls::BuildSdPsCbA(*ps, frame, matParams);
        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, impl->blsSdVsCb_);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, impl->blsSdPsCb_);

        render_detail::BindLayerAlbedo(cmd, view_.textures, layer.textureId,
                                       rs_.Textures().GetDefaults().White, rs_.Samplers());

        // G1 hook. Placed after the PSO resolved and the CBs were written, so
        // the record covers exactly the draws that were submitted — an
        // item-level hook would also record layers DrawLayer returned from.
        if (debug::DrawTraceEnabled())
            TraceThisLayer(view_, geo, layer, layerIndex, combinedAlpha, matParams, activeN,
                           hasBones, layout, reqLocal, frame);

        cmd->DrawIndexed(geo.indexCount);
    }

    void TraceThisLayer(const render_detail::RenderableView& view_, const GPUGeoset& geo,
                        const UnpackedLayer& layer, i32 layerIndex,
                        f32 combinedAlpha, const bls::MatParams& matParams, i32 activeN,
                        bool hasBones, bls::VertexLayoutKind layout, const bls::PsoRequest& req,
                        bls::FrameInputs& frame) {
        debug::TraceDraw d;
        d.shadingModel = static_cast<u8>(debug::TraceShadingModel::Wc3Sd);
        d.blendClass = static_cast<u8>(matParams.alpha);
        d.lightCount = static_cast<u8>(activeN);
        d.combinedAlpha = combinedAlpha;
        d.texIds[0] = layer.textureId;
        d.streamMask = debug::kStreamBase | (hasBones ? debug::kStreamBone : 0) |
                       ((layer.coordId == 1 &&
                         geo.unskinnedVb1 != gfx::BufferHandle::Invalid)
                            ? debug::kStreamBaseUv1
                            : 0);
        if (hasBones) {
            const bool pathA = view_.skinning && view_.skinning->UsesPerActorPalette();
            d.palettePath = pathA ? 1 : 2;
            d.paletteSlots = view_.skinning ? view_.skinning->NodeCount() : 0;
        }
        d.psoKey = debug::TracePsoKey({.vsPermute = req.vsIndex,
                                       .psPermute = req.psIndex,
                                       .matAlpha = static_cast<u32>(matParams.alpha),
                                       .disables = matParams.disables,
                                       .vertexLayout = static_cast<u32>(layout),
                                       .extraRtvCount = req.extraRtvCount,
                                       .extraColorWrite = req.extraColorWrite,
                                       .wireframe = req.wireframe,
                                       .lhClipSpace = req.lhClipSpace});
        // Hash the constant-buffer *values*, not just the list shape: an
        // off-by-one that hands layer 1's alpha to layer 0 leaves every other
        // recorded field identical.
        bls::SdVsCbA vs{};
        bls::SdPsCbA ps{};
        bls::BuildSdVsCbA(vs, frame, matParams);
        bls::BuildSdPsCbA(ps, frame, matParams);
        d.cbHash = debug::TraceHashBytes(&ps, sizeof(ps), debug::TraceHashBytes(&vs, sizeof(vs)));
        debug::RecordGeosetDraw(d, view_, geo, layer, layerIndex, frame);
    }
};

} // namespace whiteout::flakes::renderer
