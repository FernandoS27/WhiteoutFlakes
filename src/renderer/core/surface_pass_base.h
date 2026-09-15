#pragma once

#include "core/render_detail.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_pipeline_impl.h"
#include "renderer/render_service.h"
#include "whiteout/flakes/types.h"

#include <cstdio>
#include "bls/bls_draw_helpers.h"
#include "bls/bls_frame.h"
#include "renderer/assets/sampler_asset_manager.h"
#include "renderer/debug/draw_trace.h"

namespace whiteout::flakes::renderer::shading {
class IShadingModel;
}

namespace whiteout::flakes::renderer {

enum class GeosetBucket : u8 { All = 0, Opaque = 1, Transparent = 2 };

template <class Derived>
class BlsGeosetPass {
public:
    explicit BlsGeosetPass(RenderService& rs) noexcept : rs_(rs) {}

    // The pass-global half of what RunLists / PrepareInterleaved used to do:
    // compute view/proj, seed FrameInputs, bind the pass's samplers and
    // resources. The submission loop that used to follow is now
    // shading::SurfacePass, which dispatches through IShadingModel — so this is
    // reached via Wc3Shading::BeginPass rather than driving the frame itself.
    void OpenPass(const render_detail::CollectedDrawLists& lists, bls::FrameInputs& outFrame,
                  Matrix44f& outView, bls::LightingContext& outLighting) {
        Derived& d = self();
        auto* cmd = rs_.Pipeline().Gfx()->GetImmediateContext();

        Matrix44f proj;
        d.ComputeViewProj(outView, proj);
        outFrame = bls::FrameInputs{};
        outFrame.view = outView;
        outFrame.projection = proj;
        outFrame.effectTime = rs_.Scene().GetAnimationTime() * 0.001f;
        outFrame.fog = render_detail::FogParamsFrom(rs_.Settings().GetWorldFog());
        outFrame.numLights = 0;
        outFrame.viewportRect = {(f32)rs_.Pipeline().Width(), (f32)rs_.Pipeline().Height(), 0.0f,
                                 0.0f};

        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, rs_.Samplers().LinearWrap());
        // The lighting context first: the HD pass packs the scene's lights into
        // its per-pass buffers while it binds them.
        outLighting = MakeLightingContext(lists, outView);
        d.BindPassResources(cmd, outFrame, outLighting);
    }

    // `collected` must outlive the returned context — it holds the light list
    // by pointer, and the collected lists already live for the whole pass.
    bls::LightingContext MakeLightingContext(const render_detail::CollectedDrawLists& collected,
                                             const Matrix44f& view) {
        bls::LightingContext ctx;
        ctx.sceneLights = &collected.sceneLights;
        ctx.baseline = self().Baseline(view);
        ctx.mode = rs_.Settings().GetLightingMode();
        return ctx;
    }

    // The IShadingModel that owns this pass. EmitLayers routes light selection
    // through it rather than calling bls::BuildLightPalette directly, so the
    // per-model budget/algorithm/stage seam (§7) is genuinely exercised while
    // only WC3 implements it.
    void SetOwner(shading::IShadingModel* owner) {
        owner_ = owner;
    }

protected:
    RenderService& rs_;
    // Which bucket to submit is SurfacePass's decision now, not the pass's —
    // it is a property of the pass slot, not of the shading model.
    shading::IShadingModel* owner_ = nullptr;

    Derived& self() {
        return *static_cast<Derived*>(this);
    }
    const Derived& self() const {
        return *static_cast<const Derived*>(this);
    }
};

} // namespace whiteout::flakes::renderer
