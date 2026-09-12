#pragma once

// ============================================================================
// M3SurfaceTable — one entry per emitted region, six layers resolved.
//
// The simple material system's table (M3_SIMPLE_MATERIAL_DESIGN.md §3): the
// region's first batch names a MATM entry, MATM dispatches to a typed material
// array, and the six supported StandardMaterial layers land here with their
// texture ids, channel selects and bind-pose tints resolved. Composite
// materials resolve to their highest-weight standard section — HotS uses them
// heavily and drawing the dominant section beats drawing white. Everything
// else stays `valid = false` and keeps the unlit fallback.
//
// Indexed by GEOSET id, not MATM id, because the per-region UV transform
// (REGN v5+ uvScale/uvOffset) has to live somewhere material-keyed entries
// cannot carry. The emitted-region list comes from the adapter so the skip
// filter is never derived twice.
//
// Nothing animated lives here (core/surface_table.h states the split): tints
// are the bind-pose values, and a layer's UV transform is named — not stored —
// by `M3Layer::uvTransformId`, which indexes the per-frame
// `FrameState::texAnimMatrices` palette the source fills.
// ============================================================================

#include "core/surface_table.h"
#include "core/surface_vocabulary.h"
#include "whiteout/flakes/types.h"

#include <whiteout/models/m3/m3.h>

#include <memory>
#include <span>
#include <vector>

