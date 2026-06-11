#pragma once

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/service/service_types.hpp>

namespace whiteout::cornflakes {

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

struct ShapeSampler {
    ShapeKind kind = ShapeKind::Sphere;
    ShapeSphere sphere;
    ShapeBox box;
};

struct ShapeSampleResult {
    Float3 position;
    Float3 normal;
};

ShapeSampleResult sampleShapeSurface(const ShapeSampler& s, f32 u, f32 v) noexcept;

}
