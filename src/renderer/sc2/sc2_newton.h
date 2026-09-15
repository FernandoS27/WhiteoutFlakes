#pragma once

// ============================================================================
// The one Newton refinement the SC2 binary takes after a reciprocal root, and
// the SIMD test that decides which hardware seeds a transcription can use.
//
// The STEP is the same expression at every site that takes it. The SEED is
// not: `rsqrtss`, `rsqrtps`, `rcpss` or a correctly rounded `1/sqrt`, by
// routine. So the seed is a parameter and each call keeps its own; a site that
// swapped one for another would move by the hardware estimate's error, which
// is why the gates on those lanes hold a relative bound.
//
// The constants are the image's: -0.5 at `xmmword_103BC8C00[3]` and -3.0 at
// `dword_103C472C0`.
// ============================================================================

#include "whiteout/flakes/types.h"

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#define WDX_SC2_HAS_SSE 1
#include <xmmintrin.h>
#else
#define WDX_SC2_HAS_SSE 0
#endif

namespace whiteout::flakes::renderer::sc2 {

/// `(r·−0.5)·((v·r)·r − 3)`: `1/sqrt(v)`, refined once from the seed `r`.
inline f32 NewtonRsqrt(f32 v, f32 seed) {
    return (seed * -0.5f) * (((v * seed) * seed) + -3.0f);
}

/// The same step spelled as a LENGTH, `((v·r)·r − 3)·(−0.5·v·r)`: `sqrt(v)`.
/// Unmasked — a zero `v` makes a NaN, and each caller masks it where its
/// routine does.
inline f32 NewtonLength(f32 v, f32 seed) {
    const f32 vr = v * seed;
    return ((vr * seed) + -3.0f) * (-0.5f * vr);
}

} // namespace whiteout::flakes::renderer::sc2
