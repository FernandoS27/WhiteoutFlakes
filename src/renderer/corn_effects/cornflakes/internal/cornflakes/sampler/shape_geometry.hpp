
#pragma once

#include <cornflakes/interface/binding/sampler_resource.hpp>
#include <cornflakes/interface/core/types.hpp>

#include <array>

namespace whiteout::cornflakes {

using ShapePoint = std::array<f32, 3>;

[[nodiscard]] bool shapeContains(const SamplerShape& shape, const ShapePoint& local) noexcept;

[[nodiscard]] f32 shapeDistanceField(const SamplerShape& shape, const ShapePoint& local) noexcept;

[[nodiscard]] ShapePoint shapeProject(const SamplerShape& shape, const ShapePoint& local) noexcept;

[[nodiscard]] ShapePoint shapeSurfaceNormal(const SamplerShape& shape,
                                            const ShapePoint& local) noexcept;

[[nodiscard]] bool shapeIntersect(const SamplerShape& shape, const ShapePoint& origin,
                                  const ShapePoint& direction, f32 length, bool twoSided,
                                  f32& outT) noexcept;

}
