// bls::SelectPermutes against the 3.0.0 shader repo's own permutation labels.
//
// Every expected string below is what externals/Wc3Shaders/compile_all_slang.py
// says the selected index compiles to (tests/generated/wc3_perm_labels.inc, from
// tools/gen_wc3_perm_labels.py). A permuter that packs a bit into the wrong
// position picks a real permutation with a different feature set, which a count
// or range check would accept.

#include <catch2/catch_test_macros.hpp>

#include "bls/bls_permuter.h"
#include "generated/wc3_perm_labels.inc"

#include <cornflakes/render/shader_perm.hpp>

#include <string>

using whiteout::flakes::renderer::bls::ExpectedPermuteCounts;
using whiteout::flakes::renderer::bls::GxShaderID;
using whiteout::flakes::renderer::bls::ProgramForLayer;
using whiteout::flakes::renderer::bls::RenderState;
using whiteout::flakes::renderer::bls::SelectPermutes;

namespace {

std::string Label(const char* const* table, unsigned count, unsigned index) {
    REQUIRE(index < count);
    return table[index];
}

#define LABEL(family, index) Label(wc3_perm_labels::k_##family, wc3_perm_labels::k_##family##_count, index)

RenderState HdMesh(GxShaderID id) {
    RenderState s;
    s.shaderId = id;
    s.numTexCoords = 1;
    s.numTangents = 1;
    return s;
}

} // namespace

TEST_CASE("MDX layer shader types map onto 3.0.0 programs", "[bls][permuter]") {
    CHECK(ProgramForLayer(0) == GxShaderID::SD_on_HD);
    CHECK(ProgramForLayer(1) == GxShaderID::HD);
    CHECK(ProgramForLayer(2) == GxShaderID::SD_on_HD);
    // The deliberate deviation: the client sends 24 to Distortion.
    CHECK(ProgramForLayer(24) == GxShaderID::Crystal);
    CHECK(static_cast<int>(GxShaderID::Crystal) == 25);
    CHECK(static_cast<int>(GxShaderID::Tonemap) == 15);
    CHECK(static_cast<int>(GxShaderID::CornFx) == 21);
}

TEST_CASE("Permutation counts match the shader repo", "[bls][permuter]") {
    CHECK(ExpectedPermuteCounts(GxShaderID::HD).vs == wc3_perm_labels::k_hd_vs_count);
    CHECK(ExpectedPermuteCounts(GxShaderID::HD).ps == wc3_perm_labels::k_hd_ps_count);
    CHECK(ExpectedPermuteCounts(GxShaderID::Crystal).ps == wc3_perm_labels::k_crystal_ps_count);
    CHECK(ExpectedPermuteCounts(GxShaderID::SD_on_HD).ps == wc3_perm_labels::k_sd_on_hd_ps_count);
    CHECK(ExpectedPermuteCounts(GxShaderID::SD).vs == wc3_perm_labels::k_sd_highspec_vs_count);
    CHECK(ExpectedPermuteCounts(GxShaderID::SD).ps == wc3_perm_labels::k_sd_classic_ps_count);
    CHECK(ExpectedPermuteCounts(GxShaderID::CornFx).vs == wc3_perm_labels::k_popcorn_vs_count);
    CHECK(ExpectedPermuteCounts(GxShaderID::CornFx).ps == wc3_perm_labels::k_popcorn_ps_count);
    CHECK(whiteout::cornflakes::kVsPermCount == wc3_perm_labels::k_popcorn_vs_count);
    CHECK(whiteout::cornflakes::kPsPermCount == wc3_perm_labels::k_popcorn_ps_count);
}

TEST_CASE("HD vertex permutations", "[bls][permuter]") {
    RenderState s = HdMesh(GxShaderID::HD);
    CHECK(LABEL(hd_vs, SelectPermutes(s).vs) == "HDRigid+T=true+C=false+UV=1");

    s.numWeights = 4;
    CHECK(LABEL(hd_vs, SelectPermutes(s).vs) ==
          "HDFourBoneSkinning<ConstantBonePalette>+T=true+C=false+UV=1");

    s.boneBuffer = true;
    s.numTangents = 0;
    s.numColors = 1;
    s.numTexCoords = 2;
    CHECK(LABEL(hd_vs, SelectPermutes(s).vs) ==
          "HDFourBoneSkinning<StructuredBonePalette>+T=false+C=true+UV=2");

    // The bone-buffer digit means nothing without skinning.
    RenderState rigid = HdMesh(GxShaderID::HD);
    rigid.boneBuffer = true;
    CHECK(SelectPermutes(rigid).vs == SelectPermutes(HdMesh(GxShaderID::HD)).vs);

    // SD_on_HD and Crystal draw with the HD vertex shader and its layout.
    CHECK(SelectPermutes(HdMesh(GxShaderID::SD_on_HD)).vs ==
          SelectPermutes(HdMesh(GxShaderID::HD)).vs);
    CHECK(SelectPermutes(HdMesh(GxShaderID::Crystal)).vs ==
          SelectPermutes(HdMesh(GxShaderID::HD)).vs);
}

