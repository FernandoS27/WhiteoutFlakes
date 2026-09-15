#pragma once

// ============================================================================
// D3EmitterDesc — one `.prt` in the form the simulation reads.
//
// The immutable half, shared by every actor spawned from the model, exactly
// like `EmitterDesc` is for the WC3/WoW dialect. It is a separate type rather
// than more fields on `EmitterDesc` because D3 shares none of that struct's
// subjects: no lifetime curve triple, no motion params, no spawn shape object,
// no sprite sheet. See D3_PARTICLE_DESIGN.md §12.1.
// ============================================================================

#include "d3_channels.h"
#include "d3_particle_material.h"
#include "d3_path.h"
#include "particle_material.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <array>

namespace whiteout::flakes::renderer::particle::d3 {

struct EmitterDesc {
    // ---- system ----
    SystemType systemType = SystemType::Standard;
    u32 prtFlags = 0; ///< `PrtFlag` bits
    PrtRenderMode renderMode = PrtRenderMode::CameraGated;

    /// Seconds. The engine stores frame counts at 60 fps and multiplies all
    /// three by 0.016667 on load; do the conversion once, here.
    ///
    /// @ref lifetime is the emitter clock's PERIOD — every emitter channel is
    /// sampled at `elapsed / lifetime` — and, when @ref prtFlags lacks bit 0,
    /// also the system's expiry. @ref emissionPeriod is NOT a loop length: it
    /// is the wind-down the engine runs after a stop request, which a viewer
    /// never sends, so nothing here reads it. The tests set it to prove that
    /// nothing mistakes it for the period.
    f32 lifetime = 0.0f;
    f32 emissionPeriod = 0.0f;
    f32 preSimulate = 0.0f;

    /// `Particle.tLifetimeRandom`, an InterpolationScalar that SCALES
    /// @ref lifetime once at spawn: `ParticleSystem_Spawn` @0x71000AC670 runs
    /// `InterpolationScalar_Evaluate` on it and multiplies the result into the
    /// one stored lifetime, so both the expiry and the emitter clock move
    /// together.
    ///
    /// Mode 0 is "disabled" and the evaluate leaves 1.0 standing — 21,492 of
    /// 21,593 shipped files. Of the 101 that do drive it, **47 use mode 10,
    /// uniform random**, which is the only mode a viewer can answer; the other
    /// 54 (modes 1, 2, 4, 8) read live actor and game state. See
    /// `InterpolationDriver_Evaluate` @0x7100374760.
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

    /// @brief Does an emission of this system produce a whole ACTOR rather than
    ///        a particle?
    ///
    /// `ParticleSystem_EmitParticle` opens on `(type - 3) < 2 || type == 1` —
    /// types 1, 3 and 4 — and that branch never touches the particle pool: it
    /// builds a 352-byte record on the STACK, runs the simulation over it once,
    /// and hands the result to `Actor_SpawnFromSno`. The corpus agrees exactly:
    /// all 4,795 files of those three types set `snoActor` and only one other
    /// file in 21,593 does (`banner_treasureGoblin_glow.prt`, type 0, where the
    /// field is never read).
    ///
    /// So "eSystemType 1 is a ribbon" — which is what the first RE pass read
    /// into the name `cos_wings_*` — is wrong. Type 1 is 4,790 files that each
    /// spawn a MODEL, and the 176-byte segment record that reading was built on
    /// belongs to type 9, the weather systems.
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
