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

    /// @brief Where a Diablo III particle's four BAKED texcoords go.
    ///
    /// `Particle_WriteQuadVertices` writes four packed texcoords per vertex, one
    /// per texture stage, each the affine image of the quad's base rectangle
    /// under that stage's own transform. It has to bake them because the whole
    /// transform is per PARTICLE — the scroll phase and its rate are seeded at
    /// emit (12,361 of the corpus's 13,897 mode-2 entries draw the phase at
    /// random), the rotation accumulates from birth, and each stage runs its own
    /// flip-book. None of that survives a per-draw constant.
    ///
    /// Two `Vector4f` per vertex, sets (0, 1) and sets (2, 3), index-parallel
    /// with @p out — the caller keeps them the same length. Null for every other
    /// dialect, and a D3 emitter built with it null still produces its quads;
    /// they just sample every layer at the raw quad uv.
    std::vector<Vector4f>* d3Uv01 = nullptr;
    std::vector<Vector4f>* d3Uv23 = nullptr;

    /// @brief The second colour, one scalar per vertex, index-parallel with the
    ///        other two.
    ///
    /// `Particle_WriteQuadVertices` writes TWO D3DCOLORs per vertex and the
    /// second is channel 6 (`arAlphaPath`) replicated into all four lanes. Its
    /// only reader is `Billboard.fx__ps_legacy`'s erosion tail, so one float is
    /// the whole of what has to reach the shader. Per VERTEX rather than per
    /// draw because 253 of the 265 files that author channel 6 animate it.
    std::vector<f32>* d3Color1 = nullptr;
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
