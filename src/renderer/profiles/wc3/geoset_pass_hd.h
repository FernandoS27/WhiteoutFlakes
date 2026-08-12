#pragma once

// GeosetPassHd — relocated verbatim from render_pipeline.cpp (P1). The class body is
// unchanged; only its file moved. It stays a header-only class because
// RenderPipeline instantiates it directly and it is a friend of RenderPipeline
// (see render_pipeline.h), which is what gives it `impl_` access.

#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/assets/sampler_asset_manager.h"
#include "renderer/assets/texture_asset_manager.h"
#include "renderer/core/surface_pass_base.h"
#include "renderer/debug/draw_trace_hooks.h"
#include "renderer/profiles/wc3/wc3_sun.h"
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

    void BindPassResources(gfx::IGFXCommandList* cmd, bls::FrameInputs& frame) {

        const bool useDayNight = rs_.Pipeline().impl_->iblDayNightLoaded_ &&
                                 rs_.GetDncService() != nullptr &&
                                 rs_.Settings().GetLightingMode() == LightingMode::InGame;
        if (useDayNight) {
            const auto blend = rs_.GetDncService()->ComputeEnvMapBlend();

            const bool dayPrimary = blend.isDaytime;
            frame.envFromMipEnd = dayPrimary ? rs_.Pipeline().impl_->iblDayMipEnd_
                                             : rs_.Pipeline().impl_->iblNightMipEnd_;
            frame.envToMipEnd = dayPrimary ? rs_.Pipeline().impl_->iblNightMipEnd_
                                           : rs_.Pipeline().impl_->iblDayMipEnd_;
            frame.envTransitionT = blend.transitionT;
        } else {
            frame.envFromMipEnd = rs_.Pipeline().impl_->iblProbeMipEnd_;
            frame.envToMipEnd = rs_.Pipeline().impl_->iblProbeMipEnd_;
            frame.envTransitionT = 0.75f;
        }

        const gfx::SamplerHandle linWrap = rs_.Samplers().LinearWrap();
        cmd->BindSampler(gfx::ShaderStage::Pixel, 1, linWrap);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 2, linWrap);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 3, linWrap);
        // The HD pixel shader also samples s_teamColor (s4) and the IBL
        // probes / BRDF LUT at s13..s15. They were never bound explicitly
        // — d3d11/d3d12 forgive the missing slots silently, but Vulkan
        // fires VUID-vkCmdDrawIndexed-None-08114 the first time a draw
        // touches the unbound descriptor.
        cmd->BindSampler(gfx::ShaderStage::Pixel, 4, linWrap);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 13, linWrap);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 14, linWrap);
        cmd->BindSampler(gfx::ShaderStage::Pixel, 15, linWrap);

        gfx::TextureHandle from = gfx::TextureHandle::Invalid;
        gfx::TextureHandle to = gfx::TextureHandle::Invalid;
        if (useDayNight) {
            const auto day = rs_.Textures().GetOwned(RenderPipeline::kIblDayProbeName);
            const auto night = rs_.Textures().GetOwned(RenderPipeline::kIblNightProbeName);
            const auto blend = rs_.GetDncService()->ComputeEnvMapBlend();
            from = blend.isDaytime ? day : night;
            to = blend.isDaytime ? night : day;
        } else {
            from = rs_.Textures().GetOwned(RenderPipeline::kIblFromProbeName);
            to = rs_.Textures().GetOwned(RenderPipeline::kIblToProbeName);
            if (to == gfx::TextureHandle::Invalid)
                to = from;
        }
        if (from != gfx::TextureHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 13, from);
        if (to != gfx::TextureHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 14, to);
        const gfx::TextureHandle lut = rs_.Textures().GetOwned(RenderPipeline::kIblSplitSumLutName);
        if (lut != gfx::TextureHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, 15, lut);

        if (auto* shadowSvc = rs_.GetShadowService()) {
            // The HD opaque PS samples each cascade with `SampleCmpLevelZero`
            // which REQUIRES a hardware comparison sampler. Without binding
            // one, WebGPU falls back to its default-Always comparison
            // sampler (no shadows) and D3D12/Vulkan are undefined (the
            // user-visible symptom is "everything darker when shadows are
            // on"). Bind the LessEqual comparison sampler at the same slots
            // as the depth targets.
            const gfx::SamplerHandle cmpSmp = rs_.Samplers().ShadowComparison();
            for (i32 c = 0; c < 3; ++c) {
                const gfx::TextureHandle sh = shadowSvc->depthTarget(c);
                if (sh != gfx::TextureHandle::Invalid) {
                    cmd->BindShaderResource(gfx::ShaderStage::Pixel, 10 + static_cast<u32>(c), sh);
                }
                if (cmpSmp != gfx::SamplerHandle::Invalid)
                    cmd->BindSampler(gfx::ShaderStage::Pixel, 10 + static_cast<u32>(c), cmpSmp);
            }
        }

        // Hoist pass-constant CBs out of the per-draw lambda. The HD draw
        // path used to write + bind HdShadowCascadesCb, HdDebugVisCb, and
        // SdOnHdShadowCascadeCountCb on EVERY draw, even though their
        // contents are constant across the entire pass (cascade VPs,
        // global debug mode, cascade count). On Firefox each ScopedCb
        // costs ~50µs IPC; with 60 draws/frame this was ~9 ms wasted.
        // Now: write+bind once per pass, the inner loop only writes the
        // genuinely per-draw HdVsCb and HdPsCb.
        if (rs_.GetShadowService() &&
            rs_.Pipeline().impl_->blsHdShadowCb_ != gfx::BufferHandle::Invalid) {
            if (auto sc = bls::ScopedCb<bls::HdShadowCascadesCb>(
                    rs_.Pipeline().Gfx(), rs_.Pipeline().impl_->blsHdShadowCb_)) {
                rs_.GetShadowService()->FillVsCb(*sc);
            }
            cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 1,
                                    rs_.Pipeline().impl_->blsHdShadowCb_);
        }
        if (rs_.Pipeline().impl_->blsHdShadowCountCb_ != gfx::BufferHandle::Invalid) {
            if (auto cnt = bls::ScopedCb<bls::SdOnHdShadowCascadeCountCb>(
                    rs_.Pipeline().Gfx(), rs_.Pipeline().impl_->blsHdShadowCountCb_)) {
                const i32 n = (rs_.GetShadowService() && rs_.GetShadowService()->IsEnabled())
                                  ? rs_.GetShadowService()->cascadeCount()
                                  : 0;
                const u32 bits = static_cast<u32>(n);
                std::memcpy(&cnt->numCascades, &bits, sizeof(f32));
                cnt->_pad[0] = cnt->_pad[1] = cnt->_pad[2] = 0.0f;
            }
            cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 1,
                                    rs_.Pipeline().impl_->blsHdShadowCountCb_);
        }
        if (rs_.Pipeline().impl_->blsHdDebugVisCb_ != gfx::BufferHandle::Invalid) {
            if (auto dbg = bls::ScopedCb<bls::DebugVisCb>(
                    rs_.Pipeline().Gfx(), rs_.Pipeline().impl_->blsHdDebugVisCb_)) {
                const i32 dbgMode = rs_.Settings().HdDebugMode();
                u32 enabled = 0;
                i32 psMode = dbgMode;
                Vector3f overrideA = {0, 0, 0};
                Vector3f overrideO = {0, 0, 0};
                if (dbgMode >= 5 && dbgMode <= 7) {
                    enabled = 1;
                    psMode = 0;
                    overrideA = (dbgMode == 5)   ? Vector3f{1, 1, 1}
                                : (dbgMode == 6) ? Vector3f{0.5f, 0.5f, 0.5f}
                                                 : Vector3f{0, 0, 0};
                } else if (dbgMode == 8) {
                    // ORM stub from texture_asset_manager (NeutralOrm 0x0000FFFFu):
                    // AO=1, Roughness=1, Metallic=0. Shader reads .yz for
                    // (roughness, metalness); .x feeds crystal refractMask.
                    enabled = 2;
                    psMode = 0;
                    overrideO = {1.0f, 1.0f, 0.0f};
                } else if (dbgMode == 9) {
                    // "AO Only" — BLS state stays at default; the GTAO
                    // apply pass overwrites hdrColor with the AO factor
                    // (see GtaoService::SetDebugAoOnly below).
                    psMode = 0;
                }
                dbg->enabledShaders = enabled;
                const u32 modeBits = static_cast<u32>(psMode);
                std::memcpy(&dbg->debugMode, &modeBits, sizeof(f32));
                dbg->_p0[0] = dbg->_p0[1] = 0.0f;
                dbg->overrideAlbedo = overrideA;
                dbg->_p1 = 0.0f;
                dbg->overrideOrm = overrideO;
                dbg->_p2 = 0.0f;
            }
            cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 3,
                                    rs_.Pipeline().impl_->blsHdDebugVisCb_);
        }
    }

    bls::BaselineLights Baseline(const Matrix44f& view) const {

        if (auto* dnc = rs_.GetDncService();
            dnc && dnc->HasAsset() && rs_.Settings().GetLightingMode() == LightingMode::InGame) {
            const auto sample = dnc->SampleNow();
            if (sample.valid) {
                // Front-biased sun (matches the shadow pass + SD baseline);
                // keep the DNC day/night ambient+diffuse colour.
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
            .ambient = {kHdBaselineAmbientColor.x, kHdBaselineAmbientColor.y,
                        kHdBaselineAmbientColor.z},
            .diffuse = {kHdBaselineLightColor.x, kHdBaselineLightColor.y, kHdBaselineLightColor.z},
            .dirToSourceVS = {0.0f, 0.0f, -1.0f},
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

    // The HD submission, kept intact (HD/SD-on-HD/Crystal program selection,
    // fresnel, normal/ORM/emissive + team-colour maps, shadows, MRT G-buffer,
    // and the per-layer fading-opaque depth prepass) — now fed one geoset at a
    // time by RunLists instead of the legacy bucket loop.
    void EmitLayersHd(const render_detail::RenderableView& view_, const GPUGeoset& geo,
                      bls::FrameInputs& frame, const Matrix44f& viewMat, gfx::IGFXCommandList* cmd,
                      const bls::LightingContext& lighting) {
        const i32 lightCountForGeoset = owner_->SelectLights(
            frame, lighting, viewMat, render_detail::GeosetCentroidWS(view_, geo));

        const GPUMaterial* mat = nullptr;
        const i32 matId = geo.materialId;
        if (matId >= 0 && matId < (i32)view_.materials->size())
            mat = &(*view_.materials)[matId];

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
        if (hasBones) {
            const u32 boneSlot = hasTangents ? 2u : 1u;
            cmd->BindVertexBuffer(boneSlot, geo.boneVb, sizeof(BoneVertex));
            cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 3, paletteCb);
        }

        struct LayerJob {
            render_detail::UnpackedLayer layer;
            bls::MatParams mp;
            const bls::BlsProgram* program = nullptr;
            bls::GxShaderID programShaderId = bls::GxShaderID::SD_on_HD;
            i32 activeN = 0;
            f32 combinedAlpha = 1.0f;
            bool unlit = false;
            bool isOpaqueFading = false;
            bool valid = false;
        };
        std::vector<LayerJob> jobs(numLayers);

        for (i32 li = 0; li < numLayers; ++li) {
            jobs[li].layer = render_detail::UnpackLayer(mat, li);
            const auto& layer = jobs[li].layer;
            f32 combinedAlpha = geoAlpha * layer.alpha;
            if (combinedAlpha < 0.004f)
                continue;
            const bool isOpaqueFading =
                combinedAlpha < 0.99f && layer.filterMode <= FILTER_TRANSPARENT;
            i32 effectiveFilter = layer.filterMode;
            if (isOpaqueFading)
                effectiveFilter = FILTER_BLEND;

            const bool isCrystalMaterial =
                (layer.shaderId == 24) && rs_.Pipeline().impl_->blsCrystalProgram_ != nullptr;
            const bool isHdMaterial =
                isCrystalMaterial || layer.shaderId == 1 || layer.shaderId == 24;
            const bls::GxShaderID programShaderId = isCrystalMaterial ? bls::GxShaderID::Crystal
                                                    : isHdMaterial    ? bls::GxShaderID::HD
                                                                      : bls::GxShaderID::SD_on_HD;
            const bls::BlsProgram* program =
                isCrystalMaterial ? rs_.Pipeline().impl_->blsCrystalProgram_
                : isHdMaterial    ? rs_.Pipeline().impl_->blsHdProgram_
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

            const bool unlit = (mp.disables & bls::kDisableLighting) != 0;
            const i32 activeN = unlit ? 0 : lightCountForGeoset;

            jobs[li].mp = mp;
            jobs[li].program = program;
            jobs[li].programShaderId = programShaderId;
            jobs[li].activeN = activeN;
            jobs[li].combinedAlpha = combinedAlpha;
            jobs[li].unlit = unlit;
            jobs[li].isOpaqueFading = isOpaqueFading;
            jobs[li].valid = true;
        }

        auto issueHdDraw = [&](const LayerJob& job, const bls::MatParams& matParams, i32 layerIndex,
                               bool prepassTwin) {
            const auto& layer = job.layer;
            const bool unlit = job.unlit;
            const i32 activeN = job.activeN;
            const bls::GxShaderID programShaderId = job.programShaderId;
            const bls::BlsProgram* program = job.program;
            frame.numLights = activeN;
            render_detail::ApplyTexAnimPaletteToFrame(frame, view_.texAnimPalette,
                                                      layer.textureAnimationId);
            {
                bls::RenderState rs;
                rs.shaderId = programShaderId;
                rs.alphaMode = static_cast<u8>(matParams.alpha);
                rs.numColors = 0;
                rs.numTexCoords = 1;

                rs.numTangents = hasTangents ? 1 : 0;

                rs.numWeights = hasBones ? 4 : 0;
                rs.numLights = static_cast<u8>(activeN);
                rs.fogEnabled = false;
                rs.depthWrite = matParams.DepthWriteEnabled();
                rs.lightingEnabled = !unlit && activeN > 0;
                rs.prepass = false;
                rs.shadows = rs_.GetShadowService() && rs_.GetShadowService()->IsEnabled();

                rs.teamColor =
                    (layer.teamColorMapId == kHdTeamColorActive) || (layer.teamColorMapId >= 0);
                const i32 dbgMode = rs_.Settings().HdDebugMode();
                const bool debugActive = (dbgMode > 0);
                rs.debugShader = debugActive;
                auto perm = bls::SelectPermutes(rs);

                bls::PsoRequest req{};
                req.program = program;
                req.vsIndex = perm.vs;
                req.psIndex = perm.ps;
                req.material = matParams;

                if (hasBones) {
                    req.layout = hasTangents ? bls::VertexLayoutKind::MeshHDSkinned
                                             : bls::VertexLayoutKind::MeshHDSkinnedNoTangent;
                } else {
                    req.layout = hasTangents ? bls::VertexLayoutKind::MeshHDTangent
                                             : bls::VertexLayoutKind::ParticleSD;
                }
                req.topology = gfx::PrimitiveTopology::TriangleList;
                req.rtvFormat = RenderPipeline::kHdrSceneFormat;
                // Match the HD G-buffer render pass: slot 1 = linear
                // depth, slot 2 = encoded normal. Picked up by the
                // backend's multi-RTV PSO build path. The HD opaque
                // shader's WC3_IS_MRT permutation writes SV_Target1/2,
                // so enable extra-slot writes (transparent / SD / line
                // PSOs leave the flag at the default `false`).
                req.extraRtvFormats[0] = RenderPipeline::kLinearDepthFormat;
                req.extraRtvFormats[1] = RenderPipeline::kNormalBufferFormat;
                req.extraRtvCount = 2;
                // Only the colour-writing, depth-writing materials
                // (= the WC3_IS_MRT permutation) actually emit
                // SV_Target1/2. Depth-prepass materials have
                // DepthWriteEnabled=true but ColorWriteEnabled=false —
                // their shader outputs nothing at all and WebGPU
                // rejects a PSO whose extras have writeMask != 0 with
                // no matching fragment output.
                req.extraColorWrite =
                    matParams.DepthWriteEnabled() && matParams.ColorWriteEnabled();
                req.dsvFormat = rs_.Pipeline().impl_->depthStencilFormat_;
                req.lhClipSpace = true;
                auto pso = rs_.Pipeline().impl_->blsPsoBuilder_->GetOrBuild(req);
                if (pso == gfx::PipelineHandle::Invalid)
                    return;
                cmd->BindPipeline(pso);

                cmd->BindVertexBuffer(0, render_detail::PickSlot0Vb(geo, layer.coordId),
                                      sizeof(Vertex));

                frame.world = view_.worldTransform;

                if (auto vs = bls::ScopedCb<bls::HdVsCb>(rs_.Pipeline().Gfx(),
                                                         rs_.Pipeline().impl_->blsHdVsCb_)) {
                    bls::BuildHdVsCb(*vs, frame, matParams);
                }
                cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 2,
                                        rs_.Pipeline().impl_->blsHdVsCb_);

                // Pass-constant CBs (HdShadowCascadesCb @ VS slot 1,
                // SdOnHdShadowCascadeCountCb @ PS slot 1, DebugVisCb @
                // PS slot 3) are bound once in BindPassResources — they
                // don't vary per draw.

                if (program == rs_.Pipeline().impl_->blsHdProgram_ ||
                    program == rs_.Pipeline().impl_->blsCrystalProgram_) {
                    if (auto ps = bls::ScopedCb<bls::HdPsCb>(rs_.Pipeline().Gfx(),
                                                             rs_.Pipeline().impl_->blsHdPsCb_)) {
                        bls::BuildHdPsCb(*ps, frame, matParams);
                    }
                    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 2,
                                            rs_.Pipeline().impl_->blsHdPsCb_);
                } else {
                    if (auto ps = bls::ScopedCb<bls::SdOnHdPsCb>(
                            rs_.Pipeline().Gfx(), rs_.Pipeline().impl_->blsSdOnHdPsCb_)) {
                        bls::BuildSdOnHdPsCb(*ps, frame, matParams);
                    }
                    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 2,
                                            rs_.Pipeline().impl_->blsSdOnHdPsCb_);
                }

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
        d.lightCount = static_cast<u8>(job.activeN);
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
        if (job.program == rs_.Pipeline().impl_->blsHdProgram_ ||
            job.program == rs_.Pipeline().impl_->blsCrystalProgram_) {
            bls::HdPsCb ps{};
            bls::BuildHdPsCb(ps, frame, matParams);
            h = debug::TraceHashBytes(&ps, sizeof(ps), h);
        } else {
            bls::SdOnHdPsCb ps{};
            bls::BuildSdOnHdPsCb(ps, frame, matParams);
            h = debug::TraceHashBytes(&ps, sizeof(ps), h);
        }
        d.cbHash = h;
        debug::RecordGeosetDraw(d, view_, geo, layer, layerIndex, frame);
    }
};

} // namespace whiteout::flakes::renderer
