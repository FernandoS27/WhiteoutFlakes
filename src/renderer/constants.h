#pragma once

#include <whiteout/vector_types.h>
#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/coordinate_system.h"

namespace whiteout::flakes::renderer {

using whiteout::Vector3f;
using whiteout::Vector4f;

constexpr f32 kMaxSimulationDt = 0.5f;

constexpr f32 kMinVisibilityThreshold = 0.0f;
constexpr f32 kRibbonMinLifespan = 0.25f;

constexpr f32 kVectorEpsilon = 1e-6f;
constexpr f32 kBillboardDistThreshold = 0.001f;

constexpr Vector4f kGeosetLightColor = {0.95f, 0.92f, 0.85f, 1.0f};
constexpr Vector4f kGeosetAmbientColor = {0.22f, 0.24f, 0.30f, 0.0f};

constexpr Vector4f kHdBaselineLightColor = {0.9f, 0.9f, 0.9f, 1.0f};
constexpr Vector4f kHdBaselineAmbientColor = {0.3f, 0.3f, 0.3f, 0.0f};

constexpr Vector4f kParticleLightColor = {0.85f, 0.85f, 0.80f, 1.0f};
constexpr Vector4f kParticleAmbientBase = {0.35f, 0.35f, 0.40f, 0.0f};

constexpr Vector4f kCollisionLightColor = {1.0f, 1.0f, 1.0f, 1.0f};
constexpr Vector4f kCollisionAmbientColor = {1.0f, 1.0f, 1.0f, 0.0f};

namespace detail {
inline Vector4f LiftLightDir(Vector3f maxDir) {
    Vector3f d =
        CoordinateSystem::ConvertDirection(CoordSpace::Max, CoordinateSystem::Default(), maxDir);
    return {d.x, d.y, d.z, 0.0f};
}
} // namespace detail
inline const Vector4f kDefaultLightDir = detail::LiftLightDir({0.0f, -0.3f, -0.8f});

} // namespace whiteout::flakes::renderer
