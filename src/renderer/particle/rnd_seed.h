#pragma once

#include "types.h"
#include "whiteout/flakes/types.h"

#include <cmath>

namespace whiteout::flakes::renderer::particle {

struct RndSeed {
    u32 state;

    RndSeed() : state(0) {}
    explicit RndSeed(u32 seed) {
        SetSeed(seed);
    }

    void SetSeed(u32 seed) {

        state = (seed == 0) ? 0x9E3779B9u : seed;
    }
};

namespace CRandom {

inline u32 next_u32(RndSeed& s) {
    u32 x = s.state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s.state = x;
    return x;
}

inline f32 real_(RndSeed& s) {

    return (next_u32(s) >> 8) * (1.0f / 16777216.0f);
}

inline f32 reals_(RndSeed& s) {
    return real_(s) * 2.0f - 1.0f;
}

inline Vector3f C3Vector_(RndSeed& s) {
    f32 u1, u2, d2;
    do {
        u1 = reals_(s);
        u2 = reals_(s);
        d2 = u1 * u1 + u2 * u2;
    } while (d2 >= 1.0f || d2 == 0.0f);
    f32 factor = 2.0f * std::sqrt(1.0f - d2);
    return {u1 * factor, u2 * factor, 1.0f - 2.0f * d2};
}

inline u32 dice_(u32 n, RndSeed& s) {
    if (n == 0)
        return 0;

    return next_u32(s) % n;
}

} // namespace CRandom

// Deterministic seed mix. Despite the historical name of its predecessor
// (MakeSeedFromTime) this never involved a clock — it is a pure hash, which is
// what lets a fixed scene reproduce its particle motion run to run.
inline u32 MixSeed(u32 x) {

    x += 0x9E3779B9u;
    x = (x ^ (x >> 16)) * 0x7FEB352Du;
    x = (x ^ (x >> 15)) * 0x846CA68Bu;
    x = x ^ (x >> 16);
    return x;
}

// The ranges an actor's emitters mix their index into, so no two outputs of one
// actor share a stream: PE2 and M2 emitters take the index as it is, PE1 child
// model emitters and M2 trails offset it into ranges no emitter index reaches.
inline constexpr u32 kPe1SeedSalt = 0x8000u;
inline constexpr u32 kTrailSeedSalt = 0xC000u;

// Seed for one emitter, derived from its owning actor and its index within that
// actor. Stable regardless of how many emitters were constructed before it —
// which the old process-global counter was not.
inline u32 MixSeed(u32 a, u32 b) {
    return MixSeed((a * 0x9E3779B9u) ^ (b + 0x85EBCA6Bu));
}

} // namespace whiteout::flakes::renderer::particle
