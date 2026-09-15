#pragma once

#include "renderer/particle/base/particle_constants.h"
#include "types.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::particle {

// 32 bytes, mirroring the engine's CParticle2 in both clients. The interesting
// part is `aux`: Warcraft III spends those four bytes on a curve-segment
// cursor, World of Warcraft on a quantised lifespan variance plus the
// particle's own render seed. Same slot, different meaning — so it is named for
// the slot and read through the accessors the dialect implies, rather than
// widened to hold both.
struct Particle2 {
    Vector3f position{0, 0, 0};
    u32 aux = 0;
    Vector3f velocity{0, 0, 0};
    f32 age = 0.0f;

    // ---- WC3 reading: a hint into the lifetime curves ----
    u32 Cursor() const {
        return aux;
    }
    void SetCursor(u32 c) {
        aux = c;
    }

    // ---- WoW reading: lifespan variance quantum + render seed ----
    //
    // `varQ` is CRandom::frand quantised to [-32767, 32767]
    // (`QuantizeLifespanVariance`); the runtime scales it by
    // `kLifespanVarianceUnit` and multiplies by the emitter's lifespanVariation. `seed`
    // is re-seeded into a scratch RNG every frame to re-derive spin, size
    // jitter, cell choice and twinkle, so it must survive unchanged for the
    // particle's whole life.
    i16 LifespanVarQ() const {
        return static_cast<i16>(static_cast<u16>(aux & 0xFFFFu));
    }
    /// The variance quantum as the [-1, 1] fraction the runtime multiplies.
    f32 LifespanVariance() const {
        return static_cast<f32>(LifespanVarQ()) * kLifespanVarianceUnit;
    }
    u16 RenderSeed() const {
        return static_cast<u16>(aux >> 16);
    }
    void SetVarianceAndSeed(i16 varQ, u16 seed) {
        aux = static_cast<u32>(static_cast<u16>(varQ)) | (static_cast<u32>(seed) << 16);
    }
};

static_assert(sizeof(Particle2) == 32, "Particle2 must be 32 bytes to mirror CParticle2");

// The extra 32 bytes a refraction (or multi-texture) particle carries, which
// is exactly what `CMultiTexParticle` adds past `CParticle2`: two UV origins
// and two scroll rates. Kept alongside the pool rather than inside Particle2
// because the 32-byte mirror above is load-bearing and because only a handful
// of shipped emitters need this at all — a plain emitter's array stays empty.
struct MultiTexState {
    Vector2f uv[2]{{0, 0}, {0, 0}};
    Vector2f scroll[2]{{0, 0}, {0, 0}};
};

} // namespace whiteout::flakes::renderer::particle
