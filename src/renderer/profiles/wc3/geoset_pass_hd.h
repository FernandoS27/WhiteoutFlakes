#pragma once

// GeosetPassHd — the Warcraft III 3.0.0 HD mesh submission (HD, SD_on_HD and
// Crystal). It stays a header-only class because RenderPipeline instantiates it
// directly and it is a friend of RenderPipeline (see render_pipeline.h), which
// is what gives it `impl_` access.
//
// 3.0.0 binds lighting per PASS, not per draw: the main light and IBL ride in
// the per-draw PS bank, but every point light lives in the clustered set
// (PS t16-t18 + b1) that BindPassResources uploads once. See
// WC3_30_LIGHTING_DESIGN.md.

#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/assets/sampler_asset_manager.h"
#include "renderer/assets/texture_asset_manager.h"
#include "renderer/core/surface_pass_base.h"
#include "renderer/debug/draw_trace_hooks.h"
#include "renderer/profiles/wc3/wc3_lighting_frame.h"
#include "renderer/profiles/wc3/wc3_sun.h"
#include "renderer/profiles/wc3/wc3_surface_table.h"
#include "renderer/shading/shading_model.h"
#include "renderer/shadow/shadow_service.h"

#include <span>

namespace whiteout::flakes::renderer {

// The using-directives the class body relied on while it lived in
// render_pipeline.cpp.
using namespace ::whiteout::flakes::renderer::model;
using namespace ::whiteout::flakes::renderer::assets;
using namespace ::whiteout::flakes::renderer::bls;
using namespace ::whiteout::flakes::renderer::render_detail;
using profiles::wc3::ComputeSunDirWS;
using profiles::wc3::UnpackedLayer;
using profiles::wc3::Wc3SurfaceTable;

// The constant-baseline sun's ShadowIntensity (PS cb2[28].w, the IBL scale).
// A viewer choice for when no day/night rig is loaded: 0.15 is the value the
// 2.0.0 path shipped as `kHdLight0BlendWeight`, which lands the probe at the
// brightness the viewer was tuned against. A loaded rig supplies its own.
inline constexpr f32 kHdBaselineShadowIntensity = 0.15f;

class GeosetPassHd : public BlsGeosetPass<GeosetPassHd> {
public:
    using BlsGeosetPass::BlsGeosetPass;

    bool IsAvailable() const {
        return rs_.Pipeline().impl_->blsHdProgram_ && rs_.Pipeline().impl_->blsPsoBuilder_;
    }

    void ComputeViewProj(Matrix44f& view, Matrix44f& proj) const {

        const f32 aspect = (rs_.Pipeline().Height() > 0)
                               ? static_cast<f32>(rs_.Pipeline().Width()) /
                                     static_cast<f32>(rs_.Pipeline().Height())
                               : 1.0f;
        view = rs_.Pipeline().FrameCamera().ViewLH();
        proj = rs_.Pipeline().FrameCamera().ProjectionLH(aspect);
    }

