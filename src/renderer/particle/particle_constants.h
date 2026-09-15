#pragma once

// ============================================================================
// The WC3/WoW particle vocabulary: the literals the CParticleEmitter2 family's
// simulation and builders read, named once.
//
// The rule is `renderer/sc2/sc2_constants.h`'s: a value is named by the ROLE it
// plays, and one float in two roles is two names over one definition, so an RE
// correction to one reading cannot silently move the other. Everything here is
// the client's own constant or a composition choice documented at its site; the
// SC2 and Diablo III dialects keep their vocabularies beside their own code.
// ============================================================================

#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::particle {

// -- the client's float epsilon --------------------------------------------------

/// `2.3841858e-7`, the guard `CParticleEmitter2` puts in front of every 1/x.
inline constexpr f32 kClientEpsilon = 2.3841858e-7f;
/// One step's worth of emission too small to divide the emitter's travel by.
inline constexpr f32 kEmissionEpsilon = kClientEpsilon;
/// A velocity (squared) too short to orient a quad along.
inline constexpr f32 kVelocityEpsilon = kClientEpsilon;
/// Two follow-line sample speeds too close to solve a slope through.
inline constexpr f32 kFollowSpeedEpsilon = kClientEpsilon;
/// A basis row (squared) too short to normalise out of an emitter matrix.
inline constexpr f32 kBasisRowEpsilon = kClientEpsilon;

/// `2π` as the WC3/WoW clients multiply by it (the same bits as the SC2 image's).
inline constexpr f32 kWowTwoPi = 6.2831855f;

// -- per-particle lifespan (WoW) -----------------------------------------------------

/// The quantised lifespan variance a birth stores in `Particle2::aux`: a draw in
/// [-1, 1] scaled to ±32767 and rounded half up, saturating at the ends.
inline constexpr f32 kLifespanVarianceQuant = 32767.0f;
/// The scale the runtime reads that quantum back with. Not `1/32767`: the
/// client's encode and decode use different denominators.
inline constexpr f32 kLifespanVarianceUnit = 1.0f / 32768.0f;
/// A particle's lifespan never drops below this, whatever the variance says.
inline constexpr f32 kMinLifespan = 0.001f;

inline i16 QuantizeLifespanVariance(f32 r) {
    return (r < 1.0f) ? (r > -1.0f ? static_cast<i16>(r * kLifespanVarianceQuant + 0.5f)
                                   : static_cast<i16>(-32767))
                      : static_cast<i16>(32767);
}

// -- emission (WoW) --------------------------------------------------------------------

/// WoW resamples the emitter's velocity on a 1/30 s cadence rather than every
/// frame, so the value a particle inherits is frame-rate coupled by design.
inline constexpr f32 kVelocitySampleSeconds = 1.0f / 30.0f;

/// The distance falloff on emission rate: `(near − distance) · slope + 1`,
/// clamped to `[minScale, 1]`.
inline constexpr f32 kLodNearDistance = 50.0f;
inline constexpr f32 kLodFalloffPerUnit = 0.02f;
inline constexpr f32 kLodMinScale = 0.25f;

// -- the pool ----------------------------------------------------------------------------

/// Headroom over the steady-state population (rate × lifespan), so a rate spike
/// does not immediately starve the free list.
inline constexpr f32 kPoolHeadroom = 1.15f;
/// A trail's pool is its own population times its owner's, clamped here — the
/// client's own clamp (`Sync` @0x10169f860).
inline constexpr u32 kTrailPoolCap = 4096u;
/// An empty pool compacts with odds of 1 in this, off its own stream.
inline constexpr u32 kCompactOdds = 32u;

// -- seeds -----------------------------------------------------------------------------

/// Seed an emitter starts with when the caller never calls SetSeed. Fixed rather
/// than counter-derived so an un-seeded emitter is still reproducible.
inline constexpr u32 kDefaultEmitterSeed = 0x1234567u;
/// Mixed into the emitter seed for the compaction stream, which must never
/// perturb the spawn stream the trace compares.
inline constexpr u32 kCompactSeedSalt = 0x5BF03635u;

// -- twinkle ---------------------------------------------------------------------------

/// The client fills this many floats at start-up and indexes them by
/// `(seed low byte + phase) & mask`.
inline constexpr u32 kTwinkleTableSize = 128u;
inline constexpr u32 kTwinkleIndexMask = kTwinkleTableSize - 1u;
inline constexpr u32 kTwinkleSeedByteMask = 0xFFu;
/// Ours is seeded fixed, which is the one deliberate divergence: the client
/// seeds from `rand()` and its blink pattern differs between runs.
inline constexpr u32 kTwinkleTableSeed = 0x7A17C1E5u;

// -- the WoW builder -----------------------------------------------------------------

/// `max(1 + rand · variation, floor)`: a size jitter never collapses a quad.
inline constexpr f32 kSizeJitterFloor = 1e-4f;

// -- the WC3 builder -----------------------------------------------------------------

/// `ParticleEmitterConfig::particleType`: which of a WC3 particle's two quads
/// it draws.
enum class Wc3ParticleType : i32 { Head = 1, Tail = 2, Both = 3 };

/// Above this |z| a tail direction takes world Y as its cross reference instead
/// of world Z.
inline constexpr f32 kTailVerticalCos = 0.999f;

// -- the WC3 curve sampling ----------------------------------------------------------

/// WC3 samples each lifetime segment over `[bias, 1 − bias]` rather than
/// `[0, 1]`; the splat service uses the same skew on its own timeline.
inline constexpr f32 kWc3SampleBias = 0.005f;
inline constexpr f32 kWc3SampleSpan = 0.99f;

} // namespace whiteout::flakes::renderer::particle
