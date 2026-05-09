#pragma once

#include "common_types.h"
#include "particle2_emitter.h"

namespace whiteout::flakes::renderer { struct ParticleEmitterConfig; }

namespace whiteout::flakes::renderer::particle {

struct PlaneEmitterInit {

    u32                   textureRows   = 1;
    u32                   textureCols   = 1;

    ParticleKey           keys[2];

    f32                   lifeSpan      = 0.0f;
    f32                   tailLength    = 1.0f;
    f32                   angularVelocity = 0.0f;
    bool                  hasHead       = true;
    bool                  hasTail       = false;

    bool                  sortZ         = false;
    bool                  modelSpace    = false;
    bool                  xyQuads       = false;

    f32                   longitude     = 6.2831853071795864769f;

    bool                  squirtAtStart = false;

    i32                   priorityPlane = 0;
    i32                   replaceableId = 0;
    ParticleMaterialDesc  material;

    CoordSpace            coordSpace    = kDefaultCoordSpace;
};

class PlaneEmitter;

void ApplyInit(PlaneEmitter& e, const PlaneEmitterInit& init);

PlaneEmitterInit InitFromLegacyConfig(const ParticleEmitterConfig& cfg);

class PlaneEmitter : public Emitter2 {
public:
    PlaneEmitter();

    void SetWidth(f32 w)     { width_ = w; }
    void SetHeight(f32 h)    { height_ = h; }
    void SetLatitude(f32 l)  { latitude_ = l; }
    void SetLongitude(f32 l) { longitude_ = l; }

    f32 Width() const        { return width_; }
    f32 Height() const       { return height_; }
    f32 Latitude() const     { return latitude_; }
    f32 Longitude() const    { return longitude_; }

protected:
    void CreateParticle(Particle2& p, f32 elapsed) override;

private:

    f32 width_     = 0.0f;
    f32 height_    = 0.0f;
    f32 latitude_  = 0.0f;
    f32 longitude_ = 0.0f;
};

}
