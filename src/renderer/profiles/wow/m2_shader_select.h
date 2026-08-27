#pragma once

// ============================================================================
// M2 shader selection — `CM2Shared::GetEffect`'s two lookups, reproduced.
//
// A batch names its shaders through exactly two fields, `textureCount` and
// `shaderId`. Everything here is a pure function of those two, which is why it
// lives apart from the shading model: it is the half that can be tested without
// a device (M2_RENDERING_DESIGN.md §8.2).
//
// Verified against two clients. WoW 6.0.1 — M2GetPixelShaderID @ 0x100f8db90,
// M2GetVertexShaderID @ 0x100f8dcc0, s_modelShaderEffect @ 0x101c74150 — and
// retail 12.1.0.69404 `Wow.exe`, where the same two selectors live at
// 0x141917820 (pixel) and 0x141917900 (vertex) and the effect table is a plain
// {pixel, vertex} u32 pair array at 0x14443E9F0.
//
// The bit-field paths are byte-identical across the eleven years between them.
// What moved is the explicit-combo table: 30 rows -> 36, and two pixel shaders
// appended. See the notes on kNumM2Shaders and M2VertexShader below.
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
    // 35 and 36, added since 6.0.1. Neither name survives into 12.1's `Wow.exe`
    // — the client resolves shaders through a table loaded from CASC — so both
    // are identified by what the binary does still say:
    //
    //  * 35 shares the client's three-texture uber-shader with the whole
    //    Opaque_Mod2xNA_Alpha_{Add,3s,UnshAlpha,Alpha} family, so it is a
    //    three-sampler member of it. Reached only by rows 30 and 31, which no
    //    model in the corpus selects.
    //  * 36 gets a dedicated shader whose slot falls between
    //    Combiners_Mod_Masked_Dual_Crossfade and Combiners_Opaque in an
    //    alphabetically ordered table, and row 35 pairs it with
    //    Diffuse_EdgeFade_T1_T2. `Combiners_Mod_Mod_Depth` is the only name
    //    that satisfies both; it is an inference, and the one thing here to
    //    re-check against a shader dump.
    Combiners_Unnamed_35,
    Combiners_Mod_Mod_Depth,

    Count,
};

// s_modelVertexShaders. The suffix names which UV source feeds each texture
// unit: T1/T2 are the vertex's two UV sets, Env is the view-space sphere map.
//
// This is 6.0.1's index space, NOT 12.1's — the one enum here that is no longer
// the client's. 12.1 deleted Diffuse_T1_T1_T1 and Diffuse_T1_T1_T1_T2 and
// renumbered everything above them down by two, because its vertex stage emits
// a texcoord SET and duplicating T1 three times in it buys nothing: rows 18 and
// 22 now name plain Diffuse_T1 / Diffuse_T1_T2 and the pixel stage reads the
// one T1 coord from three samplers. Ours is positional — four coords, one per
// sampler — so the folded names still have to exist to say which coord each
// sampler gets. They carry 12.1's meaning: every duplicated T1 goes through
// texture matrix 0, where 6.0.1 gave the second copy matrix 1.
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
    // 12.1's vertex shaders 14 and 15, reached only by rows 30 and 31. Unnamed
    // for the same reason the two pixel shaders above are: 14 sits between
    // Diffuse_T1_T2_T1 and Diffuse_T2 in the client's ordering, and 15 is not
    // in the Diffuse_ block at all. Both pair with Combiners_Unnamed_35, so
    // both feed three samplers.
    Unnamed_14,
    Unnamed_15,

    Count,
};

/// @brief The `s_modelShaderEffect` row an explicit combo selects.
struct M2ShaderEffect {
    M2PixelShader pixel;
    M2VertexShader vertex;
};

/// 36 in 12.1.0.69404: the table runs [0x14443E9F0, 0x14443EB10), which is
/// 0x120 bytes of {u32 pixel, u32 vertex}. 6.0.1 stopped at 30, and the corpus
/// is Legion+ — rows 33/34/35 are ordinary shipped content.
inline constexpr u32 kNumM2Shaders = 36;

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
