#pragma once

#include "particle2_emitter.h"
#include "particle_material.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <functional>
#include <vector>

namespace whiteout::flakes::renderer::particle {

using FogSampler = std::function<ImVector(const Vector3f&)>;

struct BuildGeometryInput {
    const Matrix44f* worldToView = nullptr;
    bool fogEnabled = false;
    FogSampler fogSampler = nullptr;

    /// @brief Where an emitter's two extra UV sets go.
    ///
    /// One entry per emitted vertex — `(uv1.xy, uv2.xy)` — appended in lockstep
    /// with @p out, so the two arrays index together. Left null for every
    /// ordinary emitter, and ignored unless the desc asked for the layers
    /// (refraction or multi-texture); such an emitter built with it null still
    /// produces its quads, it just cannot be shaded as one. See
    /// M2_REFRACTION_DESIGN.md and M2_MULTITEX_DESIGN.md.
    std::vector<Vector4f>* extraUV = nullptr;
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
