#pragma once

// ============================================================================
// M3StandardShading — the simple M3 material system's IShadingModel.
//
// The host half of m3_standard.slang (M3_SIMPLE_MATERIAL_DESIGN.md §5): six
// resolved layers out of M3SurfaceTable, Blinn-Phong against one key light in
// the forward pass, and the packed G-buffer sidecar when the pass binds it.
// Shaped on UnlitShading (own engine shaders, own CBs, own PSO cache, the
// per-geoset palette path) plus M2CombinerShading's surface-table use.
//
// Like both of those, deliberately NOT a BLS program: engine shaders never
// enter a `.bls` container, and producing a bundle needs WDX_BUILD_WC3_SHADERS
// — mutually exclusive with the WDX_USE_PREBUILT_SHADERS CI builds with.
// ============================================================================

#include "core/surface_vocabulary.h"
#include "core/vertex_layout.h"
#include "gfx/gfx.h"
#include "shading/shading_model.h"
#include "whiteout/flakes/types.h"

#include <map>

namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes::renderer::profiles::sc2_heroes {

struct M3Surface;
class M3SurfaceTable;

class M3StandardShading final : public shading::IShadingModel {
public:
    explicit M3StandardShading(RenderService& rs) : rs_(rs) {}
    ~M3StandardShading() override;

    M3StandardShading(const M3StandardShading&) = delete;
    M3StandardShading& operator=(const M3StandardShading&) = delete;

    /// @brief Create the shaders and constant buffers. Idempotent; safe to
    ///        call before the device exists (no-ops, IsAvailable stays false).
    void Init();

    /// @brief Destroy every GPU object while the device is still alive —
    ///        called from RenderPipeline::CleanupGFX, never the destructor.
    void ReleaseGpu();

    core::ShadingModelId Id() const override {
        return core::ShadingModelId::M3Standard;
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
    static constexpr u32 kLayerCount = 11;
    /// Which PS texture register each layer slot binds to. The first nine are
    /// the free 2D window t0..t8 in slot order; the environment MASK takes
    /// t9, the last free 2D slot in the shared SRV layout; and the
    /// environment map itself takes t13, which that layout already types as a
    /// cube array for the WC3 HD IBL probe. Sharing t13 is deliberate — the
    /// layout is a slot-TYPE map, not an ownership map, and the two passes
    /// never draw in the same frame — but it does mean t13's binding is a
    /// cube everywhere, which is why the environment slot binds
    /// Defaults::BlackCube rather than Defaults::White when it is off.
    static constexpr u32 kLayerRegister[kLayerCount] = {0, 1, 2, 3,  4, 5,
                                                        6, 7, 8, 13, 9};

    // Mirrors of m3_standard.slang's constant buffers, uploaded transposed.
    struct alignas(16) M3PassCb {
        Matrix44f view;
        Matrix44f projection;
        Vector4f cameraPosWS;
        Vector4f keyLightDir;      // .xyz direction the light travels
        Vector4f keyLightDiffuse;  // .rgb colour x multiplier
        Vector4f keyLightSpecular; // .rgb spec colour x multiplier (x rig HDR spec)
        Vector4f ambient;          // .rgb; .w rig emissive multiplier
        Vector4f fillLightDir;
        Vector4f fillLightDiffuse;
        Vector4f backLightDir;
        Vector4f backLightDiffuse;
        Vector4f teamParams; // .xy p_fTeamColorIntensity
    };
    struct alignas(16) M3DrawCb {
        Matrix44f world;
        Vector4f params0;     // .x alphaRef, .y unshaded, .z spec exp, .w flags
        Vector4f uvTransform; // .x mul (SNORM-folded), .y add, .z emis mult
        Vector4f teamDiffuse;
        Vector4f teamEmissive;
        Vector4f layerTint[kLayerCount];
        Vector4f layerAdd[kLayerCount]; // .x = rgbAdd (an HLSL cbuffer array
                                        // strides by 16 whatever the element)
        u32 layerCtl[kLayerCount][4];   // x uvSet | wrap | invert | clamp,
                                        // y channels, z mode,
                                        // w blendOp / diffuse team mode
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
        u8 blend = 0; ///< m3::BlendMode.
        /// The MRT entry point + extraColorWrite: on exactly when the pass is
        /// the G-buffer one AND the sidecar attachment is bound (count == 3).
        bool mrt = false;
        bool skinned = false;
        /// m3::MaterialFlag::TwoSided — the only raster state that varies per
        /// surface, so it has to key the PSO rather than ride the draw CB.
        bool twoSided = false;

        auto operator<=>(const PsoKey&) const = default;
    };

    gfx::PipelineHandle GetOrBuildPso(const PsoKey& key);

    /// @brief UnlitShading's rules verbatim: a per-geoset palette, skinning
    ///        attributes in the interleaved buffer, no per-actor palette.
    bool ResolveSkinned(const render_detail::RenderableView& view,
                        const model::GPUGeoset& geo) const;

    static const M3SurfaceTable* TableOf(const render_detail::RenderableView& view);

    RenderService& rs_;
    bool initTried_ = false;
    gfx::ShaderHandle vs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle vsSkinned_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle ps_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle psMrt_ = gfx::ShaderHandle::Invalid;
    gfx::BufferHandle passCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle drawCb_ = gfx::BufferHandle::Invalid;
    std::map<PsoKey, gfx::PipelineHandle> psos_;

    // Captured in BeginPass, read by Draw.
    Matrix44f passView_ = Matrix44f::identity();
    Matrix44f passProj_ = Matrix44f::identity();
    Vector3f passCameraPos_ = {0.0f, 0.0f, 0.0f};
    core::PassSlot passSlot_ = core::PassSlot::OpaqueColor;
};

} // namespace whiteout::flakes::renderer::profiles::sc2_heroes
