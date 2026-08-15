#pragma once

// ============================================================================
// M2Material → GPU state. The other device-free half of the M2 path.
//
// Deliberately *not* routed through bls::MatParams. That vocabulary is MDX's —
// seven WC3 filter modes and a `disables` bitfield whose depth bits mean the
// opposite of M2's — so translating M2 into it and back out loses the blend
// mode and inverts two flags on the way. M2's state is small enough to state
// directly, and BlendFor/DepthFor/RasterFor are internal to bls_pso_builder.cpp
// anyway.
//
// Verified against `CM2SceneRender::SetupMaterial` @ 0x100f84a90 and the
// `s_gxBlend` / `s_fogModeList` tables in a WoW 6.0.1 client.
// ============================================================================

#include "gfx/gfx_pipeline_types.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::profiles::wow {

/// @brief `M2Material::blendingMode`. Maps onto EGxBlend through the client's
///        `s_gxBlend` = {0, 1, 2, 10, 3, 4, 5, 13}; we go straight to blend
///        factors instead, so the intermediate index buys nothing.
enum class M2Blend : u8 {
    Opaque = 0,
    AlphaKey = 1,
    Alpha = 2,
    NoAlphaAdd = 3,
    Add = 4,
    Mod = 5,
    Mod2x = 6,
    BlendAdd = 7,

    Count,
};

/// @brief `M2Material::flags`.
///
/// The depth bits are *disables*, despite wowdev.wiki (and our own
/// M2_FILE_FORMAT_SPECIFICATION §8.4) naming them "depthTest" and "depthWrite"
/// as if they enabled something. `SetupMaterial` settles it: `0x10` set selects
/// the preset whose write bit is *clear*. The name says which knob; the bit
/// turns it off — the same convention MDX spells out as MAT_NO_DEPTH_TEST /
/// MAT_NO_DEPTH_SET.
enum M2MaterialFlag : u16 {
    kM2Unlit = 0x01,
    kM2Unfogged = 0x02,
    kM2TwoSided = 0x04,
    /// Parsed, never honoured — the client ignores it too (M2StateFor).
    kM2NoDepthTest = 0x08,
    kM2NoDepthWrite = 0x10,
    kM2NoAlphaComposite = 0x800,
};

/// @brief What the shader does with fog for a given blend mode — `s_fogModeList`.
///
/// Additive and modulate blends need fog *neutralised* rather than disabled:
/// fogging an additive surface toward the fog colour brightens it, so the
/// client swaps in the colour that is a no-op for that blend instead.
enum class M2FogMode : u8 {
    Disabled = 0,   ///< No fog logic at all.
    FogColor = 1,   ///< The scene fog colour, unmodified.
    Black = 2,      ///< 0x000000 — additive.
    White = 3,      ///< 0xFFFFFF — modulate.
    HalfWhite = 4,  ///< 0x808080 — modulate-2x.
};

/// @brief Everything a batch's material decides, resolved once.
struct M2DrawState {
    gfx::BlendDesc blend;
    gfx::DepthStencilDesc depth;
    gfx::RasterizerDesc raster;

    f32 alphaRef = 0.0f; ///< Fragments with alpha < this are clipped.
    M2FogMode fog = M2FogMode::FogColor;
    bool lit = true;
};

/// @brief The alpha-key cutoff baseline. The client reads it from a cvar that
///        defaults to 128; WotLK and earlier used 224.
inline constexpr f32 kM2AlphaKeyRef = 128.0f / 255.0f;

/// @brief The constant reference every non-alpha-key mode uses — "discard only
///        exactly-zero alpha", which keeps modulate blends from accumulating
///        error on fully transparent texels.
inline constexpr f32 kM2DefaultAlphaRef = 1.0f / 255.0f;

gfx::BlendDesc M2BlendDesc(M2Blend mode);

/// @brief Alpha-test reference. Only `AlphaKey` scales with @p elementAlpha —
///        that multiply is what stops a fading model from punching holes in
///        itself instead of fading.
f32 M2AlphaRef(M2Blend mode, f32 elementAlpha);

M2FogMode M2FogModeFor(M2Blend mode);

/// @brief Lighting is off for the modulate blends, whatever the material says.
bool M2LightingEnabled(M2Blend mode, u16 materialFlags);

/// @brief The combiner table's `in` — what a batch multiplies its textures by.
///
/// `SetupMaterial` does not merely unlight the modulate blends: it zeroes
/// diffuse and writes a constant into emissive, ignoring the batch's colour
/// track entirely. Mod2x's constant is 0.5, and its DstColor/SrcColor blend
/// doubles that back to unity — feed it the usual ~1.0 and the surface comes
/// out twice as bright.
Vector3f M2CombinerInput(M2Blend mode, const Vector3f& batchColor, const Vector3f& geosetColor);

/// @brief The whole translation. @p elementAlpha is
///        `batch.color.alpha * batch.textureWeight * model.alpha`.
///
/// @p mirrored is the actor's reverse-culling bit. `SetupMaterial` picks front
/// or back from it and only drops to no culling on a two-sided material, so a
/// mirrored model draws its far faces rather than turning inside out.
M2DrawState M2StateFor(M2Blend mode, u16 materialFlags, f32 elementAlpha,
                       bool mirrored = false);

/// @brief Clamp a raw `blendingMode` into the enum. Values above 7 do not occur
///        in shipped data; treating one as Opaque draws something rather than
///        indexing off the end of the tables.
M2Blend M2BlendFromRaw(u16 blendingMode);

} // namespace whiteout::flakes::renderer::profiles::wow
