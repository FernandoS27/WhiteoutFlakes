#pragma once

// ============================================================================
// D3StandardShading — the Diablo III material system's IShadingModel.
//
// The host half of d3_standard.slang: six resolved slots out of D3SurfaceTable,
// Blinn-Phong against `MaterialColors`, one key directional and three point
// lights in the forward pass. Shaped on M3StandardShading (own engine shaders,
// own CBs, own PSO cache) minus the G-buffer sidecar — a gamma-LDR frame has
// no deferred pass to feed.
//
// Like M3's and M2's, deliberately NOT a BLS program: engine shaders never
// enter a `.bls` container, and producing a bundle needs WDX_BUILD_WC3_SHADERS
// — mutually exclusive with the WDX_USE_PREBUILT_SHADERS CI builds with.
// ============================================================================

#include "core/surface_vocabulary.h"
#include "core/vertex_layout.h"
#include "gfx/gfx.h"
#include "renderer/profiles/diablo3/d3_surface_table.h"
#include "shading/shading_model.h"
#include "whiteout/flakes/types.h"

#include <map>

namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes::renderer::profiles::diablo3 {

class D3StandardShading final : public shading::IShadingModel {
public:
    explicit D3StandardShading(RenderService& rs) : rs_(rs) {}
    ~D3StandardShading() override;

    D3StandardShading(const D3StandardShading&) = delete;
    D3StandardShading& operator=(const D3StandardShading&) = delete;

    /// @brief Create the shaders and constant buffers. Idempotent; safe to
    ///        call before the device exists (no-ops, IsAvailable stays false).
    void Init();

    /// @brief Destroy every GPU object while the device is still alive —
    ///        called from RenderPipeline::CleanupGFX, never the destructor.
    void ReleaseGpu();

    core::ShadingModelId Id() const override {
        return core::ShadingModelId::D3Standard;
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
        // Empty in v1. `MaterialTextureEntry.tAnimU/V/Rotate` — three
        // {amount, rate0, rate1} triples per entry — are what lands here first.
        return {};
    }

private:
    static constexpr u32 kSlotCount = kD3SlotCount;
    static constexpr u32 kPointLights = 3;

    // Mirrors of d3_standard.slang's constant buffers, uploaded transposed.
    struct alignas(16) D3PassCb {
        Matrix44f view;
        Matrix44f projection;
        Vector4f cameraPosWS;
        Vector4f keyLightDir;
        Vector4f keyLightDiffuse;
        Vector4f keyLightSpecular;
        Vector4f ambient;
        Vector4f pointPos[kPointLights];
        Vector4f pointColor[kPointLights];
        Vector4f pointAtten[kPointLights];
    };
    struct alignas(16) D3DrawCb {
        Matrix44f world;
        Vector4f params0; // .x alphaRef, .y shininess, .z twoSided
        Vector4f matDiffuse;
        Vector4f matSpecular;
        Vector4f matEmissive;
        Vector4f matAmbient;
        Matrix44f slotUv[kSlotCount];
        u32 slotCtl[kSlotCount][4];
    };

    struct PsoKey {
        gfx::Format rtv = gfx::Format::Unknown;
        gfx::Format dsv = gfx::Format::Unknown;
        gfx::Format extra0 = gfx::Format::Unknown;
        gfx::Format extra1 = gfx::Format::Unknown;
        gfx::Format extra2 = gfx::Format::Unknown;
        u32 extraRtvCount = 0;
        u32 layoutId = 0;
        u32 stride = 0;
        /// 0 opaque, 1 alpha blend. D3 has no per-material blend *table*: the
        /// original swaps the whole shader between an opaque and a translucent
        /// `Shaders` asset, and those are the two states that survive.
        u8 blend = 0;
        bool skinned = false;
        bool twoSided = false;

        auto operator<=>(const PsoKey&) const = default;
    };

    gfx::PipelineHandle GetOrBuildPso(const PsoKey& key);

    /// @brief Whether this geoset draws through the skinned entry.
    ///
    /// Unlike `.m3`, D3's weights ride the separate `BoneVertex` stream — its
    /// influence bone indices are GLOBAL skeleton indices and the loader's
    /// per-geoset palette remap is what turns them into palette slots — so the
    /// test is the stream's presence, exactly as M2's is.
    bool ResolveSkinned(const render_detail::RenderableView& view,
                        const model::GPUGeoset& geo, gfx::BufferHandle& outPalette) const;

    static const D3SurfaceTable* TableOf(const render_detail::RenderableView& view);

    RenderService& rs_;
    bool initTried_ = false;
    gfx::ShaderHandle vs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle vsSkinned_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle ps_ = gfx::ShaderHandle::Invalid;
    gfx::BufferHandle passCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle drawCb_ = gfx::BufferHandle::Invalid;
    std::map<PsoKey, gfx::PipelineHandle> psos_;

    // Captured in BeginPass, read by Draw.
    Matrix44f passView_ = Matrix44f::identity();
    Matrix44f passProj_ = Matrix44f::identity();
    Vector3f passCameraPos_ = {0.0f, 0.0f, 0.0f};
};

} // namespace whiteout::flakes::renderer::profiles::diablo3
