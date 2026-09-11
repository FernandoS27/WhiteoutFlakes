#pragma once

// ============================================================================
// Shared StarCraft II element kernels.
//
// A `RIB_` segment and a `PAR_` particle are the same runtime object under two
// names — the engine calls both an "element", stores both in the same arrays
// and runs the same LOD, overlay-wave, terrain and parent-velocity code over
// them. The ribbon dialect got here first and grew these kernels; the particle
// dialect needs them unchanged, so they live here and both include this
// (SC2_PARTICLE_DESIGN.md R6: `particle/` and `ribbon/` never include each
// other).
//
// Everything below is a reproduction of one shipped routine, gated against its
// Unicorn golden. The float operation ORDER is part of the contract — the
// arithmetic is written in the binary's order, not the algebraically tidy one.
// ============================================================================

#include "renderer/ground_query.h"
#include "renderer/sc2/sc2_rng.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::sc2 {

using whiteout::Vector3f;

// -- runtime bit names (RE §3) -----------------------------------------------
// These three words are RUNTIME state, not file fields: WhiteoutLib's
// `ParticleFlag` / `ParticleRotationFlag` enums cover the `PAR_` bits, and
// nothing covers these, which is why they were inline hex at every use site.
// Named once here (R7); the RE section is the authority for each meaning.

/// `CParticleSystem+0x120` — the emitter's derived state word.
enum SystemStateFlag : u32 {
    kStateForces = 0x1,             ///< local force pair nonzero.
    kStateWorldForces = 0x2,        ///< `localForces|worldForces >= 0x10000`.
    kStateWorldSpace = 0x4,         ///< `additionalFlags & 8`.
    kStateInheritVelocity = 0x8,    ///< `flags & 0x40`.
    kStateGpuMotion = 0x10,         ///< the CPU/GPU motion split (RE §2).
    kStateScaleTimeByParent = 0x20,
    /// Unnamed in the RE's `0x120` table. `Tick` tests `0x20|0x40` together
    /// before scaling `dtMs`, so this bit arms the same multiply — found by
    /// tracing what the tick READS, which is the only place either bit is
    /// consumed (OP3).
    kStateScaleTimeAlso = 0x40,
    kStateEmissionDisabled = 0x80,
    /// `Tick`'s second consumer of this bit: a resync forces the FULL-step
    /// path, so the sweep never straddles the discontinuity (OP3).
    kStateSquirtResync = 0x100,
    kStateUseLocalTime = 0x400,
    /// Re-entrancy guard around the sequence-change restart check; while it is
    /// set, `Tick` skips the check AND leaves bit 31 standing (OP3).
    kStateRestartBusy = 0x800,
    kStateSquirtPrime = 0x1000,
    kStateSequenceChanged = 0x80000000u, ///< set by UpdateEmitterState.
};

/// `CParticleSystem+0x34C` — the u24 emission-state word.
enum EmitStateFlag : u32 {
    kEmitAnyAnimated = 0x4,  ///< some AnimRef is animated (gates the sampler).
    kEmitNoise = 0x8,
    kEmitStillEmitting = 0x10,
    kEmitSuppressed = 0x20,
};

/// `SParticleElement+0x3E` — the per-element state word.
enum ElementFlag : u16 {
    kElemTrail = 0x1,
    kElemCollideTerrain = 0x4,
    kElemCollideObjects = 0x8,
    kElemOrientationFrozen = 0x10, ///< instanceType 6 froze its direction.
    kElemAtRest = 0x40,            ///< gravity off; collision put it to sleep.
};

// -- LOD tables (O3 golden o3_lodtables) -------------------------------------
// Indexed [5*row + quality]. The reduce row scales the emission rate; a nonzero
// cut byte suppresses emission whole.
inline constexpr f32 kLodReduce[20] = {
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f,   // row 0
    0.75f, 1.0f, 1.0f, 1.0f, 1.0f,  // row 1
    0.5f, 0.75f, 1.0f, 1.0f, 1.0f,  // row 2
    0.25f, 0.5f, 0.75f, 1.0f, 1.0f, // row 3
};
inline constexpr u8 kLodCut[20] = {
    0, 0, 0, 0, 0, //
    1, 0, 0, 0, 0, //
    1, 1, 0, 0, 0, //
    1, 1, 1, 0, 0, //
};

