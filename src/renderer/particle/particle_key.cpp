#include "renderer/particle/particle_key.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::particle {

namespace {

inline u8 byte_lerp(u8 a, u8 b, f32 t) {

    f32 v = static_cast<f32>(a) + (static_cast<f32>(b) - static_cast<f32>(a)) * t;
    if (v <= 0.0f)
        return 0;
    if (v >= 255.0f)
        return 255;
    return static_cast<u8>(v + 0.5f);
}

inline i32 ComputeCell(i32 start, i32 end, i32 repeat, f32 t) {

    f32 r = (repeat < 1) ? 1.0f : static_cast<f32>(repeat);

    i32 initial = (end >= start) ? start : (start + 1);
    i32 delta = (end >= start) ? (end - start + 1) : (end - start - 1);

    f32 effT = (r == 1.0f) ? t : std::fmod(t * r, 1.0f);
    f32 val = static_cast<f32>(initial) + static_cast<f32>(delta) * effT;
    if (val < 0.0f)
        val = 0.0f;
    return static_cast<i32>(val);
}

} // namespace

void ParticleKey::Interpolate(f32 age, f32 prevEndTime, ImVector& outColor, i32& outHeadCell,
                              i32& outTailCell, f32& outScale) const {
    f32 span = endTime - prevEndTime;
    f32 rawT = (span > 0.0f) ? (age - prevEndTime) / span : 0.0f;
    if (rawT < 0.0f)
        rawT = 0.0f;
    if (rawT > 1.0f)
        rawT = 1.0f;

    const f32 t = rawT * 0.99f + 0.005f;

    outColor.a = byte_lerp(startColor.a, endColor.a, t);
    outColor.r = byte_lerp(startColor.r, endColor.r, t);
    outColor.g = byte_lerp(startColor.g, endColor.g, t);
    outColor.b = byte_lerp(startColor.b, endColor.b, t);

    outScale = startScale + (endScale - startScale) * t;

    outHeadCell = ComputeCell(headCellStart, headCellEnd, headCellRepeat, t);
    outTailCell = ComputeCell(tailCellStart, tailCellEnd, tailCellRepeat, t);
}

} // namespace whiteout::flakes::renderer::particle
