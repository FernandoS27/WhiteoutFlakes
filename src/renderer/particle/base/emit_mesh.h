#pragma once

// ============================================================================
// EmitMesh — a mesh particles are born on: D3 `.prt` shapes 10 and 11, SC2 `PAR_`
// shape 7 (`SampleEmitterMeshSurface`, OP13). The dialects differ only in which
// triangle they pick, so that lives in the samplers. The emitter owns the random
// draws (their ORDER keeps a seeded trace aligned); the host owns the geometry.
// See M2_PARTICLE_DESIGN.md §11.13.
// ============================================================================

#include "types.h"
#include "whiteout/flakes/types.h"

#include <array>
#include <vector>

namespace whiteout::flakes::renderer::particle {

/// Influences per vertex. Three is D3's `.prt`, four is `.m3`.
inline constexpr usize kEmitMeshBones = 4;

struct EmitMesh {
    struct SubMesh {
        u32 firstTri = 0;
        u32 triCount = 0;
    };

    /// One per emitted sub-object, in emission order.
    std::vector<SubMesh> subs;
    /// Three indices into @ref rest per triangle, flattened and concatenated
    /// over @ref subs. Already global: the builder offsets each sub-object's
    /// indices by where its vertices landed.
    std::vector<u32> tris;
    /// Running triangle-area sum, restarting at each sub-object so a pick is
    /// local to the one that was chosen. One entry per triangle.
    std::vector<f32> areaCdf;

    /// Bind-pose positions in the source's own units.
    std::vector<Vector3f> rest;
    /// Up to @ref kEmitMeshBones GLOBAL skeleton indices per vertex and their
    /// weights — neither D3 nor SC2 ships a per-geoset palette here, so these
    /// need no remap. Both empty for a model with no skeleton, which samples
    /// the rest pose — most of the D3 corpus (M2_PARTICLE_DESIGN.md §11.13).
    std::vector<std::array<i32, kEmitMeshBones>> bones;
    std::vector<std::array<f32, kEmitMeshBones>> weights;
    /// R of each rest vertex's BGRA colour — the byte SC2's shape 7 rejects
    /// positions against (OP13). Empty for a mesh with no vertex colour, which
    /// that sampler reads as 255 at every corner; D3 never fills it.
    std::vector<u8> colorR;

    bool Empty() const {
        return subs.empty() || tris.empty();
    }
};

} // namespace whiteout::flakes::renderer::particle
