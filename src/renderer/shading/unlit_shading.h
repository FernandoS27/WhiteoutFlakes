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
#include "core/vertex_layout.h"
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

    // Mirrors `UnlitLightData` — a separate buffer at slot 1 rather than more
    // fields on UnlitCb, so the flat permutation's layout does not move.
    struct alignas(16) UnlitLightCb {
        Vector4f lightDirWS;
        Vector4f lightColor;
        Vector4f ambient;
        Vector4f specular; // .w = Blinn-Phong exponent
        Vector4f cameraPosWS;
    };

    // Attachment formats, plus the two axes a baked vertex buffer introduces:
    // which interned layout describes the geometry (and at what stride), and
    // how the active profile lights it. `lighting` is the *resolved* choice —
    // a geoset with no NORMAL is keyed Flat, so the fallback is a property of
    // the key rather than a branch at bind time.
    struct PsoKey {
        gfx::Format rtv = gfx::Format::Unknown;
        gfx::Format dsv = gfx::Format::Unknown;
        u32 extraRtvCount = 0;
        gfx::Format extra0 = gfx::Format::Unknown;
        gfx::Format extra1 = gfx::Format::Unknown;
        gfx::Format extra2 = gfx::Format::Unknown;
        u32 layoutId = core::VertexLayoutCache::kWc3Interleaved;
        u32 stride = 0;
        core::UnlitLightingModel lighting = core::UnlitLightingModel::Flat;
        /// @brief Pose the geometry from a bone palette.
        ///
        /// Part of the key rather than a bind-time branch for the same reason
        /// `lighting` is: it changes the vertex shader *and* the declared input
        /// elements, so it has to select a PSO.
        bool skinned = false;

        bool operator==(const PsoKey&) const = default;
    };
    struct PsoEntry {
        PsoKey key;
        gfx::PipelineHandle pso = gfx::PipelineHandle::Invalid;
    };

    gfx::PipelineHandle GetOrBuildPso(const PsoKey& key);

    /// @brief The lighting model this geoset actually gets: the profile's
    ///        choice, downgraded to Flat when the geometry has no normal.
    core::UnlitLightingModel ResolveLighting(const render_detail::RenderableView& view,
                                             const model::GPUGeoset& geo);

    /// @brief Whether this geoset can be posed: it has a palette to bind and a
    ///        buffer that carries the skinning attributes itself.
    ///
    /// Both halves matter. A geoset whose weights live in a separate stream
    /// (the MDX path) is not skinned *by this model* — that stream is not bound
    /// here — and one with no palette has nothing to pose against.
    bool ResolveSkinned(const render_detail::RenderableView& view,
                        const model::GPUGeoset& geo) const;

    RenderService& rs_;
    bool initTried_ = false;
    gfx::ShaderHandle vs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle ps_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle vsLit_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle vsSkinned_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle vsSkinnedLit_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle psLambert_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle psBlinnPhong_ = gfx::ShaderHandle::Invalid;
    gfx::BufferHandle cb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle lightCb_ = gfx::BufferHandle::Invalid;
    std::vector<PsoEntry> psos_;

    // Captured in BeginPass, read by Draw. The view and projection are
    // per-pass, not per-draw; the world transform is per-item and written into
    // the CB inside Draw.
    Matrix44f passView_ = Matrix44f::identity();
    Matrix44f passProj_ = Matrix44f::identity();
    Vector3f passCameraPos_ = {0.0f, 0.0f, 0.0f};
    core::UnlitLightingModel passLighting_ = core::UnlitLightingModel::Flat;

    // Actors already warned about a missing NORMAL, so the message is once
    // per model rather than once per geoset per frame.
    std::vector<u32> warnedNoNormal_;
};

} // namespace whiteout::flakes::renderer::shading
