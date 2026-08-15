#pragma once

// ============================================================================
// M2CombinerShading — the WoW `.m2` surface path.
//
// One draw per `.skin` batch. The batch names its shaders (m2_shader_select.h)
// and its material names its state (m2_material.h); both were resolved into
// M2SurfaceTable at load, so this file is binding and nothing else.
//
// Owns its shaders, constant buffers and PSO cache rather than going through
// BlsPsoBuilder, for the reason unlit_shading.h states: `bls::PsoRequest`
// requires a `const BlsProgram*`, engine shaders never enter a `.bls`, and
// producing a bundle needs WDX_BUILD_WC3_SHADERS — mutually exclusive with the
// WDX_USE_PREBUILT_SHADERS that CI and every fresh clone build with.
// ============================================================================

#include "core/surface_vocabulary.h"
#include "core/vertex_layout.h"
#include "gfx/gfx.h"
#include "m2_material.h"
#include "m2_shader_select.h"
#include "m2_surface_table.h"
#include "shading/shading_model.h"
#include "whiteout/flakes/types.h"

#include <array>
#include <map>

namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes::renderer::profiles::wow {

class M2CombinerShading final : public shading::IShadingModel {
public:
    explicit M2CombinerShading(RenderService& rs) : rs_(rs) {}
    ~M2CombinerShading() override;

    M2CombinerShading(const M2CombinerShading&) = delete;
    M2CombinerShading& operator=(const M2CombinerShading&) = delete;

    /// @brief Create the 51 shaders and the two constant buffers. Idempotent,
    ///        and a no-op before the device exists.
    void Init();

    /// @brief Destroy every GPU object while the device is still alive — the
    ///        same ordered teardown UnlitShading documents.
    void ReleaseGpu();

    core::ShadingModelId Id() const override {
        return core::ShadingModelId::M2Combiners;
    }
    bool IsAvailable() const override;

    bool BeginPass(const core::PassContext& ctx,
                   const render_detail::CollectedDrawLists& lists) override;
    void Draw(const render_detail::DrawItem& item, const core::PassContext& ctx) override;

    core::SurfaceClass Classify(const render_detail::RenderableView& view,
                                const model::GPUGeoset& geo) const override;
    core::SurfaceClass ClassifySurface(const render_detail::RenderableView& view,
                                       const model::GPUGeoset& geo, u32 surface) const override;

    core::VertexNeeds Needs(u32 surface) const override;

    i32 SelectLights(bls::FrameInputs& frame, const bls::LightingContext& lighting,
                     const Matrix44f& viewMat, const Vector3f& surfaceWS) const override;

    core::EmitMask Emits(u32 surface, core::PassSlot pass) const override;

    std::span<const core::SurfaceParamDecl> Params() const override {
        return {};
    }

private:
    // Mirror `M2PassData` / `M2DrawData` in m2_combiners.slang. Matrices are
    // uploaded transposed, matching the row-vector convention every shader here
    // uses.
    struct alignas(16) M2PassCb {
        Matrix44f view;
        Matrix44f projection;
        Vector4f cameraPosWS;
        Vector4f fogParams;
        Vector4f fogColor;
        Vector4f sunDirWS;
        Vector4f sunColor;
        Vector4f ambient;
    };

    struct alignas(16) M2DrawCb {
        Matrix44f world;
        Matrix44f texMtx0;
        Matrix44f texMtx1;
        Vector4f elementColor;
        Vector4f unitWeights;
        Vector4f params; // .x alphaRef, .y fogMode, .z lit, .w unused
    };

    // Everything baked into a pipeline. `blend` and `materialFlags` are here
    // rather than the derived states themselves because they are what *decides*
    // those states — keying on the inputs keeps the key small and makes two
    // batches with the same material share a PSO.
    struct PsoKey {
        gfx::Format rtv = gfx::Format::Unknown;
        gfx::Format dsv = gfx::Format::Unknown;
        gfx::Format extra0 = gfx::Format::Unknown;
        gfx::Format extra1 = gfx::Format::Unknown;
        u32 extraRtvCount = 0;
        u32 layoutId = core::VertexLayoutCache::kWc3Interleaved;
        u32 stride = 0;
        u8 vsIndex = 0;
        u8 psIndex = 0;
        u8 blend = 0;
        u16 materialFlags = 0;
        bool mirrored = false;

        auto operator<=>(const PsoKey&) const = default;
    };

    gfx::PipelineHandle GetOrBuildPso(const PsoKey& key);

    /// @brief The table for @p view, or null when the actor carries one built
    ///        by another product — which a mixed scene has plenty of.
    static const M2SurfaceTable* TableOf(const render_detail::RenderableView& view);

    RenderService& rs_;
    bool initTried_ = false;

    std::array<gfx::ShaderHandle, static_cast<usize>(M2VertexShader::Count)> vs_{};
    std::array<gfx::ShaderHandle, static_cast<usize>(M2PixelShader::Count)> ps_{};
    gfx::BufferHandle passCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle drawCb_ = gfx::BufferHandle::Invalid;

    // Ordered rather than hashed: the key is comparable by construction and a
    // model's batch count keeps this in the low hundreds, where the log factor
    // is free and a hash collision cannot bind the wrong pipeline.
    std::map<PsoKey, gfx::PipelineHandle> psos_;

    Matrix44f passView_ = Matrix44f::identity();
    Matrix44f passProj_ = Matrix44f::identity();
    Vector3f passCameraPos_ = {0.0f, 0.0f, 0.0f};
};

} // namespace whiteout::flakes::renderer::profiles::wow
