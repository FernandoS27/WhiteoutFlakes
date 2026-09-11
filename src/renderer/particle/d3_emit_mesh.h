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

#include "emit_mesh.h"

namespace whiteout::flakes::renderer::particle::d3 {

/// The struct moved to `particle::EmitMesh` (emit_mesh.h) when SC2 turned out
/// to need the same triangles, area CDF and skin weights for its own mesh
/// emitter shape. `d3::EmitMesh` stays spellable because that is how the `.prt`
/// adapter, the loader and this emitter all name it; the only difference the
/// move made is a fourth bone slot, which D3 leaves at weight zero and its
/// skinning loop therefore never reads.
using EmitMesh = ::whiteout::flakes::renderer::particle::EmitMesh;

} // namespace whiteout::flakes::renderer::particle::d3
