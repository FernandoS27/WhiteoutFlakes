#pragma once

// ============================================================================
// Particle dialect — which variant of Blizzard's CParticleEmitter2 a product
// runs.
//
// Warcraft III (Reforged) and World of Warcraft both ship a class named
// `CParticleEmitter2`, both out of a file named `ParticleSystem2.cpp`. They are
// one codebase evolved, not two systems: the fractional `m_numNew` accumulator,
// the squirt burst, the alive/dead index-stack pool with its 1.15 sizing
// headroom, the emit -> age -> kill -> move loop, the 32-byte particle, the
// sprite-cell `col & (cols-1)` / `cell >> log2(cols)` math and the
// depth-sort record are all common. WoW even re-purposed exactly the four bytes
// WC3 spent on `keyFrame` for its own per-particle lifespan variance and seed.
//
// So this is one simulation with a list of measured divergences. Each field
// below is one of them, named rather than switched on a game enum: a profile
// answers with a behaviour and `particle::Emitter2` reads it.
//
// Addresses are WoW 6.0.1.18179 (IDB `World of Warcraft.i64`, imagebase
// 0x100000000). Where this header and `C:\Projects\WoWRe\pseudocode_wow\`
// disagree, this header is right — see M2_PARTICLE_DESIGN.md "Answered
// questions", which corrected two errors in that reconstruction.
//
// Vocabulary only — no simulation state, no dependencies. Lives in core/ so
// `IRenderProfile` can return one without core depending on the particle
// module, exactly as core/ribbon_dialect.h does for ribbons.
// ============================================================================

#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::core {

/// How a frame's elapsed time is split into simulation steps.
enum class ParticleDtPolicy : u8 {
    /// WC3: clamp elapsed to [0, maxStepSeconds] and take one step.
    ClampToMax,
    /// WoW: `n = min(floor(dt/step), floor(lifespan/step))` steps of exactly
    /// `step`, then one remainder step of `dt - step*floor(dt/step)`. When the
    /// lifespan cap binds, the fixed steps cover less than dt and the rest is
    /// dropped on purpose — particles would not have outlived it.
    FixedSubSteps,
};

/// How a live particle's velocity and position advance.
enum class ParticleForceModel : u8 {
    /// WC3: `pos += v*dt + 0.5*g*dt^2; v += g*dt`, gravity a scalar down -Z.
    Wc3Gravity,
    /// WoW: forces precomputed once per frame (`CalculateForces` @0x1016a6710)
    /// then applied in a fixed order (`MoveParticle` @0x1016a1360) — wind
    /// added to velocity FIRST, displacement taken from the pre-gravity
    /// velocity, drag applied LAST as `v *= 1 - min(drag*dt, 1)`. The order and
    /// the clamp both matter; they are not the same curve as WC3's with extra
    /// terms, which is why these are two integrators rather than one
    /// parameterised one.
    WowForces,
};

struct ParticleBehavior {
    // ---- time stepping ----

    ParticleDtPolicy dtPolicy = ParticleDtPolicy::ClampToMax;

    /// WC3's `CMath::clamp_(elapsed, 0, 0.5)`. ClampToMax only.
    f32 maxStepSeconds = 0.5f;

    /// WoW's fixed sub-step size, and the threshold above which it subdivides
    /// at all (`InternalUpdate` @0x1016a5a30 tests `elapsed <= 0.1`).
    /// FixedSubSteps only.
    f32 subStepSeconds = 0.1f;

    // ---- spawn ----

    /// WoW draws two `frand`s and one raw word per birth (a sub-frame age
    /// numerator, a quantised lifespan variance, and the particle's own render
    /// seed) where WC3 draws a single `ufrand` for the age. The resulting age is
    /// `fmod(frand*dt, effectiveLifespan)`, which for a negative draw is
    /// NEGATIVE — faithful, and visible as particles that begin life slightly
    /// "before" the frame (`InitLifespanAndSeed`, ParticleGenerator.cpp).
    ///
    /// This changes the draw *count* and *order*, so it can never be data with
    /// a neutral value: any emitter running these draws consumes a different
    /// slice of the stream than one that does not.
    bool birthDrawsVarianceAndSeed = false;

    /// WoW gives each particle its own lifespan, `max(varQ*lifespanVariation +
    /// animatedLifespan, 0.001)`, re-evaluated EVERY FRAME against the current
    /// animated track — so a lifespan keyframe change retro-actively shortens
    /// or extends particles already in flight. WC3 compares age against the
    /// emitter's single `lifeSpan`.
    bool perParticleLifespan = false;

    /// `CParticleGenerator::GetEmissionRate` @0x10169eda0 is
    /// `base + frand * emissionRateVariation` — a fresh draw every frame, even
    /// when the variation is zero. Behaviour, not data: the draw itself shifts
    /// the stream.
    bool rateJitterPerFrame = false;

