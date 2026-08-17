//===----------------------------------------------------------------------===//
// snowball/polytope.h -- a convex hull as a collidable shape.
//
// The volume is cached at construction rather than derived on demand: Volume() is not a
// computation at all, it returns the stored field. Worth knowing, because it means a polytope
// built badly reports a confident wrong number instead of failing.
//===----------------------------------------------------------------------===//
#pragma once

#include "snowball/common_types.h"
#include "snowball/hull.h"
#include "snowball/mass.h"
#include "snowball/transform.h"

namespace snowball {

/// Every polytope carries this collision margin; construction clamps to it as a minimum.
inline constexpr f32 kPolytopeMargin = 0.01f;

/// SetAsBox clamps each half extent to this before emitting corners.
inline constexpr f32 kMinBoxHalfExtent = 0.02f;

struct Polytope {
    Hull hull;
    f32 margin{kPolytopeMargin};
    f32 volume{0.0f};      ///< cached at construction, never recomputed
    Vec4 centroid{};
};

/// @brief Build a polytope from a point cloud, applying `scale` to the finished hull.
///
/// `scale` is a scale and not a margin -- passing 0 gives perfect topology enclosing no volume
/// (`ScaleHull`).
Polytope MakePolytope(std::span<const Vec4> points, f32 scale = 1.0f);

/// @brief The eight corners of a box, transformed, run through the hull builder.
///
/// Deliberately not a special case: asking for a box exercises the real hull builder, which is
/// what makes a cube's 8-vertex/6-face/24-half-edge invariant worth checking at all.
Polytope MakeBox(const Vec4& halfExtents, const Transform& xf = {}, f32 scale = 1.0f);

f32 Volume(const Polytope& p);

/// @brief Mass and inertia about the shape's own origin, matching the sphere's convention.
MassProperties ComputeMass(const Polytope& p, f32 density);

}  // namespace snowball
