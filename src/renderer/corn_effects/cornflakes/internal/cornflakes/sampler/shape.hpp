#pragma once

/// @file
/// @brief Surface-sampling primitive shapes (sphere/box) used by the simple sampler family.

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/service/service_types.hpp>

namespace whiteout::cornflakes {

/// @brief Discriminator for `ShapeSampler::kind`.
enum class ShapeKind : u8 {
    Sphere,
    Box,
};

struct ShapeSphere {
    Float3 center;
    f32 radius = 1.0F;
};

struct ShapeBox {
    Float3 center;
    Float3 halfExtents{0.5F, 0.5F, 0.5F};
};

/// @brief Sphere-or-box sampler; the active payload is selected by `kind`.
struct ShapeSampler {
    ShapeKind kind = ShapeKind::Sphere;
    ShapeSphere sphere;
    ShapeBox box;
};

/// @brief Surface sample (point + outward normal).
struct ShapeSampleResult {
    Float3 position;
    Float3 normal;
};

/// @brief Map `(u,v)` in `[0,1]²` to a point on the shape's surface.
ShapeSampleResult sampleShapeSurface(const ShapeSampler& s, f32 u, f32 v) noexcept;

} // namespace whiteout::cornflakes
