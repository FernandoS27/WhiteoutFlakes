#include "m2_lighting.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::profiles::wow {

namespace {

// SetupSunlight's own guard: a squared magnitude at or below this leaves the
// direction at (0, 0, -1) instead of normalising.
constexpr f32 kDegenerateDirSq = 1.0e-5f;

} // namespace

void M2Lighting::AddAmbient(const Vector3f& color) {
    ambient_.x += color.x;
    ambient_.y += color.y;
    ambient_.z += color.z;
}

void M2Lighting::AddDiffuse(const Vector3f& color, const Vector3f& directionWS) {
    diffuse_ = color;
    direction_ = directionWS;
}

void M2Lighting::AddLight(const M2LightInput& light) {
    if (!light.visible)
        return;

    if (!light.positional) {
        AddAmbient(light.ambient);
        AddDiffuse(light.diffuse, light.directionWS);
        return;
    }

    const f32 dx = light.positionWS.x - centre_.x;
    const f32 dy = light.positionWS.y - centre_.y;
    const f32 dz = light.positionWS.z - centre_.z;
    const f32 distSq = dx * dx + dy * dy + dz * dz;

    // Full, and further than the worst kept light: nothing to do.
    if (pointCount_ >= kM2MaxPointLights && distSq >= pointDistSq_[kM2MaxPointLights - 1])
        return;

    // Insertion sort into a nearest-first array of fixed capacity. `<=` rather
    // than `<` so an equidistant light displaces the incumbent, matching the
    // client's shift loop.
    usize i = std::min<usize>(pointCount_, kM2MaxPointLights - 1);
    while (i > 0 && distSq <= pointDistSq_[i - 1]) {
        points_[i] = points_[i - 1];
        pointDistSq_[i] = pointDistSq_[i - 1];
        --i;
    }

    points_[i].positionWS = light.positionWS;
    points_[i].diffuse = light.diffuse;
    points_[i].attenuation = light.attenuation;
    pointDistSq_[i] = distSq;
    if (pointCount_ < kM2MaxPointLights)
        ++pointCount_;
}

M2LightingResult M2Lighting::Resolve() const {
    M2LightingResult r;

    // The diffuse colour is clamped per channel and the ambient is not — the
    // asymmetry is SetLocalLighting's, which runs fminf over the one and copies
    // the other. An interior with several ambient lights really does exceed 1.
    r.diffuse = {std::min(1.0f, diffuse_.x), std::min(1.0f, diffuse_.y),
                 std::min(1.0f, diffuse_.z)};
    r.ambient = ambient_;

    const f32 lenSq = direction_.x * direction_.x + direction_.y * direction_.y +
                      direction_.z * direction_.z;
    if (lenSq <= kDegenerateDirSq) {
        r.directionWS = {0.0f, 0.0f, -1.0f};
    } else {
        const f32 inv = 1.0f / std::sqrt(lenSq);
        r.directionWS = {direction_.x * inv, direction_.y * inv, direction_.z * inv};
    }

    r.pointCount = pointCount_;
    for (u32 i = 0; i < pointCount_; ++i)
        r.points[i] = points_[i];
    return r;
}

} // namespace whiteout::flakes::renderer::profiles::wow
