//===----------------------------------------------------------------------===//
// snowball/sphere.h -- the sphere shape and its mass properties.
//
// Shapes are plain data with no vtable: the engine dispatches on an enum tag rather than
// through a virtual interface, so a shape carries no pointers at all and stays trivially
// copyable and poolable.
//===----------------------------------------------------------------------===//
#pragma once

#include "snowball/common_types.h"
#include "snowball/mass.h"
#include "snowball/vec.h"

namespace snowball {

struct Sphere {
    Vec4 centre{};      ///< local position
    f32 radius{0.0f};
};

/// @brief (4/3)pi r^3, assembled from the shared constants in one fixed association.
///
/// `((r*r) * ((kOneThird * kTwo) * kTwoPi)) * r`. Algebraically ordinary, but the grouping is
/// deliberate: evaluating it any other way can move the last ULP, and regression baselines pin
/// these exact bits.
f32 Volume(const Sphere& s);

/// @brief Mass and inertia at unit density scaled by `density`.
///
/// `mass = ((r*r) * (density * 4.1887903)) * r`, with 4pi/3 folded to a literal, and the
/// diagonal term `(r*r) * (0.4 * mass)` for a solid sphere. The tensor is that scalar on the
/// diagonal plus the parallel-axis terms from the local position.
MassProperties ComputeMass(const Sphere& s, f32 density);

/// @brief Do two spheres overlap? Centre distance against the sum of the radii.
bool TestOverlap(const Sphere& a, const Sphere& b);

}  // namespace snowball
