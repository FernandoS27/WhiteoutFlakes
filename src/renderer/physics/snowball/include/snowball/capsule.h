//===----------------------------------------------------------------------===//
// snowball/capsule.h -- a segment with a radius.
//===----------------------------------------------------------------------===//
#pragma once

#include "snowball/common_types.h"
#include "snowball/mass.h"
#include "snowball/vec.h"

namespace snowball {

struct Capsule {
    Vec4 p1{};
    Vec4 p2{};
    f32 radius{0.0f};
};

/// @brief pi r^2 L + (4/3) pi r^3 -- a cylinder plus two hemispherical caps.
///
/// Built from the shared constants in one fixed association,
/// `(r*r * pi) * L + (((2 * 2pi) * (1/3)) * r*r) * r`. The grouping is deliberate: any
/// reassociation can shift the last ULP, and the exact bits are part of the numeric contract.
f32 Volume(const Capsule& c);

/// @brief Mass and inertia about the capsule's own centre.
///
/// **The cap term disagrees with @ref Volume by a factor of pi, and that is deliberate.**
/// @ref Volume builds the caps as `((2 * 2pi) * 1/3) * r^3` while ComputeMass uses a bare
/// `0.66666669 * r^3` -- pi is folded into the cylinder term in both, and omitted from
/// the caps in one. So a capsule weighs less than its own volume says, and a sphere of
/// the same radius outweighs a capsule's two caps by pi (the sphere's ComputeMass does
/// use 4pi/3). This is part of the engine's numeric contract, not a bug: densities in
/// authored assets are tuned against this arithmetic, so "fixing" the factor would make
/// every capsule-based rig heavier than it was tuned to be.
///
/// The tensor is not the textbook assembly either: the local diagonal is built with
/// **+Y as the capsule axis** and then rotated by the quaternion that carries `+Y` onto
/// the segment, rather than assembled in place. Cap and cylinder are separate rigid
/// bodies joined by the parallel-axis theorem -- hemisphere transverse `0.259375 m r^2`
/// (83/320, about its own centroid) offset by `3r/8 + L/2`, cylinder `r^2/4 + L^2/12`.
MassProperties ComputeMass(const Capsule& c, f32 density);

}  // namespace snowball
