#include <cornflakes/core/determinism.hpp>
#include <cornflakes/interface/core/fast_rand.hpp>
#include <cornflakes/sampler/turbulence.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>

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

}

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

namespace {

constexpr std::array<i32, 256> kFastNoisePerm = {
    151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,   225,
    140, 36,  103, 30,  69,  142, 8,   99,  37,  240, 21,  10,  23,  190, 6,   148,
    247, 120, 234, 75,  0,   26,  197, 62,  94,  252, 219, 203, 117, 35,  11,  32,
    57,  177, 33,  88,  237, 149, 56,  87,  174, 20,  125, 136, 171, 168, 68,  175,
    74,  165, 71,  134, 139, 48,  27,  166, 77,  146, 158, 231, 83,  111, 229, 122,
    60,  211, 133, 230, 220, 105, 92,  41,  55,  46,  245, 40,  244, 102, 143, 54,
    65,  25,  63,  161, 1,   216, 80,  73,  209, 76,  132, 187, 208, 89,  18,  169,
    200, 196, 135, 130, 116, 188, 159, 86,  164, 100, 109, 198, 173, 186, 3,   64,
    52,  217, 226, 250, 124, 123, 5,   202, 38,  147, 118, 126, 255, 82,  85,  212,
    207, 206, 59,  227, 47,  16,  58,  17,  182, 189, 28,  42,  223, 183, 170, 213,
    119, 248, 152, 2,   44,  154, 163, 70,  221, 153, 101, 155, 167, 43,  172, 9,
    129, 22,  39,  253, 19,  98,  108, 110, 79,  113, 224, 232, 178, 185, 112, 104,
    218, 246, 97,  228, 251, 34,  242, 193, 238, 210, 144, 12,  191, 179, 162, 241,
    81,  51,  145, 235, 249, 14,  239, 107, 49,  192, 214, 31,  181, 199, 106, 157,
    184, 84,  204, 176, 115, 121, 50,  45,  127, 4,   150, 254, 138, 236, 205, 93,
    222, 114, 67,  29,  24,  72,  243, 141, 128, 195, 78,  66,  215, 61,  156, 180,
};

constexpr Float3 kPotentialOffset0{0.0F, 0.0F, 0.0F};
constexpr Float3 kPotentialOffset1{31.416F, 47.853F, 12.793F};
constexpr Float3 kPotentialOffset2{-42.137F, 17.310F, 73.911F};

f32 fadeT(f32 t, u32 interp) noexcept {
    switch (interp) {
    case 1U:
        return ((6.0F * t - 15.0F) * t + 10.0F) * t * t * t;
    case 2U:
        return t * t * (3.0F - 2.0F * t);
    default:
        return t;
    }
}

f32 coherentNoise(Float3 p, std::span<const f32> grad, u32 interp) noexcept {
    const f32 fx = std::floor(p.x);
    const f32 fy = std::floor(p.y);
    const f32 fz = std::floor(p.z);
    const i32 cellX = static_cast<i32>(fx);
    const i32 cellY = static_cast<i32>(fy);
    const i32 cellZ = static_cast<i32>(fz);

    const f32 u = fadeT(p.x - fx, interp);
    const f32 v = fadeT(p.y - fy, interp);
    const f32 w = fadeT(p.z - fz, interp);

    const auto corner = [&](i32 xi, i32 yi, i32 zi) -> f32 {
        const i32 hx = kFastNoisePerm[static_cast<std::size_t>(xi & 255)];
        const i32 hy = kFastNoisePerm[static_cast<std::size_t>((hx + yi) & 255)];
        const i32 h = kFastNoisePerm[static_cast<std::size_t>((hy + zi) & 255)];
        return grad[static_cast<std::size_t>(h & 255)];
    };

    const f32 c000 = corner(cellX, cellY, cellZ);
    const f32 c100 = corner(cellX + 1, cellY, cellZ);
    const f32 c010 = corner(cellX, cellY + 1, cellZ);
    const f32 c110 = corner(cellX + 1, cellY + 1, cellZ);
    const f32 c001 = corner(cellX, cellY, cellZ + 1);
    const f32 c101 = corner(cellX + 1, cellY, cellZ + 1);
    const f32 c011 = corner(cellX, cellY + 1, cellZ + 1);
    const f32 c111 = corner(cellX + 1, cellY + 1, cellZ + 1);

    const f32 x00 = c000 + u * (c100 - c000);
    const f32 x10 = c010 + u * (c110 - c010);
    const f32 x01 = c001 + u * (c101 - c001);
    const f32 x11 = c011 + u * (c111 - c011);
    const f32 y0v = x00 + v * (x10 - x00);
    const f32 y1v = x01 + v * (x11 - x01);
    return y0v + w * (y1v - y0v);
}

f32 fbmPotential(const SamplerTurbulence& t, Float3 p, Float3 offset) noexcept {
    f32 v6 = t.globalScale * t.wavelength;
    if (!(v6 > 0.0F) && !(v6 < 0.0F)) {
        v6 = 1.0e-7F;
    }
    const u32 octaves = std::max(1U, t.octaves);
    const f32 lac =
        std::max(t.lacunarity, std::pow(1.0e-7F / v6, 1.0F / static_cast<f32>(octaves)));
    const f32 g = t.gain * t.gainMultiplier;
    f32 amp = t.globalScale * t.strength;

    const auto noiseAt = [&](f32 invFreq) -> f32 {
        const Float3 q{invFreq * p.x + offset.x, invFreq * p.y + offset.y,
                       invFreq * p.z + offset.z};
        return coherentNoise(q, t.gradients, t.interpolator);
    };

    if (lac == 1.0F || g == 0.0F) {
        f32 ampSum = 0.0F;
        f32 a = amp;
        for (u32 i = 0; i < octaves; ++i) {
            ampSum += a;
            a *= g;
        }
        return ampSum * noiseAt(1.0F / v6);
    }

    const u32 count = std::min(octaves, 24U);
    f32 sum = 0.0F;
    for (u32 i = 0; i < count; ++i) {
        sum += amp * noiseAt(1.0F / v6);
        v6 *= lac;
        amp *= g;
    }
    return sum;
}

Float3 vectorPotential(const SamplerTurbulence& t, Float3 p) noexcept {
    return Float3{fbmPotential(t, p, kPotentialOffset0), fbmPotential(t, p, kPotentialOffset1),
                  fbmPotential(t, p, kPotentialOffset2)};
}

}

