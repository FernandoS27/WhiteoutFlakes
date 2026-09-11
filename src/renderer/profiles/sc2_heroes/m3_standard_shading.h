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
namespace whiteout::flakes::model {
struct Actor;
}
namespace whiteout::flakes::renderer::bls {
struct FrameInputs;
}

namespace whiteout::flakes::renderer::profiles::sc2_heroes {

struct M3Surface;
class M3SurfaceTable;

/// Which producer a @ref M3StandardShading::DrawWorldVertices call is drawing.
///
/// Nothing about the pipeline state, the constant buffers or the layer stack
/// depends on it — a ribbon strip and a particle batch are the same geometry
/// by the time they reach here, world-space `renderer::Vertex` triangles the
/// CPU has already billboarded — so this exists ONLY so the draw trace can
/// tell the two apart. The producer field of the trace comes from the submit
/// context; this is the marker inside the PSO key hash.
enum class M3WorldVertexKind : u8 { Ribbon, Particle };

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

    /// @brief Draw one world-space vertex range through the M3 material at
    ///        `surfaceIndex`.
    ///
    /// Two producers reach it. A ribbon strip (RIBBON_SERVICE.md §7) lives in
    /// the actor's `render.ribbonVB`; an SC2 `PAR_` batch lives in the shared
    /// particle service VB. Both hand over the same thing — a `renderer::Vertex`
    /// range already in world space, already billboarded on the CPU — so both
    /// take the same VS, the same full material PS and the same cull-none
    /// state, and `vb` is what separates them.
    ///
    /// Interleaved in the transparent pass by RenderPipeline, not by the
    /// draw-item loop, so it writes its own pass CB rather than leaning on
    /// BeginPass. `emitterId` is the trace's, −1 for a ribbon.
    void DrawWorldVertices(model::Actor& actor, i32 surfaceIndex, gfx::BufferHandle vb,
                           i32 vertexOffset, i32 vertexCount, const bls::FrameInputs& frame,
                           M3WorldVertexKind kind, i32 emitterId = -1);

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
        // p_m<L>UVTransform's two live rows: .xy linear, .z translation.
        // Resolved per draw out of FrameState::texAnimMatrices, so two actors
        // sharing a template scroll independently.
        Vector4f layerUvRow0[kLayerCount];
        Vector4f layerUvRow1[kLayerCount];
        // CalcFresnelTerm — .xyz p_v<L>FresnelExponentBiasScale + .w the mode,
        // then the transform's mask and translation with their two flag bits
        // in .w. See M3Layer's fresnel fields.
        Vector4f layerFresnel[kLayerCount];
        Vector4f layerFresnelMask[kLayerCount];
        Vector4f layerFresnelTrans[kLayerCount];
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

    /// Fill a pass CB (view/proj/camera + the key/fill/back rig + ambient +
    /// team params) the one way, so the ribbon's own CB carries the SAME
    /// lighting a geoset gets — otherwise a lit ribbon shades against zero
    /// lights and comes out black.
    void WritePassCb(M3PassCb& c, const Matrix44f& view, const Matrix44f& proj,
                     const Vector3f& camPos, bool linearShading);

    /// The world-vertex PSO — its own vertex layout (renderer::Vertex),
    /// forward entry only, blend from the surface. No skinned/MRT variants,
    /// and no @ref M3WorldVertexKind in it: ribbons and particles that agree
    /// on formats and blend agree on the pipeline state too, so they share
    /// the entry rather than doubling the cache.
    struct WorldVertexPsoKey {
        gfx::Format rtv = gfx::Format::Unknown;
        gfx::Format dsv = gfx::Format::Unknown;
        gfx::Format extra0 = gfx::Format::Unknown;
        gfx::Format extra1 = gfx::Format::Unknown;
        gfx::Format extra2 = gfx::Format::Unknown;
        u32 extraRtvCount = 0;
        u8 blend = 0; ///< m3::BlendMode.
        bool twoSided = false;
        auto operator<=>(const WorldVertexPsoKey&) const = default;
    };
    gfx::PipelineHandle GetOrBuildWorldVertexPso(const WorldVertexPsoKey& key);

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
    /// `vsM3Ribbon`. Named for the shader entry, which keeps the ribbon name
    /// it was written under; the particle batch draws through it too.
    gfx::ShaderHandle vsWorld_ = gfx::ShaderHandle::Invalid;
    gfx::BufferHandle passCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle drawCb_ = gfx::BufferHandle::Invalid;
    // The world-vertex pass CB is its own: DrawWorldVertices runs in the
    // transparent interleave after BeginPass' draws, so rewriting passCb_
    // would corrupt a later geoset draw's lights. Only view/projection/
    // cameraPos are needed.
    gfx::BufferHandle worldPassCb_ = gfx::BufferHandle::Invalid;
    std::map<PsoKey, gfx::PipelineHandle> psos_;
    std::map<WorldVertexPsoKey, gfx::PipelineHandle> worldVertexPsos_;

    // Captured in BeginPass, read by Draw.
    Matrix44f passView_ = Matrix44f::identity();
    Matrix44f passProj_ = Matrix44f::identity();
    Vector3f passCameraPos_ = {0.0f, 0.0f, 0.0f};
    core::PassSlot passSlot_ = core::PassSlot::OpaqueColor;
};

} // namespace whiteout::flakes::renderer::profiles::sc2_heroes
