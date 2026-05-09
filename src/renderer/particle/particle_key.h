#pragma once

#include "whiteout/flakes/types.h"
#include "types.h"

namespace whiteout::flakes::renderer::particle {

struct ImVector {
    u8 a = 0, r = 0, g = 0, b = 0;

    static ImVector FromFloat(f32 rf, f32 gf, f32 bf, f32 af) {
        auto clamp8 = [](f32 v) -> u8 {
            if (v <= 0.0f) return 0;
            if (v >= 1.0f) return 255;
            return static_cast<u8>(v * 255.0f);
        };
        return { clamp8(af), clamp8(rf), clamp8(gf), clamp8(bf) };
    }

    Vector4f ToVec4() const {
        return { r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f };
    }
};

struct ParticleKey {
    f32      endTime        = 0.0f;
    ImVector startColor;
    ImVector endColor;
    f32      startScale     = 1.0f;
    f32      endScale       = 1.0f;
    i32      headCellStart  = 0;
    i32      headCellEnd    = 0;
    i32      headCellRepeat = 0;
    i32      tailCellStart  = 0;
    i32      tailCellEnd    = 0;
    i32      tailCellRepeat = 0;

    void Interpolate(f32 age,
                     f32 prevEndTime,
                     ImVector& outColor,
                     i32& outHeadCell,
                     i32& outTailCell,
                     f32& outScale) const;
};

}
