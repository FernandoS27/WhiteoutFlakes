#pragma once

#include "common_types.h"
#include "types.h"

namespace WhiteoutDex {

struct ParticleEmitterConfig {
    i32   textureId    = -1;
    i32   filterMode   = 0;
    i32   rows = 1, cols = 1;
    bool  unshaded     = false;

    f32   lifeSpan     = 1.0f;
    bool  squirt       = false;

    Vector3f startColor  = {1,1,1};
    Vector3f midColor    = {0.5f,0.5f,0.5f};
    Vector3f endColor    = {0,0,0};
    f32 startAlpha = 255, midAlpha = 128, endAlpha = 0;
    f32 startScale = 10, midScale = 10, endScale = 10;
    f32 midTime    = 0.5f;

    i32   particleType = 1;
    f32   tailLength   = 1.0f;

    i32 headLifeStart=0, headLifeEnd=0, headLifeRepeat=1;
    i32 headDecayStart=0, headDecayEnd=0, headDecayRepeat=1;
    i32 tailLifeStart=0, tailLifeEnd=0, tailLifeRepeat=1;
    i32 tailDecayStart=0, tailDecayEnd=0, tailDecayRepeat=1;

    bool modelSpace  = false;
    bool xyQuad      = false;
    bool sortZ       = false;
    bool lineEmitter = false;
    bool unfogged    = false;

    i32  count         = 0;
    i32  priorityPlane = 0;
    i32  replaceableId = 0;
};

struct ParticleEmitterState {
    Matrix44f transform = Matrix44f::identity();
    f32 emissionRate = 0;
    f32 speed        = 0;
    f32 variation    = 0;
    f32 coneAngle    = 0;
    f32 gravity      = 0;
    f32 width        = 0;
    f32 length       = 0;
    f32 visibility   = 1.0f;
};

}