inline constexpr i32 LodIndex(i32 row, i32 quality) {
    return 5 * row + quality;
}

/// `M3_SampleAnimValue` (0x1028022f0), the overlay-wave sampler: type 1 =
/// sin(phase)·amp, 2 = cos(phase)·amp, 3 = sawtooth, 4 = square (±amp about
/// frac 0.5), 5 = `Rand_RangeF(−amp, amp)`, 6 = `Noise1D_Sample(table)·amp`.
/// `phase` is `freq·time + phaseOffset`.
///
/// **Type 5 consumes a draw**, which is why `rng` is here at all: OP5 measured
/// a type-5 overlay leaving the generator exactly one draw further on than an
/// unarmed one, so an emitter carrying one shifts every later draw in the
/// frame. An earlier note called the stand-in below "curve-shape only"; it is
/// not — a caller that passes no generator desynchronises the stream. Pass one
/// wherever the caller has one. Where it does not (the ribbon dialect, which
/// has no generator plumbed yet) the fallback is a deterministic hash of the
/// phase: the right SHAPE, the wrong stream, and owed work rather than a
/// harmless deviation.
///
/// Type 6 samples a seed-0 `NoiseTable`. Not `g_NoiseTable`: retail reads a
/// SECOND instance (`0x108254834`, built by `InitFunc_3733` with the same
/// seed), so the numbers are identical and @ref GlobalNoiseTable serves.
f32 SampleWave(u32 type, f32 phase, f32 amp, Rng* rng = nullptr);

// -- terrain collision --------------------------------------------------------
// `CRibbon_CollideSegment_Terrain`'s response, replayed against the ground
// query. The engine sweeps a step through the map colliders (radius 0.03,
// dword_103C2C9D0) and on contact reflects the post-integrate velocity off the
// hit surface as v' = -bounce·v_norm + friction·v_tan (friction only above a
// speed floor, dword_103C458F8 = 0.01), then advances from the contact point
// for the rest of the step. The query stands in for the colliders and answers a
// HEIGHT, so the surface is horizontal (normal = up); the dual query's
// forward-particle-system half has nothing to hit in the viewer. Positions and
// velocity are in the space the query answers (scene).
inline constexpr f32 kCollideRadius = 0.03f;
inline constexpr f32 kCollideSpeedSq = 0.01f;

struct GroundHit {
    Vector3f pos;
    Vector3f vel;
    bool hit = false;
};

GroundHit GroundCollide(const Vector3f& oldPos, const Vector3f& newPos,
                        const Vector3f& vel, f32 dt, f32 friction, f32 bounce,
                        const GroundQuery& query);

// -- parent velocity ----------------------------------------------------------

/// The 8-tap emitter-motion ring behind inherit-parent-velocity: push the
/// world translation delta and its dt each tick, read back the dt-weighted
/// average velocity (Σ delta / Σ dt) over the window.
///
/// The gate that decides whether to push at all belongs to the CALLER — for
/// ribbons it is `flags & 0x10` and it does NOT test world/local, so a local
/// ribbon also accumulates a nonzero direction (DRIFT-2).
class SmoothedVelocity {
public:
    static constexpr u8 kTaps = 8;

    void Reset() {
        for (Vector3f& p : pos_)
            p = {0, 0, 0};
        for (f32& w : dt_)
            w = 0.0f;
        slot_ = 0;
        count_ = 0;
        value_ = {0, 0, 0};
    }

    void Push(const Vector3f& delta, f32 dt);

    const Vector3f& Value() const {
        return value_;
    }

private:
    Vector3f pos_[kTaps] = {};
    f32 dt_[kTaps] = {};
    u8 slot_ = 0, count_ = 0;
    Vector3f value_ = {0, 0, 0};
};

