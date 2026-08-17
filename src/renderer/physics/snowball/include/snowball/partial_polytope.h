//===----------------------------------------------------------------------===//
// snowball/partial_polytope.h -- a triangle promoted to something the SAT can run against.
//
// The triangle/polytope entry does not collide a triangle. It collides a four-plane convex body
// built from the triangle *and its neighbours*, and that is the whole internal-edge fix: three
// of the four planes come from the adjacent triangles, so a face can never push a body along an
// edge that is really interior to a flat surface.
//
// Two of the four planes' jobs are easy to conflate and are not the same:
//
//   * As **separating axes** the edge planes use their adjacency-modified normals, and an edge
//     plane that wins the sweep means the contact belongs to the neighbour -- so this triangle
//     produces *nothing* and the neighbour produces it. That is what stops a body picking up two
//     contacts as it crosses a seam.
//   * As **clip planes** the face contact uses the plain geometric side planes,
//     `cross(edge, faceNormal)`, not the adjacency-modified ones. Reproducing the modified
//     planes here instead quietly narrows every contact patch near a fold.
//===----------------------------------------------------------------------===//
#pragma once

#include "snowball/common_types.h"
#include "snowball/triangle.h"

namespace snowball {

/// A neighbour must rise by more than this above the face plane to count as convex.
inline constexpr f32 kAdjacencyEpsilon = 1.1920929e-06f;

/// How close to the face normal an edge axis has to be for `CheckAxis` to wave it through:
/// `cos(5 degrees)`, the same constant the manifold clustering uses.
inline constexpr f32 kAxisFaceCosine = 0.99619472f;

struct Plane {
    Vec4 normal{};
    f32 offset{0.0f};

    f32 Distance(const Vec4& p) const { return Dot3(normal, p) - offset; }
};

/// A triangle and the four planes it collides through.
struct PartialPolytope {
    Vec4 v1{};
    Vec4 v2{};
    Vec4 v3{};
    /// Face first, then one per edge in the order v1->v2, v2->v3, v3->v1.
    Plane planes[4]{};
    Vec4 centroid{};
};

/// @brief Build the four planes, from the triangle and its neighbours.
///
/// The edge planes are where the neighbours earn their place, and each case is deliberate
/// behaviour worth stating:
///
///   * **Convex neighbour** (it rises more than `kAdjacencyEpsilon` above the face): the edge
///     plane becomes the *face* normal. The edge is a real silhouette, and clamping to the face
///     is what stops a body being pushed sideways off a ridge.
///   * **Concave or flat neighbour**: the plane through the edge that contains the neighbour, so
///     contacts are free to lie in the fold. For an exactly coplanar neighbour that *is* the
///     face plane again, which is the fix doing its job.
///   * **No neighbour at all**: the *reversed* face normal, which no contact can be outside.
///     An open edge therefore constrains nothing, and a mesh with missing adjacency silently
///     behaves like one with no edge planes rather than one walled on every edge.
///
/// There is no zero guard on the face normal: a zero-area triangle gives every plane a NaN.
/// The missing guard is deliberate and part of the engine's numeric contract; do not add one.
PartialPolytope MakePartialPolytope(const Triangle& t);

/// @brief Is `axis` a direction this triangle is allowed to push along?
///
/// The internal-edge fix applied to *edge* axes. An axis within 5 degrees of the face normal is
/// waved through; otherwise the triangle works out which edge's Voronoi region the axis falls
/// into -- by the sign of `dot(cross(vertex - centroid, axis), faceNormal)` around the fan --
/// and tests it against that edge's plane alone. An axis pointing into territory a neighbour
/// occupies is refused, and the contact is left for the neighbour to generate.
///
/// **Retained but deliberately uncalled.** `CollideTrianglePolytope` does not consult this
/// gate; leaving it out of that path is part of the engine's contract, not an oversight, and
/// wiring it back in would change which contacts a mesh emits. It stays here, under test, as
/// the reference for edge-axis ownership.
bool CheckAxis(const PartialPolytope& p, const Vec4& axis);

}  // namespace snowball
