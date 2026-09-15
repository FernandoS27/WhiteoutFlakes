#pragma once

// ============================================================================
// What one frame's particle build hands the render pipeline: the draw lists,
// and the side streams the dialects whose vertices the shared `Vertex` cannot
// hold write alongside it.
//
// Split out of `particle_service.h` so the pipeline and the shading programs
// name the stream types without seeing the emitter map.
// ============================================================================

#include "particle_material.h"
#include "particle_output.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <vector>

namespace whiteout::flakes::renderer::particle {

struct EmitterDrawList {
    ModelId model;
    i32 emitterId;
    i32 vertexOffset;
    i32 vertexCount;
    i32 priorityPlane;
    ParticleMaterialDesc material;
    // Emitter origin in world space — sort key for the back-to-front
    // transparent pass (interleaves with geosets/ribbons/corn).
    Vector3f worldOrigin = {0, 0, 0};
    // EmitterDrawHeader::materialTimeSec at build time. Only a D3 draw reads it.
    f32 materialTimeSec = 0.0f;
};

// One frame's geometry for the emitters whose particles are the client's
// `CMultiTexParticle` — refraction and multi-texture both. `extraUV` is
// index-parallel with `vertices` and holds the two scrolling texture layers;
// nothing else in the engine has three UV sets, which is why these emitters
// need a stream of their own rather than a wider shared vertex.
//
// The two kinds fill separate instances and are consumed differently:
// refraction leaves the transparent pass entirely (the client buckets it into
// M2PASS_REFRACTION and nowhere else, `AddParticleElement` @0x100f78590),
// while multi-texture stays in it and only changes shader. See
// M2_REFRACTION_DESIGN.md and M2_MULTITEX_DESIGN.md.
struct MultiTexGeometry {
    std::vector<Vertex> vertices;
    std::vector<Vector4f> extraUV;
    std::vector<EmitterDrawList> draws;

    void Clear() {
        vertices.clear();
        extraUV.clear();
        draws.clear();
    }
};

/// @brief What a Diablo III particle vertex carries that the shared one cannot.
///
/// Index-parallel with the ORDINARY vertex stream rather than a stream of its
/// own: a D3 quad is an ordinary billboard in position, colour and sort order,
/// and only its four baked texcoords and its SECOND colour need more room than
/// the shared vertex has. The service keeps every array the same length as that
/// stream, so a draw's `vertexOffset` indexes all of them.
/// See BuildGeometryInput::d3Uv01 and ::d3Color1.
struct D3VertexStream {
    std::vector<Vector4f> uv01; ///< uv set 0 in .xy, set 1 in .zw
    std::vector<Vector4f> uv23;
    std::vector<f32> color1; ///< ch6, the erosion tail's exponent scale

    bool Empty() const {
        return uv01.empty();
    }
    void Clear() {
        uv01.clear();
        uv23.clear();
        color1.clear();
    }
};

} // namespace whiteout::flakes::renderer::particle
