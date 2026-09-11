#include "renderer/sc2/sc2_element.h"

#include <algorithm>
#include <bit>
#include <cmath>

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#define WDX_SC2_HAS_RSQRTSS 1
#include <xmmintrin.h>
#else
#define WDX_SC2_HAS_RSQRTSS 0
#endif

namespace whiteout::flakes::renderer::sc2 {

f32 SampleWave(u32 type, f32 phase, f32 amp, Rng* rng) {
    switch (type) {
    case 1:
        return std::sin(phase) * amp;
    case 2:
        return std::cos(phase) * amp;
    case 4: {
        // Square: ±amp about the half-period (M3_SampleAnimValue case 4).
        const f32 frac = phase - std::floor(phase);
        return (frac > 0.5f) ? -amp : amp;
    }
    case 3:
        // Sawtooth (M3_SampleAnimValue case 3, recovered from a clean disasm):
        // amp·(2·fmod(phase, 1) − 1) — a bipolar ramp per unit period. Retail
        // runs the fmod in double then narrows; K=1.0, C=−1.0 (both doubles).
        return amp * (2.0f * std::fmod(phase, 1.0f) - 1.0f);
    case 5: {
        // `Rand_RangeF(&g_rng, −amp, amp)`. Retail's generator is global but
        // NOT nondeterministic — OP0 transcribed it exactly — so with one in
        // hand this is the shipped behaviour, draw and all.
        if (rng != nullptr)
            return rng->RangeF(-amp, amp);
        // No generator: a deterministic hash of the phase bits into [−amp,
        // amp]. Right shape, wrong stream — see the header.
        u32 h = std::bit_cast<u32>(phase);
        h ^= h >> 16;
        h *= 0x7feb352du;
        h ^= h >> 15;
        h *= 0x846ca68bu;
        h ^= h >> 16;
        const f32 unit = static_cast<f32>(h >> 8) * (1.0f / 16777216.0f); // [0,1)
        return (unit * 2.0f - 1.0f) * amp;
    }
    case 6:
        // `Noise1D_Sample(&unk_108254834, phase) · amp` — a second seed-0
        // table, so the one we already generate answers it.
        return GlobalNoiseTable().Sample1D(phase) * amp;
    default: // 0 off, and anything unrecognised.
        return 0.0f;
    }
}

GroundHit GroundCollide(const Vector3f& oldPos, const Vector3f& newPos,
                        const Vector3f& vel, f32 dt, f32 friction, f32 bounce,
                        const GroundQuery& query) {
    GroundHit r{newPos, vel, false};
    // Height under the step's end; a generous reach so a fast fall is not
    // missed (the flat grid ignores x/y, a host's terrain answers in range).
    constexpr f32 kReach = 1000.0f;
    f32 gz = 0.0f;
    if (!query(newPos, kReach, kReach, gz))
        return r;
    const f32 contactZ = gz + kCollideRadius;
    if (newPos.z > contactZ)
        return r; // ended above the surface: no contact this step.

    // Time of impact along the (z-monotone) step, then the contact point.
    const f32 dz = oldPos.z - newPos.z;
    const f32 frac =
        (dz > 1e-6f) ? std::clamp((oldPos.z - contactZ) / dz, 0.0f, 1.0f) : 0.0f;
    const Vector3f hit = {oldPos.x + (newPos.x - oldPos.x) * frac,
                          oldPos.y + (newPos.y - oldPos.y) * frac, contactZ};

    // Reflect only a velocity moving into the surface (dot(vel, up) < 0).
    const f32 vn = vel.z; // normal = grid up {0, 0, 1}
    if (vn >= 0.0f)
        return r;
    const Vector3f vNorm = {0.0f, 0.0f, vn};
    Vector3f vNew = {-bounce * vNorm.x, -bounce * vNorm.y, -bounce * vNorm.z};
    const f32 speedSq = vel.x * vel.x + vel.y * vel.y + vel.z * vel.z;
    if (speedSq > kCollideSpeedSq) {
        vNew.x += friction * (vel.x - vNorm.x);
        vNew.y += friction * (vel.y - vNorm.y);
        vNew.z += friction * (vel.z - vNorm.z);
    }
    // Advance in the binary's op order: (v'·dt)·(1 − frac), not v'·(dt·(1 − frac))
    // — f32 multiply is not associative, and O8 pins the binary's association.
    const f32 rem = 1.0f - frac;
    r.pos = {hit.x + (vNew.x * dt) * rem, hit.y + (vNew.y * dt) * rem,
             hit.z + (vNew.z * dt) * rem};
    r.vel = vNew;
    r.hit = true;
    return r;
}

void SmoothedVelocity::Push(const Vector3f& delta, f32 dt) {
    pos_[slot_] = delta;
    dt_[slot_] = dt;
    slot_ = static_cast<u8>((slot_ + 1) & (kTaps - 1));
    if (count_ < kTaps)
        ++count_;
    // value = Σ posDelta / Σ dt over the window: the dt-weighted average
    // emitter velocity (units/s). Summed in SLOT order, which is what the
    // ribbon accumulated before the ring moved here.
    Vector3f sum = {0, 0, 0};
    f32 wsum = 0;
    for (u8 i = 0; i < count_; ++i) {
        sum = {sum.x + pos_[i].x, sum.y + pos_[i].y, sum.z + pos_[i].z};
        wsum += dt_[i];
    }
    value_ = (wsum > 1e-6f) ? Vector3f{sum.x / wsum, sum.y / wsum, sum.z / wsum}
                            : Vector3f{0, 0, 0};
}

// -- noise --------------------------------------------------------------------

namespace {

constexpr f32 kDrawScale = 0.00390625f; // 1/256 (dword_103BC9A6C)
constexpr f32 kNegOne = -1.0f;          // flt_103C472BC
constexpr f32 kNegHalf = -0.5f;         // xmmword_103BC8C00[3]
constexpr f32 kNegThree = -3.0f;        // dword_103C472C0
constexpr f32 kFadeK1 = -2.0f;          // dword_103AD5BF0
constexpr f32 kFadeK2 = 3.0f;           // dword_103AAD5F4
// The three row biases. They are what makes truncation read as floor (§16.3),
// and the 100-apart spacing is what separates the three noise rows.
constexpr f32 kBiasRow0 = 4096.0f; // dword_103C458E4
constexpr f32 kBiasRow1 = 3996.0f; // dword_103AD81C8
constexpr f32 kBiasRow2 = 3896.0f; // dword_103AD81CC

/// `rsqrtss` plus the engine's one Newton step, in ITS association:
/// `(seed·−0.5) · ((len2·seed)·seed + −3)`. The hardware estimate is a 12-bit
/// seed, so the refined result is not `1/sqrt` to the last bit — which is
/// exactly why OP2 carries rtol 2e-6 on the gradients and why porting the
/// instruction sequence beats porting the algebra.
f32 RsqrtNewton(f32 len2) {
#if WDX_SC2_HAS_RSQRTSS
    const f32 r = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(len2)));
#else
    const f32 r = 1.0f / std::sqrt(len2);
#endif
    return (r * kNegHalf) * (((len2 * r) * r) + kNegThree);
}

} // namespace

