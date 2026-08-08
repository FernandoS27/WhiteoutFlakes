#pragma once

#include "render_detail.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_pipeline_impl.h"
#include "renderer/render_service.h"
#include "whiteout/flakes/types.h"

#include <cstdio>
#include "bls/bls_draw_helpers.h"
#include "bls/bls_frame.h"
#include "renderer/assets/sampler_asset_manager.h"

namespace whiteout::flakes::renderer {

enum class GeosetBucket : u8 { All = 0, Opaque = 1, Transparent = 2 };

template <class Derived>
class BlsGeosetPass {
public:
    explicit BlsGeosetPass(RenderService& rs, GeosetBucket bucket = GeosetBucket::All) noexcept
        : rs_(rs), bucket_(bucket) {}

    // List-driven driver: classify + sort once (WC3 per-layer opaque / whole-
    // geoset transparent), then submit the bucket's items via the Derived
    // DrawOpaqueItem / DrawTransparentItem. Lighting is built per item by the
    // submission. Replaces Run()'s per-geoset bucket filter.
    bool RunLists() {
        Derived& d = self();
        if (!d.IsAvailable())
            return false;
        if (rs_.Scene().Actors().All().empty())
            return true;

        auto* cmd = rs_.Pipeline().Gfx()->GetImmediateContext();
        const Vector3f camPos = rs_.Pipeline().FrameCamera().GetSource();
        auto collected = render_detail::BuildDrawLists(
            rs_.Scene().Actors().All(), rs_.Pipeline().ComputeSelectedLod(), camPos);
        if (collected.lists.opaque.empty() && collected.lists.transparent.empty())
            return true;

        Matrix44f view, proj;
        d.ComputeViewProj(view, proj);

        bls::FrameInputs frame;
        frame.view = view;
        frame.projection = proj;
        frame.effectTime = rs_.Scene().GetAnimationTime() * 0.001f;
        frame.numLights = 0;
        frame.viewportRect = {(f32)rs_.Pipeline().Width(), (f32)rs_.Pipeline().Height(), 0.0f,
                              0.0f};

        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, rs_.Samplers().LinearWrap());
        d.BindPassResources(cmd, frame);

        const bls::LightingContext lighting = MakeLightingContext(collected, view);

        if (bucket_ != GeosetBucket::Transparent)
            for (const auto& item : collected.lists.opaque)
                d.DrawOpaqueItem(item, frame, view, cmd, lighting);
        if (bucket_ != GeosetBucket::Opaque)
            for (const auto& item : collected.lists.transparent)
                d.DrawTransparentItem(item, frame, view, cmd, lighting);
        return true;
    }

    // Set up the pass and hand back its state (collected lists, frame, view,
    // baseline) for the unified transparent orchestrator, which interleaves
    // DrawTransparentItem with other producers by depth. Pass-global binds
    // (sampler, BindPassResources) happen here, once. Returns false if the pass
    // isn't available or there are no actors.
    bool PrepareInterleaved(render_detail::CollectedDrawLists& outCollected,
                            bls::FrameInputs& outFrame, Matrix44f& outView,
                            bls::LightingContext& outLighting) {
        Derived& d = self();
        if (!d.IsAvailable() || rs_.Scene().Actors().All().empty())
            return false;

        auto* cmd = rs_.Pipeline().Gfx()->GetImmediateContext();
        const Vector3f camPos = rs_.Pipeline().FrameCamera().GetSource();
        outCollected = render_detail::BuildDrawLists(
            rs_.Scene().Actors().All(), rs_.Pipeline().ComputeSelectedLod(), camPos);

        Matrix44f proj;
        d.ComputeViewProj(outView, proj);
        outFrame.view = outView;
        outFrame.projection = proj;
        outFrame.effectTime = rs_.Scene().GetAnimationTime() * 0.001f;
        outFrame.numLights = 0;
        outFrame.viewportRect = {(f32)rs_.Pipeline().Width(), (f32)rs_.Pipeline().Height(), 0.0f,
                                 0.0f};

        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, rs_.Samplers().LinearWrap());
        d.BindPassResources(cmd, outFrame);
        outLighting = MakeLightingContext(outCollected, outView);
        return true;
    }

protected:
    RenderService& rs_;
    GeosetBucket bucket_;

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
    Derived& self() {
        return *static_cast<Derived*>(this);
    }
    const Derived& self() const {
        return *static_cast<const Derived*>(this);
    }
};

} // namespace whiteout::flakes::renderer
