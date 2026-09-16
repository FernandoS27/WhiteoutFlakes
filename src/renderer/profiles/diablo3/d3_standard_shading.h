#pragma once

// ============================================================================
// D3StandardShading — the Diablo III material system's IShadingModel.
//
// The host half of d3_standard.slang: nine resolved slots out of
// D3SurfaceTable and the original's own light rig — `colAmbient` plus a light
// array and one cylindrical light, each laid out as
// Render_UploadLightConstants writes it. Shaped on M3StandardShading (own
// engine shaders, own CBs, own PSO cache) minus the G-buffer sidecar — a
// gamma-LDR frame has no deferred pass to feed.
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

/// @brief One blend factor as a RenderPass stores it, which is the ENGINE's
///        enum and NOT D3DBLEND.
///
/// The Windows 2.8.x build translates it on the way to `SetRenderState` —
/// `sub_73DAD0`, a bare switch — and the translation is not the identity:
///
///     1 -> ZERO   2 -> ONE   3 -> SRCCOLOR   4 -> INVSRCCOLOR
///     5 -> SRCALPHA   6 -> INVSRCALPHA
///     7 -> DESTCOLOR(9)   8 -> INVDESTCOLOR(10)
///     9 -> DESTALPHA(7)  10 -> INVDESTALPHA(8)
///    11 -> BLENDFACTOR(14), with D3DRS_BLENDFACTOR pinned to 0x00FFFFFF
///
/// so 7/8 and 9/10 are the opposite way round from D3DBLEND, and 11 is the
/// CONSTANT, not SrcAlphaSat. Reading them as D3DBLEND is what drew 3,175 of
/// the corpus's 21,593 particle systems as opaque black rectangles: the
/// premultiplied family pairs `src = 11` with `dst = SRCALPHA`, and mapping 11
/// to SrcAlpha makes both factors the source alpha, so every texel the sprite
/// meant to leave alone resolves to `C*0 + dst*0`.
///
/// The constant has no per-channel equivalent in this gfx layer and needs none:
/// 0x00FFFFFF is exactly One for the colour and Zero for the alpha, so @p alpha
/// picks the channel and the answer is exact.
///
/// Over 1,831 shipped passes the source takes only {1, 2, 5, 9, 11} and the
/// destination {1, 2, 5, 6, 9, 10}; the op is ADD on every one. Pairs are led
/// by (5, 6) on 1,029 passes, (5, 2) on 283 and (11, 5) on 55.
///
/// Shared with the particle path, which reads the same field off the same
/// struct — see d3_particle_shading.h.
gfx::BlendFactor D3BlendFactor(u32 engine, gfx::BlendFactor fallback, bool alpha = false);