NoiseTable::NoiseTable(u32 seed) : seed_(seed) {
    u32 s = seed;
    auto draw = [&s]() -> f32 {
        s = 214013u * s + 2531011u;
        return static_cast<f32>(static_cast<i32>((s >> 16) & 0x1FFu) - 256) * kDrawScale;
    };

    for (i32 i = 0; i < 256; ++i) {
        perm_[i] = static_cast<u8>(i);
        grad1_[i] = draw(); // the raw draw; never normalised.

        f32 x = draw(), y = draw();
        if (x == 0.0f && y == 0.0f) { // a zero gradient has no direction: redraw.
            x = draw();
            y = draw();
        }
        f32 k = RsqrtNewton((y * y) + (x * x));
        grad2_[2 * i] = x * k;
        grad2_[2 * i + 1] = k * y;

        f32 a = draw(), b = draw(), c = draw();
        if (a == 0.0f && b == 0.0f && c == 0.0f) {
            a = draw();
            b = draw();
            c = draw();
        }
        k = RsqrtNewton((c * c) + ((b * b) + (a * a)));
        grad3_[3 * i] = a * k;
        grad3_[3 * i + 1] = b * k;
        grad3_[3 * i + 2] = k * c;
    }

    // Fisher-Yates over the whole 256, shipped unrolled by two.
    for (i32 i = 255; i >= 1; --i) {
        s = 214013u * s + 2531011u;
        const u32 j = (s >> 16) & 0xFFu;
        std::swap(perm_[i], perm_[j]);
    }
}

