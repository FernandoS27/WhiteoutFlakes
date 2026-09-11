#pragma once

// ============================================================================
// sc2::Rng — the StarCraft II engine random generator, transcribed.
//
// Every random a `PAR_` emitter draws (spawn position, velocity, lifespan,
// size, colour, rotation, flipbook phase, noise phase) comes out of ONE
// generator with an explicit 8-byte state, over a 256-byte table baked into the
// image. That is what makes the whole particle runtime replayable: a gate can
// pin the state in, run the shipped code under Unicorn, and compare draw for
// draw (SC2_PARTICLE_PLAN.md A1). A stand-in generator would have made every
// downstream gate a tolerance argument instead of an equality.
//
// `RandomNextU32` (4.8 `0x100D6AE90`) reads a little-endian u32 at a BYTE
// offset of the table — unaligned by construction, which is why the table is
// carried as bytes and not as 64 dwords. Its three consumers are two
// algorithms, not three: `Rand_RangeU16` and `RandomInt32InRange` are the same
// 214 bytes of code at two addresses.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <array>

namespace whiteout::flakes::renderer::sc2 {

/// The 256 bytes at `0x103AD8270`, gated byte-for-byte by OP0.
extern const std::array<u8, 256> kRngTable;

class Rng {
public:
    Rng() = default;
    Rng(u32 acc, u32 idx4) : acc_(acc), idx4_(idx4) {}

    /// `g_Rand`'s 8 bytes. A gate pins both ends of a call with these.
    u32 acc() const {
        return acc_;
    }
    u32 idx4() const {
        return idx4_;
    }
    void SetState(u32 acc, u32 idx4) {
        acc_ = acc;
        idx4_ = idx4;
    }

    /// `RandomNextU32` — the return value is the NEW accumulator.
    u32 NextU32();

    /// `Rand_RangeF` (`0x100D6B110`). The span is computed BEFORE the draw and
    /// the unit value is a mantissa bitcast minus one, so the op order is a
    /// float32 subtract, multiply and add in that sequence — not `lerp`.
    f32 RangeF(f32 a, f32 b);

    /// `Rand_RangeU16` / `RandomInt32InRange` — one kernel, hi-EXCLUSIVE.
    u32 RangeInt(u32 lo, u32 hi);

private:
    u32 acc_ = 0;
    u32 idx4_ = 0;
};

} // namespace whiteout::flakes::renderer::sc2
