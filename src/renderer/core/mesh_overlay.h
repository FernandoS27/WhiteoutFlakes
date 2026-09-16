#pragma once

// Mesh overlay, device-free half: the edges, vertex dots and flat faces the
// wireframe debug views draw, and the per-element state a model editor will
// select with (DEBUG_VIEW_DESIGN.md §9). The shader half is
// shaders/mesh_overlay.slang, the GPU half renderer/mesh_overlay/.
//
// An element is named in its geoset's own numbering: vertex v is the v-th
// vertex the geoset uploaded, face f the triangle at indices[3f .. 3f+2]. That
// is the numbering an editor edits in, and nothing here renumbers it. Edges are
// derived from the triangles, so they carry no state of their own: an edge is
// selected because its ends or a face beside it are.

#include "whiteout/flakes/enums.h"
#include "whiteout/flakes/types.h"

#include <span>
#include <vector>

namespace whiteout::flakes::renderer::core {

// One byte per vertex and per face. Bits, so a hovered selection is both.
enum MeshElementFlag : u8 {
    kMeshElementSelected = 1u << 0,
    kMeshElementHovered = 1u << 1,
    // Not drawn by the overlay. The surface itself is the product's to hide.
    kMeshElementHidden = 1u << 2,
};

enum class MeshElementKind : u8 { Vertex = 0, Face = 1 };

// One geoset's element states. An empty vector is "no element of that kind
// carries a flag", which is every geoset nobody has selected in.
struct MeshElementStates {
    std::vector<u8> vertices;
    std::vector<u8> faces;

    bool Empty() const {
        return vertices.empty() && faces.empty();
    }
};

// Applies `flags` under `mask` to the listed elements: state = (state & ~mask)
// | (flags & mask). Grows the vector to `elementCount` on first use; ids past it
// are ignored. Returns whether anything changed.
bool SetMeshElementFlags(MeshElementStates& states, MeshElementKind kind, u32 elementCount,
                         std::span<const u32> ids, u8 mask, u8 flags);

inline constexpr u32 kMeshNoFace = 0xFFFFFFFFu;

// A unique undirected edge (a < b) and the first two faces that share it.
struct MeshEdge {
    u32 a = 0;
    u32 b = 0;
    u32 faces[2] = {kMeshNoFace, kMeshNoFace};
};

// The unique edges of a triangle list, in first-seen order. A degenerate
// triangle contributes only its real edges; an index at or past `vertexCount`
// drops the triangle.
std::vector<MeshEdge> BuildMeshEdges(std::span<const u32> indices, u32 vertexCount);

// Selected or hovered when both ends are, or when a face beside it is; hidden
// when an end is, or when every face beside it is.
u8 MeshEdgeState(const MeshEdge& edge, const MeshElementStates& states);

// Whether any face is selected or hovered — the tint pass is skipped otherwise.
bool AnyMarkedFace(const MeshElementStates& states);

// Where each section starts in the element buffer (MeshOverlayData::sections).
// Every element is float4: three per vertex (position + state, weights,
// indices), one per edge (a, b, state), one per face (a, b, c, state).
struct MeshOverlayLayout {
    u32 vertexBase = 0;
    u32 edgeBase = 0;
    u32 faceBase = 0;
    u32 elementCount = 0;
    u32 vertexCount = 0;
    u32 edgeCount = 0;
    u32 faceCount = 0;
};

MeshOverlayLayout MeshOverlayLayoutFor(u32 vertexCount, u32 edgeCount, u32 faceCount);

// What the packer reads. The two bone spans are four bytes per vertex, or empty
// for a rigid geoset; weights are UNORM bytes, divided here exactly as the input
// stage divides them.
struct MeshOverlayGeometry {
    std::span<const Vector3f> positions;
    std::span<const u8> boneWeights;
    std::span<const u8> boneIndices;
    std::span<const u32> indices;
    std::span<const MeshEdge> edges;
};

// Fills `out` (layout.elementCount * 4 floats). `states` may be null.
void PackMeshOverlay(std::span<f32> out, const MeshOverlayGeometry& geometry,
                     const MeshElementStates* states, const MeshOverlayLayout& layout);

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

struct MeshOverlayStyle {
    // Flat faces in the actor's team colour, drawn by the overlay instead of
    // the product's surfaces.
    bool faces = false;
    bool edges = false;
    bool vertices = false;
    // Depth-tested against the scene, so the far side of a mesh hides.
    bool occluded = false;

    bool Any() const {
        return faces || edges || vertices;
    }
};

inline constexpr MeshOverlayStyle MeshOverlayStyleFor(DebugView v) {
    MeshOverlayStyle s;
    switch (v) {
    case DebugView::Wireframe:
        s.edges = true;
        break;
    case DebugView::WireframeVertices:
        s.edges = true;
        s.vertices = true;
        s.occluded = true;
        break;
    case DebugView::WireframeTeamColor:
        s.faces = true;
        s.edges = true;
        s.occluded = true;
        break;
    default:
        break;
    }
    return s;
}

// MeshOverlayData::ctl.x
enum class MeshOverlayPass : u32 { Faces = 0, Edges = 1, Vertices = 2 };

// MeshOverlayData::ctl.y
inline constexpr u32 kMeshOverlayTargetEncodesSrgb = 1u;
inline constexpr u32 kMeshOverlayMarkedFacesOnly = 2u;

// Display colours, 0..1 with alpha.
struct MeshOverlayColors {
    f32 face[4] = {0, 0, 0, 1};
    f32 edge[4] = {0, 0, 0, 1};
    f32 vertex[4] = {0, 0, 0, 1};
    f32 selected[4] = {1.0f, 0.55f, 0.1f, 1.0f};
    f32 hovered[4] = {1.0f, 0.85f, 0.35f, 1.0f};
};

// Colours are packed r | g << 8 | b << 16, as Actor::teamColor and the
// background setting store them. A bare wireframe has only the background to
// read against, so its edges take the opposite brightness.
MeshOverlayColors MeshOverlayColorsFor(DebugView view, u32 teamColorRgb, u32 backgroundRgb);

// Clip-space z an overlay element subtracts to win against its own surface:
// `fraction` of the eye distance at every depth. For either handedness of
// Matrix44f::perspective_fov_*, |P[3][2]| = fn / (f - n), and moving a point a
// fraction k toward the eye moves NDC z by k * fn / ((f - n) * w).
inline f32 MeshOverlayDepthPull(const Matrix44f& projection, f32 fraction) {
    const f32 c = projection.data[3][2];
    return fraction * (c < 0.0f ? -c : c);
}

// MeshOverlayData in shaders/mesh_overlay.slang.
struct MeshOverlayCbData {
    u32 pass = 0;
    u32 flags = 0;
    u32 reserved0 = 0;
    u32 reserved1 = 0;
    u32 vertexBase = 0;
    u32 edgeBase = 0;
    u32 faceBase = 0;
    u32 reserved2 = 0;
    f32 halfViewport[2] = {0, 0};
    f32 edgeWidth = 1.0f;
    f32 vertexSize = 6.0f;
    f32 depthPull = 0.0f;
    f32 reserved3[3] = {0, 0, 0};
    f32 faceColor[4] = {0, 0, 0, 1};
    f32 edgeColor[4] = {0, 0, 0, 1};
    f32 vertexColor[4] = {0, 0, 0, 1};
    f32 selectedColor[4] = {0, 0, 0, 1};
    f32 hoveredColor[4] = {0, 0, 0, 1};
};

} // namespace whiteout::flakes::renderer::core
