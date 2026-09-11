#pragma once

// ============================================================================
// EmitMesh — a mesh particles are born on, for any dialect that has one.
//
// Promoted verbatim from `d3::EmitMesh`: triangles, a running area CDF, rest
// positions and skin weights are not a Diablo III idea. D3's `.prt` shape 10
// picks an area-weighted point on it and shape 11 walks it sequentially; SC2's
// `PAR_` emitter shape 7 (`SampleEmitterMeshSurface`, oracle OP13) does the
// same against `.m3` regions. One struct, because the two dialects disagree
// about which triangle to pick and about nothing else — the split lives in the
// samplers, not in the storage.
//
// The one widening over the D3 original is the fourth bone slot: `.m3` skins
// with four influences and `.prt` with three. It costs D3 nothing because its
// skinning loop already skips a slot whose weight is not positive, so the
// fourth lane a D3 build leaves at zero is never read. The builder still
// splits the work the way the D3 comment describes — the emitter owns the
// random draws (their ORDER is what keeps a seeded trace aligned) and the host
// owns the geometry.
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
    /// the rest pose and is 63% of the D3 corpus.
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
