#pragma once

// ============================================================================
// What one frame's particle build hands the render pipeline: the draw lists,
// and the side streams the dialects whose vertices the shared `Vertex` cannot
// hold write alongside it.
//
// Split out of `particle_service.h` so the pipeline and the shading programs
// name the stream types without seeing the emitter map.
// ============================================================================

#include "renderer/particle/output/particle_material.h"
#include "renderer/particle/output/particle_output.h"
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

// One frame's geometry for `CMultiTexParticle` emitters (refraction and
// multi-texture, one instance each). `extraUV` is index-parallel with `vertices`
// and holds the two scrolling layers no shared vertex has room for. Refraction
// leaves the transparent pass; multi-texture only changes shader (see
// `EmitterDesc::refraction`, M2_REFRACTION_DESIGN.md, M2_MULTITEX_DESIGN.md).
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

/// The stream interleaved into a consumer's vertex @p V, which names its fields
/// `position`, `color`, `uv0`, `uv1` and `uv2`. The caller checks that the two
/// arrays agree in length.
template <typename V>
void InterleaveMultiTex(const MultiTexGeometry& geo, V* dst) {
    for (usize i = 0; i < geo.vertices.size(); ++i) {
        const Vertex& v = geo.vertices[i];
        const Vector4f& extra = geo.extraUV[i];
        dst[i].position = v.position;
        dst[i].color = v.color;
        dst[i].uv0 = v.uv;
        dst[i].uv1 = {extra.x, extra.y};
        dst[i].uv2 = {extra.z, extra.w};
    }
}

/// @brief What a Diablo III particle vertex carries that the shared one cannot:
///        four baked per-stage texcoords and a second colour (channel 6,
///        `arAlphaPath`, read only by the erosion tail).
///
/// Index-parallel with the ORDINARY vertex stream, every array kept its length,
/// so a draw's `vertexOffset` indexes all of them. Baked per vertex because the
/// stage transforms are per particle. M2_PARTICLE_DESIGN.md §11.11.
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
