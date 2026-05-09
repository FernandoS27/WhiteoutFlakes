#pragma once

/// @file
/// @brief `[start, end)` time interval handed to per-page evolve tasks.

#include <cornflakes/interface/core/types.hpp>

namespace whiteout::cornflakes {

/// @brief Half-open time window in scene seconds.
struct SceneTimeWindow {
    f32 start = 0.0F;
    f32 end = 0.0F;

    constexpr f32 dt() const noexcept {
        return end - start;
    }
};

} // namespace whiteout::cornflakes
