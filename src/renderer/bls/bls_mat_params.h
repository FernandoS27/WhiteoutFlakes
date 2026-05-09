#pragma once

#include "common_types.h"
#include "bls_permuter.h"
#include "model/model_types.h"
#include "particle/particle_material.h"
#include "types.h"

namespace whiteout::flakes::renderer::bls {

enum class GxMatAlpha : u8 {
    Opaque     = 0,
    AlphaKey   = 1,
    Blend      = 2,
    Add        = 3,
    Modulate   = 4,
    Modulate2X = 5,
};

inline constexpr u32 kDisableLighting   = 0x01;
inline constexpr u32 kDisableFog        = 0x02;
inline constexpr u32 kDisableDepthTest  = 0x04;
inline constexpr u32 kDisableDepthWrite = 0x08;
inline constexpr u32 kDisableCull       = 0x10;
inline constexpr u32 kDisableBit5       = 0x20;
inline constexpr u32 kDisableBit8       = 0x100;

struct MatParams {
    GxShaderID shaderId     = GxShaderID::SD;
    GxMatAlpha alpha        = GxMatAlpha::Opaque;
    u32        disables     = 0;
    Vector4f   diffuseColor = {1, 1, 1, 1};
    Vector4f   vertexPad    = {-3.4028235e38f, 1.0f, 0.0f, 0.0f};
    f32        cornEffectsScale = 1.0f;
    f32        emissiveGain = 0.0f;
    u32        spriteFlags  = 0;

    f32        inverseSoftness = 1.0f;
    f32        cloakAmount     = 0.0f;
    f32        fresnelTeamColor = 0.0f;
    Vector3f   fresnelColor    = {0.0f, 0.0f, 0.0f};
    f32        fresnelOpacity  = 0.0f;

    bool LightingEnabled() const { return (disables & kDisableLighting)   == 0; }
    bool FogEnabled()      const { return (disables & kDisableFog)        == 0; }
    bool DepthTestEnabled()const { return (disables & kDisableDepthTest)  == 0; }
    bool DepthWriteEnabled()const{ return (disables & kDisableDepthWrite) == 0; }
    bool CullEnabled()     const { return (disables & kDisableCull)       == 0; }

    bool ColorWriteEnabled()const{ return (disables & kDisableBit8)       == 0; }
};

GxMatAlpha FilterToGxAlpha(i32 filterMode);
u32        FilterToDisables(i32 filterMode);

MatParams FromMdxLayer(i32 filterMode, i32 matFlags, GxShaderID shaderId);

MatParams FromParticleDesc(const particle::ParticleMaterialDesc& desc,
                           GxShaderID shaderId);

}
