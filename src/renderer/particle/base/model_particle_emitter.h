#pragma once

// ============================================================================
// M2 model particles — `CModelParticle`. PE1's pool, integration and birth/death
// channel, plus what `CParticleEmitter2::RenderParticle(CModelParticle&)`
// @0x1016a35d0 adds: a tumbling per-particle ORIENTATION, and a SIZE off the
// scale track every frame. Twinkle's blink is carried; the per-particle diffuse
// colour is not. See M2_PARTICLE_DESIGN.md §11.7.
// ============================================================================

#include "renderer/particle/base/child_model_emitter.h"

#include <vector>

namespace whiteout::flakes::renderer::particle {

class ModelParticleEmitter final : public ChildModelEmitter {
public:
    using ChildModelEmitter::ChildModelEmitter;

protected:
    Matrix44f TransformFor(u32 poolIndex) const override;
    f32 VisibilityFor(u32 poolIndex) const override;
    /// An `.m2`, which the child-template cache cannot build.
    ChildModelEvent::Route BirthRoute() const override {
        return ChildModelEvent::Route::ModelParticle;
    }

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
