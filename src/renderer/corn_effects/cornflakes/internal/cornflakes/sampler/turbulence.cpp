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
constexpr Float3 kPotentialOffset1{31.416F, 47.853001F, 12.793F};
constexpr Float3 kPotentialOffset2{-233.145F, -113.408F, -185.31F};

f32 fadeT(f32 t, u32 interp) noexcept {
    switch (interp) {
    case 1U:
        return t * t * (3.0F - 2.0F * t);
    case 2U:
        return ((6.0F * t - 15.0F) * t + 10.0F) * t * t * t;
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

struct Octave {
    f32 invFreq;
    f32 amp;
};

inline constexpr std::size_t kMaxOctaves = 24;

u32 buildOctaves(const SamplerTurbulence& t, std::array<Octave, kMaxOctaves>& out) noexcept {
    f32 wavelength = t.globalScale * t.wavelength;
    if (!(wavelength > 0.0F) && !(wavelength < 0.0F)) {
        wavelength = 1.0e-7F;
    }
    const u32 octaves = std::max(1U, t.octaves);
    const f32 lac =
        std::max(t.lacunarity, std::pow(1.0e-7F / wavelength, 1.0F / static_cast<f32>(octaves)));
    const f32 g = t.gain * t.gainMultiplier;
    f32 amp = t.globalScale * t.strength;

    if (lac == 1.0F || g == 0.0F) {
        f32 ampSum = 0.0F;
        f32 a = amp;
        for (u32 i = 0; i < octaves; ++i) {
            ampSum += a;
            a *= g;
        }
        out[0] = Octave{1.0F / wavelength, ampSum};
        return 1U;
    }

    const u32 count = std::min(octaves, static_cast<u32>(kMaxOctaves));
    for (u32 i = 0; i < count; ++i) {
        out[i] = Octave{1.0F / wavelength, amp};
        wavelength *= lac;
        amp *= g;
    }
    return count;
}

Float3 noiseGradient(Float3 s, std::span<const f32> grad, u32 interp, f32 delta,
                     f32 dnorm) noexcept {
    const f32 d = delta;
    const f32 n = dnorm;
    return Float3{
        (coherentNoise(Float3{s.x + d, s.y, s.z}, grad, interp) -
         coherentNoise(Float3{s.x - d, s.y, s.z}, grad, interp)) *
            n,
        (coherentNoise(Float3{s.x, s.y + d, s.z}, grad, interp) -
         coherentNoise(Float3{s.x, s.y - d, s.z}, grad, interp)) *
            n,
        (coherentNoise(Float3{s.x, s.y, s.z + d}, grad, interp) -
         coherentNoise(Float3{s.x, s.y, s.z - d}, grad, interp)) *
            n,
    };
}

}

void generateTurbulenceBasis(u32 seed, f32 timeRandomVariation, std::span<f32> rigidBasis,
                             std::span<f32> spinRate) noexcept {
    constexpr f32 kTau = 6.2831853071795864F;
    constexpr f32 kPi = 3.1415926535897932F;
    const bool uniformSpin = timeRandomVariation < 1.0e-5F;

    std::mt19937 mt(seed);
    const auto next01 = [&mt]() noexcept {
        return static_cast<f32>(mt() >> 8U) * (1.0F / 16777216.0F);
    };
    for (std::size_t i = 0; i < rigidBasis.size(); ++i) {
        rigidBasis[i] = next01() * kTau;
        if (i < spinRate.size()) {
            spinRate[i] =
                uniformSpin ? kTau : kTau + ((next01() * 2.0F) - 1.0F) * timeRandomVariation * kPi;
        }
    }
}

void rotateTurbulenceBasis(std::span<const f32> rigidBasis, std::span<const f32> spinRate,
                           f32 rotation, std::span<f32> out) noexcept {
    const std::size_t n = std::min({rigidBasis.size(), spinRate.size(), out.size()});
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = std::sin(rigidBasis[i] + rotation * spinRate[i]);
    }
}

Float3 sampleTurbulenceVelocity(const SamplerTurbulence& t, Float3 posWorld, f32 time) noexcept {
    if (t.gradients.empty()) {
        return Float3{0.0F, 0.0F, 0.0F};
    }

    const Float3 pos{posWorld.x, posWorld.z, -posWorld.y};

    const Float3 p = pos;
    const f32 rotation = time * t.timeScale + t.timeBase;
    std::span<const f32> grad = t.gradients;
    std::array<f32, kTurbulenceGradientCount> rotated{};
    if (rotation != 0.0F && !t.rigidBasis.empty() && !t.spinRate.empty()) {
        rotateTurbulenceBasis(t.rigidBasis, t.spinRate, rotation, rotated);
        grad = std::span<const f32>{rotated.data(), rotated.size()};
    }

    std::array<Octave, kMaxOctaves> oct{};
    const u32 count = buildOctaves(t, oct);

    Float3 dPsiX{0.0F, 0.0F, 0.0F};
    Float3 dPsiY{0.0F, 0.0F, 0.0F};
    Float3 dPsiZ{0.0F, 0.0F, 0.0F};
    for (u32 i = 0; i < count; ++i) {
        const f32 f = oct[i].invFreq;
        const f32 a = oct[i].amp;
        const auto accumulate = [&](Float3 offset, Float3& dst) noexcept {
            const Float3 s{(f * p.x) + offset.x, (f * p.y) + offset.y, (f * p.z) + offset.z};
            const Float3 g3 = noiseGradient(s, grad, t.interpolator, t.delta, t.dnorm);
            dst.x += a * g3.x;
            dst.y += a * g3.y;
            dst.z += a * g3.z;
        };
        accumulate(kPotentialOffset0, dPsiX);
        accumulate(kPotentialOffset1, dPsiY);
        accumulate(kPotentialOffset2, dPsiZ);
    }

    const Float3 curl{dPsiZ.y - dPsiY.z, dPsiX.z - dPsiZ.x, dPsiY.x - dPsiX.y};

    return Float3{curl.x, -curl.z, curl.y};
}

}
