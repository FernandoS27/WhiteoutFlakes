#include <cornflakes/interface/sim/lod.hpp>

#include <algorithm>
#include <cmath>

namespace whiteout::cornflakes {

namespace {

f32 saturate(f32 v) noexcept {
    return std::min(1.0F, std::max(0.0F, v));
}

f32 computeDistanceMetric(const LodConfigPrecomp& pc, f32 dSq) noexcept {
    if (pc.minMinDistSq >= dSq) {
        return 0.0F;
    }
    const f32 vd = std::sqrt(std::sqrt(dSq));
    return saturate((vd - pc.minDistSqrt) / (pc.maxDistSqrt - pc.minDistSqrt));
}

std::array<f32, 3> closestPointInBox(const std::array<f32, 3>& p, const std::array<f32, 3>& lo,
                                     const std::array<f32, 3>& hi) noexcept {
    return {std::min(std::max(p[0], lo[0]), hi[0]), std::min(std::max(p[1], lo[1]), hi[1]),
            std::min(std::max(p[2], lo[2]), hi[2])};
}

bool boxTouchesFrustum(const SceneCamera& cam, const std::array<f32, 3>& centre,
                       const std::array<f32, 3>& halfExtent) noexcept {
    for (const auto& plane : cam.frustum) {
        const f32 dist = (plane[0] * centre[0]) + (plane[1] * centre[1]) + (plane[2] * centre[2]) +
                         plane[3];
        const f32 radius = (std::fabs(plane[0]) * halfExtent[0]) +
                           (std::fabs(plane[1]) * halfExtent[1]) +
                           (std::fabs(plane[2]) * halfExtent[2]);
        if (dist + radius < 0.0F) {
            return false;
        }
    }
    return true;
}

}

LodConfigPrecomp::LodConfigPrecomp(const LodConfig& cfg) noexcept
    : minMinDistSq(cfg.minMinDist * cfg.minMinDist), minDistSqrt(std::sqrt(cfg.minDist)),
      maxDistSqrt(std::sqrt(cfg.maxDist)) {}

f32 computeLodMetric(const LodConfig& cfg, const LodConfigPrecomp& pc, const SceneCamera& cam,
                     const std::array<f32, 3>& boxMin, const std::array<f32, 3>& boxMax) noexcept {
    const std::array<f32, 3>& viewPos = cam.position;
    const std::array<f32, 3> closest = closestPointInBox(viewPos, boxMin, boxMax);
    const f32 dx = viewPos[0] - closest[0];
    const f32 dy = viewPos[1] - closest[1];
    const f32 dz = viewPos[2] - closest[2];
    const f32 distanceMetric = computeDistanceMetric(pc, (dx * dx) + (dy * dy) + (dz * dz));
    if (distanceMetric == 0.0F) {
        return 0.0F;
    }

    const std::array<f32, 3>& viewDir = cam.basis[1];
    const f32 planeD = -((viewDir[0] * viewPos[0]) + (viewDir[1] * viewPos[1]) +
                         (viewDir[2] * viewPos[2]));

    const std::array<f32, 3> centre{(boxMin[0] + boxMax[0]) * 0.5F, (boxMin[1] + boxMax[1]) * 0.5F,
                                    (boxMin[2] + boxMax[2]) * 0.5F};
    const std::array<f32, 3> halfExtent{boxMax[0] - centre[0], boxMax[1] - centre[1],
                                        boxMax[2] - centre[2]};

    const f32 boxRadius = (std::fabs(viewDir[0]) * halfExtent[0]) +
                          (std::fabs(viewDir[1]) * halfExtent[1]) +
                          (std::fabs(viewDir[2]) * halfExtent[2]);
    const f32 distanceToPlane = (viewDir[0] * centre[0]) + (viewDir[1] * centre[1]) +
                                (viewDir[2] * centre[2]) + planeD;

    f32 frustumMetricBias = 0.0F;
    if (distanceToPlane + boxRadius < 0.0F) {
        frustumMetricBias = cfg.frustumBiasBehind;
    } else if (cam.hasFrustum && !boxTouchesFrustum(cam, centre, halfExtent)) {
        frustumMetricBias = cfg.frustumBiasOutside;
    }

    return saturate(distanceMetric + frustumMetricBias + cam.lodBias);
}

f32 computeLodLevel(const LodConfig& cfg, std::span<const SceneCamera> cameras,
                    const std::array<f32, 3>& boxMin, const std::array<f32, 3>& boxMax,
                    bool boxValid) noexcept {
    if (cameras.empty()) {
        return cfg.defaultLod;
    }
    const LodConfigPrecomp pc{cfg};
    f32 level = 10.0F;
    for (const auto& cam : cameras) {
        if (boxValid) {
            level = std::min(level, computeLodMetric(cfg, pc, cam, boxMin, boxMax));
        } else {
            level = cam.lodBias;
        }
    }
    return level;
}

}