/// @brief A RenderPass's depth or alpha compare, D3DCMPFUNC, as a gfx op.
///
/// `sub_73DBB0` maps the engine's value onto D3DCMPFUNC unchanged except that 0
/// means Always, so this is the D3D9 table plus that. NotEqual has no value in
/// this gfx layer and falls to Always; one corpus pass asks for it. A compare of
/// 8 is not really a compare at all — `sub_73DA60` turns it into ZENABLE 0 —
/// which callers handle by dropping the test, not by passing it here.
gfx::CompareOp D3CompareOp(u32 func);

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
    /// The pass-global dye ramp. It binds to PS register t15, not the slot
    /// immediately past the ten-slot table (t10): the WebGPU bind-group layout
    /// reserves PS registers t10..t14 for shadow maps (Depth) and IBL cube
    /// arrays, and a plain 2D texture bound there fails Dawn's sampleType check
    /// and invalidates the whole D3 PSO. t15 is the last Float / 2D PS slot.
    /// Must match texDyeRamp's register in d3_standard.slang.
    static constexpr u32 kDyeRampRegister = 15;

    /// @brief Slots in the generic light array.
    ///
    /// The original budgets 16 per type across five types and clamps there, but
    /// nothing shipped comes close: over the corpus's 1,466 distinct vertex
    /// programs **796 carry exactly one point-light block and none carries
    /// two**, and the runtime's own uniform names give it away — `lightSpots[0]`
    /// and `lightPointLinears[0]` keep their index while `lightDirectionals`,
    /// `lightPoints` and `lightCylindricals` lose theirs, which is what an
    /// array of one collapses to. So the forward budget is one point (a
    /// directional demotes into it) plus one cylindrical; four leaves room for
    /// a rig to add fill without another CB layout.
    static constexpr u32 kLights = 4;

    // Mirrors of d3_standard.slang's constant buffers, uploaded transposed.
    //
    // The light block is the original's, field for field:
    // Render_UploadLightConstants writes a point light as five float4
    // {pos, 1}, {k0,k1,k2, 1}, ambient, diffuse, specular — and a DIRECTIONAL
    // that overflows its own array is re-homed into the point array with its
    // direction negated and `.w` 0, attenuation (1, 0, 0). One evaluation
    // covers both, which is why there is no separate directional array here.
    struct alignas(16) D3PassCb {
        Matrix44f view;
        Matrix44f projection;
        Vector4f cameraPosWS;
        /// Constant 21. Not one authored value: the engine starts it from a
        /// scene base and ADDS every directional light's own ambient onto it.
        Vector4f colAmbient;
        /// Constant 39, `.x`. A GLOBAL in the original, not a material field —
        /// which agrees with `flShininess` being 0.0 on 99.4% of the corpus.
        Vector4f specularPower;
        Vector4f lightPos[kLights];
        Vector4f lightAtten[kLights];
        Vector4f lightAmbient[kLights];
        Vector4f lightDiffuse[kLights];
        Vector4f lightSpecular[kLights];
        /// The cylindrical light: six float4 in the original, of which the
        /// shipped programs read five. `cylRange` is {1/max(end-start, 0.001),
        /// end} — the engine computes exactly that reciprocal.
        Vector4f cylPos;
        Vector4f cylAxis;
        Vector4f cylRange;
        Vector4f cylAmbient;
        Vector4f cylDiffuse;
    };
    struct alignas(16) D3DrawCb {
        Matrix44f world;
        Vector4f params0; // .x alphaRef, .y shininess, .z twoSided, .w vertex-colour mode
        /// The fixed-function chain's output gain, `.x` colour and `.y` alpha.
        /// See D3PassState::colorGain.
        Vector4f params1;
        /// `.x` the pass's depth bias, applied in the vertex shader because
        /// D3D9 states it in depth units and the modern APIs do not. `.y` the
        /// two-tex distortion switch. `.z` the dye ramp row (`tintRampUV`,
        /// `((dye-2)+0.5)/21`) and `.w` `bUseDyeType` — the geoset's dye, off
        /// unless a D3 outfit dyed it. See D3PassState::depthBias.
        Vector4f params2;
        /// The `Legacy.fx` chain: `.x` live stage count (0 = run the named
        /// slots), `.y` where the vertex colour and the texture factor enter,
        /// `.z` `TAG_VS_EDGEALPHA`, `.w` the facing term's exponent.
        Vector4f params3;
        /// D3DTA_TFACTOR. `(1, 1, 1, elementAlpha)` — see D3PassState's note on
        /// where the original gets it, which is the DRAW and not the pass.
        Vector4f factor;
        Vector4f matDiffuse;
        Vector4f matSpecular;
        Vector4f matEmissive;
        Vector4f matAmbient;
        Matrix44f slotUv[kSlotCount];
        u32 slotCtl[kSlotCount][4];
        /// A chain stage's own gain and clamp: `.x/.y` the MODULATE2X/4X gain
        /// per channel, `.z/.w` non-zero to saturate that channel HERE.
        Vector4f slotGain[kSlotCount];
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
        /// 0 opaque, 1 blended. The factors come with it: the RenderPass
        /// states them (in its OWN enum, see D3BlendFactor) and they are not one
        /// pair — (5, 6) alpha and (5, 2) additive are both common in shipped
        /// content.
        u8 blend = 0;
        u32 blendSrc = 5;
        u32 blendDst = 6;
        bool depthWrite = true;
        /// The pass's depth compare, D3DCMPFUNC. 8 is Always, which the engine
        /// spells as no depth test at all — 150 passes, the `_pma` family among
        /// them.
        u32 depthFunc = 4;
        bool skinned = false;
        bool twoSided = false;
        /// The pass's two colour-write flags. See D3PassState::colorWrite.
        bool colorWrite = true;
        bool alphaWrite = true;
        /// 0 solid, 1 wireframe. See D3PassState::fillMode.
        u32 fillMode = 0;
        /// The debug entry in place of the material's.
        bool debug = false;

        auto operator<=>(const PsoKey&) const = default;
    };

    gfx::PipelineHandle GetOrBuildPso(const PsoKey& key);

    /// @brief The `dye_ramp` core texture, acquired once per storage through
    ///        the asset manager (by file id where the storage answers one, by
    ///        path for a plain tree). White until it lands, which degrades a
    ///        dye toward "no recolour" rather than to black.
    gfx::TextureHandle DyeRampTexture();
    u32 rampSlot_ = 0; ///< AssetManager::kInvalidSlot until first asked.

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
    gfx::ShaderHandle psDebug_ = gfx::ShaderHandle::Invalid;
    // The mesh overlay's vertex stages (shaders/d3_overlay.slang).
    gfx::ShaderHandle vsOverlay_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle vsOverlaySkinned_ = gfx::ShaderHandle::Invalid;
    /// DebugViewData at b3, written per draw only while a debug view is on.
    gfx::BufferHandle debugCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle passCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle drawCb_ = gfx::BufferHandle::Invalid;
    std::map<PsoKey, gfx::PipelineHandle> psos_;

    // Captured in BeginPass, read by Draw.
    Matrix44f passView_ = Matrix44f::identity();
    Matrix44f passProj_ = Matrix44f::identity();
    Vector3f passCameraPos_ = {0.0f, 0.0f, 0.0f};
    core::DebugFrame passDebug_;
    core::DebugTargetInfo passDebugTarget_;
};

} // namespace whiteout::flakes::renderer::profiles::diablo3
