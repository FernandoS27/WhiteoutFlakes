#pragma once

/// @file
/// @brief Per-frame camera/lighting/fog parameters consumed by render backends.

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/vm/bytecode_exec_context.hpp>

#include <array>

namespace whiteout::cornflakes {

/// @brief Row-major 4x4 matrix used for view/projection.
struct Mat4 {
    std::array<f32, 16> m{
        1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F,
    };
    static constexpr Mat4 identity() noexcept {
        return Mat4{};
    }
};

/// @brief Per-light constants packed for shader upload.
struct ShaderLight {
    std::array<f32, 3> ambient{};
    f32 _pad0 = 0.0F;
    std::array<f32, 3> diffuse{};
    f32 _pad1 = 0.0F;
    std::array<f32, 3> position{};
    f32 type = 0.0F;
    std::array<f32, 4> _pad2{};
};

/// @brief Fog formula selector.
enum class FogMode : u8 {
    None = 0,
    Linear = 1,
    Exp = 2,
    ExpSq = 3,
};

/// @brief Per-frame view/projection/fog/lighting bundle handed to render backends.
struct ViewParams {

    Mat4 view = Mat4::identity();
    Mat4 proj = Mat4::identity();

    std::array<f32, 4> viewport{0.0F, 0.0F, 1280.0F, 720.0F};

    FogMode fogMode = FogMode::None;
    f32 fogStart = 0.0F;
    f32 fogEnd = 1.0e6F;
    f32 fogDensity = 0.0F;
    std::array<f32, 4> fogColor{0.0F, 0.0F, 0.0F, 1.0F};

    f32 effectTime = 0.0F;

    std::array<ShaderLight, 8> lights{};
    u32 lightCount = 0;

    f32 softParticlesScale = 0.0F;
};

} // namespace whiteout::cornflakes
