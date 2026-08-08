#pragma once

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/sim/proximity_hash.hpp>
#include <cornflakes/interface/vm/bytecode_exec_context.hpp>

#include <cstddef>
#include <span>

namespace whiteout::cornflakes::simt {

struct LayerFrameUniforms {
    Mat4x3 sceneL2W = Mat4x3::identity();
    std::span<const SceneCamera> cameras;

    f32 simLod = 0.0F;
    f32 simLodDistanceMin = 5.0F;
    f32 simLodDistanceMax = 200.0F;

    f32 effectAge = 0.0F;
    bool effectIsRunning = true;

    f32 initSceneTime = 0.0F;

    std::span<ProximityHash* const> spatialHashesRead;
    std::span<ProximityHash* const> spatialHashesWrite;
};

inline constexpr std::size_t kUniformWritesPerParticlePerFrame = 8U;

}