TEST_CASE("HD pixel permutations", "[bls][permuter]") {
    RenderState s = HdMesh(GxShaderID::HD);
    CHECK(LABEL(hd_ps, SelectPermutes(s).ps) == "AlphaTestOff+StandardMaterial");

    s.lighting = true;
    s.multiLayer = true;
    s.mrt = true;
    CHECK(LABEL(hd_ps, SelectPermutes(s).ps) == "AlphaTestOff+MultiLayerMaterial+LIT+MRT");

    RenderState t = HdMesh(GxShaderID::HD);
    t.lighting = true;
    t.aoMap = true;
    t.shadowCascade = true;
    t.alphaMode = 1;
    CHECK(LABEL(hd_ps, SelectPermutes(t).ps) == "AlphaTestOn+StandardMaterial+LIT+AO+SC");

    RenderState u = HdMesh(GxShaderID::HD);
    u.lighting = true;
    u.shadowCascade = true;
    u.shadowCascade2 = true;
    u.pointShadows = true;
    u.lightDebug = true;
    CHECK(LABEL(hd_ps, SelectPermutes(u).ps) == "AlphaTestOff+StandardMaterial+LIT+SC+SC2+PS+DBG");

    // The shadow caster: depth prepass with the material's alpha test.
    RenderState caster = HdMesh(GxShaderID::HD);
    caster.depthPrepass = true;
    caster.alphaMode = 1;
    caster.multiLayer = true;
    CHECK(LABEL(hd_ps, SelectPermutes(caster).ps) == "AlphaTestOn+MultiLayerMaterial+DP");
}

TEST_CASE("Crystal pixel permutations", "[bls][permuter]") {
    RenderState s = HdMesh(GxShaderID::Crystal);
    s.lighting = true;
    s.shadowCascade = true;
    s.mrt = true;
    s.alphaMode = 1;
    s.multiLayer = true;
    CHECK(LABEL(crystal_ps, SelectPermutes(s).ps) == "AlphaTestOn+MultiLayerMaterial+LIT+SC+MRT");

    RenderState dp = HdMesh(GxShaderID::Crystal);
    dp.depthPrepass = true;
    CHECK(LABEL(crystal_ps, SelectPermutes(dp).ps) == "AlphaTestOff+StandardMaterial+DP");
}

TEST_CASE("SD-on-HD pixel permutations", "[bls][permuter]") {
    RenderState s = HdMesh(GxShaderID::SD_on_HD);
    s.lighting = true;
    s.mrt = true;
    CHECK(LABEL(sd_on_hd_ps, SelectPermutes(s).ps) == "AlphaTestOff+LIT+MRT");

    RenderState a = HdMesh(GxShaderID::SD_on_HD);
    a.lighting = true;
    a.shadowCascade = true;
    a.alphaMode = 1;
    CHECK(LABEL(sd_on_hd_ps, SelectPermutes(a).ps) == "AlphaTestOn+LIT+SC");

    RenderState e = HdMesh(GxShaderID::SD_on_HD);
    e.lighting = true;
    e.srgbOutput = true;
    CHECK(LABEL(sd_on_hd_ps, SelectPermutes(e).ps) == "AlphaTestOff+LIT+SRGB");

    RenderState dp = HdMesh(GxShaderID::SD_on_HD);
    dp.depthPrepass = true;
    dp.alphaMode = 1;
    CHECK(LABEL(sd_on_hd_ps, SelectPermutes(dp).ps) == "AlphaTestOn+DP");
}

TEST_CASE("SD permutations", "[bls][permuter]") {
    RenderState s;
    s.shaderId = GxShaderID::SD;
    s.numWeights = 4;
    s.numColors = 1;
    s.numTexCoords = 1;
    s.numLights = 3;
    s.alphaMode = 1;
    const auto perm = SelectPermutes(s);
    CHECK(LABEL(sd_highspec_vs, perm.vs) == "SDFourBoneSkinning+C=true+UV=1+NL=3");
    CHECK(LABEL(sd_classic_ps, perm.ps) == "T0=1+T1=0+fog0+AlphaTestOn");

    RenderState f;
    f.shaderId = GxShaderID::SD;
    f.sdFogMode = 2;
    f.sdStage0 = 2;
    f.sdStage1 = 3;
    CHECK(LABEL(sd_classic_ps, SelectPermutes(f).ps) == "T0=2+T1=3+fog2+AlphaTestOff");
}

TEST_CASE("PopcornFX permutations", "[bls][permuter]") {
    using namespace whiteout::cornflakes;
    LayerRendererFlags flags{};
    flags.hasUV = true;
    flags.hasVC = true;
    auto key = classifyPopcornPerm(flags, RenderPass::Color);
    CHECK(LABEL(popcorn_vs, key.vsPerm) == "BasicUV+R=0+VC=1+NT=0");
    CHECK(LABEL(popcorn_ps, key.psPerm) ==
          "PopcornBasicUV+M=false+G=false+SP=false+ALUT=false+VC=true+LIT=false");

    flags.isAtlas = true;
    flags.hasAlphaLut = true;
    flags.hasRandom = true;
    key = classifyPopcornPerm(flags, RenderPass::Color);
    CHECK(LABEL(popcorn_vs, key.vsPerm) == "Atlas+R=1+VC=1+NT=0");
    CHECK(LABEL(popcorn_ps, key.psPerm) ==
          "PopcornAtlas+M=false+G=false+SP=false+ALUT=true+VC=true+LIT=false");
}
