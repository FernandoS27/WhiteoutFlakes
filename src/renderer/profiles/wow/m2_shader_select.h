#pragma once

// ============================================================================
// M2 shader selection — `CM2Shared::GetEffect`'s two lookups, reproduced.
//
// A batch names its shaders through exactly two fields, `textureCount` and
// `shaderId`. Everything here is a pure function of those two, which is why it
// lives apart from the shading model: it is the half that can be tested without
// a device (M2_RENDERING_DESIGN.md §8.2).
//
// Verified against a symbolised WoW 6.0.1 client — M2GetPixelShaderID
// @ 0x100f8db90, M2GetVertexShaderID @ 0x100f8dcc0, s_modelShaderEffect
// @ 0x101c74150 — and agrees row for row with wowdev.wiki/M2/.skin.
// ============================================================================

#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::profiles::wow {

// s_modelPixelShaders. Order is load-bearing: it is the index space
// s_modelShaderEffect and the two 2-texture tables select into.
enum class M2PixelShader : u8 {
    Combiners_Opaque = 0,
    Combiners_Mod,
    Combiners_Opaque_Mod,
    Combiners_Opaque_Mod2x,
    Combiners_Opaque_Mod2xNA,
    Combiners_Opaque_Opaque,
    Combiners_Mod_Mod,
    Combiners_Mod_Mod2x,
    Combiners_Mod_Add,
    Combiners_Mod_Mod2xNA,
    Combiners_Mod_AddNA,
    Combiners_Mod_Opaque,
    Combiners_Opaque_Mod2xNA_Alpha,
    Combiners_Opaque_AddAlpha,
    Combiners_Opaque_AddAlpha_Alpha,
    Combiners_Opaque_Mod2xNA_Alpha_Add,
    Combiners_Mod_AddAlpha,
    Combiners_Mod_AddAlpha_Alpha,
    Combiners_Opaque_Alpha_Alpha,
    Combiners_Opaque_Mod2xNA_Alpha_3s,
    Combiners_Opaque_AddAlpha_Wgt,
    Combiners_Mod_Add_Alpha,
    Combiners_Opaque_ModNA_Alpha,
    Combiners_Mod_AddAlpha_Wgt,
    Combiners_Opaque_Mod_Add_Wgt,
    Combiners_Opaque_Mod2xNA_Alpha_UnshAlpha,
    Combiners_Mod_Dual_Crossfade,
    Combiners_Opaque_Mod2xNA_Alpha_Alpha,
    Combiners_Mod_Masked_Dual_Crossfade,
    Combiners_Opaque_Alpha,
    Guild,
    Guild_NoBorder,
    Guild_Opaque,
    Combiners_Mod_Depth,
    Illum,

    Count,
};

// s_modelVertexShaders. The suffix names which UV source feeds each texture
// unit: T1/T2 are the vertex's two UV sets, Env is the view-space sphere map.
enum class M2VertexShader : u8 {
    Diffuse_T1 = 0,
    Diffuse_Env,
    Diffuse_T1_T2,
    Diffuse_T1_Env,
    Diffuse_Env_T1,
    Diffuse_Env_Env,
    Diffuse_T1_Env_T1,
    Diffuse_T1_T1,
    Diffuse_T1_T1_T1,
    Diffuse_EdgeFade_T1,
    Diffuse_T2,
    Diffuse_T1_Env_T2,
    Diffuse_EdgeFade_T1_T2,
    Diffuse_T1_T1_T1_T2,
    Diffuse_EdgeFade_Env,
    Diffuse_T1_T2_T1,

    Count,
};

/// @brief The 30-entry `s_modelShaderEffect` row an explicit combo selects.
struct M2ShaderEffect {
    M2PixelShader pixel;
    M2VertexShader vertex;
};

inline constexpr u32 kNumM2Shaders = 30;

/// @brief `shaderId & 0x8000` — the batch names a pre-baked combo rather than
///        letting the bit fields compute one.
inline constexpr bool M2IsExplicitCombo(u16 shaderId) {
    return (shaderId & 0x8000u) != 0;
}

/// @brief Row @p index of `s_modelShaderEffect`. Out-of-range clamps to row 0
///        rather than trapping: a malformed `shaderId` should draw wrong, not
///        take the viewer down.
M2ShaderEffect M2ExplicitEffect(u32 index);

/// @brief `M2GetPixelShaderID`. @p textureCount is `M2Batch::textureCount`.
M2PixelShader M2PixelShaderFor(u32 textureCount, u16 shaderId);

/// @brief `M2GetVertexShaderID`.
M2VertexShader M2VertexShaderFor(u32 textureCount, u16 shaderId);

/// @brief Blizzard's own names, for traces and diagnostics.
const char* M2PixelShaderName(M2PixelShader id);
const char* M2VertexShaderName(M2VertexShader id);

/// @brief How many texture units the pixel shader actually samples. The batch's
///        `textureCount` is what *selects* the shader; this is what the shader
///        then reads, and the two disagree — a 2-texture batch can select a
///        3-sampler combiner through the explicit-combo path.
u32 M2PixelShaderTextureCount(M2PixelShader id);

} // namespace whiteout::flakes::renderer::profiles::wow