f32 NoiseTable::Sample1D(f32 phase) const {
    const f32 t = phase + kBiasRow0;
    const i32 xi = static_cast<i32>(t); // cvttss2si
    const f32 tf = t - static_cast<f32>(xi);
    const f32 fade = (kFadeK1 * tf) + kFadeK2;
    const f32 g0 = grad1_[Perm(xi)];
    const f32 g1 = grad1_[Perm(xi + 1)];
    const f32 lo = tf * g0;
    return (((tf * tf) * fade) * (((kNegOne + tf) * g1) - lo)) + lo;
}

void NoiseTable::Sample(f32 x, f32 y, f32 out[3]) const {
    const f32 tx = x + kBiasRow0;
    const i32 xi = static_cast<i32>(tx);
    const f32 xf = tx - static_cast<f32>(xi);
    const f32 xm1 = kNegOne + xf;
    // The x fade is `(K1·t + K2)·(t·t)`; the y fade below is written
    // `(3 − (t+t))·(t·t)` instead — the shipped code spells the same polynomial
    // two ways, and in float32 the two spellings are not the same number, so
    // both are transcribed as found.
    const f32 fx = ((kFadeK1 * xf) + kFadeK2) * (xf * xf);
    const u8 px0 = Perm(xi);
    const u8 px1 = Perm(xi + 1);

    const f32 biases[3] = {kBiasRow0, kBiasRow1, kBiasRow2};
    for (i32 row = 0; row < 3; ++row) {
        const f32 ty = biases[row] + y;
        const i32 yi = static_cast<i32>(ty);
        const f32 yf = ty - static_cast<f32>(yi);
        const f32 ym1 = yf + kNegOne;

        const u8 a = Perm(yi + px0);
        const u8 b = Perm(yi + px1);
        const u8 c = Perm(yi + px0 + 1);
        const u8 d = Perm(yi + px1 + 1);

        // Each corner dot is written `(g.y·v) + (g.x·u)` — the y term first.
        const f32 lo = (grad2_[2 * a + 1] * yf) + (grad2_[2 * a] * xf);
        const f32 bot = ((((grad2_[2 * b] * xm1) - lo) + (yf * grad2_[2 * b + 1])) * fx) + lo;
        const f32 hi = (grad2_[2 * c + 1] * ym1) + (grad2_[2 * c] * xf);
        const f32 top = (((grad2_[2 * d] * xm1) - hi) + (ym1 * grad2_[2 * d + 1])) * fx;
        const f32 fy = (kFadeK2 - (yf + yf)) * (yf * yf);
        out[row] = (((hi - bot) + top) * fy) + bot;
    }
}

