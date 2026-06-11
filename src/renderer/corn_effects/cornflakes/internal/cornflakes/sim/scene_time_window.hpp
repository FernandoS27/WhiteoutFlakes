#pragma once

#include <cornflakes/interface/core/types.hpp>

namespace whiteout::cornflakes {

struct SceneTimeWindow {
    f32 start = 0.0F;
    f32 end = 0.0F;

    constexpr f32 dt() const noexcept {
        return end - start;
    }
};

}
