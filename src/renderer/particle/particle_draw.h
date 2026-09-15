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
///
/// `Particle_WriteQuadVertices` writes four packed texcoords per vertex, one per
/// texture stage, each the affine image of the quad's base rectangle under that
/// stage's own transform. It has to bake them because the whole transform is per
/// PARTICLE — the scroll phase and its rate are seeded at emit (12,361 of the
/// corpus's 13,897 mode-2 entries draw the phase at random), the rotation
/// accumulates from birth, and each stage runs its own flip-book. None of that
/// survives a per-draw constant.
///
/// It also writes TWO D3DCOLORs per vertex, and the second is channel 6
/// (`arAlphaPath`) replicated into all four lanes. Its only reader is
/// `Billboard.fx__ps_legacy`'s erosion tail, so one float is the whole of what
/// has to reach the shader. Per VERTEX rather than per draw because 253 of the
/// 265 files that author channel 6 animate it.
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
    /// All three arrays to @p n entries — the one place they are levelled,
    /// so no caller can level two and forget the third. A vertex no D3
    /// emitter wrote samples its layers at the raw quad uv and erodes nothing.
    void LevelTo(usize n) {
        uv01.resize(n);
        uv23.resize(n);
        color1.resize(n, 1.0f);
    }
};

/// @brief Where one frame's particle build writes: the shared vertex stream and
///        its draws, and the side streams a dialect needs.
///
/// A null side stream is a pass or program the frame does not have. An emitter
/// that would have written to one still draws, through the shared stream.
struct ParticleStreams {
    std::vector<Vertex>& vertices;
    std::vector<EmitterDrawList>& draws;
    /// M2 `Refraction`: its vertices AND its draws, which leave the scene pass.
    MultiTexGeometry* refraction = nullptr;
    /// M2 `MultiTexture`: its vertices; its draws join @ref draws.
    MultiTexGeometry* multiTex = nullptr;
    /// Diablo III: index-parallel with @ref vertices.
    D3VertexStream* d3 = nullptr;
};

} // namespace whiteout::flakes::renderer::particle