    /// WoW spawns along the emitter's path between last frame's position and
    /// this one rather than all at the current transform
    /// (`EmitNewParticles` @0x1016a5c90). Two sub-modes, both live here because
    /// both are the same divergence from "spawn at the origin":
    ///   * linear — each spawn advances by one emission period
    ///     `(cur-prev)/(rate*dt)`, the first placed at
    ///     `prev + (1-carriedFraction)*step`, so emission phase is continuous
    ///     across frames;
    ///   * random (file flag InheritPosition) — each spawn at
    ///     `prev + u*(cur-prev)`, `u` uniform, one extra draw per particle.
    bool emitAlongPath = false;

    /// Distance LOD on the emission rate:
    /// `clamp((50 - viewDist)*0.02 + 1, 0.25, 1)`, skipped per-emitter by the
    /// LodIgnoreDistance flag. Needs a view position at simulate time, which is
    /// why `ParticleService` grows a per-frame `SetViewPos`.
    bool lodEmissionScale = false;

    /// WoW adds the emitter's own motion to a new particle's velocity, scaled
    /// by `1 + frand*inheritVelocityScale`. The emitter velocity is refreshed
    /// at most every 1/30 s AND ONLY WHILE THE POOL IS EMPTY — with live
    /// particles the tick zeroes it instead (verified at instruction level:
    /// `cmp qword [emitter+0x38], 0` / `jz` to the compute branch, 0x1016a587f).
    /// So only spawn-onset bursts inherit motion; a steady stream stops after
    /// the first tick. Frame-rate coupled, and faithful — do not "fix" it.
    bool inheritEmitterVelocity = false;

    // ---- motion ----

    ParticleForceModel forceModel = ParticleForceModel::Wc3Gravity;

    /// This dialect HAS an implosion filter — kill a particle whose
    /// displacement this step points away from the emitter centre. Whether a
    /// given emitter uses it is `EmitterDesc::implosionFilter`; both must be
    /// set, because in the client this is a per-emitter runtime flag. (In 6.0.1
    /// the loader keys it off the DynamicWind sign bit for sphere emitters
    /// rather than the documented ImplosionFilter file bit — a quirk of that
    /// build.) Dialect-wide it would kill almost everything, which is what the
    /// filter is for.
    bool implosionKill = false;

    /// This dialect HAS a follow-position pull: particles older than `2*dt` are
    /// dragged by the emitter's scaled motion delta so a trail keeps up with its
    /// owner. Per-emitter enable is `EmitterDesc::followPosition`.
    bool followPosition = false;

    // ---- appearance ----

    /// Everything random about how a particle LOOKS — size jitter, spin, the
    /// random flipbook cell, the twinkle phase — is re-derived every frame by
    /// re-seeding a scratch RNG from the particle's own stored seed
    /// (`InterpolateAllTracks` @0x1016a1b70, `GetSpin` @0x1016a2120), rather
    /// than resolved once at spawn. Requires birthDrawsVarianceAndSeed.
    bool renderRandomsFromSeed = false;

    /// WoW multiplies particle alpha by the model's alpha (`m_alphaScale`,
    /// +0x1B4) and lets emission continue regardless; WC3 treats model
    /// visibility as an on/off gate on the emitter.
    bool modelAlphaScalesParticles = false;

    /// The shipped defect. All six sorted `IBuildVertices` clones dereference
    /// `&s_pq.top()` AFTER calling `pop()`, so for N particles the emitted
    /// order is ranks 2,3,...,N,N — the farthest particle is dropped and the
    /// nearest drawn twice. Proven by emulating the binary's own queue.
    /// Reforged's sorted path dequeues by value and is correct, and even in
    /// WoW `PlaceParticleModels` @0x1016a3e10 reads top() BEFORE popping — so
    /// this applies to the billboard builder only.
    ///
    /// Reproduce, do not repair: fixing it would diverge from the client.
    bool sortedBuilderDefect = false;

    static ParticleBehavior Wc3() {
        return ParticleBehavior{};
    }

    static ParticleBehavior Wow() {
        ParticleBehavior b;
        b.dtPolicy = ParticleDtPolicy::FixedSubSteps;
        b.birthDrawsVarianceAndSeed = true;
        b.perParticleLifespan = true;
        b.rateJitterPerFrame = true;
        b.emitAlongPath = true;
        b.lodEmissionScale = true;
        b.inheritEmitterVelocity = true;
        b.forceModel = ParticleForceModel::WowForces;
        b.implosionKill = true;
        b.followPosition = true;
        b.renderRandomsFromSeed = true;
        b.modelAlphaScalesParticles = true;
        b.sortedBuilderDefect = true;
        return b;
    }
};

} // namespace whiteout::flakes::renderer::core
