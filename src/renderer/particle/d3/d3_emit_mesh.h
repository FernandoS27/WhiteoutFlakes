#pragma once

// ============================================================================
// d3::EmitMesh — the owning model's surface, for emitter shapes 6, 7 and 11, as
// `ParticleSystem_SampleEmitterShape` @0x7100372D50 samples it: a mesh, an
// area-uniform or sequential triangle, a barycentric point, skinned. The point
// REPLACES the emitter position. The emitter owns the draws, the host the
// geometry (BuildD3EmitMesh, io/d3/d3_particle_adapter.h). See §18.4, §30.9.
// ============================================================================

#include "renderer/particle/base/emit_mesh.h"

namespace whiteout::flakes::renderer::particle::d3 {

/// Moved to `particle::EmitMesh` (emit_mesh.h) when SC2's mesh emitter needed
/// the same data; this alias keeps the D3 spelling. D3 leaves the added fourth
/// bone slot at weight zero, so its skinning loop never reads it.
using EmitMesh = ::whiteout::flakes::renderer::particle::EmitMesh;

} // namespace whiteout::flakes::renderer::particle::d3
