#pragma once

// Constant-buffer and structured-buffer layouts of the Warcraft III 3.0.0
// shader pack. Every offset is pinned against
// externals/Wc3Shaders/wc3_shaders/types/cb_structs.slang; where a row's
// engine source is not obvious the comment names it (WC3_30_LIGHTING_DESIGN.md
// §3.2 / §3.3 carry the full tables).

#include "types.h"
#include "whiteout/flakes/types.h"

#include <cstring>

namespace whiteout::flakes::renderer::bls {

// Several lanes are integers the shader reinterprets with asint / asuint, so
// the bit pattern is what has to land in the buffer, not the float value.
inline f32 IntBits(i32 v) {
    f32 out;
    std::memcpy(&out, &v, sizeof(out));
    return out;
}

struct ShaderBone {
    Vector4f row0;
    Vector4f row1;
    Vector4f row2;
};
static_assert(sizeof(ShaderBone) == 48);

struct ShaderTexMtx {
    Vector4f rows[2];
};
static_assert(sizeof(ShaderTexMtx) == 32);

// One SD highspec per-vertex light (`SDHighspecLight`). 3.0.0 tightened this
// family's stride from 64 to 48 bytes: at two lights retail declares cb0[19]
// and reads ambient cb0[16], diffuse cb0[17], position cb0[18].
struct ShaderLight {
    Vector4f ambient;
    Vector4f diffuse;
    Vector4f position; // .w > 0 = point light
};
static_assert(sizeof(ShaderLight) == 48);

inline constexpr i32 kMaxLights = 8;
inline constexpr i32 kMaxBones = 256;

// SD highspec VS, b0 (`SDHighspecVSPerDraw`).
struct SdVsCbA {
    Matrix44f world;
    Matrix44f worldViewProj;
    Vector4f diffuseColor;
    ShaderTexMtx texMtx0;
    ShaderTexMtx texMtx1;
    ShaderLight lights[kMaxLights];
};
static_assert(offsetof(SdVsCbA, world) == 0x00);
static_assert(offsetof(SdVsCbA, worldViewProj) == 0x40);
static_assert(offsetof(SdVsCbA, diffuseColor) == 0x80);
static_assert(offsetof(SdVsCbA, texMtx0) == 0x90);
static_assert(offsetof(SdVsCbA, texMtx1) == 0xB0);
static_assert(offsetof(SdVsCbA, lights) == 0xD0);
static_assert(sizeof(SdVsCbA) == 0xD0 + 48 * kMaxLights);

inline u32 SdVsCbASize(i32 numLights) {
    return 208u + 48u * static_cast<u32>(numLights);
}

// SD classic PS, b0 (`SDClassicPSPerDraw`). 3.0.0 re-ordered it: the fog colour
// moved into row 1 with the start in .w, the shape HD's cb2[1..2] has.
struct SdPsCbA {
    f32 alphaRef;
    f32 _p0[3];
    Vector4f fogColorStart;
    f32 fogEnd;
    f32 fogDensity;
    f32 _p2[2];
};
static_assert(offsetof(SdPsCbA, fogColorStart) == 0x10);
static_assert(offsetof(SdPsCbA, fogEnd) == 0x20);
static_assert(sizeof(SdPsCbA) == 48);

// HD mesh VS, b2 (`HDVSPerDraw`). Also the PopcornFX VS bank.
struct HdVsCb {
    Matrix44f world;
    Matrix44f worldView;
    Matrix44f worldViewProj;
    // [12-15] view -> clip. Not read by hd_vs; cameraocclusion_vs projects
    // through it.
    Matrix44f projection;
    f32 effectTime;
    f32 popcornScale;
    f32 clipHeight;
    f32 underWater; // 0 = no water clip; the PS discards on a negative product
    f32 boneBufferBase; // int: first bone of this draw in VS t16
    f32 _p17[3];
    Vector4f diffuseColor;
    ShaderTexMtx texMtx0;
    ShaderTexMtx texMtx1;
};
static_assert(offsetof(HdVsCb, worldViewProj) == 0x080);
static_assert(offsetof(HdVsCb, projection) == 0x0C0);
static_assert(offsetof(HdVsCb, effectTime) == 0x100);
static_assert(offsetof(HdVsCb, boneBufferBase) == 0x110);
static_assert(offsetof(HdVsCb, diffuseColor) == 0x120);
static_assert(offsetof(HdVsCb, texMtx0) == 0x130);
static_assert(offsetof(HdVsCb, texMtx1) == 0x150);
static_assert(sizeof(HdVsCb) == 368);

// HD mesh VS, b1 (`HDVSBlight`): the blight-map rect the VS turns world XY into
// a lookup through, and water's wave height.
struct HdVsBlightCb {
    Vector4f rect; // .xy origin, .zw 1 / extent
    Vector4f waterWave;
};
static_assert(sizeof(HdVsBlightCb) == 32);

// HD mesh PS, b2 (`HDPSPerDraw`) — shared by HD, Crystal, SD_on_HD and
// PopcornFX. Filled by CGxDevice::IStateSync per draw.
struct HdPsCb {
    f32 alphaRef;
    f32 blendMode; // int: CGxDevice+1563, picks the fog application
    f32 _p0[2];
    Vector4f fogColorStart; // linear fog colour, .w fog start
    f32 fogEnd;
    f32 fogDensity;
    f32 heightTop;
    f32 heightBottom;
    f32 radialInner;
    f32 radialOuter;
    f32 radialStrength;
    f32 fogEverywhere; // int
    Matrix44f worldView;
    Matrix44f invView;
    Matrix44f invProjection;
    // [16-19] view -> clip, addressed per row (`.xyw`).
    Matrix44f projection;
    Vector4f fogCentre; // camera world position
    Vector4f depthUVRemap;
    f32 outputAlphaScale; // HD: an enable (flag 0x400); popcorn: soft-particle scale
    f32 cloakAmount;
    f32 fresnelTeamColor;
    f32 blightEnable;
    f32 waterShallowDepth;
    f32 waterDeepDepth;
    f32 _p23_z;
    f32 depthTestEnable;
    Vector4f fresnelColor; // .w fresnel alpha
    // [25] IBL mip COUNT of each probe (CGxTex+0x48 = log2(max(w,h)) + 1), and
    // the day/night transition. Both counts zero = no probe bound.
    f32 envFromMipCount;
    f32 envToMipCount;
    f32 envTransitionT;
    f32 _p25_w;
    f32 effectTime;
    f32 emissiveGain;
    f32 shadowDepthBias;
    f32 useNdf; // int: specular anti-aliasing
    f32 normalStrength;
    f32 mainLightEnable; // int
    f32 fogMode;         // int: 0 off, else engine fog type + 1
    f32 _p27_w;
    // [28-30] the main light (CGxLightToMainLightBlock). The shader repo names
    // rows 28 / 29 `ambientAdd` / `ambientColor`; the engine writes the AMBIENT
    // into 28 and the light COLOUR into 29.
    Vector4f ambient;    // ambient colour x ambient intensity, .w = shadowIntensity
    Vector4f lightColor; // colour x intensity
    Vector4f lightDirVS; // direction to the light, view space
};
static_assert(offsetof(HdPsCb, fogColorStart) == 0x010);
static_assert(offsetof(HdPsCb, worldView) == 0x040);
static_assert(offsetof(HdPsCb, invView) == 0x080);
static_assert(offsetof(HdPsCb, invProjection) == 0x0C0);
static_assert(offsetof(HdPsCb, projection) == 0x100);
static_assert(offsetof(HdPsCb, fogCentre) == 0x140);
static_assert(offsetof(HdPsCb, outputAlphaScale) == 0x160);
static_assert(offsetof(HdPsCb, depthTestEnable) == 0x17C);
static_assert(offsetof(HdPsCb, fresnelColor) == 0x180);
static_assert(offsetof(HdPsCb, envFromMipCount) == 0x190);
static_assert(offsetof(HdPsCb, effectTime) == 0x1A0);
static_assert(offsetof(HdPsCb, normalStrength) == 0x1B0);
static_assert(offsetof(HdPsCb, ambient) == 0x1C0);
static_assert(offsetof(HdPsCb, lightColor) == 0x1D0);
static_assert(offsetof(HdPsCb, lightDirVS) == 0x1E0);
static_assert(sizeof(HdPsCb) == 496);

// One cube-shadow record (`HDCubeShadow`), 26 rows.
struct HdCubeShadow {
    Vector4f position; // .xyz world
    Vector4f range;    // .x near, .y far, .z strength
    Matrix44f faces[6];
};
static_assert(sizeof(HdCubeShadow) == 26 * 16);

inline constexpr i32 kMaxCubeShadows = 4;

// HD mesh PS, b1 (`HDPSClustered`), bound by the LIGHTING permutations. Rows
// 0-36.y come from WorldShadowBind, rows 39.w-42 from
// CGxDevice_FillClusterConstants.
struct HdPsClusteredCb {
    // Three cascade sets of three matrices, index `set * 3 + cascade`.
    Matrix44f cascadeSets[9];
    f32 cascadeCount;     // uint
    f32 shadowLightCount; // uint
    f32 occlusionAlpha;
    f32 occlusionInnerRadius;
    Vector4f waterProbeColor;
    f32 waterRefractMin;
    f32 waterRefractMax;
    f32 waterReflectStrength;
    f32 waterGlowStrength;
    f32 waterFadeScale;
    f32 waterRippleStrength;
    f32 waterReflectScale;
    f32 lightDebugMode; // uint
    f32 invClusterDimX;
    f32 invClusterDimY;
    f32 lightArrayBase; // uint: first record of this set in t16
    f32 lightIndexBase; // uint: first u32 of this set in t17
    f32 clusterBase;    // int: first tile record in t18
    f32 gridStride;     // int: tiles per row
    f32 gridRows;       // int
    f32 _p41_w;
    Vector4f viewportRect; // x0, y0, x1, y1 in pixels
    HdCubeShadow cubeShadows[kMaxCubeShadows];
};
static_assert(offsetof(HdPsClusteredCb, cascadeCount) == 36 * 16);
static_assert(offsetof(HdPsClusteredCb, lightDebugMode) == 39 * 16 + 12);
static_assert(offsetof(HdPsClusteredCb, invClusterDimX) == 40 * 16);
static_assert(offsetof(HdPsClusteredCb, clusterBase) == 41 * 16);
static_assert(offsetof(HdPsClusteredCb, viewportRect) == 42 * 16);
static_assert(offsetof(HdPsClusteredCb, cubeShadows) == 43 * 16);
static_assert(sizeof(HdPsClusteredCb) == 2352);

// One clustered light as packed into PS t16 (`HDClusterLight`, stride 40).
struct HdClusterLight {
    f32 colorR, colorG, colorB; // linear colour x intensity
    f32 shadowIdx;              // int: -1 none, -2 caster without a slot
    f32 posX, posY, posZ;       // view space
    f32 quadAtten;
    f32 linAtten;
    f32 expAtten;
};
static_assert(sizeof(HdClusterLight) == 40);

// A tile record in PS t18: `listOffset << 10 | lightCount`.
inline constexpr u32 kClusterCountBits = 10;
inline constexpr u32 kClusterMaxCount = (1u << kClusterCountBits) - 1u;

struct BonePaletteCb {
    ShaderBone bones[kMaxBones];
};
static_assert(sizeof(BonePaletteCb) == 12288);

// The b3 bank of the full-screen post programs. 3.0.0 moved tonemap, bloom,
// the gaussian blur and depth of field from b1 to b3.
inline constexpr u32 kPostPassCbSlot = 3;

} // namespace whiteout::flakes::renderer::bls
