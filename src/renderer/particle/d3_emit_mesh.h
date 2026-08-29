#pragma once

// ============================================================================
// d3::EmitMesh — the owning model's surface, for emitter shapes 6, 7 and 11.
//
// Those three are 1,454 of the 21,593 shipped `.prt` (6 x 1,253, 7 x 17,
// 11 x 184) and they are not a long tail: a mesh emitter is how Diablo wears
// his fire, and it is the biggest emitter on the models that have one.
// `ParticleSystem_SampleEmitterShape` @0x7100372D50 samples them like this:
//
//   * pick a MESH — the system's own sub-object index, or a uniform draw over
//     the model's meshes when it names none. A viewer never names one.
//   * pick a TRIANGLE inside it. Shapes 6 and 7 draw AREA-UNIFORMLY from a
//     cumulative table `sub_7100373D80` builds once per mesh and caches (a
//     14-bit fixed-point area per triangle, then `rand % total` and a linear
//     scan); shape 11 walks `sequentialIndex % triCount` in order.
//   * pick a BARYCENTRIC point, with the reflect-into-the-triangle fold below.
//   * skin the three corners with the live pose and interpolate.
//
// The result REPLACES the emitter position rather than offsetting it — the
// mesh cases never add `basePos`. And any lookup failure falls through to the
// point case, which is what a model with no surface gets here.
//
// This struct is deliberately free of every model and scene type: the emitter
// owns the random draws (their ORDER is what keeps a seeded trace aligned) and
// the host owns the geometry. BuildD3EmitMesh in io/d3/d3_particle_adapter.h
// fills it from a parsed `.app`.
// ============================================================================

#include "types.h"
#include "whiteout/flakes/types.h"

#include <array>
#include <vector>

namespace whiteout::flakes::renderer::particle::d3 {

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

    /// Bind-pose positions in `.prt` units.
    std::vector<Vector3f> rest;
    /// Up to three GLOBAL skeleton indices per vertex and their weights — D3
    /// ships no per-geoset palette, so these need no remap. Both empty for a
    /// model with no skeleton, which samples the rest pose and is 63% of the
    /// corpus.
    std::vector<std::array<i32, 3>> bones;
    std::vector<Vector3f> weights;

    bool Empty() const {
        return subs.empty() || tris.empty();
    }
};

} // namespace whiteout::flakes::renderer::particle::d3