    void BindPassResources(gfx::IGFXCommandList* cmd, bls::FrameInputs& frame,
                           const bls::LightingContext& lighting) {
        auto* impl = rs_.Pipeline().impl_.get();
        const auto& defs = rs_.Textures().GetDefaults();

        BindIbl(cmd, frame);

        const gfx::SamplerHandle linWrap = rs_.Samplers().LinearWrap();
        const gfx::SamplerHandle linClamp = rs_.Samplers().WrapVariant(WrapMode::ClampClamp);
        for (u32 s : {1u, 2u, 3u, 4u})
            cmd->BindSampler(gfx::ShaderStage::Pixel, s, linWrap);

        // Blight mask + ramp (t6 / t8) and the manual depth-test target (t7).
        // Both features are off (cb2[22].w / [23].w stay 0), but the colour
        // permutations declare the textures, and backends that validate
        // descriptors want something bound.
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 6, defs.Black);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 7, defs.Black);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 8, defs.Black);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 6, linClamp);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 8, linClamp);

        frame.mainLight = SelectMainLight(lighting, frame.view);

        // Point-light shadows: only slots the shadow pass drew this frame count,
        // and lights record their slot through the cluster set below.
        const shadow::ShadowService* shadows = rs_.GetShadowService();
        const bool shadowsOn = shadows && shadows->IsEnabled();
        pointShadows_ = (shadowsOn && shadows->PointShadowArray() != gfx::TextureHandle::Invalid)
                            ? std::min<i32>(shadows->RenderedPointShadows(),
                                            static_cast<i32>(shadows->PointShadows().size()))
                            : 0;
        std::array<Vector3f, shadow::kPointShadowSlots> slotPositions{};
        for (i32 s = 0; s < pointShadows_; ++s)
            slotPositions[s] = shadows->PointShadows()[s].position;
        if (pointShadows_ > 0) {
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 9, shadows->PointShadowArray());
            cmd->BindSampler(gfx::ShaderStage::Pixel, 9, rs_.Samplers().ShadowComparison());
        }

        // The clustered light set, binned into the engine's ~16-px tiles
        // (WC3_30_LIGHTING_DESIGN.md D5 step 2).
        static const std::vector<FrameState::LightState> kNoLights;
        const auto& sceneLights = lighting.sceneLights ? *lighting.sceneLights : kNoLights;
        profiles::wc3::BuildBinnedClusterSet(
            sceneLights, frame.view, frame.projection, static_cast<u32>(rs_.Pipeline().Width()),
            static_cast<u32>(rs_.Pipeline().Height()), clusterSet_,
            std::span<const Vector3f>(slotPositions.data(), static_cast<usize>(pointShadows_)));
        clusterLightCount_ = std::min(clusterSet_.lightCount, bls::kClusterMaxCount);
        UploadStructured(impl->blsHdLightsSb_, impl->blsHdLightsCapacity_,
                         std::span<const bls::HdClusterLight>(clusterSet_.lights));
        UploadStructured(impl->blsHdLightIndicesSb_, impl->blsHdLightIndicesCapacity_,
                         std::span<const u32>(clusterSet_.indices));
        UploadStructured(impl->blsHdClustersSb_, impl->blsHdClustersCapacity_,
                         std::span<const u32>(clusterSet_.tiles));
        if (impl->blsHdLightsSb_ != gfx::BufferHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 16, impl->blsHdLightsSb_);
        if (impl->blsHdLightIndicesSb_ != gfx::BufferHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 17, impl->blsHdLightIndicesSb_);
        if (impl->blsHdClustersSb_ != gfx::BufferHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 18, impl->blsHdClustersSb_);

        // VS b1: no blight map, so a zero rect (the lookup clamps to its border).
        if (impl->blsHdVsBlightCb_ != gfx::BufferHandle::Invalid) {
            if (auto cb = bls::ScopedCb<bls::HdVsBlightCb>(rs_.Pipeline().Gfx(),
                                                           impl->blsHdVsBlightCb_)) {
                *cb = bls::HdVsBlightCb{};
            }
            cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 1, impl->blsHdVsBlightCb_);
        }

        // Cascade shadows: the array at t10 with its comparison sampler at s10,
        // sampled only when this frame's shadow pass filled at least one
        // cascade (the count gates the SHADOW_CASCADE permutation per draw).
        shadowCascades_ = (shadowsOn && shadows->DepthArray() != gfx::TextureHandle::Invalid)
                              ? shadows->RenderedCascades()
                              : 0;
        if (shadowCascades_ > 0) {
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 10, shadows->DepthArray());
            cmd->BindSampler(gfx::ShaderStage::Pixel, 10, rs_.Samplers().ShadowComparison());
        }

        // PS b1: cascades and the cluster grid. Only set 2's matrices are
        // meaningful (see shadow_service.h); the other rows stay identity.
        if (impl->blsHdClusteredCb_ != gfx::BufferHandle::Invalid) {
            if (auto cb = bls::ScopedCb<bls::HdPsClusteredCb>(rs_.Pipeline().Gfx(),
                                                              impl->blsHdClusteredCb_)) {
                std::memset(&*cb, 0, sizeof(bls::HdPsClusteredCb));
                for (auto& m : cb->cascadeSets)
                    m = Matrix44f::identity();
                for (i32 c = 0; c < shadowCascades_; ++c)
                    cb->cascadeSets[shadow::kRenderedCascadeSet * shadow::kCascadeSets + c] =
                        shadows->cascadeVP(c);
                cb->cascadeCount = bls::IntBits(shadowCascades_);
                cb->shadowLightCount = bls::IntBits(pointShadows_);
                for (i32 s = 0; s < pointShadows_; ++s) {
                    const shadow::PointShadow& ps = shadows->PointShadows()[s];
                    bls::HdCubeShadow& rec = cb->cubeShadows[s];
                    rec.position = {ps.position.x, ps.position.y, ps.position.z, 1.0f};
                    rec.range = {ps.nearZ, ps.farZ, ps.strength, 0.0f};
                    for (i32 f = 0; f < 6; ++f)
                        rec.faces[f] = ps.faceViewProj[f];
                }
                profiles::wc3::FillClusterConstants(clusterSet_,
                                                    static_cast<f32>(rs_.Pipeline().Width()),
                                                    static_cast<f32>(rs_.Pipeline().Height()), *cb);
            }
            cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 1, impl->blsHdClusteredCb_);
        }
    }

    bls::BaselineLights Baseline(const Matrix44f& view) const {

        if (auto* dnc = rs_.GetDncService();
            dnc && dnc->HasAsset() && rs_.Settings().GetLightingMode() == LightingMode::InGame) {
            const auto sample = dnc->SampleNow();
            if (sample.valid) {
                // Front-biased sun (matches the shadow pass + SD baseline);
                // keep the DNC day/night colours.
                const Vector3f sunWS = ComputeSunDirWS(dnc);
                const Vector3f dirVS = whiteout::transform_normal(
                    Vector3f{-sunWS.x, -sunWS.y, -sunWS.z}, view);
                const Vector3f amb = profiles::wc3::EngineLightColor(sample.ambientColor);
                return {.ambient = sample.ambient,
                        .diffuse = sample.diffuse,
                        .ambientColor = sample.ambientColor,
                        .dirToSourceVS = dirVS,
                        .main = {.ambient = {amb.x * sample.ambientIntensity,
                                             amb.y * sample.ambientIntensity,
                                             amb.z * sample.ambientIntensity},
                                 .shadowIntensity = sample.shadowIntensity,
                                 .color = sample.diffuse,
                                 .dirToLightVS = dirVS,
                                 .enabled = true}};
            }
        }

        const Vector3f amb = {kHdBaselineAmbientColor.x, kHdBaselineAmbientColor.y,
                              kHdBaselineAmbientColor.z};
        const Vector3f col = {kHdBaselineLightColor.x, kHdBaselineLightColor.y,
                              kHdBaselineLightColor.z};
        return {
            .ambient = amb,
            .diffuse = col,
            .dirToSourceVS = {0.0f, 0.0f, -1.0f},
            .main = {.ambient = amb,
                     .shadowIntensity = kHdBaselineShadowIntensity,
                     .color = col,
                     .dirToLightVS = {0.0f, 0.0f, -1.0f},
                     .enabled = true},
        };
    }

    void DrawOpaqueItem(const render_detail::DrawItem& item, bls::FrameInputs& frame,
                        const Matrix44f& viewMat, gfx::IGFXCommandList* cmd,
                        const bls::LightingContext& lighting) {
        const auto& view_ = *item.view;
        EmitLayersHd(view_, (*view_.geosets)[item.geoIdx], frame, viewMat, cmd, lighting);
    }

    void DrawTransparentItem(const render_detail::DrawItem& item, bls::FrameInputs& frame,
                             const Matrix44f& viewMat, gfx::IGFXCommandList* cmd,
                             const bls::LightingContext& lighting) {
        const auto& view_ = *item.view;
        EmitLayersHd(view_, (*view_.geosets)[item.geoIdx], frame, viewMat, cmd, lighting);
    }

    // The HD submission (HD / SD-on-HD / Crystal program selection, fresnel,
    // normal/ORM/emissive + team-colour maps, MRT G-buffer, and the per-layer
    // fading-opaque depth prepass), fed one geoset at a time.
    void EmitLayersHd(const render_detail::RenderableView& view_, const GPUGeoset& geo,
                      bls::FrameInputs& frame, const Matrix44f& viewMat, gfx::IGFXCommandList* cmd,
                      const bls::LightingContext& lighting) {
        (void)viewMat;
        (void)lighting;
        const auto* table = core::SurfaceTableCast<Wc3SurfaceTable>(view_.surfaceTable);
        const GPUMaterial* mat = table ? table->Material(geo.materialId) : nullptr;

        const f32 geoAlpha = geo.geosetAlpha * view_.parentVisibility;

        if (geoAlpha <= 0.0f)
            return;

        i32 numLayers = mat ? (i32)mat->cpu.layers.size() : 0;
        if (numLayers <= 0)
            numLayers = 1;

        cmd->BindIndexBuffer(geo.ib, gfx::Format::R32_UINT);

        const bool hasTangents = (geo.tangentVb != gfx::BufferHandle::Invalid);
        if (hasTangents)
            cmd->BindVertexBuffer(1, geo.tangentVb, sizeof(Vector4f));

        // The second unwrap, at a fixed slot so it doesn't shuffle the tangent
        // and bone slots below. `hd_ps` reads baked ambient occlusion (ORM.x)
        // through it; without the stream the VS duplicates UV0 into uv.zw and
        // the AO_MAP permutation has to stay off.
        const bool hasUv1 = (geo.uv1Vb != gfx::BufferHandle::Invalid);
        if (hasUv1)
            cmd->BindVertexBuffer(3, geo.uv1Vb, sizeof(Vector2f));

        // Bone palette CB: prefer the per-actor handle (Path A) when
        // the actor's SkinningSystem owns it; otherwise fall back to
        // the per-geoset CB (Path B). The vertex buffer's boneIdx
        // values were rewritten at load time to match whichever path
        // this actor sits on, so the shader code doesn't change.
        gfx::BufferHandle paletteCb = geo.bonePaletteCb;
        if (view_.skinning && view_.skinning->UsesPerActorPalette()) {
            paletteCb = view_.skinning->ActorPaletteCb();
        }
        const bool hasBones =
            (geo.boneVb != gfx::BufferHandle::Invalid) && (paletteCb != gfx::BufferHandle::Invalid);
        // Path B actors (more bones than one palette holds) read the whole
        // skeleton's palettes from VS t16 at this geoset's base, as 3.0.0 does
        // for > 256 bones; the CB stays bound for the SD programs' sake.
        const i32 boneBufferBase =
            (hasBones && view_.skinning && !view_.skinning->UsesPerActorPalette() &&
             view_.skinning->BoneBuffer() != gfx::BufferHandle::Invalid)
                ? view_.skinning->BoneBufferBase(geo.geosetId)
                : -1;
        const bool boneBuffer = boneBufferBase >= 0;
        if (hasBones) {
            const u32 boneSlot = hasTangents ? 2u : 1u;
            cmd->BindVertexBuffer(boneSlot, geo.boneVb, sizeof(BoneVertex));
            cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 3, paletteCb);
            if (boneBuffer)
                cmd->BindShaderResource(gfx::ShaderStage::Vertex, 16, view_.skinning->BoneBuffer());
        }

        struct LayerJob {
            UnpackedLayer layer;
            bls::MatParams mp;
            const bls::BlsProgram* program = nullptr;
            bls::GxShaderID programShaderId = bls::GxShaderID::SD_on_HD;
            f32 combinedAlpha = 1.0f;
            bool unlit = false;
            bool isOpaqueFading = false;
            bool valid = false;
        };
        std::vector<LayerJob> jobs(numLayers);

        for (i32 li = 0; li < numLayers; ++li) {
            jobs[li].layer = Wc3SurfaceTable::Layer(mat, li);
            const auto& layer = jobs[li].layer;
            f32 combinedAlpha = geoAlpha * layer.alpha;
            if (combinedAlpha < 0.004f)
                continue;
            const bool isOpaqueFading =
                combinedAlpha < 0.99f && layer.filterMode <= FILTER_TRANSPARENT;
            i32 effectiveFilter = layer.filterMode;
            if (isOpaqueFading)
                effectiveFilter = FILTER_BLEND;

            bls::GxShaderID programShaderId = bls::ProgramForLayer(layer.shaderId);
            if (programShaderId == bls::GxShaderID::Crystal &&
                rs_.Pipeline().impl_->blsCrystalProgram_ == nullptr)
                programShaderId = bls::GxShaderID::HD;
            const bls::BlsProgram* program =
                programShaderId == bls::GxShaderID::Crystal ? rs_.Pipeline().impl_->blsCrystalProgram_
                : programShaderId == bls::GxShaderID::HD    ? rs_.Pipeline().impl_->blsHdProgram_
                                                            : rs_.Pipeline().impl_->blsSdOnHdProgram_;

            bls::MatParams mp = bls::FromMdxLayer(effectiveFilter, layer.flags, programShaderId);
            // Promoting an alpha-key layer to a blend keeps its cutoff: the clip
            // ref stays at the alpha-key value (matches the SD ResolveLayerMaterial).
            if (isOpaqueFading && layer.filterMode == FILTER_TRANSPARENT)
                mp.alphaRef = bls::kAlphaKeyRef;
            if (mp.alpha == bls::GxMatAlpha::Modulate) {
                mp.diffuseColor = {combinedAlpha, 1, 1, 1};
            } else {
                mp.diffuseColor = {geo.geosetColor.x, geo.geosetColor.y, geo.geosetColor.z,
                                   combinedAlpha};
            }
            mp.emissiveGain = layer.emissiveGain;
            mp.fresnelTeamColor = layer.fresnelTeamColor;
            mp.fresnelOpacity = layer.fresnelOpacity;
            mp.fresnelColor = layer.fresnelColor;

            jobs[li].mp = mp;
            jobs[li].program = program;
            jobs[li].programShaderId = programShaderId;
            jobs[li].combinedAlpha = combinedAlpha;
            jobs[li].unlit = (mp.disables & bls::kDisableLighting) != 0;
            jobs[li].isOpaqueFading = isOpaqueFading;
            jobs[li].valid = true;
        }

        auto issueHdDraw = [&](const LayerJob& job, const bls::MatParams& matParams, i32 layerIndex,
                               bool prepassTwin) {
            const auto& layer = job.layer;
            render_detail::ApplyTexAnimPaletteToFrame(frame, view_.texAnimPalette,
                                                      layer.textureAnimationId);
            {
                const bool teamLayer =
                    (layer.teamColorMapId == kHdTeamColorActive) || (layer.teamColorMapId >= 0);

                bls::RenderState rs;
                rs.shaderId = job.programShaderId;
                rs.alphaMode = static_cast<u8>(matParams.alpha);
                rs.numColors = 0;
                rs.numTexCoords = hasUv1 ? 2 : 1;
                // AO_MAP samples ORM.x at UV1. The engine keys it on the layer's
                // AmbientOcclusion flag alone, not on a second UV set being
                // present; the stream and an ORM are our own preconditions.
                rs.aoMap = (layer.flags & MAT_AMBIENT_OCCLUSION) != 0 && hasUv1 &&
                           layer.ormMapId >= 0;
                rs.numTangents = hasTangents ? 1 : 0;
                rs.numWeights = hasBones ? 4 : 0;
                rs.boneBuffer = boneBuffer;
                // The fading-opaque twin writes depth only: DEPTH_PREPASS
                // compiles every output away.
                rs.depthPrepass = !matParams.ColorWriteEnabled();
                rs.lighting = !job.unlit;
                rs.shadowCascade = rs.lighting && !rs.depthPrepass && shadowCascades_ > 0;
                rs.pointShadows = rs.lighting && !rs.depthPrepass && pointShadows_ > 0;
                // Opaque, colour-writing materials take the G-buffer permutation
                // that also writes SV_Target1 (linear view-Z) and SV_Target2
                // (encoded normal); transparents stay single-target.
                rs.mrt = matParams.DepthWriteEnabled() && matParams.ColorWriteEnabled();
                rs.multiLayer = teamLayer && job.programShaderId != bls::GxShaderID::SD_on_HD;
                auto perm = bls::SelectPermutes(rs);

                bls::PsoRequest req{};
                req.program = job.program;
                req.vsIndex = perm.vs;
                req.psIndex = perm.ps;
                req.material = matParams;

                if (hasBones) {
                    if (hasUv1)
                        req.layout = hasTangents
                                         ? bls::VertexLayoutKind::MeshHDSkinnedUv1
                                         : bls::VertexLayoutKind::MeshHDSkinnedNoTangentUv1;
                    else
                        req.layout = hasTangents
                                         ? bls::VertexLayoutKind::MeshHDSkinned
                                         : bls::VertexLayoutKind::MeshHDSkinnedNoTangent;
                } else {
                    if (hasUv1)
                        req.layout = hasTangents ? bls::VertexLayoutKind::MeshHDTangentUv1
                                                 : bls::VertexLayoutKind::MeshHDPlainUv1;
                    else
                        req.layout = hasTangents ? bls::VertexLayoutKind::MeshHDTangent
                                                 : bls::VertexLayoutKind::ParticleSD;
                }
                req.topology = gfx::PrimitiveTopology::TriangleList;
                req.rtvFormat = RenderPipeline::kHdrSceneFormat;
                // Match the HD G-buffer render pass: slot 1 = linear depth,
                // slot 2 = encoded normal. Only the MRT permutation writes them,
                // so only it enables extra-slot writes — WebGPU rejects a PSO
                // whose extras have a write mask with no fragment output.
                req.extraRtvFormats[0] = RenderPipeline::kLinearDepthFormat;
                req.extraRtvFormats[1] = RenderPipeline::kNormalBufferFormat;
                req.extraRtvCount = 2;
                req.extraColorWrite = rs.mrt;
                req.dsvFormat = rs_.Pipeline().impl_->depthStencilFormat_;
                req.lhClipSpace = true;
                // A debug view swaps the pixel program and nothing else, so the
                // vertex permutation, streams and blend above are the real
                // draw's. The fading-opaque depth twin keeps its own.
                const bool debugDraw = debug_.debugSurfaces && !rs.depthPrepass;
                if (debugDraw) {
                    const bool lightingView =
                        core::KindOf(debug_.view) == core::DebugViewKind::Lighting;
                    req.psOverride = DebugPrograms().Hd(lightingView && rs.shadowCascade,
                                                        lightingView && rs.pointShadows);
                    req.extraColorWrite = false;
                }
                auto pso = rs_.Pipeline().impl_->blsPsoBuilder_->GetOrBuild(req);
                if (pso == gfx::PipelineHandle::Invalid)
                    return;
                cmd->BindPipeline(pso);

                cmd->BindVertexBuffer(0, render_detail::PickSlot0Vb(geo, layer.coordId),
                                      sizeof(Vertex));

                frame.world = view_.worldTransform;
                frame.boneBufferBase = boneBuffer ? boneBufferBase : 0;

                if (auto vs = bls::ScopedCb<bls::HdVsCb>(rs_.Pipeline().Gfx(),
                                                         rs_.Pipeline().impl_->blsHdVsCb_)) {
                    bls::BuildHdVsCb(*vs, frame, matParams);
                }
                cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 2,
                                        rs_.Pipeline().impl_->blsHdVsCb_);

                // One per-draw PS bank for all three programs in 3.0.0.
                if (auto ps = bls::ScopedCb<bls::HdPsCb>(rs_.Pipeline().Gfx(),
                                                         rs_.Pipeline().impl_->blsHdPsCb_)) {
                    bls::BuildHdPsCb(*ps, frame, matParams);
                }
                cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 2,
                                        rs_.Pipeline().impl_->blsHdPsCb_);

                auto bindMaterialTex = [&](u32 slot, i32 texId, gfx::TextureHandle fallback,
                                           u32* outWrap) {
                    if (texId >= 0 && view_.textures) {
                        const gfx::TextureHandle h = view_.textures->Get(texId);
                        if (h != gfx::TextureHandle::Invalid) {
                            cmd->BindShaderResource(gfx::ShaderStage::Pixel, slot, h);
                            if (outWrap)
                                *outWrap = view_.textures->WrapFlags(texId) & kSamplerWrapBitsMask;
                            return true;
                        }
                    }
                    cmd->BindShaderResource(gfx::ShaderStage::Pixel, slot, fallback);
                    return false;
                };

                const auto& defs = rs_.Textures().GetDefaults();
                u32 wrapFlags = kSamplerWrapBitsMask;
                bindMaterialTex(0, layer.textureId, defs.White, &wrapFlags);
                bindMaterialTex(1, layer.normalMapId, defs.FlatNormal, nullptr);
                bindMaterialTex(2, layer.ormMapId, defs.NeutralOrm, nullptr);
                bindMaterialTex(3, layer.emissiveMapId, defs.Black, nullptr);

                if (layer.teamColorMapId == kHdTeamColorActive) {
                    const u32 ownerRgba = view_.teamColor | 0xFF000000u;
                    cmd->BindShaderResource(gfx::ShaderStage::Pixel, 4,
                                            rs_.Replaceables().GetHdSwatchTextureFor(ownerRgba));
                } else if (layer.teamColorMapId >= 0) {
                    bindMaterialTex(4, layer.teamColorMapId, defs.Black, nullptr);
                } else {
                    cmd->BindShaderResource(gfx::ShaderStage::Pixel, 4, defs.Black);
                }
                cmd->BindSampler(gfx::ShaderStage::Pixel, 0, rs_.Samplers().WrapVariant(wrapFlags));

                if (debugDraw) {
                    u32 dbgFlags = 0;
                    if (job.programShaderId != bls::GxShaderID::SD_on_HD)
                        dbgFlags |= profiles::wc3::kWc3DebugPbr;
                    if (rs.multiLayer)
                        dbgFlags |= profiles::wc3::kWc3DebugTeamLayer;
                    if (rs.aoMap)
                        dbgFlags |= profiles::wc3::kWc3DebugAoMap;
                    if (rs.lighting)
                        dbgFlags |= profiles::wc3::kWc3DebugLit;
                    // SelectPermutes' ALPHA_TEST axis.
                    if (rs.alphaMode != 0)
                        dbgFlags |= profiles::wc3::kWc3DebugAlphaTest;
                    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 3,
                                            DebugPrograms().Write(core::MakeDebugViewCb(
                                                debug_, debugTarget_, 0, dbgFlags)));
                }

                // G1 hook — see the SD path's TraceThisLayer for why this sits
                // after the PSO resolved rather than at item level.
                if (debug::DrawTraceEnabled())
                    TraceThisLayer(view_, geo, job, matParams, layerIndex, prepassTwin, req,
                                   hasTangents, hasBones, frame);

                cmd->DrawIndexed(geo.indexCount);
            }
        };

        for (i32 li = 0; li < numLayers; ++li) {
            if (!jobs[li].valid || !jobs[li].isOpaqueFading)
                continue;
            bls::MatParams prepass = jobs[li].mp;
            prepass.diffuseColor = {1.0f, 1.0f, 1.0f, 1.0f};
            prepass.disables &= ~bls::kDisableDepthWrite;
            prepass.disables |= bls::kDisableBit8;
            issueHdDraw(jobs[li], prepass, li, true);
        }

        for (i32 li = 0; li < numLayers; ++li) {
            if (!jobs[li].valid)
                continue;
            issueHdDraw(jobs[li], jobs[li].mp, li, false);
        }
    }

    template <class Job>
    void TraceThisLayer(const render_detail::RenderableView& view_, const GPUGeoset& geo,
                        const Job& job, const bls::MatParams& matParams, i32 layerIndex,
                        bool prepassTwin, const bls::PsoRequest& req, bool hasTangents,
                        bool hasBones, bls::FrameInputs& frame) {
        const auto& layer = job.layer;
        debug::TraceDraw d;
        d.shadingModel = static_cast<u8>(
            job.programShaderId == bls::GxShaderID::Crystal  ? debug::TraceShadingModel::Wc3Crystal
            : job.programShaderId == bls::GxShaderID::HD     ? debug::TraceShadingModel::Wc3Hd
                                                             : debug::TraceShadingModel::Wc3SdOnHd);
        d.blendClass = static_cast<u8>(matParams.alpha);
        // The cluster set the pass bound; every lit draw reads the same one.
        d.lightCount = static_cast<u8>(job.unlit ? 0u : std::min(clusterLightCount_, 255u));
        d.combinedAlpha = job.combinedAlpha;
        // The fading-opaque twin is a real depth-only draw, so it says so
        // rather than tying with its colour draw on everything but the PSO.
        if (prepassTwin)
            d.depthFill = static_cast<u8>(bls::DepthFill::Depth);
        d.texIds[0] = layer.textureId;
        d.texIds[1] = layer.normalMapId;
        d.texIds[2] = layer.ormMapId;
        d.texIds[3] = layer.emissiveMapId;
        d.texIds[4] = layer.teamColorMapId;
        d.streamMask = debug::kStreamBase | (hasTangents ? debug::kStreamTangent : 0) |
                       (hasBones ? debug::kStreamBone : 0) |
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
                                       .vertexLayout = static_cast<u32>(req.layout),
                                       .extraRtvCount = req.extraRtvCount,
                                       .extraColorWrite = req.extraColorWrite,
                                       .wireframe = req.wireframe,
                                       .lhClipSpace = req.lhClipSpace});
        bls::HdVsCb vs{};
        bls::BuildHdVsCb(vs, frame, matParams);
        u64 h = debug::TraceHashBytes(&vs, sizeof(vs));
        bls::HdPsCb ps{};
        bls::BuildHdPsCb(ps, frame, matParams);
        h = debug::TraceHashBytes(&ps, sizeof(ps), h);
        d.cbHash = h;
        debug::RecordGeosetDraw(d, view_, geo, layer, layerIndex, frame);
    }