f32 NoiseTable::Sample3D(f32 x, f32 y, f32 z) const {
    const f32 tx = x + kBiasRow0;
    const i32 xi = static_cast<i32>(tx); // cvttss2si, as the 2-D sampler
    const f32 xf = tx - static_cast<f32>(xi);
    const f32 xm1 = xf + kNegOne;
    const f32 ty = y + kBiasRow0;
    const i32 yi = static_cast<i32>(ty);
    const f32 yf = ty - static_cast<f32>(yi);
    const f32 ym1 = yf + kNegOne;
    const f32 tz = z + kBiasRow0;
    const i32 zi = static_cast<i32>(tz);
    const u32 z0 = static_cast<u8>(zi);
    const u32 z1 = static_cast<u8>(zi + 1);
    const f32 zf = tz - static_cast<f32>(zi);
    const f32 zm1 = kNegOne + zf;

    const u8 a = Perm(xi);
    const u8 b = Perm(xi + 1);
    const u32 aa = Perm(yi + a);
    const u32 ba = Perm(yi + b);
    const u32 ab = Perm(a + yi + 1);
    const u32 bb = Perm(b + yi + 1);

    // x's fade is spelled `(K1·t + K2)·t²` and y's and z's `(3 − 2t)·t²`, as in
    // the 2-D sampler; in float32 the two spellings are different numbers.
    const f32 fx = ((kFadeK1 * xf) + kFadeK2) * (xf * xf);
    const f32 fy = (kFadeK2 - (yf + yf)) * (yf * yf);
    const f32 fz = (kFadeK2 - (zf + zf)) * (zf * zf);
    // The unmasked `hash + zByte`, read through the wrap copy's `i % 256`.
    const auto grad = [this](u32 i) { return &grad3_[3 * (i & 0xFFu)]; };

    // Each corner dot is written z term first, then y, then x — and the x
    // lerps subtract the near corner inside the sum, as the shipped code does.
    const f32* g000 = grad(aa + z0);
    const f32 c000 = (g000[2] * zf) + ((g000[1] * yf) + (g000[0] * xf));
    const f32* g100 = grad(ba + z0);
    const f32 lx00 = ((((g100[2] * zf) + (g100[1] * yf)) + ((g100[0] * xm1) - c000)) * fx) + c000;
    const f32* g010 = grad(ab + z0);
    const f32 c010 = (g010[2] * zf) + ((g010[1] * ym1) + (g010[0] * xf));
    const f32* g110 = grad(bb + z0);
    const f32 ly0 =
        (((c010 - lx00) + ((((zf * g110[2]) + (g110[1] * ym1)) + ((g110[0] * xm1) - c010)) * fx)) *
         fy) +
        lx00;

    const f32* g001 = grad(z1 + aa);
    const f32 c001 = (g001[2] * zm1) + ((g001[1] * yf) + (g001[0] * xf));
    const f32* g101 = grad(z1 + ba);
    const f32 lx01 = ((((g101[2] * zm1) + (yf * g101[1])) + ((g101[0] * xm1) - c001)) * fx) + c001;
    const f32* g011 = grad(z1 + ab);
    const f32 c011 = (g011[2] * zm1) + ((g011[1] * ym1) + (xf * g011[0]));
    const f32* g111 = grad(z1 + bb);
    const f32 top =
        ((c011 - lx01) + ((((zm1 * g111[2]) + (ym1 * g111[1])) + ((xm1 * g111[0]) - c011)) * fx)) *
        fy;
    return (((lx01 - ly0) + top) * fz) + ly0;
}

const NoiseTable& GlobalNoiseTable() {
    static const NoiseTable table(0);
    return table;
}

void ConvertColorNode(f32 keys[3], f32 midTime) {
    const f32 inv = 1.0f - midTime;
    keys[1] = (keys[1] - (((midTime * midTime) * keys[2]) + ((inv * inv) * keys[0]))) /
              ((inv + inv) * midTime);
}

u32 ConvertColorNode3(u32 c0, u32 c1, u32 c2, f32 midTime) {
    constexpr f32 kByteToUnit = 0.003921560011804104f; // dword_103AA4994 = 1/255
    constexpr f32 kUnitToByte = 255.0f;                // xmmword_103BC8C00[1]
    constexpr f32 kRound = 0.5f;                       // xmmword_103BC8C00[0]
    const f32 inv = 1.0f - midTime;
    const f32 sq = inv * inv;
    const f32 tt = midTime * midTime;
    const f32 rcp = 1.0f / ((inv + inv) * midTime);

    u32 out = 0;
    for (i32 k = 0; k < 4; ++k) {
        const i32 shift = 8 * k;
        const f32 b0 = static_cast<f32>((c0 >> shift) & 0xFFu);
        const f32 b1 = static_cast<f32>((c1 >> shift) & 0xFFu);
        const f32 b2 = static_cast<f32>((c2 >> shift) & 0xFFu);
        const f32 v = ((b1 - (b0 * sq)) - (b2 * tt)) * kByteToUnit;
        const f32 c = std::max(v * rcp, 0.0f);
        // Truncate, then take the low byte: that IS the shipped pack, and it is
        // why an overshoot wraps instead of clamping.
        out |= (static_cast<u32>(static_cast<i32>(c * kUnitToByte + kRound)) & 0xFFu) << shift;
    }
    return out;
}

} // namespace whiteout::flakes::renderer::sc2