// -- noise (RE §8.5) ----------------------------------------------------------

/// `g_NoiseTable`, regenerated rather than extracted.
///
/// The engine builds one 12860-byte table at static init with seed 0
/// (`NoiseTable_ctor(&g_NoiseTable, 0)`), and every gradient in it comes out of
/// an MS LCG seeded from that 0 — so the table is a pure function of the
/// algorithm and a port reproduces it bit for bit instead of shipping a dump.
/// OP2 is what turns that claim into a check.
///
/// The shipped table also carries 258-entry wrap copies of each array. The 1-D
/// and 2-D samplers mask every index to u8 and never read them; the 3-D sampler
/// does, and since a copy is `array[i % 256]` it folds its index back instead.
/// They are not reproduced here.
class NoiseTable {
public:
    explicit NoiseTable(u32 seed = 0);

    /// `Noise_Sample` — THREE 2-D Perlin values from one call, at `y`, `y−100`
    /// and `y−200`. The three rows come from three different `+bias` constants
    /// (4096 / 3996 / 3896), which is also what makes the negative-x behaviour
    /// row-dependent (RE §16.3: the instruction is `cvttss2si`, a truncation,
    /// and the bias is what makes truncation and floor agree over the expected
    /// range).
    void Sample(f32 x, f32 y, f32 out[3]) const;

    /// `Noise1D_Sample` — the 1-D variant over the unnormalised `grad1` row.
    f32 Sample1D(f32 phase) const;

    /// `sub_100D69F60`, the 3-D variant the ribbon's animated vertex fillers
    /// call (gate O13): one value over `grad3`. The corner gradient is a
    /// permutation hash PLUS the z byte with no mask, so its index runs to 510
    /// — into the shipped wrap copy, which is why this folds it.
    f32 Sample3D(f32 x, f32 y, f32 z) const;

private:
    /// `perm[(u8)(seed + i)]` — the seed offset is part of the index, not a
    /// separate shuffle.
    u8 Perm(i32 i) const {
        return perm_[static_cast<u8>(static_cast<i32>(seed_) + i)];
    }

    u32 seed_ = 0;
    u8 perm_[256] = {};
    f32 grad1_[256] = {};  ///< the raw draw in [−1,1): deliberately NOT unit.
    f32 grad2_[512] = {};  ///< 256 unit float2.
    f32 grad3_[768] = {};  ///< 256 unit float3 (unused by the two samplers).
};

/// The seed-0 table the engine keeps as `g_NoiseTable`, built on first use.
const NoiseTable& GlobalNoiseTable();

// -- authored mid key → Bezier control point (OP10) ---------------------------
// A Bezier channel does not store its control point: it stores the value the
// curve should PASS THROUGH at `midTime`, and `M3_ConvertColorNode` solves for
// the control point that puts it there —
// `c = (k1 − (1−t)²·k0 − t²·k2) / (2t(1−t))`. `UpdateAnimatedParams` runs it on
// the sampled keys whenever a channel's smoothing is Bezier (2); the port's
// caller is `particle::Sc2ConvertBezierKeys`.
//
// Note whose `midTime`: the emitter passes `sizeMidTime` for size, rotation AND
// colour (RE §5.4), so a model whose `colorMidTime` differs gets a control
// point solved for the wrong t. That is the caller's bug to reproduce, not
// this function's to fix.

/// In place, on `keys[1]`.
void ConvertColorNode(f32 keys[3], f32 midTime);

/// The packed-BGRA variant, per channel, returning the rewritten mid word.
///
/// Each channel goes through a `max(·, 0)` floor and a `·255 + 0.5` truncate
/// with **no upper clamp**, so a control point above 1.0 wraps in the byte pack
/// rather than saturating — reproduced, not repaired.
u32 ConvertColorNode3(u32 c0, u32 c1, u32 c2, f32 midTime);

} // namespace whiteout::flakes::renderer::sc2
