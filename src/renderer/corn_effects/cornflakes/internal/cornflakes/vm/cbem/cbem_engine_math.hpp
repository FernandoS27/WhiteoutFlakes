#pragma once

#include <cornflakes/interface/core/types.hpp>

#include <bit>
#include <cmath>

namespace whiteout::cornflakes::cbem::enginemath {

inline constexpr f32 bitsToF32(u32 w) noexcept {
    return std::bit_cast<f32>(w);
}
inline constexpr u32 f32ToBits(f32 v) noexcept {
    return std::bit_cast<u32>(v);
}

inline i32 truncateToInt(f32 v) noexcept {
    constexpr f32 kIntMinAsFloat = -2147483648.0F;
    constexpr f32 kIntMaxExclusive = 2147483648.0F;
    if (!(v >= kIntMinAsFloat) || !(v < kIntMaxExclusive)) {
        return static_cast<i32>(0x80000000U);
    }
    return static_cast<i32>(v);
}

inline constexpr f32 kExp2ClampMin = bitsToF32(0xC2FDFFFFU);
inline constexpr f32 kExp2ClampMax = bitsToF32(0x4300FFFFU);
inline constexpr i32 kExp2ExponentBase = 127;

inline constexpr f32 kExp2C0 = bitsToF32(0x3F800000U);
inline constexpr f32 kExp2C1 = bitsToF32(0x3F31727BU);
inline constexpr f32 kExp2C2 = bitsToF32(0x3E75EAD4U);
inline constexpr f32 kExp2C3 = bitsToF32(0x3D64AA23U);
inline constexpr f32 kExp2C4 = bitsToF32(0x3C134806U);
inline constexpr f32 kExp2C5 = bitsToF32(0x3AF61905U);

inline constexpr f32 kInvLn2 = bitsToF32(0x3FB8AA3BU);

inline f32 exp2(f32 x) noexcept {
    const f32 lo = (x < kExp2ClampMax) ? x : kExp2ClampMax;
    const f32 safeInput = (lo > kExp2ClampMin) ? lo : kExp2ClampMin;

    const f32 fl = std::floor(safeInput);
    const i32 rInt = truncateToInt(fl);
    const f32 t = safeInput - fl;

    const f32 e = bitsToF32(static_cast<u32>(rInt + kExp2ExponentBase) << 23U);

    const f32 t2 = t * t;
    const f32 p0 = t * kExp2C5 + kExp2C4;
    const f32 p1 = t * kExp2C3 + kExp2C2;
    const f32 p2 = t * kExp2C1 + kExp2C0;
    const f32 p3 = t2 * p0 + p1;
    const f32 p4 = t2 * p3 + p2;
    return p4 * e;
}

inline f32 exp(f32 x) noexcept {
    return exp2(x * kInvLn2);
}

inline constexpr f32 kFourOverPi = bitsToF32(0x3FA2F983U);
inline constexpr f32 kDP1 = bitsToF32(0xBF490000U);
inline constexpr f32 kDP2 = bitsToF32(0xB97DA000U);
inline constexpr f32 kDP3 = bitsToF32(0xB3222169U);
inline constexpr f32 kSinC0 = bitsToF32(0xB94CA1F9U);
inline constexpr f32 kSinC1 = bitsToF32(0x3C08839EU);
inline constexpr f32 kSinC2 = bitsToF32(0xBE2AAAA3U);
inline constexpr f32 kNegHalf = bitsToF32(0xBF000000U);
inline constexpr f32 kCosC0 = bitsToF32(0x37CCF5CEU);
inline constexpr f32 kCosC1 = bitsToF32(0xBAB6061AU);
inline constexpr f32 kCosC2 = bitsToF32(0x3D2AAAA5U);
inline constexpr f32 kSinCosMax = bitsToF32(0x4B000000U);

inline constexpr u32 kSignMask = 0x80000000U;
inline constexpr u32 kAbsMask = 0x7FFFFFFFU;

inline void sinCos(f32 xo, f32& outSin, f32& outCos) noexcept {
    f32 x = bitsToF32(f32ToBits(xo) & kAbsMask);

    const u32 overflowMask = (x < kSinCosMax) ? 0xFFFFFFFFU : 0U;

    f32 y = x * kFourOverPi;
    i32 emm2 = truncateToInt(y);
    emm2 = (emm2 + 1) & ~1;
    y = static_cast<f32>(emm2);

    x = y * kDP1 + x;
    x = y * kDP2 + x;
    x = y * kDP3 + x;
    x = bitsToF32(f32ToBits(x) & overflowMask);

    const i32 enn2 = emm2 - 2;
    const u32 polyMask = ((emm2 & 2) != 0) ? 0xFFFFFFFFU : 0U;
    const u32 bt4 = static_cast<u32>(emm2) << 29U;
    const u32 bt5 = static_cast<u32>(enn2) << 29U;

    const u32 sinSignMask = kSignMask & (f32ToBits(xo) ^ bt4);
    const u32 cosSignMask = kSignMask & ~bt5;

    const f32 x2 = x * x;
    const f32 x3 = x2 * x;
    const f32 x4 = x2 * x2;

    const f32 cR = x2 * kNegHalf + 1.0F;

    f32 y1 = kCosC0;
    f32 y2 = kSinC0;
    y1 = y1 * x2 + kCosC1;
    y2 = y2 * x2 + kSinC1;
    y1 = y1 * x2 + kCosC2;
    y2 = y2 * x2 + kSinC2;
    y1 = y1 * x4 + cR;
    y2 = y2 * x3 + x;

    const f32 ys = (polyMask != 0U) ? y1 : y2;
    const f32 yc = (polyMask != 0U) ? y2 : y1;

    outSin = bitsToF32(f32ToBits(ys) ^ sinSignMask);
    outCos = bitsToF32(f32ToBits(yc) ^ cosSignMask);
}

inline f32 rcp(f32 x) noexcept {
    const f32 r = 1.0F / x;
    const u32 zMask = (x != 0.0F) ? 0xFFFFFFFFU : 0U;
    const u32 rMask = (r != 0.0F) ? 0xFFFFFFFFU : 0U;
    const u32 term = f32ToBits((x * r) * r) & zMask & rMask;
    return (r + r) - bitsToF32(term);
}

inline f32 sqrt(f32 x) noexcept {
    return std::sqrt(bitsToF32(f32ToBits(x) & kAbsMask));
}

inline f32 rsqrt(f32 x) noexcept {
    return 1.0F / std::sqrt(bitsToF32(f32ToBits(x) & kAbsMask));
}

}
