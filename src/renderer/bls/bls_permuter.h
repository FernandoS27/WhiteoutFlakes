#pragma once

#include "whiteout/flakes/types.h"

#include <array>

namespace whiteout::flakes::renderer::bls {

// The engine's program table, in `CGxDevice::ILoadShaders` order (3.0.0).
// 3.0.0 inserted WaterReflection at 10, which moved every program from
// DepthOfField on up by one, and appended four programs at the end. See
// WC3_HD_PIPELINE_3_0_RE.md §3.1.
enum class GxShaderID : u8 {
    SD = 0,
    HD = 1,
    SD_on_HD = 2,
    Terrain = 3,
    Water = 4,
    Fog = 5,
    Foliage = 6,
    FoliagePush = 7,
    Sprite = 8,
    DebugTexture = 9,
    WaterReflection = 10,
    DepthOfField = 11,
    BloomCombine = 12,
    BloomExtract = 13,
    GaussianBlur = 14,
    Tonemap = 15,
    Movie = 16,
    FFXCMAAEdge0 = 17,
    FFXCMAAEdge1 = 18,
    FFXCMAAEdgeCombine = 19,
    FFXCMAAProcessAndApply = 20,
    CornFx = 21,
    ConeIndicator = 22,
    CliffBlightMiscTerrain = 23,
    Distortion = 24,
    Crystal = 25,
    Imgui = 26,
    SSAA = 27,
    CameraOcclusion = 28,
    VolumetricFog = 29,
    Greyscale = 30,
};

// The shader type an MDX layer stores (`s_shaderNames`, read by IReadShader).
// This namespace was NOT renumbered with GxShaderID, so a layer's value is not
// a program id and must go through ProgramForLayer.
enum class MdxShaderType : i32 {
    SdLegacy = 0,
    HdDefaultUnit = 1,
    SdFixedFunction = 2,
    HdCrystal = 24,
};

// The program an MDX layer draws with in HD mode.
//
// 0/2 draw through SD_on_HD and 1 through HD, as the client does. 24 is the one
// deliberate deviation: the 3.0.0 client hands it to `Distortion`, a full-screen
// program with the Sprite VS that cannot mean anything for a mesh (RE doc §3.8),
// so we keep the authored intent and draw it as Crystal.
GxShaderID ProgramForLayer(i32 mdxShaderType);

// The inputs the permuters read. Which fields matter depends on the program;
// each group below names the programs that read it.
struct RenderState {
    GxShaderID shaderId = GxShaderID::Sprite;

    // Sprite / Movie / Imgui: the program's flag word.
    u32 materialFlags = 0;
    // Non-zero = alpha test (every family with an ALPHA_TEST axis).
    u8 alphaMode = 0;

    // HD / Crystal / SD_on_HD pixel feature mask (GetShaderIndices case 1).
    bool lighting = false;
    bool multiLayer = false;     // team-colour layer; HD and Crystal only
    bool mrt = false;            // writes the G-buffer targets
    bool depthPrepass = false;
    bool shadowCascade = false;
    bool shadowCascade2 = false;
    bool pointShadows = false;
    bool lightDebug = false;
    bool aoMap = false;          // HD only
    bool srgbOutput = false;     // SD_on_HD only

    // HD vertex format (HD / Crystal / SD_on_HD all use the HD VS).
    u8 numColors = 0;
    u8 numTexCoords = 0;
    u8 numTangents = 0;
    u8 numWeights = 0;
    bool boneBuffer = false;     // palette > 256 bones, read from VS t16

    // SD highspec VS light count; SD classic PS fog mode (0..6) and stages.
    u8 numLights = 0;
    u8 sdFogMode = 0;
    u8 sdStage0 = 1;
    u8 sdStage1 = 0;

    bool clampBloomOutput = false;
};

struct PermuteIndices {
    u32 vs = 0;
    u32 ps = 0;
};

struct PermuteCounts {
    u32 vs;
    u32 ps;
};

PermuteIndices SelectPermutes(const RenderState& state);
PermuteCounts ExpectedPermuteCounts(GxShaderID id);

} // namespace whiteout::flakes::renderer::bls
