#pragma once

// ============================================================================
// M2 model particles — `CModelParticle`.
//
// Same pool, same integration, same birth/death channel as PE1: the sim does
// not know a model particle from a billboard, and the actor tree does not know
// it from an attachment. Two things are genuinely different, and both come
// straight out of `CParticleEmitter2::RenderParticle(CModelParticle&)`
// @0x1016a35d0:
//
//  - a model particle has an ORIENTATION, tumbling about a per-particle axis
//    drawn at birth (`CreateParticle(CModelParticle&)` @0x1016a08e0), and
//  - its SIZE comes off the emitter's scale track every frame rather than from
//    one fixed multiplier the way PE1's `childScale` does.
//
// What is deliberately not carried, because the actor layer has no channel for
// it: the per-particle diffuse colour and alpha the client pushes onto the
// child model (`CM2Model::SetDiffuse` / `+544`). Twinkle's blink IS carried,
// through the event's visibility, because hiding an actor is something the
// actor layer already does.
// ============================================================================

#include "child_model_emitter.h"

#include <vector>

namespace whiteout::flakes::renderer::particle {

class ModelParticleEmitter final : public ChildModelEmitter {
public:
    using ChildModelEmitter::ChildModelEmitter;

protected:
    Matrix44f TransformFor(u32 poolIndex) const override;
    f32 VisibilityFor(u32 poolIndex) const override;

    void OnPoolResized(usize capacity) override;
    void OnParticleBorn(u32 poolIndex) override;

private:
    // Per-particle rotation state. Index-parallel with the pool for the same
    // reason the child handles are: Particle2 is frozen at 32 bytes.
    struct Spin {
        // Angular velocity, radians/s, in the particle's own frame.
        Vector3f omega{0, 0, 0};
        // The emitter's orientation at birth, which is what a world-space
        // particle keeps. Identity for a model-space one, which reads the
        // emitter's live basis at placement instead — exactly the split the
        // client makes.
        Matrix44f basis = Matrix44f::identity();
    };
    std::vector<Spin> spin_;
};

} // namespace whiteout::flakes::renderer::particle
