#pragma once

#include "whiteout/flakes/types.h"

#include <array>

namespace whiteout::flakes::renderer::bls {

enum class GxShaderID : u8 {
    SD                     = 0x00,
    HD                     = 0x01,
    SD_on_HD               = 0x02,
    Terrain                = 0x03,
    Water                  = 0x04,
    Fog                    = 0x05,
    Foliage                = 0x06,
    FoliagePush            = 0x07,
    Sprite                 = 0x08,
    DebugTexture           = 0x09,
    DepthOfField           = 0x0A,
    BloomCombine           = 0x0B,
    BloomExtract           = 0x0C,
    GaussianBlur           = 0x0D,
    Tonemap                = 0x0E,
    Movie                  = 0x0F,
    FFXCMAAEdge0           = 0x10,
    FFXCMAAEdge1           = 0x11,
    FFXCMAAEdgeCombine     = 0x12,
    FFXCMAAProcessAndApply = 0x13,
    CornFx              = 0x14,
    ConeIndicator          = 0x15,
    CliffBlightMiscTerrain = 0x16,
    Distortion             = 0x17,
    Crystal                = 0x18,
    Imgui                  = 0x19,
};

struct RenderState {
    GxShaderID shaderId       = GxShaderID::Sprite;

    u32        materialFlags  = 0;
    u8         alphaMode      = 0;
    bool       teamColor      = false;
    bool       debugShader    = false;

    bool       lightingEnabled = false;
    u8         numLights      = 0;
    u8         fogStyle       = 0;
    bool       fogEnabled     = false;
    bool       depthWrite     = false;
    bool       shadows        = false;
    bool       prepass        = false;
    bool       darkerShadows  = false;

    u8         numColors      = 0;
    u8         numTexCoords   = 0;
    u8         numTangents    = 0;
    u8         numWeights     = 0;

    bool       clampBloomOutput = false;
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
PermuteCounts  ExpectedPermuteCounts(GxShaderID id);

}
