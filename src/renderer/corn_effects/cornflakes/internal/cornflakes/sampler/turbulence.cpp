#include <cornflakes/core/determinism.hpp>
#include <cornflakes/interface/core/fast_rand.hpp>
#include <cornflakes/sampler/turbulence.hpp>

#include <cmath>

namespace whiteout::cornflakes {

namespace {

u32 hashCell(u32 seed, i32 x, i32 y, i32 z) noexcept {
    u32 state = seed;
    state = TFastRandU32::advanceStatic(state ^ static_cast<u32>(x));
    state = TFastRandU32::advanceStatic(state ^ static_cast<u32>(y));
    state = TFastRandU32::advanceStatic(state ^ static_cast<u32>(z));
    return state;
}

f32 cellValue(u32 seed, i32 x, i32 y, i32 z) noexcept {
    constexpr f32 kScale = 1.0F / static_cast<f32>(1U << 24);
    const u32 top24 = hashCell(seed, x, y, z) >> 8U;
    return static_cast<f32>(top24) * kScale;
}

f32 lerpScalar(f32 a, f32 b, f32 t) noexcept {
    return a + t * (b - a);
}

f32 fade(f32 t) noexcept {
    return t * t * (3.0F - 2.0F * t);
}

} // namespace

f32 sampleTurbulence3D(const TurbulenceSampler& t, Float3 pos) noexcept {
    const f32 sx = pos.x * t.frequency;
    const f32 sy = pos.y * t.frequency;
    const f32 sz = pos.z * t.frequency;

    const i32 x0 = static_cast<i32>(std::floor(sx));
    const i32 y0 = static_cast<i32>(std::floor(sy));
    const i32 z0 = static_cast<i32>(std::floor(sz));

    const f32 fx = fade(sx - static_cast<f32>(x0));
    const f32 fy = fade(sy - static_cast<f32>(y0));
    const f32 fz = fade(sz - static_cast<f32>(z0));

    const f32 c000 = cellValue(t.seed, x0, y0, z0);
    const f32 c100 = cellValue(t.seed, x0 + 1, y0, z0);
    const f32 c010 = cellValue(t.seed, x0, y0 + 1, z0);
    const f32 c110 = cellValue(t.seed, x0 + 1, y0 + 1, z0);
    const f32 c001 = cellValue(t.seed, x0, y0, z0 + 1);
    const f32 c101 = cellValue(t.seed, x0 + 1, y0, z0 + 1);
    const f32 c011 = cellValue(t.seed, x0, y0 + 1, z0 + 1);
    const f32 c111 = cellValue(t.seed, x0 + 1, y0 + 1, z0 + 1);

    const f32 x00 = lerpScalar(c000, c100, fx);
    const f32 x10 = lerpScalar(c010, c110, fx);
    const f32 x01 = lerpScalar(c001, c101, fx);
    const f32 x11 = lerpScalar(c011, c111, fx);
    const f32 y0v = lerpScalar(x00, x10, fy);
    const f32 y1v = lerpScalar(x01, x11, fy);
    const f32 zv = lerpScalar(y0v, y1v, fz);

    return zv * t.amplitude;
}

} // namespace whiteout::cornflakes
