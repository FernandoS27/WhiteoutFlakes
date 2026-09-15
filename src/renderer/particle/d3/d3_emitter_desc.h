#pragma once

// ============================================================================
// D3EmitterDesc — one `.prt` in the form the simulation reads: the immutable
// half, shared by every actor spawned from the model. A separate type because D3
// shares none of `EmitterDesc`'s subjects. See D3_PARTICLE_DESIGN.md §12.1, §30.7.
// ============================================================================

#include "renderer/particle/d3/d3_channels.h"
#include "renderer/particle/d3/d3_particle_material.h"
#include "renderer/particle/d3/d3_path.h"
#include "renderer/particle/output/particle_material.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <array>

namespace whiteout::flakes::renderer::particle::d3 {

struct EmitterDesc {
    // ---- system ----
    SystemType systemType = SystemType::Standard;
    u32 prtFlags = 0; ///< `PrtFlag` bits
    PrtRenderMode renderMode = PrtRenderMode::CameraGated;

    /// Seconds, converted once from the engine's 60 fps frame counts. @ref lifetime
    /// is the emitter clock's PERIOD and, without @ref prtFlags bit 0, the expiry.
    /// @ref emissionPeriod is the post-stop wind-down a viewer never sends, so
    /// nothing reads it; the tests set it to prove that (§19.1, §30.7).
    f32 lifetime = 0.0f;
    f32 emissionPeriod = 0.0f;
    f32 preSimulate = 0.0f;

    /// `Particle.tLifetimeRandom`, an InterpolationScalar that SCALES
    /// @ref lifetime once at spawn (`ParticleSystem_Spawn` @0x71000AC670), moving
    /// expiry and emitter clock together. Only mode 10 is answerable (§4.1, §30.7).
    Driver lifetimeRandom{};

    /// Kill radius, and the normalising divisor for driver mode 3.
    f32 maxDistance = 10.0f;
    /// Camera-relative placement scale, and the divisor for driver mode 6.
    f32 cameraDistScale = 0.8f;

    // ---- the wind spring (system types 6 and 8 only) ----
    f32 swayFrequency = 1.0f;
    f32 swayDamping = 0.3f;
    f32 swayMaxOffset = 1.0f;
    f32 swayGustAmount = 1.25f;
    f32 swayBaseAmount = 0.0f;

    // ---- emitter shape ----
    Shape shape = Shape::Point;
    Path shapeExtent0;
    Path shapeExtent1;
    Path shapeExtent2;

    /// Every channel, indexed by ENGINE CHANNEL ID. Slot 0 and the gaps
    /// (4, 26, 27) stay empty and evaluate to zero, which is what an absent
    /// channel does anyway.
    std::array<Path, kChannelIdCount> channels;

    /// Derived at load exactly the way `ParticleSystem_Spawn` derives it: ask
    /// each channel whether its value range is non-trivial. Without this the
    /// step function integrates five motion models for every particle of every
    /// asset; with it the typical asset runs one.
    u32 caps = 0;
    /// Channel 23 takes one value whatever the seed and time — absent, or one
    /// node with no driver and start == end. Bit 0x200 of the constancy mask
    /// `ParticleSystem_Spawn` stores at `sys+0x27C`: a constant axis is read
    /// once at birth, a varying one every step.
    bool spinAxisConstant = true;

    /// What the draw list carries: one texture id and a blend class, filled
    /// from @ref d3mat once its layers have actor texture ids. The dialects
    /// share this struct, so it is the narrow view of the material and
    /// @ref d3mat is the whole of it.
    ParticleMaterialDesc material;
    /// The four stage binds, the pass state and the per-layer UV transforms.
    MaterialDesc d3mat;
    i32 priorityPlane = 0;

    /// The `.prt`'s own SNO id, for diagnostics and the trace.
    i32 snoId = -1;
    /// Actor spawned per particle, if any (4,796 of 21,593 files set one).
    /// Reported through `ChildModelEvent`; the host owns the spawn.
    i32 snoActor = -1;
    /// That actor's tags 65543 (scale) and 65544 (a random width added to it),
    /// which the emission multiplies into the child's size. The defaults are the
    /// tag table's own (@0x7101065730), for an actor that carries neither.
    f32 actorScale = 1.0f;
    f32 actorScaleRandom = 0.0f;

    /// @brief Does an emission of this system produce a whole ACTOR rather than
    ///        a particle? Types 1, 3 and 4 with an actor — the
    ///        `(type - 3) < 2 || type == 1` branch of `ParticleSystem_EmitParticle`,
    ///        which never touches the pool. See §5.6a.
    bool SpawnsChildActors() const {
        return snoActor >= 0 && EmitsActors(systemType);
    }
    /// @brief Do this system's particles come off the owning model's surface?
    ///
    /// Shapes 6, 7 and 11 — 1,454 shipped files. The host builds the surface
    /// only for an emitter that says yes here.
    bool SamplesModelSurface() const {
        return shape == Shape::MeshRandom || shape == Shape::MeshActorKind4 ||
               shape == Shape::MeshSequential;
    }

    bool Has(PrtFlag f) const {
        return d3::Has(prtFlags, f);
    }

    const Path& Channel(i32 id) const {
        return channels[(id >= 0 && id < kChannelIdCount) ? static_cast<usize>(id) : 0];
    }
    bool Has(i32 id) const {
        return !Channel(id).nodes.empty();
    }
    bool Cap(u32 bit) const {
        return (caps & bit) != 0;
    }

    /// Recompute @ref caps from the channels. Called by the adapter once the
    /// paths are in place; separated so a hand-built desc in a test gets the
    /// same treatment as a parsed one.
    void DeriveCapabilities();
};

} // namespace whiteout::flakes::renderer::particle::d3
