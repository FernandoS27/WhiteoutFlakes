#include "m2_shader_select.h"

namespace whiteout::flakes::renderer::profiles::wow {
namespace {

using PS = M2PixelShader;
using VS = M2VertexShader;

// s_modelShaderEffect, 6.0.1. The client's rows also carry hull/domain ids and
// the fixed-function fallback ops; neither survives into a shader-only path, so
// only the two we dispatch on are kept.
constexpr M2ShaderEffect kExplicitEffects[kNumM2Shaders] = {
    {PS::Combiners_Opaque_Mod2xNA_Alpha, VS::Diffuse_T1_Env},
    {PS::Combiners_Opaque_AddAlpha, VS::Diffuse_T1_Env},
    {PS::Combiners_Opaque_AddAlpha_Alpha, VS::Diffuse_T1_Env},
    {PS::Combiners_Opaque_Mod2xNA_Alpha_Add, VS::Diffuse_T1_Env_T1},
    {PS::Combiners_Mod_AddAlpha, VS::Diffuse_T1_Env},
    {PS::Combiners_Opaque_AddAlpha, VS::Diffuse_T1_T1},
    {PS::Combiners_Mod_AddAlpha, VS::Diffuse_T1_T1},
    {PS::Combiners_Mod_AddAlpha_Alpha, VS::Diffuse_T1_Env},
    {PS::Combiners_Opaque_Alpha_Alpha, VS::Diffuse_T1_Env},
    {PS::Combiners_Opaque_Mod2xNA_Alpha_3s, VS::Diffuse_T1_Env_T1},
    {PS::Combiners_Opaque_AddAlpha_Wgt, VS::Diffuse_T1_T1},
    {PS::Combiners_Mod_Add_Alpha, VS::Diffuse_T1_Env},
    {PS::Combiners_Opaque_ModNA_Alpha, VS::Diffuse_T1_Env},
    {PS::Combiners_Mod_AddAlpha_Wgt, VS::Diffuse_T1_Env},
    {PS::Combiners_Mod_AddAlpha_Wgt, VS::Diffuse_T1_T1},
    {PS::Combiners_Opaque_AddAlpha_Wgt, VS::Diffuse_T1_T2},
    {PS::Combiners_Opaque_Mod_Add_Wgt, VS::Diffuse_T1_Env},
    {PS::Combiners_Opaque_Mod2xNA_Alpha_UnshAlpha, VS::Diffuse_T1_Env_T1},
    {PS::Combiners_Mod_Dual_Crossfade, VS::Diffuse_T1_T1_T1},
    {PS::Combiners_Mod_Depth, VS::Diffuse_EdgeFade_T1},
    {PS::Combiners_Opaque_Mod2xNA_Alpha_Alpha, VS::Diffuse_T1_Env_T2},
    {PS::Combiners_Mod_Mod, VS::Diffuse_EdgeFade_T1_T2},
    {PS::Combiners_Mod_Masked_Dual_Crossfade, VS::Diffuse_T1_T1_T1_T2},
    {PS::Combiners_Opaque_Alpha, VS::Diffuse_T1_T1},
    {PS::Combiners_Opaque_Mod2xNA_Alpha_UnshAlpha, VS::Diffuse_T1_Env_T2},
    {PS::Combiners_Mod_Depth, VS::Diffuse_EdgeFade_Env},
    {PS::Guild, VS::Diffuse_T1_T2_T1},
    {PS::Guild_NoBorder, VS::Diffuse_T1_T2},
    {PS::Guild_Opaque, VS::Diffuse_T1_T2_T1},
    {PS::Illum, VS::Diffuse_T1_T1},
};

// The client's two 8-entry tables, indexed by `shaderId & 7`. They are the
// binary's form of the wiki's ternary chains; both encodings agree.
constexpr PS kPixelShader2Tex[8] = {
    PS::Combiners_Opaque_Opaque, PS::Combiners_Opaque_Mod,     PS::Combiners_Opaque_Mod,
    PS::Combiners_Opaque_AddAlpha, PS::Combiners_Opaque_Mod2x, PS::Combiners_Opaque_Mod,
    PS::Combiners_Opaque_Mod2xNA, PS::Combiners_Opaque_AddAlpha,
};

constexpr PS kPixelShader2TexEnv[8] = {
    PS::Combiners_Mod_Opaque, PS::Combiners_Mod_Mod,      PS::Combiners_Mod_Mod,
    PS::Combiners_Mod_Add,    PS::Combiners_Mod_Mod2x,    PS::Combiners_Mod_Mod,
    PS::Combiners_Mod_Mod2xNA, PS::Combiners_Mod_AddNA,
};

constexpr const char* kPixelShaderNames[static_cast<usize>(PS::Count)] = {
    "Combiners_Opaque",
    "Combiners_Mod",
    "Combiners_Opaque_Mod",
    "Combiners_Opaque_Mod2x",
    "Combiners_Opaque_Mod2xNA",
    "Combiners_Opaque_Opaque",
    "Combiners_Mod_Mod",
    "Combiners_Mod_Mod2x",
    "Combiners_Mod_Add",
    "Combiners_Mod_Mod2xNA",
    "Combiners_Mod_AddNA",
    "Combiners_Mod_Opaque",
    "Combiners_Opaque_Mod2xNA_Alpha",
    "Combiners_Opaque_AddAlpha",
    "Combiners_Opaque_AddAlpha_Alpha",
    "Combiners_Opaque_Mod2xNA_Alpha_Add",
    "Combiners_Mod_AddAlpha",
    "Combiners_Mod_AddAlpha_Alpha",
    "Combiners_Opaque_Alpha_Alpha",
    "Combiners_Opaque_Mod2xNA_Alpha_3s",
    "Combiners_Opaque_AddAlpha_Wgt",
    "Combiners_Mod_Add_Alpha",
    "Combiners_Opaque_ModNA_Alpha",
    "Combiners_Mod_AddAlpha_Wgt",
    "Combiners_Opaque_Mod_Add_Wgt",
    "Combiners_Opaque_Mod2xNA_Alpha_UnshAlpha",
    "Combiners_Mod_Dual_Crossfade",
    "Combiners_Opaque_Mod2xNA_Alpha_Alpha",
    "Combiners_Mod_Masked_Dual_Crossfade",
    "Combiners_Opaque_Alpha",
    "Guild",
    "Guild_NoBorder",
    "Guild_Opaque",
    "Combiners_Mod_Depth",
    "Illum",
};

constexpr const char* kVertexShaderNames[static_cast<usize>(VS::Count)] = {
    "Diffuse_T1",         "Diffuse_Env",           "Diffuse_T1_T2",
    "Diffuse_T1_Env",     "Diffuse_Env_T1",        "Diffuse_Env_Env",
    "Diffuse_T1_Env_T1",  "Diffuse_T1_T1",         "Diffuse_T1_T1_T1",
    "Diffuse_EdgeFade_T1", "Diffuse_T2",           "Diffuse_T1_Env_T2",
    "Diffuse_EdgeFade_T1_T2", "Diffuse_T1_T1_T1_T2", "Diffuse_EdgeFade_Env",
    "Diffuse_T1_T2_T1",
};

// How many samplers each combiner reads. Cross-checked against the vertex
// shader each explicit-combo row pairs it with — the VS name's T/Env suffix
// count is the same number, for every row.
constexpr u8 kPixelShaderTexCount[static_cast<usize>(PS::Count)] = {
    1, // Combiners_Opaque
    1, // Combiners_Mod
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, // Opaque_Mod2xNA_Alpha
    2, 2,
    3, // Opaque_Mod2xNA_Alpha_Add
    2, 2, 2,
    3, // Opaque_Mod2xNA_Alpha_3s
    2, 2, 2, 2, 2,
    3, // Opaque_Mod2xNA_Alpha_UnshAlpha
    3, // Mod_Dual_Crossfade
    3, // Opaque_Mod2xNA_Alpha_Alpha
    4, // Mod_Masked_Dual_Crossfade
    2, // Opaque_Alpha
    3, // Guild
    2, // Guild_NoBorder
    3, // Guild_Opaque
    1, // Mod_Depth
    2, // Illum
};

} // namespace

M2ShaderEffect M2ExplicitEffect(u32 index) {
    return kExplicitEffects[index < kNumM2Shaders ? index : 0];
}

M2PixelShader M2PixelShaderFor(u32 textureCount, u16 shaderId) {
    if (M2IsExplicitCombo(shaderId))
        return M2ExplicitEffect(shaderId & 0x7FFFu).pixel;

    if (textureCount == 1)
        return (shaderId & 0x70u) ? PS::Combiners_Mod : PS::Combiners_Opaque;

    return ((shaderId & 0x70u) ? kPixelShader2TexEnv : kPixelShader2Tex)[shaderId & 7u];
}

M2VertexShader M2VertexShaderFor(u32 textureCount, u16 shaderId) {
    if (M2IsExplicitCombo(shaderId))
        return M2ExplicitEffect(shaderId & 0x7FFFu).vertex;

    if (textureCount == 1) {
        if (shaderId & 0x80u)
            return VS::Diffuse_Env;
        return (shaderId & 0x4000u) ? VS::Diffuse_T2 : VS::Diffuse_T1;
    }

    if (shaderId & 0x80u)
        return (shaderId & 0x8u) ? VS::Diffuse_Env_Env : VS::Diffuse_Env_T1;
    if (shaderId & 0x8u)
        return VS::Diffuse_T1_Env;
    return (shaderId & 0x4000u) ? VS::Diffuse_T1_T2 : VS::Diffuse_T1_T1;
}

const char* M2PixelShaderName(M2PixelShader id) {
    const auto i = static_cast<usize>(id);
    return i < static_cast<usize>(PS::Count) ? kPixelShaderNames[i] : "?";
}

const char* M2VertexShaderName(M2VertexShader id) {
    const auto i = static_cast<usize>(id);
    return i < static_cast<usize>(VS::Count) ? kVertexShaderNames[i] : "?";
}

u32 M2PixelShaderTextureCount(M2PixelShader id) {
    const auto i = static_cast<usize>(id);
    return i < static_cast<usize>(PS::Count) ? kPixelShaderTexCount[i] : 1u;
}

} // namespace whiteout::flakes::renderer::profiles::wow
