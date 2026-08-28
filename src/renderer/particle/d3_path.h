#pragma once

// ============================================================================
// D3 InterpolationPath — the one animated primitive every Diablo III `.prt`
// channel is built from.
//
// A `.prt` has forty of these and nothing else that varies over time: thirteen
// emitter channels, three emitter-shape channels and twenty-four particle
// channels, all the same 28-byte header plus a node array. See
// D3_PARTICLE_DESIGN.md §2.
//
// Four things here are easy to get wrong and were each measured rather than
// assumed:
//
//  * `start`/`end` on a node is a PER-PARTICLE RANDOM RANGE, not a keyframe
//    pair. One draw `r` is taken per channel per evaluation and applied to
//    every node, so a particle stays on its own lane through the whole curve.
//  * The random stream is re-seeded from scratch on every evaluation with
//    `(channelId * particleSeed, 666)`, so `r` is a pure function of the
//    particle and the channel — stable for the particle's life, independent of
//    evaluation order.
//  * Interpolation is ALWAYS linear. The field the old spec read as a curve
//    type is the node count.
//  * A vector channel draws THREE randoms, one per component
//    (`InterpolationPath_EvalVector` @0x7100375410); scalar, int and colour
//    draw one.
// ============================================================================

#include "types.h"
#include "whiteout/flakes/types.h"

#include <vector>

namespace whiteout::flakes::renderer::particle::d3 {

// Multiply-with-carry, `Rand_MWC_Next` @0x710098BA40. Every particle random in
// the game comes out of this.
struct MwcRng {
    u32 lo = 0;
    u32 hi = 666;

    u32 Next() {
        const u64 s = static_cast<u64>(hi) + 1791398085ull * static_cast<u64>(lo);
        lo = static_cast<u32>(s);
        hi = static_cast<u32>(s >> 32);
        return lo;
    }

    /// Uniform [0,1), the `× 2^-32` every caller applies to the raw word.
    f32 NextUnit() {
        return static_cast<f32>(Next()) * 2.3283064365386963e-10f;
    }

    /// `Rand_MWC_Seed` @0x710098BA20 — note `hi` is the literal 666.
    static MwcRng Seed(u32 v) {
        return MwcRng{v, 666u};
    }

    /// The seed a channel evaluation uses. The multiply is done in 32-bit
    /// signed arithmetic in the engine (`a3 * a2`), so it wraps; reproduce the
    /// wrap rather than widening.
    static MwcRng ForChannel(i32 channelId, u32 particleSeed) {
        return Seed(static_cast<u32>(channelId) * particleSeed);
    }
};

/// The nine remaps of the uniform draw, `InterpolationPath_DrawRandom`
/// @0x71003751F0. Anything outside 0..8 returns 0, which is the switch's
/// `default` case, not a guess.
f32 ApplyDistribution(i32 distribution, f32 u);

/// `InterpolationScalar` — 12 bytes on disk. `nMode` picks a GAMEPLAY driver
/// (actor health, distance, height); the result multiplies the sampled value.
/// Mode 0 is disabled, and 99.2% of authored paths use it.
struct Driver {
    i32 mode = 0;
    f32 lo = 0.0f;
    f32 hi = 1.0f;
};

/// Everything a driver can read that a model viewer can actually supply.
/// Modes 3 and 6 are computable from the particle and system positions; every
/// other non-zero mode reaches gameplay state we do not have and resolves to
/// "not applied", which is exactly what mode 0 does. See §12.4.
struct DriverInputs {
    f32 distNorm = 0.0f;   ///< |particlePos - sysPos| / flMaxDistance   (mode 3)
    f32 heightNorm = 0.0f; ///< (particle.z - sys.z) / flCameraDistScale (mode 6)
};

/// @returns false when the driver does not apply, in which case the caller
/// leaves the sample alone. Matches `InterpolationScalar_Evaluate`
/// @0x7100374DD0 returning zero for mode 0.
bool EvalDriver(const Driver& d, const DriverInputs& in, f32& outMultiplier);

/// One node. Widened to four components so a scalar, a vector and a packed
/// colour share one array type; `Path::components` says how many are live.
struct PathNode {
    Vector4f start{0, 0, 0, 0};
    Vector4f end{0, 0, 0, 0};
    f32 time = 0.0f;
};

/// The per-evaluation context, `Particle_BuildEvalContext` @0x710037AD70.
/// `timeMode` is the engine's `nTimeMode`: 0 takes `time/period` raw, 1 wraps
/// it into the path's loop sub-range, 2 additionally cross-fades toward a
/// second sample near the end of the curve.
struct EvalCtx {
    i32 timeMode = 1;
    f32 time = 0.0f;   ///< the particle's age in seconds (or the emitter's elapsed)
    f32 period = 1.0f; ///< the particle's lifetime in seconds (or the emission period)
    f32 blend = 0.0f;  ///< system normalised emission time; weights the end-blend
    f32 blendT = 0.0f;
    DriverInputs driver;
};

struct Path {
    std::vector<PathNode> nodes;
    f32 loopStart = 0.0f;
    f32 loopEnd = 1.0f;
    i32 distribution = 0;
    Driver driver;
    u8 components = 1; ///< 1 scalar/int/colour-as-float4, 3 vector

    /// The engine's fast path: one node whose start equals its end. `r` is
    /// then irrelevant and the draw is skipped — which is safe precisely
    /// because the seed does not depend on the draw history.
    bool IsConstant() const;

    /// Value range across every node, both endpoints. This is what
    /// `ParticleSystem_Spawn` asks each channel before setting its capability
    /// bit, and it is how an "absent" channel is recognised: v180 stores every
    /// channel inline, so absence is a default-valued node, never a null.
    void ScalarRange(f32& lo, f32& hi) const;

    /// True when the channel can only ever produce @p value. Used to build the
    /// capability mask the same way the engine does.
    bool IsInert(f32 value = 0.0f) const;

    /// Full evaluation. @p channelId selects the random stream and must be the
    /// engine's channel id, never a slot index.
    Vector4f Eval(u32 particleSeed, i32 channelId, const EvalCtx& ctx) const;

    f32 EvalScalar(u32 particleSeed, i32 channelId, const EvalCtx& ctx) const {
        return Eval(particleSeed, channelId, ctx).x;
    }
    Vector3f EvalVector(u32 particleSeed, i32 channelId, const EvalCtx& ctx) const {
        const Vector4f v = Eval(particleSeed, channelId, ctx);
        return {v.x, v.y, v.z};
    }

    /// The two endpoints of a scalar path's value range, in the form
    /// `InterpolationPath_GetScalarEndpoints` hands the emit context: `lo` and
    /// the span above it. The shape sampler draws inside that annulus.
    void ScalarEndpoints(f32& lo, f32& span) const;
};

/// Sample one path at an explicit random vector, skipping the draw. This is
/// the `GetVectorAt` shape used by emitter shape 8, which evaluates the extent
/// path at `r = (0,0,0)` and `r = (1,1,1)` to recover the box corners.
Vector4f SampleAt(const Path& p, const Vector4f& r, const EvalCtx& ctx);

} // namespace whiteout::flakes::renderer::particle::d3