namespace whiteout::flakes::renderer::profiles::sc2_heroes {

/// @brief Layer slots, matching io::M3LayerSlot and the shader's layerCtl.
inline constexpr u32 kM3LayerCount = 11;
/// The two ordinals the renderer treats specially — the normal layer switches
/// itself off until its texture lands, and the environment layer is the one
/// cube. Mirrors of io::M3LayerSlot, which m3_model_adapter.h owns; pinned
/// against it by static_assert in m3_surface_table.cpp.
inline constexpr u32 kM3LayerSpecular = 2;
inline constexpr u32 kM3LayerNormal = 5;
inline constexpr u32 kM3LayerEnvironment = 9;

struct M3Layer {
    i32 textureId = -1; ///< Index into the adapter's CollectM3Textures order.
    u8 uvSource = 0;    ///< 0 = UV set 0, 1 = UV set 1.
    /// `b_iUVMapping[slot] == UVMAP_PARTICLE_FLIPBOOK` (`Particle.fx:131`).
    /// The mapping mode is a whole enum and @ref uvSource keeps only the two
    /// explicit-set values out of it, so this is the one other value anything
    /// reads — a particle's flipbook cell walk is a UV MAPPING mode on the
    /// material, not a flag on the `PAR_`. The emitter's own flipbook fields
    /// say how many cells and when; this says whether they are used at all.
    bool particleFlipbookUv = false;
    u8 channels = 0;    ///< Raw ColorChannelSelect.
    u8 mode = 0;        ///< 0 off, 1 texture, 2 solid colour.
    u8 blendOp = 0;     ///< Raw LayerBlendOp; decal and emissive slots only.
    /// TEAMCOLOR_DIFFUSE (psmateriallayer.fx): 1 on the diffuse layer when its
    /// channel select is not RGB — the diffuse alpha is the team mask. The
    /// engine keys this on "the instance has a team colour", which the viewer
    /// always does.
    u8 teamColorMode = 0;
    u32 wrapFlags = 0x3;
    /// Bind-pose colour x rgbMultiply; .w = alpha. The specular tint also
    /// folds hdrSpecularMultiplier and the energy-conserving dim; the emissive
    /// multiplier stays OUT of the tint (see M3Surface::emissiveMultiplier —
    /// retail scales the additive emissive sum, team-colour adds included).
    Vector4f tint = {1.0f, 1.0f, 1.0f, 1.0f};
    /// rgbAdd — the `+ add` half of psmateriallayer.fx's
    /// `cResult.rgba = cResult.rgba * multiply + add`, applied to rgb AND
    /// alpha. Carries whatever extra multiplier the tint carries, so the pair
    /// still reads `(texel * rgbMultiply + rgbAdd) * extra`.
    f32 add = 0.0f;
    /// ColorInvert (0x10) / ColorClamp (0x20) — `1 - c` before the multiply,
    /// `saturate` after it. Clamp is what decides whether a layer whose
    /// multiply-add lifts it past 1 stays past 1.
    u8 invert = 0;
    u8 clampColor = 0;
    /// `FresnelMode`: 0 off, 1 standard (edge glow), 2 inverted (centre glow).
    /// 26463 of the corpus's 2862196 standard layers carry one, 14036 mode 1
    /// and 12427 mode 2, and they sit mostly on emissive1 (11196) and the
    /// alpha masks (5513) — this is how a hero's rim light is authored.
    u8 fresnelMode = 0;
    /// bit 0 = `TextureLayerFlag::FresnelTransform`, bit 1 = `FresnelNormalize`.
    u8 fresnelFlags = 0;
    /// `p_v<L>FresnelExponentBiasScale` — psmateriallayer.fx applies
    /// `saturate(f * scale + bias)` after the pow, so the record's
    /// `fresnelMin`/`fresnelMax` are the output range: bias = min,
    /// scale = max - min. 797 layers author min > max, which is a negative
    /// scale and an intentionally inverted ramp.
    Vector3f fresnelExponentBiasScale = {1.0f, 0.0f, 1.0f};
    /// `p_m<L>FresnelTransform` reduced to what shipped content puts in it:
    /// the view direction is scaled per axis by the mask and offset by the
    /// translation before the dot. The rotation half is not built — see
    /// `ResolveFresnel`.
    Vector3f fresnelMask = {1.0f, 1.0f, 1.0f};
    Vector3f fresnelTranslation = {0.0f, 0.0f, 0.0f};
    /// `FrameState::texAnimMatrices` id for this layer's `p_m<L>UVTransform`
    /// (io::M3UvTransformId). Stamped on every resolved layer; the palette
    /// carries an entry only for the layers whose transform actually moves,
    /// and a miss is the identity.
    i32 uvTransformId = -1;
    /// The `mapAlpha` a frame with no sample for this layer draws with —
    /// retail's `cResult.a *= p_vMultiplyAddAlphaTrans.z` right after the
    /// channel select. The rest is read on a solid-colour alpha mask only (a
    /// textured layer's ships unauthored more often than not) and is one
    /// everywhere else; a driven track overrides it through
    /// `layerMapAlphaPalette`, keyed by @ref uvTransformId.
    f32 mapAlpha = 1.0f;
};

struct M3Surface {
    bool valid = false; ///< Resolved to a standard material.
    ::whiteout::m3::BlendMode blendMode = ::whiteout::m3::BlendMode::Opaque;
    u32 materialFlags = 0; ///< Raw m3::MaterialFlag bits.
    i32 priority = 0;
    /// The material constant. A gloss layer scales it per pixel by its alpha
    /// squared (psmaterial.fx MaterialSpecularity), so this is the ceiling
    /// rather than the value whenever `layers[Gloss]` is on.
    f32 specularExponent = 20.0f;
    /// FakeEnergyConservingSpec could not be folded into the specular tint —
    /// the gloss layer makes the exponent, and therefore the dim, per-pixel.
    bool dimPerPixel = false;
    /// hdrSpecularMultiplier with the folded dim: what the specular tint
    /// carries beyond the layer's own colour, and what the team colour takes
    /// in retail's team-coloured specular.
    f32 specularScale = 1.0f;
    /// The environment layer looks the cube up along the REFLECTED view
    /// vector rather than along the normal — UVMappingMode Reflect*Envio
    /// against plain *Envio. 710 of the 744 shipped env layers reflect.
    bool envReflect = true;
    /// SimulateRoughness beside a gloss layer: the cube is read at a mip
    /// biased by `1 - gloss` (psmainshading.fx b_iBlurEnvironmentMap), so a
    /// rough texel reflects a blur and a glossy one a mirror. The range is the
    /// bound cube's own last level, read where the draw is written.
    bool envBlur = false;
    /// Normalised [0,1]; 0 disables the test.
    f32 alphaTestThreshold = 0.0f;
    /// REGN v5+ per-region decode (`uv = i16 * uvMultiply + uvOffset`); older
    /// regions imply the spec constant 1/2048.
    f32 uvMultiply = 1.0f / 2048.0f;
    f32 uvOffset = 0.0f;
    /// hdrEmissiveMultiplier — applied by the shader to the Add-family
    /// emissive accumulation (psmaterial.fx:270 `cEmissive *
    /// p_fEmissiveMultiplier`), so the TeamColor*Add contributions scale too.
    f32 emissiveMultiplier = 1.0f;
    M3Layer layers[kM3LayerCount];
};

class M3SurfaceTable final : public core::ISurfaceTable {
public:
    static constexpr core::ProductId kProduct = core::ProductId::Sc2;

