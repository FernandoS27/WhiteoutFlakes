//===----------------------------------------------------------------------===//
// snowball/shape.h -- any collidable shape, and the queries every kind answers.
//
// Shapes are deliberately plain data dispatched on a type tag, not a class hierarchy: with no
// polymorphic root they stay trivially copyable and poolable. The tag and the dispatch come
// from a variant rather than a hand-rolled enum-and-switch, which keeps that property in a
// form the compiler checks for exhaustiveness.
//===----------------------------------------------------------------------===//
#pragma once

#include <variant>

#include "snowball/capsule.h"
#include "snowball/common_types.h"
#include "snowball/heightfield.h"
#include "snowball/mass.h"
#include "snowball/polytope.h"
#include "snowball/sphere.h"
#include "snowball/tree_mesh.h"
#include "snowball/triangle.h"

namespace snowball {

/// The tags the narrowphase switches on. A triangle is a shape but never a fixture, which is
/// why this enumeration and the one a fixture is authored with are **not** the same list --
/// reusing one for both hands the distance solver a polytope tagged as a triangle, which reads
/// three vertices from the wrong place and produces plausible garbage.
///
/// The two mesh kinds are fixtures but never *convex* shapes: they answer "which triangles"
/// and the triangle entries do the rest, so they must never reach the distance solver or the
/// pair dispatch. Contacts branch on `IsMesh` before either.
enum class ShapeType { Sphere, Capsule, Triangle, Polytope, TreeMesh, HeightField };

using Shape = std::variant<Sphere, Capsule, Triangle, Polytope, TreeMesh, HeightField>;

ShapeType TypeOf(const Shape& shape);

inline bool IsMesh(ShapeType type) {
    return type == ShapeType::TreeMesh || type == ShapeType::HeightField;
}

/// @brief The shape's collision margin: its own radius, or a polytope's fixed 0.01.
f32 Margin(const Shape& shape);

/// @brief The shape's own idea of its centre, which the mesh paths cull one-sided triangles
/// against.
///
/// Each kind answers in its own terms: the sphere its centre, the capsule its midpoint, the
/// polytope its stored centroid, the triangle its vertex average. Meshes are never asked --
/// they are the thing being culled against -- and answer zero.
Vec4 ShapeCentroid(const Shape& shape);

f32 Volume(const Shape& shape);

/// @brief Mass and inertia about the shape's local origin.
///
/// A triangle has no volume, so it returns zero mass: meshes are static geometry and never
/// contribute to a body's mass.
MassProperties ComputeMass(const Shape& shape, f32 density);

/// @brief The two radii a fixture stores for the continuous-collision predicate: the shape's
/// thinnest feature and its farthest reach from the body origin.
///
/// Per shape: sphere {radius, |centre|}, capsule {radius, max(|p1|, |p2|)}, polytope
/// {nearest face plane from the centroid, farthest vertex}. `Scene::IsFast` compares a
/// quarter of the min against the step's motion times the max -- see scene.cpp.
void ShapeExtents(const Shape& shape, f32& minExtent, f32& maxExtent);

/// @brief The same shape expressed in another frame.
///
/// The nine narrowphase entry points take their shapes in one shared frame (only the two
/// face-face pairs accept a second transform), so a stepping world hands them world-space
/// copies rather than teaching each entry about body transforms. A polytope copies its hull to
/// do it, which is the honest cost of keeping the entries frame-free.
Shape TransformShape(const Shape& shape, const Transform& xf);

/// @brief World-space bounds, before the broadphase fattens them.
///
/// The core geometry expanded by the shape's margin -- one rule for all four, since a sphere's
/// radius and a polytope's 0.01 skin enter identically.
Aabb ComputeAabb(const Shape& shape, const Transform& xf);

}  // namespace snowball
