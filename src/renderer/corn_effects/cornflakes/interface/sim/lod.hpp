#pragma once

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/vm/bytecode_exec_context.hpp>

#include <array>
#include <span>

namespace whiteout::cornflakes {

struct LodConfig {
    f32 defaultLod = 0.0F;
    f32 minMinDist = 2.0F;
    f32 minDist = 5.0F;
    f32 maxDist = 200.0F;
    f32 frustumBiasBehind = 0.7F;
    f32 frustumBiasOutside = 0.4F;
};

struct LodConfigPrecomp {
    f32 minMinDistSq = 4.0F;
    f32 minDistSqrt = 0.0F;
    f32 maxDistSqrt = 0.0F;

    LodConfigPrecomp() noexcept = default;
    explicit LodConfigPrecomp(const LodConfig& cfg) noexcept;
};

f32 computeLodMetric(const LodConfig& cfg, const LodConfigPrecomp& pc, const SceneCamera& cam,
                     const std::array<f32, 3>& boxMin,
                     const std::array<f32, 3>& boxMax) noexcept;

f32 computeLodLevel(const LodConfig& cfg, std::span<const SceneCamera> cameras,
                    const std::array<f32, 3>& boxMin, const std::array<f32, 3>& boxMax,
                    bool boxValid) noexcept;

}
