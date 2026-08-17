//===----------------------------------------------------------------------===//
// snowball/math.h -- approximate reciprocals, and the 3x3 solve built on them.
//
// This engine does not divide on the simulation path. `Rcp` and `Rsqrt` compute a hardware
// estimate refined by one Newton-Raphson step -- roughly 22 bits of precision, deliberately NOT
// a correctly-rounded divide -- and everything on the solver path routes through them: the
// contact solver, every joint's velocity-constraint setup, quaternion normalisation, distance
// queries. Because the approximation feeds every impulse, rewriting any of those sites as
// `1.0f / x` changes results from the first step, compounds through the solver, and breaks
// bit-for-bit reproducibility, while looking completely reasonable in review.
//
// The `-ffp-contract=off` / `/fp:precise` in CMakeLists is part of the same contract: a
// compiler allowed to rewrite rcp-plus-Newton back into a true divide would silently undo it.
//===----------------------------------------------------------------------===//
#pragma once

#include "snowball/common_types.h"
#include "snowball/vec.h"

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define SNOWBALL_HAS_SSE_RECIPROCAL 1
#include <xmmintrin.h>
#else
#define SNOWBALL_HAS_SSE_RECIPROCAL 0
#include <cmath>
#endif

namespace snowball {

namespace detail {

#if SNOWBALL_HAS_SSE_RECIPROCAL
inline __m128 Load(const Vec4& v) { return _mm_load_ps(&v.x); }
inline Vec4 Store(__m128 m) {
    Vec4 out;
    _mm_store_ps(&out.x, m);
    return out;
}
#endif

}  // namespace detail

/// @brief Reciprocal: a ~12-bit hardware estimate refined by one Newton-Raphson step.
///
/// `r = rcpps(v); return (2 - v*r) * r` -- roughly 22 bits, and NOT a correctly-rounded divide.
inline Vec4 Rcp(const Vec4& v) {
#if SNOWBALL_HAS_SSE_RECIPROCAL
    const __m128 x = detail::Load(v);
    const __m128 r = _mm_rcp_ps(x);
    return detail::Store(_mm_mul_ps(_mm_sub_ps(detail::Load(constants::kTwo), _mm_mul_ps(x, r)), r));
#else
    // No equivalent instruction on this ISA. Cross-platform bit-reproducibility would require
    // emulating the x86 estimate-plus-Newton sequence in portable arithmetic; until that lands
    // this is a true reciprocal, correct to more bits than the SSE path and therefore NOT
    // bit-compatible with it -- the same simulation diverges across ISAs.
    return {1.0f / v.x, 1.0f / v.y, 1.0f / v.z, 1.0f / v.w};
#endif
}

/// @brief Reciprocal square root: hardware estimate plus one Newton-Raphson step.
///
/// The refinement is `((0.5 - r*r*(0.5*v)) * r) + r` -- the textbook `r * (1.5 - 0.5*v*r*r)`
/// in one specific association, using the one-half constant twice and no other. That
/// association is deliberate and part of the numeric contract: the algebraically identical
/// `(0.5*r) * (3 - v*r*r)` differs from it in the last ulp on ~14% of inputs, and because this
/// primitive sits under every normalisation, a last-ulp change here shifts bits everywhere
/// downstream even though no per-call tolerance would ever see the difference.
inline Vec4 Rsqrt(const Vec4& v) {
#if SNOWBALL_HAS_SSE_RECIPROCAL
    const __m128 x = detail::Load(v);
    const __m128 r = _mm_rsqrt_ps(x);
    const __m128 half = detail::Load(constants::kOneHalf);
    const __m128 t = _mm_sub_ps(half, _mm_mul_ps(_mm_mul_ps(r, r), _mm_mul_ps(half, x)));
    return detail::Store(_mm_add_ps(_mm_mul_ps(t, r), r));
#else
    return {1.0f / std::sqrt(v.x), 1.0f / std::sqrt(v.y), 1.0f / std::sqrt(v.z),
            1.0f / std::sqrt(v.w)};
#endif
}

inline f32 Rcp(f32 v) { return Rcp(Vec4::Splat(v)).x; }
inline f32 Rsqrt(f32 v) { return Rsqrt(Vec4::Splat(v)).x; }

/// @brief A 3x3 (really 3x4) matrix stored as its COLUMNS.
///
/// The images of the basis vectors, one per lane group, so `A*v == c[0]*v.x + c[1]*v.y +
/// c[2]*v.z (+ c[3])`. Reading them as rows makes Solve33 solve the transposed system and still
/// look plausible; only the residual exposes it (~1e-5 for the column reading, ~1e+2 for rows).
struct Mtx {
    Vec4 c[4]{};

    constexpr Vec4 Transform(const Vec4& v) const {
        return c[0] * v.x + c[1] * v.y + c[2] * v.z;
    }
};

/// @brief The transpose of the 3x3 block, which for a rotation is its inverse.
constexpr Mtx Transpose3(const Mtx& m) {
    Mtx out;
    out.c[0] = {m.c[0].x, m.c[1].x, m.c[2].x, 0.0f};
    out.c[1] = {m.c[0].y, m.c[1].y, m.c[2].y, 0.0f};
    out.c[2] = {m.c[0].z, m.c[1].z, m.c[2].z, 0.0f};
    out.c[3] = constants::kUnitW;
    return out;
}

/// @brief Matrix product: columns in, columns out.
constexpr Mtx Concat3(const Mtx& a, const Mtx& b) {
    Mtx out;
    for (i32 i = 0; i < 3; ++i) {
        out.c[i] = a.Transform(b.c[i]);
    }
    out.c[3] = constants::kUnitW;
    return out;
}

/// @brief Solve `A x = b` by Cramer's rule, with a masked approximate reciprocal.
///
/// A singular system returns exactly zero rather than NaN or infinity, so the result is always
/// defined and callers never have to branch on a poisoned value.
Vec4 Solve33(const Mtx& a, const Vec4& b);

}  // namespace snowball
