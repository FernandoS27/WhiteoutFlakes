#pragma once

// ============================================================================
// IShadingModel — the surface axis's binding interface (design §3(C)).
//
// One implementation per (product, shading path) pair. It owns everything that
// is a property of *how a surface is shaded*: PSO selection, texture and
// constant-buffer binds, the draw call, visibility classification, vertex-stream
// requirements, and light selection. The core owns only bucketing and ordering.
//
// `BlsGeosetPass<Derived>` was already this shape, expressed as CRTP. The change
// here is making it runtime-polymorphic — the product is a runtime choice — and
// moving the submission loop out into SurfacePass, which dispatches each item to
// whichever model it names.
// ============================================================================

#include "core/render_detail.h"
#include "core/surface_vocabulary.h"
#include "renderer/bls/bls_draw_helpers.h"
#include "renderer/bls/bls_frame.h"
#include "whiteout/flakes/types.h"

#include <span>

namespace whiteout::flakes::renderer::shading {

class IShadingModel {
public:
    virtual ~IShadingModel() = default;

    virtual core::ShadingModelId Id() const = 0;

    /// @brief Are this model's shaders resolved? A model that answers false is
    ///        skipped wholesale rather than drawing wrong.
    virtual bool IsAvailable() const = 0;

    /// @brief Once per (pass × model). Binds pass-global CBs, samplers and
    ///        probes, and captures the per-pass state the draws need. Must be
    ///        cheap and self-checking: the transparent queue can re-enter it.
    ///        Return false to skip every draw of this model in this pass.
    ///
    ///        `lists` must outlive the pass — the model keeps a pointer into
    ///        its `sceneLights`. The design wrote this as `BeginPass(ctx)`
    ///        alone; the light list has to arrive somehow and threading it
    ///        through PassContext would put a core-owned collection type into a
    ///        struct that is otherwise pure values.
    virtual bool BeginPass(const core::PassContext& ctx,
                           const render_detail::CollectedDrawLists& lists) = 0;

    virtual void EndPass(const core::PassContext& ctx) {
        (void)ctx;
    }

    /// @brief One draw: PSO selection, binds, and the draw call.
    virtual void Draw(const render_detail::DrawItem& item, const core::PassContext& ctx) = 0;

    /// @brief Visibility and blend class for this surface at its current
    ///        animated state. Not cacheable — WC3 classification moves with the
    ///        timeline. P3b makes this the sole authority; today BuildDrawLists
    ///        still calls the free function and this forwards to it.
    virtual core::SurfaceClass Classify(const render_detail::RenderableView& view,
                                        const model::GPUGeoset& geo) const = 0;

    /// @brief Classification for one surface of a multi-surface geoset. Only a
    ///        model whose geosets carry a `surfaceCount` is ever asked; the
    ///        default forwards, so a model whose blend class is a property of
    ///        the whole geoset — every WC3 one — implements nothing.
    ///
    ///        Additive rather than a new parameter on `Classify` because M2 is
    ///        the first format where the answer differs per batch, and widening
    ///        the signature would mean touching three models to change one.
    virtual core::SurfaceClass ClassifySurface(const render_detail::RenderableView& view,
                                               const model::GPUGeoset& geo, u32 surface) const {
        (void)surface;
        return Classify(view, geo);
    }

    /// @brief Vertex streams this surface requires. WC3 asks for none of the
    ///        optional ones — its `Vertex` is interleaved and stays that way.
    virtual core::VertexNeeds Needs(u32 surface) const = 0;

    /// @brief Fill `frame`'s light palette for a surface at `surfaceWS` and
    ///        return how many lights landed.
    ///
    ///        This is a model method, never a core helper, because the light
    ///        budget, the selection algorithm *and the target shader stage* are
    ///        all per-model: WC3 takes 8 (SD→VS, HD→PS), WoW 3 point lights in
    ///        VS constants with sun and ambient in PS constants, SC2 3
    ///        directional + 3 point + 7 spot. Landing it now, while only WC3
    ///        implements it, is what avoids changing three models at once later
    ///        with no byte-identical gate available.
    virtual i32 SelectLights(bls::FrameInputs& frame, const bls::LightingContext& lighting,
                             const Matrix44f& viewMat, const Vector3f& surfaceWS) const = 0;

    /// @brief Which attachments this model writes for `surface` in `pass`.
    ///        The profile uses it to pick the RTV set and validate a pass's
    ///        declared writes; the model folds it into the PSO key alongside
    ///        the pass itself.
    ///
    ///        We already carry a one-bit version of this: RenderState::depthWrite
    ///        doubles as the WC3_IS_MRT permutation axis (bls_permuter.h), which
    ///        is precisely "does this shader emit the extra G-buffer
    ///        attachments" smuggled in as a depth-state flag. This generalises a
    ///        hack rather than adding a concept — and it has to exist from here,
    ///        because retrofitting an output signature means revisiting every
    ///        shading model and every pass a second time.
    virtual core::EmitMask Emits(u32 surface, core::PassSlot pass) const = 0;

    /// @brief Animatable parameters this model exposes. Empty until the first
    ///        animated format; the `surfaceParams` blob that consumes it is
    ///        deferred with the animation work.
    virtual std::span<const core::SurfaceParamDecl> Params() const = 0;
};

} // namespace whiteout::flakes::renderer::shading
