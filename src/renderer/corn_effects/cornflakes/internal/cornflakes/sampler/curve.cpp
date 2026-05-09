#include <cornflakes/core/determinism.hpp>
#include <cornflakes/sampler/curve.hpp>

namespace whiteout::cornflakes {

f32 sampleCurveScalar(std::span<const CurveKnot> knots, f32 t) noexcept {
    if (knots.empty()) {
        return 0.0F;
    }
    if (t <= knots.front().t) {
        return knots.front().value;
    }
    if (t >= knots.back().t) {
        return knots.back().value;
    }

    for (std::size_t i = 1; i < knots.size(); ++i) {
        if (t <= knots[i].t) {
            const f32 denom = knots[i].t - knots[i - 1].t;
            const f32 alpha = (denom > 0.0F) ? ((t - knots[i - 1].t) / denom) : 0.0F;
            return knots[i - 1].value + alpha * (knots[i].value - knots[i - 1].value);
        }
    }
    return knots.back().value;
}

} // namespace whiteout::cornflakes
