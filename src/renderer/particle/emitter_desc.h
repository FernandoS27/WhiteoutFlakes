#pragma once

// ============================================================================
// EmitterDesc — the immutable half of an emitter.
//
// Everything here is fixed at load time and identical for every actor spawned
// from the same model, so it is built once per ModelTemplate and shared by
// shared_ptr. Only the animated state (emission rate, speed, cone, plane size,
// visibility, transform) plus the pool and RNG live on the emitter instance.
//
// This is also the seam the other formats target: an M2 or M3 adapter fills in
// the same struct, so nothing downstream of here knows which format a given
// emitter came from.
// ============================================================================

#include "particle_curve.h"
#include "particle_material.h"
#include "particle_motion.h"
#include "particle_shape.h"
#include "types.h"
#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/coordinate_system.h"

#include <memory>
#include <string>

namespace whiteout::flakes::renderer {
struct ParticleEmitterConfig;
}

namespace whiteout::flakes::renderer::particle {

// Sprite-sheet layout. `log2Cols` and the reciprocals are derived once at build
// time because the geometry builder needs them per particle per frame.
struct SpriteSheet {
    u32 rows = 1;
    u32 cols = 1;
    u32 log2Cols = 0;
    f32 ooWidth = 1.0f;
    f32 ooHeight = 1.0f;

    void Set(u32 r, u32 c) {
        rows = (r > 0) ? r : 1;
        cols = (c > 0) ? c : 1;
        ooWidth = 1.0f / static_cast<f32>(cols);
        ooHeight = 1.0f / static_cast<f32>(rows);
        log2Cols = 0;
        for (u32 n = cols; n > 1; n >>= 1)
            ++log2Cols;
    }
};

// What a live particle *is* at frame time. Orthogonal to the spawn shape: a
// cone that emits child models is ConeShape + ChildModel, not a subclass of
// both. Ray / Mesh / Light slot in here without touching the sim.
enum class ParticleOutput : u8 { Billboard = 0, ChildModel = 1 };

// How an emitter releases particles. WC3 is Continuous plus an optional
// one-shot burst when its emission rate crosses zero ("squirt"); M3's
// emit-N-total is the third mode this leaves room for.
struct EmissionDesc {
    enum class Mode : u8 { Continuous, Burst };
    Mode mode = Mode::Continuous;
    bool squirtAtStart = false;
};

struct EmitterDesc {
    // Where particles are born and which way they head. Stateless and shared;
    // the animated half travels in SpawnParams.
    std::shared_ptr<const IParticleShape> shape;

    ParticleOutput output = ParticleOutput::Billboard;

    // Child-model output only: which model each particle becomes, and its scale.
    std::string childModelPath;
    f32 childScale = 1.0f;

    // How particles are released, and how they move once released.
    EmissionDesc emission;
    MotionDesc motion;

    SpriteSheet sheet;

    // Colour / alpha / size / cell animation over normalised particle age.
    LifetimeCurves curves;

    f32 lifeSpan = 0.0f;
    // Initial longitudinal sweep. Seeds SpawnParams::longitude at registration;
    // PE1 animates it per frame, PE2 never does.
    f32 longitude = 6.2831853071795864769f;
    f32 tailLength = 1.0f;
    f32 angularVelocity = 0.0f;

    bool hasHead = true;
    bool hasTail = false;
    bool sortZ = false;
    bool modelSpace = false;
    bool xyQuads = false;

    i32 priorityPlane = 0;
    ParticleMaterialDesc material;
    CoordSpace coordSpace = kDefaultCoordSpace;
};

} // namespace whiteout::flakes::renderer::particle
