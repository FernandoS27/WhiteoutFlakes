#pragma once

#include "particle2_emitter.h"
#include "particle_material.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <vector>

namespace whiteout::flakes::renderer::particle {

struct D3VertexStream;

/// What a builder needs besides the emitter. Particles take their fog in the
/// shader (`bls::DrawFogMode`), never here.
struct BuildGeometryInput {
    const Matrix44f* worldToView = nullptr;

    /// @brief Where an emitter's two extra UV sets go.
    ///
    /// One entry per emitted vertex — `(uv1.xy, uv2.xy)` — appended in lockstep
    /// with @p out, so the two arrays index together. Left null for every
    /// ordinary emitter, and ignored unless the desc asked for the layers
    /// (refraction or multi-texture); such an emitter built with it null still
    /// produces its quads, it just cannot be shaded as one. See
    /// M2_REFRACTION_DESIGN.md and M2_MULTITEX_DESIGN.md.
    std::vector<Vector4f>* extraUV = nullptr;

    /// @brief Where a Diablo III particle's baked texcoords and second colour
    ///        go, index-parallel with @p out — the caller keeps them level (see
    ///        `D3VertexStream`).
    ///
    /// Null for every other dialect, and a D3 emitter built with it null still
    /// produces its quads; they just sample every layer at the raw quad uv.
    D3VertexStream* d3 = nullptr;
};

i32 BuildEmitterGeometry(const Emitter2& emitter, const BuildGeometryInput& in,
                         std::vector<Vertex>& out);

/// @brief The 128-entry twinkle table, one copy for the whole process.
///
/// Exposed because model particles twinkle too, and they are placed by the
/// emitter rather than built here — two tables would blink out of step.
const f32* TwinkleTable();

/// @brief Twinkle index for one particle: `(seed + age*speed) & 0x7F`.
u32 TwinkleIndex(u16 seed, f32 age, f32 twinkleSpeed);

} // namespace whiteout::flakes::renderer::particle
