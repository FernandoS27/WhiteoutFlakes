#pragma once

#include "common_types.h"
#include "particle2.h"
#include "particle_key.h"
#include "particle_material.h"
#include "particle_pool.h"
#include "rnd_seed.h"
#include "coordinate_system.h"
#include "types.h"

namespace whiteout::flakes::renderer::particle {

void  SetGlobalEmissionScaler(f32 s);
f32   GetGlobalEmissionScaler();

enum EmitterFlag : u32 {
    kFlagVisible        = 0x001,
    kFlagEnabled2       = 0x002,
    kFlagHasHead        = 0x004,
    kFlagHasTail        = 0x008,
    kFlagSortZ          = 0x010,
    kFlagNeedSquirt     = 0x020,
    kFlagUpdated        = 0x040,
    kFlagPaused         = 0x080,
    kFlagSystemDead     = 0x100,
    kFlagUseModelSpace  = 0x200,
    kFlagXYQuads        = 0x400,
    kFlagUpdatedByAnim  = 0x800
};

enum class EmitterType : u32 {
    Base  = 0,
    Plane = 1
};

class Emitter2 {
public:
    Emitter2();
    virtual ~Emitter2() = default;

    void SetVisible(bool v)             { SetFlag(kFlagVisible, v); }
    void SetEnabled2(bool v)            { SetFlag(kFlagEnabled2, v); }
    void SetSortZ(bool v)               { SetFlag(kFlagSortZ, v); }
    void SetUseModelSpace(bool v)       { SetFlag(kFlagUseModelSpace, v); }
    void SetXYQuads(bool v)             { SetFlag(kFlagXYQuads, v); }
    void SetSquirtPending(bool v)       { SetFlag(kFlagNeedSquirt, v); }

    void SetEmissionRate(f32 v)         { emissionRate_ = v; }
    void SetLifeSpan(f32 v)             { lifeSpan_ = v; }
    void SetVelocity(f32 v)             { velocity_ = v; }
    void SetAcceleration(f32 v)         { acceleration_ = v; }
    void SetVelocityVariation(f32 v)    { velocityVariation_ = v; }
    void SetAngularVelocity(f32 v)      { angularVelocity_ = v; }
    void SetTailLength(f32 v)           { tailLength_ = v; }

    void SetParticleStyle(bool hasHead, bool hasTail, f32 tailLength);
    void SetTextureDimensions(u32 rows, u32 cols);
    void SetMaterial(const ParticleMaterialDesc& d) { material_ = d; }
    void SetKey(i32 index, const ParticleKey& k);
    void SetPriorityPlane(i32 p)        { priorityPlane_ = p; }
    void SetReplaceableId(i32 id)       { replaceableId_ = id; }
    void SetCoordSpace(CoordSpace s)    { coordSpace_ = s; }

    void Squirt()                       { flags_ |= kFlagNeedSquirt; }
    void SetDead()                      { flags_ |= kFlagSystemDead; }
    void Flush();

    void SetModelToWorld(const Matrix44f& m) { modelToWorld_ = m; }

    void Update(f32 elapsed);

    void Update(f32 elapsed, const Matrix44f& worldMatrix);

    bool Enabled() const                { return (flags_ & (kFlagVisible | kFlagEnabled2)) == (kFlagVisible | kFlagEnabled2); }
    bool IsDead() const                 { return (flags_ & kFlagSystemDead) != 0; }
    bool Visible() const                { return (flags_ & kFlagVisible) != 0; }
    bool HasHead() const                { return (flags_ & kFlagHasHead) != 0; }
    bool HasTail() const                { return (flags_ & kFlagHasTail) != 0; }
    bool SortZ() const                  { return (flags_ & kFlagSortZ) != 0; }
    bool UseModelSpace() const          { return (flags_ & kFlagUseModelSpace) != 0; }
    bool XYQuads() const                { return (flags_ & kFlagXYQuads) != 0; }
    u32  Flags() const                  { return flags_; }

    EmitterType Type() const            { return type_; }
    CoordSpace  GetCoordSpace() const   { return coordSpace_; }

    f32  LifeSpan() const               { return lifeSpan_; }
    f32  EmissionRate() const           { return emissionRate_; }
    f32  Velocity() const               { return velocity_; }
    f32  Acceleration() const           { return acceleration_; }
    f32  VelocityVariation() const      { return velocityVariation_; }
    f32  AngularVelocity() const        { return angularVelocity_; }
    f32  TailLength() const             { return tailLength_; }
    i32  PriorityPlane() const          { return priorityPlane_; }
    i32  ReplaceableId() const          { return replaceableId_; }

    u32  TextureRows() const            { return textureRows_; }
    u32  TextureCols() const            { return textureCols_; }
    u32  TextureLog() const             { return textureLog_; }
    f32  OoTextureWidth() const         { return ooTextureWidth_; }
    f32  OoTextureHeight() const        { return ooTextureHeight_; }

    const ParticleKey& Key(i32 i) const { return keys_[i]; }
    const ParticleMaterialDesc& Material() const { return material_; }
    const Matrix44f& ModelToWorld() const { return modelToWorld_; }

    const ParticlePool& Pool() const    { return pool_; }
    ParticlePool&       Pool()          { return pool_; }

    i32 TotalAlive() const              { return static_cast<i32>(pool_.AliveCount()); }

protected:

    virtual void CreateParticle(Particle2& p, f32 elapsed) = 0;

    f32 CalcVelocity();

    void SetFlag(u32 mask, bool on) {
        if (on) flags_ |= mask; else flags_ &= ~mask;
    }

    void MoveParticle(Particle2& p, f32 elapsed) const;

    void InternalUpdate(f32 elapsed);

    void Sync() { pool_.Sync(emissionRate_, lifeSpan_); }

protected:
    EmitterType type_                    = EmitterType::Base;

    u32 flags_                           = kFlagEnabled2 | kFlagHasHead;
    CoordSpace coordSpace_               = kDefaultCoordSpace;

    f32 emissionRate_                    = 0.0f;
    f32 lifeSpan_                        = 0.0f;
    f32 tailLength_                      = 1.0f;
    f32 velocity_                        = 0.0f;
    f32 acceleration_                    = 0.0f;
    f32 velocityVariation_               = 0.1f;
    f32 angularVelocity_                 = 0.0f;

    f32 numNew_                          = 0.0f;

    u32 textureRows_                     = 1;
    u32 textureCols_                     = 1;
    u32 textureLog_                      = 0;
    f32 ooTextureWidth_                  = 1.0f;
    f32 ooTextureHeight_                 = 1.0f;

    ParticleKey keys_[2];

    i32 replaceableId_                   = 0;
    i32 priorityPlane_                   = 0;

    ParticleMaterialDesc material_;

    Matrix44f modelToWorld_              = Matrix44f::identity();

    RndSeed randSeed_;

    ParticlePool pool_;
};

}
