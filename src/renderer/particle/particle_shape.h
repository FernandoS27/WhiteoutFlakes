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
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <span>
#include <utility>
#include <vector>

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
//
// WoW's generators reuse the same two "area" floats for different meanings —
// rect width/length for a plane, min/max radius for a sphere, t0/t1 for a
// spline — so `width`/`height` carry those rather than the struct growing a
// field per generator. That reuse is the client's own, not a shortcut here.
struct SpawnParams {
    f32 width = 0.0f;    // plane extent along local X | sphere min radius | spline t0
    f32 height = 0.0f;   // plane extent along local Y | sphere max radius | spline t1
    f32 latitude = 0.0f;  // cone half-angle from local Z; WoW's "vertical range"
    f32 longitude = 0.0f; // sweep around local Z
    Ranged<f32> speed{0.0f, 0.1f};

    // WoW only. `horizontalRange` is the azimuth its generators draw over —
    // WC3 folds that into `longitude`, but WoW animates the two separately.
    f32 horizontalRange = 0.0f;
    // When above kMinZSource, velocity aims from (0, 0, zSource) through the
    // spawn point instead of being drawn from the angle ranges — which also
    // means two fewer random draws for that particle.
    f32 zSource = 0.0f;

    // The model's bone-emitter table, for the bone generator only. A view, not
    // storage: the emitter owns the copy and keeps it alive across the spawn.
    // Empty for every other generator, which is also what makes an unresolved
    // table a silent no-spawn rather than a crash.
    std::span<const ::whiteout::flakes::renderer::model::FrameState::BoneSpawn> boneTable;
};

// CGeneratorAniProp::MIN_ZSOURCE.
inline constexpr f32 kMinZSource = 0.001f;

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

// ---------------------------------------------------------------------------
// WoW generators. Separate classes rather than flags on the WC3 shapes because
// they draw different numbers of randoms in a different order, and the WC3
// shapes' draw order is pinned by the corpus traces.
// ---------------------------------------------------------------------------

// CPlaneGenerator @0x1016c6730. Uniform point in the centred W x L rectangle at
// z = 0; velocity from a polar/azimuth pair, or aimed from zSource.
class WowPlaneShape final : public IParticleShape {
public:
    void Sample(SpawnSample& out, const SpawnParams& p, RndSeed& rnd) const override;
};

// CSphereGenerator @0x1016c6f00. Radius drawn between the two area floats,
// direction from elevation/azimuth; velocity radial unless zSource or the
// hemisphere flag override it.
class WowSphereShape final : public IParticleShape {
public:
    explicit WowSphereShape(bool hemisphereUp = false) : hemisphereUp_(hemisphereUp) {}
    void Sample(SpawnSample& out, const SpawnParams& p, RndSeed& rnd) const override;

private:
    bool hemisphereUp_ = false;
};

// CBoneGeneratorBone @0x10169e140 + CBoneGeneratorBase @0x10169d7f0. Spawns
// along a randomly chosen bone of the model rather than in an area of its own,
// scattered radially in that bone's plane. The table it picks from is per
// MODEL, so it arrives through SpawnParams every frame instead of living on
// the desc.
class WowBoneShape final : public IParticleShape {
public:
    void Sample(SpawnSample& out, const SpawnParams& p, RndSeed& rnd) const override;
};

// CSplineGenerator @0x1016c7990. Position on a polyline through the record's
// points; velocity along the tangent, optionally rotated about it.
class WowSplineShape final : public IParticleShape {
public:
    explicit WowSplineShape(std::vector<Vector3f> points) : points_(std::move(points)) {}
    void Sample(SpawnSample& out, const SpawnParams& p, RndSeed& rnd) const override;

    // Position and tangent at normalised parameter t across the whole polyline.
    void Evaluate(f32 t, Vector3f& pos, Vector3f& tangent) const;

private:
    std::vector<Vector3f> points_;
};

} // namespace whiteout::flakes::renderer::particle
