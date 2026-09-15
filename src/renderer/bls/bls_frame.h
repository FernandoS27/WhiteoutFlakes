#pragma once

#include "bls_cb_layout.h"
#include "bls_mat_params.h"
#include "types.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::bls {

// The main directional light as CGxLightToMainLightBlock writes it in HD mode
// (PS cb2 rows 28-30).
struct MainLight {
    Vector3f ambient = {0, 0, 0};    // ambient colour x ambient intensity
    f32 shadowIntensity = 0.0f;      // CGxLight+28: scales the IBL term
    Vector3f color = {0, 0, 0};      // colour x intensity
    Vector3f dirToLightVS = {0, 0, 1};
    bool enabled = false;
};

// World fog as the 3.0.0 banks carry it (HD PS cb2 rows 1-3 and 27.z, SD PS b0
// and the SD PS fog digit). `mode` is the shader's: 0 off, 1 linear, 2 exp,
// 3 exp^2, 4 volumetric, 5 banded exp, 6 banded exp^2 — the engine writes its
// fog type + 1. Distances are world units; the colour is the engine's 8-bit
// sRGB, linearised for HD only (SD shades in gamma space).
struct FogParams {
    i32 mode = 0;
    Vector3f colorSrgb = {0, 0, 0};
    f32 start = 0.0f;
    f32 end = 0.0f;
    f32 density = 0.0f;
    // Volumetric only: the height band the fog fades in over, and the outer
    // radial band around the camera.
    f32 heightTop = 0.0f;
    f32 heightBottom = 0.0f;
    f32 radialInner = 0.0f;
    f32 radialOuter = 0.0f;
    f32 radialStrength = 0.0f;
    bool everywhere = false;
};

// The linearisation IStateSync applies to the fog colour for the HD banks:
// x * (x * (0.305x + 0.682) + 0.0125).
f32 EngineFogLinear(f32 srgb);

// The fog mode a draw of `mat` runs: the frame's, unless the material is
// unfogged. SD PS permutations fold mode 4 into 0 themselves.
i32 DrawFogMode(const FogParams& fog, const MatParams& mat);

struct FrameInputs {
    Matrix44f world = Matrix44f::identity();
    Matrix44f view = Matrix44f::identity();
    Matrix44f projection = Matrix44f::identity();

    FogParams fog;

    Vector4f viewportRect = {1, 1, 0, 0};

    f32 effectTime = 0.0f;
    i32 numLights = 0;

    // First bone of the draw's palette in VS t16 (the BONE_BUFFER permutation).
    i32 boneBufferBase = 0;

    i32 useNdf = 1;

    // IBL mip COUNT of each probe (log2(max(w,h)) + 1), 0 when none is bound.
    f32 envFromMipCount = 0.0f;
    f32 envToMipCount = 0.0f;
    f32 envTransitionT = 0.0f;

    MainLight mainLight;

    ShaderTexMtx texMtx0 = {{Vector4f{1, 0, 0, 0}, Vector4f{0, 1, 0, 0}}};
    ShaderTexMtx texMtx1 = {{Vector4f{1, 0, 0, 0}, Vector4f{0, 1, 0, 0}}};

    // The SD highspec VS's per-vertex light palette.
    ShaderLight lights[kMaxLights] = {};

    // Parallel to `lights`: the KLBC ambient colour of each selected light. The
    // draw trace hashes it with the palette.
    Vector3f lightAmbientColors[kMaxLights] = {};
};

void BuildSdVsCbA(SdVsCbA& out, const FrameInputs& in, const MatParams& mat);
void BuildSdPsCbA(SdPsCbA& out, const FrameInputs& in, const MatParams& mat);

// The HD mesh banks, shared by HD, Crystal and SD_on_HD (and PopcornFX, which
// overrides the lanes it reads differently).
void BuildHdVsCb(HdVsCb& out, const FrameInputs& in, const MatParams& mat);
void BuildHdPsCb(HdPsCb& out, const FrameInputs& in, const MatParams& mat);

// The engine's material blend-mode number (CGxDevice+1563), which the 3.0.0
// pixel shaders switch their fog application on.
i32 EngineBlendMode(GxMatAlpha alpha);

void PackBone(ShaderBone& out, const Matrix44f& m);
void BuildBonePalette(BonePaletteCb& out, const Matrix44f* src, i32 numBones);
void PackBoneVertex(BoneVertex& out, const i32 indices[4], const f32 weights[4]);

inline ShaderTexMtx IdentityTexMtx() {
    return {{Vector4f{1, 0, 0, 0}, Vector4f{0, 1, 0, 0}}};
}

ShaderTexMtx ComposeTexAnimMatrix(const Quaternion& rot, const Vector3f& scale,
                                  const Vector3f& trans);

} // namespace whiteout::flakes::renderer::bls
