// Device-free half of the M2 path: the two lookups CM2Shared::GetEffect makes.
//
// Every expectation here is transcribed from a symbolised WoW 6.0.1 client
// (M2GetPixelShaderID @ 0x100f8db90, M2GetVertexShaderID @ 0x100f8dcc0,
// s_modelShaderEffect @ 0x101c74150) and cross-checked against
// wowdev.wiki/M2/.skin. A failure here means the table drifted, not that a
// model is unusual.

#include "renderer/profiles/wow/m2_shader_select.h"

#include <catch2/catch_test_macros.hpp>

using namespace whiteout::flakes::renderer::profiles::wow;
using PS = M2PixelShader;
using VS = M2VertexShader;

TEST_CASE("M2 single-texture selection", "[m2][shader]") {
    // op_count 1: bit 0x70 is the whole decision for the pixel shader.
    CHECK(M2PixelShaderFor(1, 0x0000) == PS::Combiners_Opaque);
    CHECK(M2PixelShaderFor(1, 0x0010) == PS::Combiners_Mod);
    CHECK(M2PixelShaderFor(1, 0x0070) == PS::Combiners_Mod);
    CHECK(M2PixelShaderFor(1, 0x0007) == PS::Combiners_Opaque); // low bits don't count

    // 0x80 (env) outranks 0x4000 (second UV set).
    CHECK(M2VertexShaderFor(1, 0x0000) == VS::Diffuse_T1);
    CHECK(M2VertexShaderFor(1, 0x0080) == VS::Diffuse_Env);
    CHECK(M2VertexShaderFor(1, 0x4000) == VS::Diffuse_T2);
    CHECK(M2VertexShaderFor(1, 0x4080) == VS::Diffuse_Env);
}

TEST_CASE("M2 two-texture selection walks both lookup tables", "[m2][shader]") {
    // s_m2PixelShader2Tex, indexed by shaderId & 7.
    const PS expectPlain[8] = {
        PS::Combiners_Opaque_Opaque,   PS::Combiners_Opaque_Mod,
        PS::Combiners_Opaque_Mod,      PS::Combiners_Opaque_AddAlpha,
        PS::Combiners_Opaque_Mod2x,    PS::Combiners_Opaque_Mod,
        PS::Combiners_Opaque_Mod2xNA,  PS::Combiners_Opaque_AddAlpha,
    };
    // s_m2PixelShader2TexEnv, selected when 0x70 is set.
    const PS expectEnv[8] = {
        PS::Combiners_Mod_Opaque,   PS::Combiners_Mod_Mod,
        PS::Combiners_Mod_Mod,      PS::Combiners_Mod_Add,
        PS::Combiners_Mod_Mod2x,    PS::Combiners_Mod_Mod,
        PS::Combiners_Mod_Mod2xNA,  PS::Combiners_Mod_AddNA,
    };
    for (whiteout::flakes::u16 low = 0; low < 8; ++low) {
        CHECK(M2PixelShaderFor(2, low) == expectPlain[low]);
        CHECK(M2PixelShaderFor(2, static_cast<whiteout::flakes::u16>(0x0010 | low)) ==
              expectEnv[low]);
    }

    CHECK(M2VertexShaderFor(2, 0x0000) == VS::Diffuse_T1_T1);
    CHECK(M2VertexShaderFor(2, 0x4000) == VS::Diffuse_T1_T2);
    CHECK(M2VertexShaderFor(2, 0x0008) == VS::Diffuse_T1_Env);
    CHECK(M2VertexShaderFor(2, 0x0080) == VS::Diffuse_Env_T1);
    CHECK(M2VertexShaderFor(2, 0x0088) == VS::Diffuse_Env_Env);
    // 0x8 outranks 0x4000, and 0x80 outranks both.
    CHECK(M2VertexShaderFor(2, 0x4008) == VS::Diffuse_T1_Env);
    CHECK(M2VertexShaderFor(2, 0x4080) == VS::Diffuse_Env_T1);
}