    core::ProductId Product() const override {
        return kProduct;
    }
    usize Count() const override {
        return surfaces_.size();
    }

    /// @brief Surface @p index, or null when out of range — which a draw can
    ///        legitimately hit for an actor mid-reload.
    const M3Surface* Surface(u32 index) const {
        return index < surfaces_.size() ? &surfaces_[index] : nullptr;
    }

    std::vector<M3Surface>& Surfaces() {
        return surfaces_;
    }
    const std::vector<M3Surface>& Surfaces() const {
        return surfaces_;
    }

    /// @brief First entry of the per-`RIB_` block BuildM3SurfaceTable appends
    ///        after the geoset entries (a material only a ribbon references
    ///        has no geoset, so it needs its own row). Ribbon i's surface is
    ///        `ribbonSurfaceBase + i`; -1 when the model has no ribbons.
    i32 RibbonSurfaceBase() const {
        return ribbonSurfaceBase_;
    }
    void SetRibbonSurfaceBase(i32 base) {
        ribbonSurfaceBase_ = base;
    }

    /// @brief First entry of the per-`PAR_` block, appended after the ribbon
    ///        one for the same reason: a material only a particle emitter
    ///        references has no geoset. Particle i's surface is
    ///        `particleSurfaceBase + i`; -1 when the model has no `PAR_`.
    i32 ParticleSurfaceBase() const {
        return particleSurfaceBase_;
    }
    void SetParticleSurfaceBase(i32 base) {
        particleSurfaceBase_ = base;
    }

private:
    std::vector<M3Surface> surfaces_;
    i32 ribbonSurfaceBase_ = -1;
    i32 particleSurfaceBase_ = -1;
};

/// @brief Build the table for @p model. @p emittedRegions and
///        @p emittedMaterials are the adapter's emission order
///        (M3ModelAdapter::EmittedRegions / EmittedMaterials) — entry g
///        describes geoset g, naming the region it covers and the `MATM` entry
///        it draws. The two are separate because a composite material emits one
///        geoset per section over the same region. Returns an empty table
///        rather than null when the model has no divisions, so callers never
///        branch on a null table.
std::unique_ptr<M3SurfaceTable> BuildM3SurfaceTable(const ::whiteout::m3::Model& model,
                                                    std::span<const std::size_t> emittedRegions,
                                                    std::span<const u32> emittedMaterials);

/// @brief Which bucket a surface draws in. Opaque with an alpha test is
///        AlphaKey; every real blend mode is transparent. Invalid surfaces are
///        invisible to this model — the loader keeps them on Unlit instead.
core::SurfaceClass M3ClassifySurface(const M3Surface& surface);

/// @brief Does an SC2 `PAR_` drawn with @p surface walk its flipbook cells?
///
/// `b_iUVMapping[slot]` is per texture SLOT (`Particle.fx:131`) and retail
/// builds a UV per slot in the vertex shader. A `renderer::Vertex` carries ONE
/// UV set, so one slot has to answer for all of them, and it cannot be slot 0
/// unconditionally: surveyed over 5037 corpus emitters, **1223 have no active
/// diffuse layer at all** and draw out of the emissive one instead. Reading the
/// empty slot reported "no flipbook" on every one of those — 899 emitters that
/// retail cell-walks and this did not.
///
/// So: the first slot the material actually fills, which is the diffuse
/// whenever there is one. 1057 of the 5037 have active layers that DISAGREE
/// about the mapping; no single baked UV set can serve those, and there the
/// dominant layer wins rather than nothing does.
bool M3ParticleFlipbookUv(const M3Surface& surface);

} // namespace whiteout::flakes::renderer::profiles::sc2_heroes
