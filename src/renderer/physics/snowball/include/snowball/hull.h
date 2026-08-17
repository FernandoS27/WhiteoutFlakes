//===----------------------------------------------------------------------===//
// snowball/hull.h -- convex hulls as a half-edge mesh.
//
// The builder is an ordinary incremental hull, but what it is held to is its output contract,
// checked by Euler's formula: a cube must come back as 8 vertices, 6 faces and 24 half-edges,
// a claim any correct builder has to meet whatever algorithm it uses. The exact combinatorics
// matter, not the construction order.
//
// **Faces are polygons, not triangles.** That distinction is what the 6-and-24 result is
// really testing: a triangulating hull gives a cube 12 faces and 36 half-edges, both perfectly
// correct and both wrong here. Coplanar faces are merged.
//===----------------------------------------------------------------------===//
#pragma once

#include <span>
#include <vector>

#include "snowball/common_types.h"
#include "snowball/vec.h"

namespace snowball {

struct HalfEdge {
    i32 origin{-1};   ///< vertex this edge leaves
    i32 twin{-1};     ///< the same edge walked the other way, on the adjacent face
    i32 face{-1};
    i32 next{-1};     ///< next edge around `face`
};

struct HullFace {
    i32 firstEdge{-1};
    Vec4 normal{};
    f32 offset{0.0f};     ///< plane is dot(normal, x) == offset
};

struct Hull {
    std::vector<Vec4> vertices;
    std::vector<HullFace> faces;
    std::vector<HalfEdge> edges;

    bool IsValid() const { return !faces.empty(); }

    /// @brief Walk a face's edge loop, returning its vertices in winding order.
    std::vector<i32> FaceVertices(i32 face) const;
};

/// @brief The convex hull of a point cloud.
///
/// Returns an empty hull when the input encloses no volume -- fewer than four distinct points,
/// or all of them collinear or coplanar. That is a real answer, not a failure: a degenerate
/// hull has no volume and weighs nothing, and the caller has to decide what that means.
///
/// Quadratic in the point count, which is the right trade for authored collision hulls (tens
/// of vertices) and would not be for a mesh.
Hull BuildHull(std::span<const Vec4> points, f32 tolerance = 1e-5f);

/// @brief Scale every vertex about the origin. Topology is untouched.
///
/// The trap, and it is silent: a scale of zero collapses every vertex to the origin while
/// leaving the combinatorics perfect, so the shape inspects as a valid cube that encloses no
/// volume and weighs nothing. Nothing asserts. A host adapter must default the field to 1.0.
void ScaleHull(Hull& hull, f32 scale);

}  // namespace snowball