void generateTurbulenceGradients(u32 seed, std::span<f32> out) noexcept {

    std::mt19937 mt(seed);
    for (f32& gradient : out) {
        const u32 r = mt() >> 8U;
        gradient = static_cast<f32>(r) * (2.0F / 16777216.0F) - 1.0F;
    }
}

Float3 sampleTurbulenceVelocity(const SamplerTurbulence& t, Float3 pos, f32 time) noexcept {
    if (t.gradients.empty()) {
        return Float3{0.0F, 0.0F, 0.0F};
    }

    Float3 p = pos;
    const f32 angle = time * t.timeScale + t.timeBase;
    if (angle != 0.0F) {
        const f32 c = std::cos(angle);
        const f32 s = std::sin(angle);
        p = Float3{c * pos.x - s * pos.y, s * pos.x + c * pos.y, pos.z};
    }

    const f32 d = t.delta;
    const f32 n = t.dnorm;
    const Float3 dxP = vectorPotential(t, Float3{p.x + d, p.y, p.z});
    const Float3 dxM = vectorPotential(t, Float3{p.x - d, p.y, p.z});
    const Float3 dyP = vectorPotential(t, Float3{p.x, p.y + d, p.z});
    const Float3 dyM = vectorPotential(t, Float3{p.x, p.y - d, p.z});
    const Float3 dzP = vectorPotential(t, Float3{p.x, p.y, p.z + d});
    const Float3 dzM = vectorPotential(t, Float3{p.x, p.y, p.z - d});

    const f32 dPyx = (dxP.y - dxM.y) * n;
    const f32 dPzx = (dxP.z - dxM.z) * n;
    const f32 dPxy = (dyP.x - dyM.x) * n;
    const f32 dPzy = (dyP.z - dyM.z) * n;
    const f32 dPxz = (dzP.x - dzM.x) * n;
    const f32 dPyz = (dzP.y - dzM.y) * n;

    return Float3{dPzy - dPyz, dPxz - dPzx, dPyx - dPxy};
}

}
