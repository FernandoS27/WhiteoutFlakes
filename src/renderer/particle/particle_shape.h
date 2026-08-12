#pragma once

// ============================================================================
// Spawn shapes — where a particle is born and which way it heads.
//
// Shapes are stateless and immutable, so they live on the shared EmitterDesc
// and cost nothing per actor. Everything animated (plane size, cone angle,
// speed) arrives per sample in SpawnParams, which is what the emitter fills in
// from its frame state.
//
// Keeping this axis separate from the output axis is what stops the type
// matrix from multiplying: a cone that emits child models is ConeShape plus a
// child-model output, not a ConeChildModelEmitter.
// ============================================================================

#include "particle2.h"
#include "rnd_seed.h"
#include "types.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::particle {

// A value plus the spread it is randomised over at spawn. WC3's
// speed/variation pair is exactly this; M2 and M3 randomise many more
// properties the same way.
template <class T>
struct Ranged {
    T base{};
    T variance{};
};

// The animated inputs to one spawn. Each shape reads the subset it has.
struct SpawnParams {
    f32 width = 0.0f;    // plane extent along local X
    f32 height = 0.0f;   // plane extent along local Y
    f32 latitude = 0.0f;  // cone half-angle from local Z
    f32 longitude = 0.0f; // sweep around local Z
    Ranged<f32> speed{0.0f, 0.1f};
};

struct SpawnSample {
    Vector3f localPos{0, 0, 0};
    Vector3f localVel{0, 0, 0};
};

// Speed draw, shared by every shape. It comes off the same stream as the
// shape's own angle draws and WC3 takes it *after* them, so the shape decides
// when it happens rather than being handed a pre-drawn value — the ordering is
// observable in the particle trace.
inline f32 DrawSpeed(RndSeed& rnd, const SpawnParams& p) {
    return p.speed.base * (1.0f + CRandom::reals_(rnd) * p.speed.variance);
}

class IParticleShape {
public:
    virtual ~IParticleShape() = default;
    virtual void Sample(SpawnSample& out, const SpawnParams& p, RndSeed& rnd) const = 0;
};

// Rectangle in the local XY plane, emitting into a latitude/longitude cone.
// WC3's "line emitter" is this with longitude collapsed to zero — kept as a
// value rather than a separate LineShape because a real LineShape would skip
// the longitude draw and shift every subsequent random number.
class PlaneShape final : public IParticleShape {
public:
    void Sample(SpawnSample& out, const SpawnParams& p, RndSeed& rnd) const override;
};

// Point origin emitting into a latitude/longitude cone — what child-model
// particles spawn from.
class ConeShape final : public IParticleShape {
public:
    void Sample(SpawnSample& out, const SpawnParams& p, RndSeed& rnd) const override;
};

} // namespace whiteout::flakes::renderer::particle
