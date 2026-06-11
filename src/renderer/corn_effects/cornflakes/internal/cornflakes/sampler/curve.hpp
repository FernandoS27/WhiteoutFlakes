#pragma once

#include <cornflakes/interface/core/types.hpp>

#include <span>

namespace whiteout::cornflakes {

struct CurveKnot {
    f32 t;
    f32 value;
};

f32 sampleCurveScalar(std::span<const CurveKnot> knots, f32 t) noexcept;

}