private:
    // PS t11 / t12 (probe cube arrays) + t13 (split-sum LUT), and their mip
    // counts in the frame inputs.
    void BindIbl(gfx::IGFXCommandList* cmd, bls::FrameInputs& frame) {
        auto* impl = rs_.Pipeline().impl_.get();
        const bool useDayNight = impl->iblDayNightLoaded_ && rs_.GetDncService() != nullptr &&
                                 rs_.Settings().GetLightingMode() == LightingMode::InGame;

        gfx::TextureHandle from = gfx::TextureHandle::Invalid;
        gfx::TextureHandle to = gfx::TextureHandle::Invalid;
        if (useDayNight) {
            const auto blend = rs_.GetDncService()->ComputeEnvMapBlend();
            const bool dayPrimary = blend.isDaytime;
            frame.envFromMipCount = dayPrimary ? impl->iblDayMipCount_ : impl->iblNightMipCount_;
            frame.envToMipCount = dayPrimary ? impl->iblNightMipCount_ : impl->iblDayMipCount_;
            frame.envTransitionT = blend.transitionT;
            const auto day = rs_.Textures().GetOwned(RenderPipeline::kIblDayProbeName);
            const auto night = rs_.Textures().GetOwned(RenderPipeline::kIblNightProbeName);
            from = dayPrimary ? day : night;
            to = dayPrimary ? night : day;
        } else {
            frame.envFromMipCount = impl->iblProbeMipCount_;
            frame.envToMipCount = impl->iblProbeMipCount_;
            frame.envTransitionT = 0.75f;
            from = rs_.Textures().GetOwned(RenderPipeline::kIblFromProbeName);
            to = rs_.Textures().GetOwned(RenderPipeline::kIblToProbeName);
            if (to == gfx::TextureHandle::Invalid)
                to = from;
        }
        // Both counts zero is the shader's "no probe" test; leave them zero
        // rather than sampling an unbound cube.
        if (from == gfx::TextureHandle::Invalid || to == gfx::TextureHandle::Invalid) {
            frame.envFromMipCount = 0.0f;
            frame.envToMipCount = 0.0f;
        }

        const gfx::SamplerHandle linWrap = rs_.Samplers().LinearWrap();
        const auto& defs = rs_.Textures().GetDefaults();
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 11,
                                from != gfx::TextureHandle::Invalid ? from : defs.BlackCube);
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, 12,
                                to != gfx::TextureHandle::Invalid ? to : defs.BlackCube);
        const gfx::TextureHandle lut = rs_.Textures().GetOwned(RenderPipeline::kIblSplitSumLutName);
        if (lut != gfx::TextureHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 13, lut);
        for (u32 s : {11u, 12u, 13u})
            cmd->BindSampler(gfx::ShaderStage::Pixel, s, linWrap);
    }

    // The engine's main light is light 0 of the device, the world sun. In the
    // viewer that is the DNC rig or the constant baseline, except in Dynamic
    // mode, where a model's own directional light takes the slot and a scene
    // with only point lights has no main light at all (the SD palette's rule).
    bls::MainLight SelectMainLight(const bls::LightingContext& lighting,
                                   const Matrix44f& view) const {
        switch (lighting.mode) {
        case LightingMode::Glue:
            return {};
        case LightingMode::InGame:
            return lighting.baseline.main;
        case LightingMode::Dynamic:
            break;
        }
        if (!lighting.sceneLights)
            return lighting.baseline.main;
        bool anyEnabled = false;
        for (const auto& L : *lighting.sceneLights) {
            if (!L.enabled)
                continue;
            anyEnabled = true;
            if (L.kind == FrameState::LightKind::Omni)
                continue;
            const f32 n = std::sqrt(L.worldDir.x * L.worldDir.x + L.worldDir.y * L.worldDir.y +
                                    L.worldDir.z * L.worldDir.z);
            const Vector3f d = n > 1e-6f ? Vector3f{L.worldDir.x / n, L.worldDir.y / n,
                                                    L.worldDir.z / n}
                                         : Vector3f{0, 0, -1};
            const Vector3f amb = profiles::wc3::EngineLightColor(L.ambientColor);
            return {.ambient = {amb.x * L.ambIntensity, amb.y * L.ambIntensity,
                                amb.z * L.ambIntensity},
                    .shadowIntensity = L.shadowIntensity,
                    .color = L.diffuse,
                    .dirToLightVS = whiteout::transform_normal(Vector3f{-d.x, -d.y, -d.z}, view),
                    .enabled = true};
        }
        return anyEnabled ? bls::MainLight{} : lighting.baseline.main;
    }

    // Write `data` into a CPU-writable structured buffer, growing it when the
    // set outgrew the current allocation.
    template <class T>
    void UploadStructured(gfx::BufferHandle& handle, u32& capacity, std::span<const T> data) {
        auto* gfx = rs_.Pipeline().Gfx();
        if (!gfx || data.empty())
            return;
        const u32 count = static_cast<u32>(data.size());
        if (handle == gfx::BufferHandle::Invalid || count > capacity) {
            if (handle != gfx::BufferHandle::Invalid)
                gfx->Destroy(handle);
            capacity = std::max<u32>(count, capacity * 2u);
            capacity = std::max<u32>(capacity, 16u);
            handle = gfx->CreateBuffer({
                .size = static_cast<u64>(capacity) * sizeof(T),
                .elementStride = sizeof(T),
                .usage = gfx::BufferUsage::ShaderResource | gfx::BufferUsage::CpuWritable,
            });
            if (handle == gfx::BufferHandle::Invalid) {
                capacity = 0;
                return;
            }
        }
        gfx->UpdateBuffer(handle, data.data(), data.size_bytes());
    }

    profiles::wc3::ClusterSet clusterSet_;
    u32 clusterLightCount_ = 0;
    i32 shadowCascades_ = 0;
    i32 pointShadows_ = 0;
};

} // namespace whiteout::flakes::renderer
