//===----------------------------------------------------------------------===//
// snowball/raycast.h -- shooting a ray at the world.
//
// The other question a rendering service asks of collision geometry, and the one that has
// nothing to do with the solver: what is under the cursor, where does the ground sit, is there
// line of sight. So it is here rather than folded into the contact path.
//
// Rays are **one-sided**, like every other triangle entry: a ray leaving the ground from below
// passes straight through. And a ray always reports the *nearest* hit -- both traversals shorten
// `maxFraction` as they go, so a later cell or leaf only reports something closer.
//===----------------------------------------------------------------------===//
#pragma once

#include "snowball/common_types.h"
#include "snowball/heightfield.h"
#include "snowball/transform.h"
#include "snowball/triangle.h"

namespace snowball {

struct RayCastInput {
    Vec4 p1{};
    Vec4 p2{};
    /// How far along `p1 -> p2` to accept a hit. Shortened in place as a traversal finds closer
    /// ones, which is what makes "nearest" fall out of an unordered walk.
    f32 maxFraction{1.0f};
    /// Tested against a mesh triangle's own mask with a bitwise AND, and a triangle sharing no
    /// bit with the ray is skipped. Height fields have no equivalent and ignore it.
    u16 mask{0xFFFF};
};

struct RayCastOutput {
    Vec4 normal{};
    f32 fraction{0.0f};
    i32 triangle{-1};
    u16 material{0};
};

/// @brief Intersect a segment with one triangle, in the triangle's own frame.
///
/// Three scalar triple products decide containment before any division happens -- the segment
/// has to pass on the inner side of all three edges -- and only then is the plane crossing
/// solved. The normal comes back **unflipped**, so it always points out of the front face.
///
/// The fraction is a true divide. Most of the engine reaches for `Rcp` instead; this one does
/// not, deliberately, and the difference is visible in the last bits of a hit position.
bool TriangleRayCast(const Triangle& t, const Vec4& p1, const Vec4& p2, f32 maxFraction,
                     RayCastOutput& out);

/// @brief Cast against terrain, walking the grid cell by cell.
///
/// A two-dimensional DDA in the field's own frame: the ray is brought into local space once, the
/// cells it crosses are visited in order, and each solid one tests its two triangles. Cells
/// outside the grid and cells flagged as holes are stepped over rather than clamped.
bool HeightFieldRayCast(const HeightField& field, const RayCastInput& input, const Transform& xf,
                        RayCastOutput& out);

}  // namespace snowball
