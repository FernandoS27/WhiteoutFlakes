#pragma once

#include "whiteout/flakes/types.h"
#include "types.h"

namespace whiteout::flakes::renderer::bls {

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

struct ShaderLight {
    Vector4f ambient;
    Vector4f diffuse;
    Vector4f position;
    Vector4f _pad;
};
static_assert(sizeof(ShaderLight) == 64);

inline constexpr i32 kMaxLights = 8;
inline constexpr i32 kMaxBones  = 256;

struct SdVsCbA {
    Matrix44f    world;
    Matrix44f    worldViewProj;
    Vector4f     diffuseColor;
    ShaderTexMtx texMtx0;
    ShaderTexMtx texMtx1;
    ShaderLight  lights[kMaxLights];
};
static_assert(offsetof(SdVsCbA, world)         == 0x00);
static_assert(offsetof(SdVsCbA, worldViewProj) == 0x40);
static_assert(offsetof(SdVsCbA, diffuseColor)  == 0x80);
static_assert(offsetof(SdVsCbA, texMtx0)       == 0x90);
static_assert(offsetof(SdVsCbA, texMtx1)       == 0xB0);
static_assert(offsetof(SdVsCbA, lights)        == 0xD0);

inline u32 SdVsCbASize(i32 numLights) { return 208u + 64u * static_cast<u32>(numLights); }

struct SdPsCbA {
    f32      alphaRef;
    f32      _pad[3];
    Vector4f fogParams;
    Vector4f fogColor;
};
static_assert(sizeof(SdPsCbA) == 48);

struct HdVsCb {
    Matrix44f    world;
    Matrix44f    worldView;
    Matrix44f    worldViewProj;
    Vector4f     misc;
    Vector4f     diffuseColor;
    ShaderTexMtx texMtx0;
    ShaderTexMtx texMtx1;
};
static_assert(sizeof(HdVsCb) == 288);
static_assert(offsetof(HdVsCb, world)         == 0x000);
static_assert(offsetof(HdVsCb, worldView)     == 0x040);
static_assert(offsetof(HdVsCb, worldViewProj) == 0x080);
static_assert(offsetof(HdVsCb, misc)          == 0x0C0);
static_assert(offsetof(HdVsCb, diffuseColor)  == 0x0D0);
static_assert(offsetof(HdVsCb, texMtx0)       == 0x0E0);
static_assert(offsetof(HdVsCb, texMtx1)       == 0x100);

struct HdPsCb {
    f32         alphaRef;
    f32         _pad0[3];
    Vector4f    fogParams;
    Vector4f    fogColor;
    Matrix44f   worldView;
    Matrix44f   view;
    Matrix44f   projection;
    Vector4f    viewportRect;
    Vector4f    pixelParams1;
    Vector4f    pixelParams2;
    Vector4f    fresnelColor;
    Vector4f    envMapParams;
    f32         effectTime;
    f32         emissiveGain;
    f32         lightCount;
    f32         useNdf;
    ShaderLight lights[kMaxLights];
};
static_assert(offsetof(HdPsCb, lights) == 0x150);
static_assert(sizeof(HdPsCb) == 0x150 + 64 * kMaxLights);

inline u32 HdPsCbSize(i32 numLights) {
    return 336u + 64u * static_cast<u32>(numLights);
}

struct SdOnHdPsCb {
    f32         alphaRef;
    f32         _pad0[3];
    Vector4f    fogParams;
    Vector4f    fogColor;
    Vector4f    _pad3;
    Vector4f    _pad4;
    Vector4f    _pad5;
    Vector4f    _pad6;
    Vector4f    invViewRow0;
    Vector4f    invViewRow1;
    Vector4f    invViewRow2;
    Vector4f    _pad10;
    Vector4f    _pad11;
    Vector4f    _pad12;
    Vector4f    _pad13;
    Vector4f    _pad14;
    Vector4f    _pad15;
    Vector4f    pixelParams1;
    Vector4f    _pad17;
    Vector4f    _pad18;
    Vector4f    envMapParams;
    Vector4f    lightCountSlot;
    ShaderLight lights[kMaxLights];
};
static_assert(offsetof(SdOnHdPsCb, invViewRow0) == 0x070);
static_assert(offsetof(SdOnHdPsCb, envMapParams) == 0x130);
static_assert(offsetof(SdOnHdPsCb, lightCountSlot) == 0x140);
static_assert(offsetof(SdOnHdPsCb, lights) == 0x150);
static_assert(sizeof(SdOnHdPsCb) == 0x150 + 64 * kMaxLights);

inline u32 SdOnHdPsCbSize(i32 numLights) {
    return 336u + 64u * static_cast<u32>(numLights);
}

struct HdShadowCascadesCb {
    Matrix44f cascade0;
    Matrix44f cascade1;
    Matrix44f cascade2;
};
static_assert(sizeof(HdShadowCascadesCb) == 192);

struct SdOnHdShadowCascadeCountCb {
    f32 numCascades;
    f32 _pad[3];
};
static_assert(sizeof(SdOnHdShadowCascadeCountCb) == 16);

struct DebugVisCb {
    u32      enabledShaders;  f32      debugMode;
    f32      _p0[2];
    Vector3f overrideAlbedo;  f32      _p1;
    Vector3f overrideOrm;     f32      _p2;
};
static_assert(sizeof(DebugVisCb) == 48);

struct BonePaletteCb {
    ShaderBone bones[kMaxBones];
};
static_assert(sizeof(BonePaletteCb) == 12288);

}
