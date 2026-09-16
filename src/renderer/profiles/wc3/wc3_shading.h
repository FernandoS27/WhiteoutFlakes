#pragma once

// Wc3Shading — the runtime-polymorphic face of the two WC3 geoset passes.
//
// The CRTP pass bodies (GeosetPassBls / GeosetPassHd) are kept intact inside:
// they already own PSO selection, texture and CB binds, and the draw call,
// which is exactly IShadingModel's job. What this adds is the vtable, the
// per-pass state that used to be locals in RunLists, and the SelectLights
// forward.

#include "profiles/wc3/wc3_classify.h"
#include "core/surface_vocabulary.h"
#include "profiles/wc3/geoset_pass_bls.h"
#include "profiles/wc3/geoset_pass_hd.h"
#include "shading/shading_model.h"

namespace whiteout::flakes::renderer::profiles::wc3 {

template <class Pass, core::ShadingModelId kId>
class Wc3Shading final : public shading::IShadingModel {
public:
    explicit Wc3Shading(RenderService& rs) : rs_(rs), pass_(rs) {
        pass_.SetOwner(this);
    }

    core::ShadingModelId Id() const override {
        return kId;
    }

    bool IsAvailable() const override {
        return pass_.IsAvailable();
    }

    bool BeginPass(const core::PassContext& ctx,
                   const render_detail::CollectedDrawLists& lists) override {
        if (!pass_.IsAvailable())
            return false;
        pass_.SetDebug(ctx.debug, ctx.debugTarget);
        pass_.OpenPass(lists, frame_, view_, lighting_);
        return true;
    }

    void Draw(const render_detail::DrawItem& item, const core::PassContext& ctx) override {
        auto* cmd = rs_.Pipeline().Gfx()->GetImmediateContext();
        pass_.SetPassSlot(ctx.pass);
        // The opaque and transparent entry points differ only in the depth-fill
        // mode they pass down; the pass slot is what selects between them.
        if (ctx.pass == core::PassSlot::OpaqueColor)
            pass_.DrawOpaqueItem(item, frame_, view_, cmd, lighting_);
        else
            pass_.DrawTransparentItem(item, frame_, view_, cmd, lighting_);
    }

    core::SurfaceClass Classify(const render_detail::RenderableView& view,
                                const model::GPUGeoset& geo) const override {
        // Both WC3 models share one rule body — this template is instantiated
        // for each, so they cannot drift apart.
        const GeosetClass gc = ClassifyGeoset(view, geo);
        return {.visible = gc.visible,
                .blend = gc.opaque ? core::BlendClass::Opaque : core::BlendClass::Transparent,
                .needsDepthFill = gc.needsDepthFill};
    }

    core::VertexNeeds Needs(u32 surface) const override {
        (void)surface;
        // WC3's Vertex is fully interleaved and its two UV sets are baked into
        // two complete vertex-buffer copies, so it requests none of the
        // standalone streams. De-interleaving it is explicitly out of scope.
        return {};
    }

    i32 SelectLights(bls::FrameInputs& frame, const bls::LightingContext& lighting,
                     const Matrix44f& viewMat, const Vector3f& surfaceWS) const override {
        return bls::BuildLightPalette(frame, lighting, viewMat, surfaceWS);
    }

    core::EmitMask Emits(u32 surface, core::PassSlot pass) const override {
        (void)surface;
        switch (pass) {
        case core::PassSlot::ShadowMap:
        case core::PassSlot::DepthPrepass:
            return core::EmitMask::Depth;
        case core::PassSlot::GBuffer:
            // The HD opaque MRT permutation forward-shades and writes linear
            // depth + normal in the same draw.
            return core::EmitMask::DefaultColor | core::EmitMask::LinearDepth |
                   core::EmitMask::Normal;
        default:
            return core::EmitMask::DefaultColor;
        }
    }

    std::span<const core::SurfaceParamDecl> Params() const override {
        return {};
    }

private:
    RenderService& rs_;
    Pass pass_;
    // Per-pass state, captured by BeginPass and read by every Draw until
    // EndPass. `lighting_` points into the caller's CollectedDrawLists, which
    // is why BeginPass documents that the lists must outlive the pass.
    bls::FrameInputs frame_;
    Matrix44f view_ = Matrix44f::identity();
    bls::LightingContext lighting_;
};

using Wc3SdShading = Wc3Shading<GeosetPassBls, core::ShadingModelId::Wc3Sd>;
using Wc3HdShading = Wc3Shading<GeosetPassHd, core::ShadingModelId::Wc3Hd>;

} // namespace whiteout::flakes::renderer::profiles::wc3
