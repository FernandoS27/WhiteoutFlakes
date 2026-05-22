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

    bool Run() {
        Derived& d = self();
        if (!d.IsAvailable())
            return false;
        if (rs_.Scene().Actors().All().empty())
            return true;

        auto* cmd = rs_.Pipeline().Gfx()->GetImmediateContext();

        auto collected = render_detail::CollectSortedRenderables(
            rs_.Scene().Actors().All(), rs_.Pipeline().ComputeSelectedLod());
        if (collected.refs.empty())
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

        const bls::BaselineLights baseline = d.Baseline(view);

        for (auto& ref : collected.refs) {

            if (bucket_ == GeosetBucket::Opaque && ref.renderOrder > 1)
                continue;
            if (bucket_ == GeosetBucket::Transparent && ref.renderOrder <= 1)
                continue;

            const auto& view_ = *ref.view;
            // Skip skinned actors whose bone palette hasn't been
            // populated yet (frame_ticker's UpdateAnimation gates its
            // upload on SkinningSystem::IsReady). Drawing here would
            // bind a zero-initialised bone palette and emit degenerate
            // skinned geometry.
            if (view_.skinning && view_.skinning->HasSkeleton() && !view_.skinning->IsReady())
                continue;
            const auto& geo = (*view_.geosets)[ref.idx];
            if (geo.unskinnedVb == gfx::BufferHandle::Invalid ||
                geo.ib == gfx::BufferHandle::Invalid || geo.indexCount == 0)
                continue;

            const i32 lightCount = bls::BuildLightPalette(
                frame, *view_.activeLights, view, baseline, rs_.Settings().GetLightingMode());

            d.DrawGeoset(ref, frame, view, cmd, lightCount);
        }
        return true;
    }

protected:
    RenderService& rs_;
    GeosetBucket bucket_;
    Derived& self() {
        return *static_cast<Derived*>(this);
    }
    const Derived& self() const {
        return *static_cast<const Derived*>(this);
    }
};

} // namespace whiteout::flakes::renderer
