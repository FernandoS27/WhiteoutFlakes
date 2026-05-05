#pragma once

#include "common_types.h"
#include "renderer/render_service.h"
#include "render_service_internal.h"
#include "renderer/sampler_asset_manager.h"
#include "bls/bls_draw_helpers.h"
#include "bls/bls_frame.h"

#include <mutex>

namespace WhiteoutDex {

enum class GeosetBucket : u8 { All = 0, Opaque = 1, Transparent = 2 };

template <class Derived>
class BlsGeosetPass {
public:
    explicit BlsGeosetPass(RenderService& rs, GeosetBucket bucket = GeosetBucket::All) noexcept
        : rs_(rs), bucket_(bucket) {}

    bool Run() {
        Derived&  d  = self();
        if (!d.IsAvailable()) return false;
        if (rs_.scene_->Actors().All().empty()) return true;

        auto* cmd = rs_.gfx_->GetImmediateContext();

        auto collected = render_detail::CollectSortedRenderables(
            rs_.scene_->Actors().All(), rs_.ComputeSelectedLod());
        if (collected.refs.empty()) return true;

        Matrix44f view, proj;
        d.ComputeViewProj(view, proj);

        bls::FrameInputs frame;
        frame.view         = view;
        frame.projection   = proj;
        frame.effectTime   = rs_.scene_->GetAnimationTime() * 0.001f;
        frame.numLights    = 0;
        frame.viewportRect = { (f32)rs_.width_, (f32)rs_.height_, 0.0f, 0.0f };

        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, rs_.samplers_->LinearWrap());
        d.BindPassResources(cmd, frame);

        const bls::BaselineLights baseline = d.Baseline(view);

        for (auto& ref : collected.refs) {

            if (bucket_ == GeosetBucket::Opaque       && ref.renderOrder >  1) continue;
            if (bucket_ == GeosetBucket::Transparent  && ref.renderOrder <= 1) continue;

            const auto& view_  = *ref.view;
            const auto& geo    = (*view_.geosets)[ref.idx];
            if (geo.unskinnedVb == gfx::BufferHandle::Invalid ||
                geo.ib == gfx::BufferHandle::Invalid ||
                geo.indexCount == 0) continue;

            const i32 lightCount = bls::BuildLightPalette(
                frame, *view_.activeLights, view, baseline,
                rs_.GetLightingMode());

            d.DrawGeoset(ref, frame, view, cmd, lightCount);
        }
        return true;
    }

protected:
    RenderService& rs_;
    GeosetBucket   bucket_;
    Derived&       self()       { return *static_cast<Derived*>(this); }
    const Derived& self() const { return *static_cast<const Derived*>(this); }
};

}
