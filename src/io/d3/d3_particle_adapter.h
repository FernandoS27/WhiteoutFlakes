#pragma once

// ============================================================================
// `.prt` -> d3::EmitterDesc.
//
// The one rule that makes this survive a format revision: every channel is
// keyed by its ENGINE CHANNEL ID, never by its slot index. Shipped assets are
// struct version 180 with all forty paths inline; the Switch binary compiles
// version 213, which moved twelve of them into a side block. The slot order
// differs between the two, the ids do not.
// ============================================================================

#include "d3_types.h"
#include "renderer/particle/d3_emit_mesh.h"
#include "renderer/particle/d3_emitter_desc.h"
#include "whiteout/flakes/types.h"

#include <memory>
#include <span>

namespace whiteout::sno::d3::native {
struct Particle;
struct Appearances;
}

namespace whiteout::flakes::io::d3 {

namespace pd3 = ::whiteout::flakes::renderer::particle::d3;

/// Build the shared, immutable description one `.prt` becomes.
///
/// Never fails: a file whose channels are all empty produces a desc that emits
/// nothing, which is what the engine does with it too.
///
/// Mutable on purpose. The material's layers come back with SNO texture ids and
/// no actor texture ids, because which index a texture takes is a property of
/// the actor the emitter is about to ride, not of the file; `D3BindParticleTextures`
/// finishes it.
std::shared_ptr<pd3::EmitterDesc>
BuildD3EmitterDesc(const ::whiteout::sno::d3::native::Particle& prt, i32 snoId);

/// @brief Build the surface emitter shapes 6, 7 and 11 sample.
///
/// One sub-mesh per emitted sub-object, in emission order, because that is the
/// array the engine's mesh index addresses. Null when the appearance carries no
/// triangles at all, which leaves those shapes on the point case.
///
/// The area table is built from the BIND pose, like the engine's — it caches
/// one per mesh and never rebuilds it, so a posed model still draws triangles
/// in their rest-pose proportion.
std::shared_ptr<const pd3::EmitMesh>
BuildD3EmitMesh(const ::whiteout::sno::d3::native::Appearances& app,
                std::span<const D3SubObjectRef> emitted);

} // namespace whiteout::flakes::io::d3
