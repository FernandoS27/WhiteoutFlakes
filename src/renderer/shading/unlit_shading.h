#pragma once

// ============================================================================
// UnlitShading — the second IShadingModel, and the one that proves the seam.
//
// Position-only vertex stream, flat white, no lighting, no textures, no
// surface table. Two jobs:
//
//   1. It is what P9/P10 render M2 and M3 through before either format has
//      materials, so "the model loads" is a visible, gate-able claim rather
//      than a parse test.
//   2. It gives SurfacePass a second model to switch between. Until now the
//      registry held two entries but exactly one was ever live per frame, so
//      the tried/open transition was never taken twice and the `key.model`
//      sort term was a no-op. A seam nothing exercises is a seam nobody knows
//      is broken.
//
// Deliberately NOT a BLS program. Engine shaders never enter a `.bls`
// container, `bls::PsoRequest` requires a `const BlsProgram*`, and producing a
// new bundle needs WDX_BUILD_WC3_SHADERS — mutually exclusive with the
// WDX_USE_PREBUILT_SHADERS that CI and every fresh clone use. So this owns its
// own shaders, its own constant buffer and its own small PSO cache, and adds no
// GxShaderID value.
// ============================================================================

#include "core/surface_vocabulary.h"
#include "gfx/gfx.h"
#include "shading/shading_model.h"
#include "whiteout/flakes/types.h"

#include <vector>

namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes::renderer::shading {

class UnlitShading final : public IShadingModel {
public:
    explicit UnlitShading(RenderService& rs) : rs_(rs) {}
    ~UnlitShading() override;

    UnlitShading(const UnlitShading&) = delete;
    UnlitShading& operator=(const UnlitShading&) = delete;

    /// @brief Create the shaders and the constant buffer. Idempotent; safe to
    ///        call before the device exists (it no-ops and IsAvailable stays
    ///        false). Releases nothing — see ReleaseGpu.
    void Init();

    /// @brief Destroy every GPU object while the device is still alive. Must
    ///        run before the device goes away: the PSO cache and the CB are
    ///        released through it, and a destructor-time release would
    ///        dereference a dead device — the same teardown-ordering bug the
    ///        post-process services shipped with.
    void ReleaseGpu();

    core::ShadingModelId Id() const override {
        return core::ShadingModelId::Unlit;
    }
    bool IsAvailable() const override;

    bool BeginPass(const core::PassContext& ctx,
                   const render_detail::CollectedDrawLists& lists) override;
    void Draw(const render_detail::DrawItem& item, const core::PassContext& ctx) override;

    core::SurfaceClass Classify(const render_detail::RenderableView& view,
                                const model::GPUGeoset& geo) const override;

    core::VertexNeeds Needs(u32 surface) const override;

    i32 SelectLights(bls::FrameInputs& frame, const bls::LightingContext& lighting,
                     const Matrix44f& viewMat, const Vector3f& surfaceWS) const override;

    core::EmitMask Emits(u32 surface, core::PassSlot pass) const override;

    std::span<const core::SurfaceParamDecl> Params() const override {
        return {};
    }

private:
    // Mirrors `UnlitData` in unlit.slang. Uploaded transposed, matching the
    // row-vector convention every other shader here uses.
    struct alignas(16) UnlitCb {
        Matrix44f world;
        Matrix44f view;
        Matrix44f projection;
    };

    // A PSO varies only with the attachment formats it renders into — there is
    // no material, no permutation and no light count to key on. SD and HD are
    // therefore two entries, plus one more per distinct swap-chain format a
    // Metal-backed target reports.
    struct PsoKey {
        gfx::Format rtv = gfx::Format::Unknown;
        gfx::Format dsv = gfx::Format::Unknown;
        u32 extraRtvCount = 0;
        gfx::Format extra0 = gfx::Format::Unknown;
        gfx::Format extra1 = gfx::Format::Unknown;

        bool operator==(const PsoKey&) const = default;
    };
    struct PsoEntry {
        PsoKey key;
        gfx::PipelineHandle pso = gfx::PipelineHandle::Invalid;
    };

    gfx::PipelineHandle GetOrBuildPso(const PsoKey& key);

    RenderService& rs_;
    bool initTried_ = false;
    gfx::ShaderHandle vs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle ps_ = gfx::ShaderHandle::Invalid;
    gfx::BufferHandle cb_ = gfx::BufferHandle::Invalid;
    std::vector<PsoEntry> psos_;

    // Captured in BeginPass, read by Draw. The view and projection are
    // per-pass, not per-draw; the world transform is per-item and written into
    // the CB inside Draw.
    Matrix44f passView_ = Matrix44f::identity();
    Matrix44f passProj_ = Matrix44f::identity();
};

} // namespace whiteout::flakes::renderer::shading
