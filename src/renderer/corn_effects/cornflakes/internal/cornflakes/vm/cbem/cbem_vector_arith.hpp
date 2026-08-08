#pragma once

#include "cbem_backend.hpp"
#include "cbem_engine_math.hpp"

#include <cornflakes/interface/simt/packet_register_file.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

#include <bit>
#include <cmath>
#include <cstddef>

#if defined(__clang__)
#define CF_PLANE_LOOP _Pragma("clang loop vectorize(enable) unroll(disable)")
#else
#define CF_PLANE_LOOP
#endif

namespace whiteout::cornflakes::cbem {

inline constexpr f32 u2f(u32 w) noexcept {
    return std::bit_cast<f32>(w);
}
inline constexpr i32 u2i(u32 w) noexcept {
    return std::bit_cast<i32>(w);
}
inline constexpr u32 f2u(f32 v) noexcept {
    return std::bit_cast<u32>(v);
}
inline constexpr u32 i2u(i32 v) noexcept {
    return std::bit_cast<u32>(v);
}

template <std::size_t N>
struct PlaneSource {
    const u32* plane[4] = {nullptr, nullptr, nullptr, nullptr};
    u8 components = 0U;
    alignas(64) u32 splat[4][N]{};

    void setUniform(u8 component, u32 word) noexcept {
        CF_PLANE_LOOP
        for (std::size_t l = 0; l < N; ++l) {
            splat[component][l] = word;
        }
        plane[component] = splat[component];
    }
};

enum class VectorOp : u8 {
    FAdd, FSub, FMul, FDiv, FNeg,
    FLt, FLe, FGt, FGe, FEq, FNe,
    IAdd, ISub, IMul, IDiv, INeg, INot,
    IShl, IShr, IAnd, IOr, IXor,
    ILt, ILe, IGt, IGe, IEq, INe,
};

namespace ops {

inline u32 fAdd(u32 x, u32 y) noexcept { return f2u(u2f(x) + u2f(y)); }
inline u32 fSub(u32 x, u32 y) noexcept { return f2u(u2f(x) - u2f(y)); }
inline u32 fMul(u32 x, u32 y) noexcept { return f2u(u2f(x) * u2f(y)); }
inline u32 fDiv(u32 x, u32 y) noexcept { return f2u(u2f(x) / u2f(y)); }
inline u32 fNeg(u32 x) noexcept { return f2u(-u2f(x)); }

inline u32 fLt(u32 x, u32 y) noexcept { return i2u(u2f(x) < u2f(y) ? 1 : 0); }
inline u32 fLe(u32 x, u32 y) noexcept { return i2u(u2f(x) <= u2f(y) ? 1 : 0); }
inline u32 fGt(u32 x, u32 y) noexcept { return i2u(u2f(x) > u2f(y) ? 1 : 0); }
inline u32 fGe(u32 x, u32 y) noexcept { return i2u(u2f(x) >= u2f(y) ? 1 : 0); }
inline u32 fEq(u32 x, u32 y) noexcept { return i2u(u2f(x) == u2f(y) ? 1 : 0); }
inline u32 fNe(u32 x, u32 y) noexcept { return i2u(u2f(x) != u2f(y) ? 1 : 0); }

inline u32 iAdd(u32 x, u32 y) noexcept { return i2u(u2i(x) + u2i(y)); }
inline u32 iSub(u32 x, u32 y) noexcept { return i2u(u2i(x) - u2i(y)); }
inline u32 iMul(u32 x, u32 y) noexcept { return i2u(u2i(x) * u2i(y)); }
inline u32 iDiv(u32 x, u32 y) noexcept { return i2u(u2i(y) == 0 ? 0 : u2i(x) / u2i(y)); }
inline u32 iNeg(u32 x) noexcept { return i2u(-u2i(x)); }
inline u32 iNot(u32 x) noexcept { return i2u(~u2i(x)); }

inline u32 iShl(u32 x, u32 y) noexcept { return i2u(u2i(x) << (u2i(y) & 31)); }
inline u32 iShr(u32 x, u32 y) noexcept { return i2u(u2i(x) >> (u2i(y) & 31)); }
inline u32 iAnd(u32 x, u32 y) noexcept { return i2u(u2i(x) & u2i(y)); }
inline u32 iOr(u32 x, u32 y) noexcept { return i2u(u2i(x) | u2i(y)); }
inline u32 iXor(u32 x, u32 y) noexcept { return i2u(u2i(x) ^ u2i(y)); }

inline u32 iLt(u32 x, u32 y) noexcept { return i2u(u2i(x) < u2i(y) ? 1 : 0); }
inline u32 iLe(u32 x, u32 y) noexcept { return i2u(u2i(x) <= u2i(y) ? 1 : 0); }
inline u32 iGt(u32 x, u32 y) noexcept { return i2u(u2i(x) > u2i(y) ? 1 : 0); }
inline u32 iGe(u32 x, u32 y) noexcept { return i2u(u2i(x) >= u2i(y) ? 1 : 0); }
inline u32 iEq(u32 x, u32 y) noexcept { return i2u(u2i(x) == u2i(y) ? 1 : 0); }
inline u32 iNe(u32 x, u32 y) noexcept { return i2u(u2i(x) != u2i(y) ? 1 : 0); }

inline u32 fMadd(u32 a, u32 b, u32 c) noexcept {
    return f2u(u2f(a) * u2f(b) + u2f(c));
}

inline u32 fSqrt(u32 x) noexcept { return f2u(enginemath::sqrt(u2f(x))); }
inline u32 fRSqrt(u32 x) noexcept { return f2u(enginemath::rsqrt(u2f(x))); }
inline u32 fRcp(u32 x) noexcept { return f2u(enginemath::rcp(u2f(x))); }
inline u32 fExp(u32 x) noexcept { return f2u(enginemath::exp(u2f(x))); }
inline u32 fExp2(u32 x) noexcept { return f2u(enginemath::exp2(u2f(x))); }

inline u32 fAbs(u32 x) noexcept { return f2u(std::fabs(u2f(x))); }
inline u32 fSign(u32 x) noexcept {
    const f32 v = u2f(x);
    return f2u(static_cast<f32>((v > 0.0F) - (v < 0.0F)));
}
inline u32 fCeil(u32 x) noexcept { return f2u(std::ceil(u2f(x))); }
inline u32 fFloor(u32 x) noexcept { return f2u(std::floor(u2f(x))); }
inline u32 fFracUnsigned(u32 x) noexcept {
    const f32 v = u2f(x);
    return f2u(v - std::floor(v));
}
inline u32 fFrac(u32 x) noexcept {
    const f32 v = u2f(x);
    return f2u(v - std::trunc(v));
}
inline u32 fSaturate(u32 x) noexcept {
    const f32 v = u2f(x);
    return f2u((v < 0.0F) ? 0.0F : (v > 1.0F ? 1.0F : v));
}
inline u32 fIsFinite(u32 x) noexcept { return f2u(std::isfinite(u2f(x)) ? 1.0F : 0.0F); }
inline u32 fIsInfinite(u32 x) noexcept { return f2u(std::isinf(u2f(x)) ? 1.0F : 0.0F); }

inline u32 select(u32 whenFalse, u32 whenTrue, u32 cond) noexcept {
    const bool truthy = (u2i(cond) != 0) || (u2f(cond) != 0.0F);
    return truthy ? whenTrue : whenFalse;
}

}

inline u32 blend(u32 result, u32 keep, u32 previous) noexcept {
    return (result & keep) | (previous & ~keep);
}

template <std::size_t N, u32 (*Op)(u32, u32)>
inline void planeBinary(const u32* a, const u32* b, const u32* keep, u32* dst) noexcept {
    CF_PLANE_LOOP
    for (std::size_t l = 0; l < N; ++l) {
        dst[l] = blend(Op(a[l], b[l]), keep[l], dst[l]);
    }
}

template <std::size_t N, u32 (*Op)(u32)>
inline void planeUnary(const u32* a, const u32* keep, u32* dst) noexcept {
    CF_PLANE_LOOP
    for (std::size_t l = 0; l < N; ++l) {
        dst[l] = blend(Op(a[l]), keep[l], dst[l]);
    }
}

template <std::size_t N, u32 (*Op)(u32, u32, u32)>
inline void planeTernary(const u32* a, const u32* b, const u32* c, const u32* keep,
                         u32* dst) noexcept {
    CF_PLANE_LOOP
    for (std::size_t l = 0; l < N; ++l) {
        dst[l] = blend(Op(a[l], b[l], c[l]), keep[l], dst[l]);
    }
}

template <std::size_t N>
inline void applyPlane(VectorOp op, const u32* a, const u32* b, const u32* keep,
                       u32* dst) noexcept {
    switch (op) {
    case VectorOp::FAdd: planeBinary<N, ops::fAdd>(a, b, keep, dst); return;
    case VectorOp::FSub: planeBinary<N, ops::fSub>(a, b, keep, dst); return;
    case VectorOp::FMul: planeBinary<N, ops::fMul>(a, b, keep, dst); return;
    case VectorOp::FDiv: planeBinary<N, ops::fDiv>(a, b, keep, dst); return;
    case VectorOp::FNeg: planeUnary<N, ops::fNeg>(a, keep, dst); return;
    case VectorOp::FLt:  planeBinary<N, ops::fLt>(a, b, keep, dst); return;
    case VectorOp::FLe:  planeBinary<N, ops::fLe>(a, b, keep, dst); return;
    case VectorOp::FGt:  planeBinary<N, ops::fGt>(a, b, keep, dst); return;
    case VectorOp::FGe:  planeBinary<N, ops::fGe>(a, b, keep, dst); return;
    case VectorOp::FEq:  planeBinary<N, ops::fEq>(a, b, keep, dst); return;
    case VectorOp::FNe:  planeBinary<N, ops::fNe>(a, b, keep, dst); return;

    case VectorOp::IAdd: planeBinary<N, ops::iAdd>(a, b, keep, dst); return;
    case VectorOp::ISub: planeBinary<N, ops::iSub>(a, b, keep, dst); return;
    case VectorOp::IMul: planeBinary<N, ops::iMul>(a, b, keep, dst); return;
    case VectorOp::IDiv: planeBinary<N, ops::iDiv>(a, b, keep, dst); return;
    case VectorOp::INeg: planeUnary<N, ops::iNeg>(a, keep, dst); return;
    case VectorOp::INot: planeUnary<N, ops::iNot>(a, keep, dst); return;
    case VectorOp::IShl: planeBinary<N, ops::iShl>(a, b, keep, dst); return;
    case VectorOp::IShr: planeBinary<N, ops::iShr>(a, b, keep, dst); return;
    case VectorOp::IAnd: planeBinary<N, ops::iAnd>(a, b, keep, dst); return;
    case VectorOp::IOr:  planeBinary<N, ops::iOr>(a, b, keep, dst); return;
    case VectorOp::IXor: planeBinary<N, ops::iXor>(a, b, keep, dst); return;
    case VectorOp::ILt:  planeBinary<N, ops::iLt>(a, b, keep, dst); return;
    case VectorOp::ILe:  planeBinary<N, ops::iLe>(a, b, keep, dst); return;
    case VectorOp::IGt:  planeBinary<N, ops::iGt>(a, b, keep, dst); return;
    case VectorOp::IGe:  planeBinary<N, ops::iGe>(a, b, keep, dst); return;
    case VectorOp::IEq:  planeBinary<N, ops::iEq>(a, b, keep, dst); return;
    case VectorOp::INe:  planeBinary<N, ops::iNe>(a, b, keep, dst); return;
    }
}

template <std::size_t N>
inline void applyMaddPlane(const u32* a, const u32* b, const u32* c, const u32* keep,
                           u32* dst) noexcept {
    planeTernary<N, ops::fMadd>(a, b, c, keep, dst);
}

template <std::size_t N>
inline void applySelectPlane(const u32* whenFalse, const u32* whenTrue, const u32* cond,
                             const u32* keep, u32* dst) noexcept {
    planeTernary<N, ops::select>(whenFalse, whenTrue, cond, keep, dst);
}

enum class VectorFunc1 : u8 {
    Sqrt, RSqrt, Rcp, Exp, Exp2,
    Abs, Sign, Ceil, Floor, FracUnsigned, Frac, Saturate, IsFinite, IsInfinite,
};

template <std::size_t N>
inline void applyFunc1Plane(VectorFunc1 fn, const u32* a, const u32* keep, u32* dst) noexcept {
    switch (fn) {
    case VectorFunc1::Sqrt:         planeUnary<N, ops::fSqrt>(a, keep, dst); return;
    case VectorFunc1::RSqrt:        planeUnary<N, ops::fRSqrt>(a, keep, dst); return;
    case VectorFunc1::Rcp:          planeUnary<N, ops::fRcp>(a, keep, dst); return;
    case VectorFunc1::Exp:          planeUnary<N, ops::fExp>(a, keep, dst); return;
    case VectorFunc1::Exp2:         planeUnary<N, ops::fExp2>(a, keep, dst); return;
    case VectorFunc1::Abs:          planeUnary<N, ops::fAbs>(a, keep, dst); return;
    case VectorFunc1::Sign:         planeUnary<N, ops::fSign>(a, keep, dst); return;
    case VectorFunc1::Ceil:         planeUnary<N, ops::fCeil>(a, keep, dst); return;
    case VectorFunc1::Floor:        planeUnary<N, ops::fFloor>(a, keep, dst); return;
    case VectorFunc1::FracUnsigned: planeUnary<N, ops::fFracUnsigned>(a, keep, dst); return;
    case VectorFunc1::Frac:         planeUnary<N, ops::fFrac>(a, keep, dst); return;
    case VectorFunc1::Saturate:     planeUnary<N, ops::fSaturate>(a, keep, dst); return;
    case VectorFunc1::IsFinite:     planeUnary<N, ops::fIsFinite>(a, keep, dst); return;
    case VectorFunc1::IsInfinite:   planeUnary<N, ops::fIsInfinite>(a, keep, dst); return;
    }
}

template <std::size_t N>
inline void fillPlane(u32 word, const u32* keep, u32* dst) noexcept {
    CF_PLANE_LOOP
    for (std::size_t l = 0; l < N; ++l) {
        dst[l] = blend(word, keep[l], dst[l]);
    }
}

template <std::size_t N>
inline void copyPlane(const u32* src, const u32* keep, u32* dst) noexcept {
    CF_PLANE_LOOP
    for (std::size_t l = 0; l < N; ++l) {
        dst[l] = blend(src[l], keep[l], dst[l]);
    }
}

}