TEST_CASE("M2 explicit combos index s_modelShaderEffect", "[m2][shader]") {
    CHECK(M2IsExplicitCombo(0x8000));
    CHECK(!M2IsExplicitCombo(0x7FFF));

    // Row 0 is the armour-shine combo every character model leans on.
    CHECK(M2PixelShaderFor(2, 0x8000) == PS::Combiners_Opaque_Mod2xNA_Alpha);
    CHECK(M2VertexShaderFor(2, 0x8000) == VS::Diffuse_T1_Env);
    // The explicit path ignores textureCount entirely.
    CHECK(M2PixelShaderFor(1, 0x8000) == PS::Combiners_Opaque_Mod2xNA_Alpha);
    CHECK(M2PixelShaderFor(4, 0x8000) == PS::Combiners_Opaque_Mod2xNA_Alpha);

    // Spot-checks across the table, including the two rows that repeat a pixel
    // shader under a different vertex one (5/6 vs 1/4) and the last row.
    CHECK(M2ExplicitEffect(3).pixel == PS::Combiners_Opaque_Mod2xNA_Alpha_Add);
    CHECK(M2ExplicitEffect(3).vertex == VS::Diffuse_T1_Env_T1);
    CHECK(M2ExplicitEffect(5).pixel == PS::Combiners_Opaque_AddAlpha);
    CHECK(M2ExplicitEffect(5).vertex == VS::Diffuse_T1_T1);
    CHECK(M2ExplicitEffect(22).pixel == PS::Combiners_Mod_Masked_Dual_Crossfade);
    CHECK(M2ExplicitEffect(22).vertex == VS::Diffuse_T1_T1_T1_T2);
    CHECK(M2ExplicitEffect(29).pixel == PS::Illum);
    CHECK(M2ExplicitEffect(29).vertex == VS::Diffuse_T1_T1);

    // Out of range clamps rather than reading past the table.
    CHECK(M2ExplicitEffect(kNumM2Shaders).pixel == PS::Combiners_Opaque_Mod2xNA_Alpha);
    CHECK(M2PixelShaderFor(2, 0xFFFF) == PS::Combiners_Opaque_Mod2xNA_Alpha);
}

TEST_CASE("M2 every selectable shader is in range and named", "[m2][shader]") {
    for (whiteout::flakes::u32 count = 1; count <= 4; ++count) {
        for (whiteout::flakes::u32 id = 0; id <= 0xFFFF; ++id) {
            const auto sid = static_cast<whiteout::flakes::u16>(id);
            const auto ps = M2PixelShaderFor(count, sid);
            const auto vs = M2VertexShaderFor(count, sid);
            REQUIRE(static_cast<whiteout::flakes::u32>(ps) <
                    static_cast<whiteout::flakes::u32>(PS::Count));
            REQUIRE(static_cast<whiteout::flakes::u32>(vs) <
                    static_cast<whiteout::flakes::u32>(VS::Count));
            // A texture count of 0 would leave a sampler unbound, which is a
            // validation error on Vulkan, WebGPU and Metal.
            REQUIRE(M2PixelShaderTextureCount(ps) >= 1);
            REQUIRE(M2PixelShaderTextureCount(ps) <= 4);
        }
    }
    CHECK(std::string(M2PixelShaderName(PS::Combiners_Opaque)) == "Combiners_Opaque");
    CHECK(std::string(M2PixelShaderName(PS::Illum)) == "Illum");
    CHECK(std::string(M2VertexShaderName(VS::Diffuse_T1_T2_T1)) == "Diffuse_T1_T2_T1");
}

TEST_CASE("M2 combiner sampler counts match their paired vertex shader", "[m2][shader]") {
    // Every explicit-combo row pairs a pixel shader with a vertex shader whose
    // name spells the same unit count. That cross-check is what catches a typo
    // in either table.
    const whiteout::flakes::u32 vsUnits[] = {1, 1, 2, 2, 2, 2, 3, 2, 3, 1, 1, 3, 2, 4, 1, 3};
    for (whiteout::flakes::u32 i = 0; i < kNumM2Shaders; ++i) {
        const auto e = M2ExplicitEffect(i);
        INFO("s_modelShaderEffect row " << i);
        CHECK(M2PixelShaderTextureCount(e.pixel) ==
              vsUnits[static_cast<whiteout::flakes::u32>(e.vertex)]);
    }
}
