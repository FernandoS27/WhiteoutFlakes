#pragma once

// ============================================================================
// TrailSet — the emitters that trail every live particle of their owner.
//
// M2 RPID. The client's recursion model owns the child emitters its parent
// borrows pointers to; here the owner holds them outright, adopts at most four
// (MAX_CHILD_EMITTERS, ParticleSystem2.cpp:2503), and is the only thing that
// drives them. A trail's own trails are dropped at load, so the set is at most
// one level deep. See M2_TRAIL_EMITTER_DESIGN.md.
// ============================================================================

#include "particle_output.h"
#include "types.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <memory>
#include <vector>

namespace whiteout::flakes::renderer::particle {

class Emitter2;
struct Particle2;

/// The animated state a trail runs on: the fields `Emitter2::ApplyState`
/// reads, and nothing else.
///
/// A trail has no tracks of its own — the model its record came from is never
/// placed, so nothing walks them — so its record's constant state is kept here
/// and re-applied every frame with the four fields that belong to the OWNING
/// actor patched in. Carrying these rather than a whole `ParticleFrameState`
/// keeps the StarCraft II block out of a per-frame, per-trail copy.
struct TrailState {
    Matrix44f transform = Matrix44f::identity();
    f32 emissionRate = 0.0f;
    f32 speed = 0.0f;
    f32 variation = 0.0f;
    f32 coneAngle = 0.0f;
    f32 gravity = 0.0f;
    f32 width = 0.0f;
    f32 length = 0.0f;
    f32 visibility = 0.0f;
    bool squirting = false;
    Vector3f gravityVector{0, 0, 0};
    bool hasGravityVector = false;
    f32 lifeSpan = 0.0f;
    f32 horizontalRange = 0.0f;
    f32 zSource = 0.0f;
    f32 modelAlpha = 1.0f;
    Vector3f worldPosition{0, 0, 0};
    f32 unitScale = 1.0f;

    static TrailState From(const model::FrameState::ParticleFrameState& st);
};

class TrailSet {
public:
    static constexpr usize kMax = kMaxTrailsPerEmitter;

    TrailSet();
    ~TrailSet();
    TrailSet(const TrailSet&) = delete;
    TrailSet& operator=(const TrailSet&) = delete;

    /// Adopt one trail emitter. The fifth is dropped, as the client's assert
    /// intends.
    void Adopt(std::unique_ptr<Emitter2> trail);

    /// Hand every trail the owner's per-frame state. What the client's
    /// `Update` @0x1016a55a0 does before stepping: run UpdateXform over its
    /// children with the parent's world matrix.
    void ApplyOwner(const Emitter2& owner);

    /// One live particle of @p owner drives every trail's emission, at its
    /// own position.
    void Drive(const Emitter2& owner, const Particle2& p, f32 dt, f32 emissionScaler);

    /// Step every trail, after the owner's own particles have moved and driven
    /// them — exactly where `StepUpdate` @0x1016a95c0 recurses.
    void Step(f32 dt, f32 emissionScaler);

    /// Size every trail's pool for its own steady-state population times the
    /// owner's @p ownerCapacity.
    void GrowPools(u32 ownerCapacity);

    /// A rewind of the owner rewinds its trails.
    void Reset();

    bool Empty() const {
        return trails_.empty();
    }

    /// In adoption order. The service walks these to build their geometry and
    /// to count them; nothing else may drive them.
    const std::vector<std::unique_ptr<Emitter2>>& All() const {
        return trails_;
    }

private:
    std::vector<std::unique_ptr<Emitter2>> trails_;
};

} // namespace whiteout::flakes::renderer::particle
