//===----------------------------------------------------------------------===//
// snowball/transform.h -- rigid transforms and axis-aligned bounds.
//
// Quaternions are stored xyzw with the **scalar last**, which the engine fixes by way of its
// identity constant {0,0,0,1}. Getting that backwards is quiet and catastrophic: a
// scalar-first reader sees the identity as a 180-degree rotation about x.
//===----------------------------------------------------------------------===//
#pragma once

#include "snowball/common_types.h"
#include "snowball/math.h"
#include "snowball/vec.h"

namespace snowball {

struct Transform {
    Vec4 rotation{constants::kQuatIdentity};
    Vec4 position{};
};

struct Aabb {
    Vec4 lower{};
    Vec4 upper{};

    constexpr Vec4 Centre() const { return (lower + upper) * 0.5f; }
    constexpr Vec4 Extent() const { return (upper - lower) * 0.5f; }
};

/// @brief The rotation matrix for a unit quaternion, as three column basis images.
Mtx RotationMatrix(const Vec4& q);

/// @brief Hamilton product, scalar last.
Vec4 QuatMultiply(const Vec4& a, const Vec4& b);

/// @brief Renormalise, through the approximate reciprocal square root the engine uses.
///
/// Normalisation routes through `Rsqrt` -- estimate plus one Newton step -- rather than a true
/// divide, deliberately: the approximation is part of the numeric contract, and every rotated
/// body inherits the difference, so rewriting this as `q / sqrt(len2)` shifts bits in every
/// orientation downstream.
Vec4 QuatNormalize(const Vec4& q);

/// @brief Advance an orientation by a world-frame angular velocity over `dt`.
Vec4 IntegrateRotation(const Vec4& q, const Vec4& angularVelocity, f32 dt);

/// @brief The tight axis-aligned bounds of `box` after `xf` is applied.
///
/// Rotating the extent by the absolute-value matrix, rather than transforming eight corners:
/// same answer, and it is the standard form for this operation.
Aabb TransformAabb(const Transform& xf, const Aabb& box);

}  // namespace snowball
