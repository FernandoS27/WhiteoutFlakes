#pragma once

/// @file
/// @brief Math-opcode (MathOp / MathFunc1 / MathFunc2 / MathFunc3) ID enums and scalar evaluators.
///
/// IDs match the engine's `m_FunctionDetails` tables and are interchangeable
/// with the raw u8 sub-id encoded in the bytecode operand.

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>

#include <optional>

namespace whiteout::cornflakes {

/// @brief Two-operand math opcode sub-id (Add/Sub/Mul/.../comparison ops).
enum class MathOp : u8 {
    Add = 0,
    Sub = 1,
    Mul = 2,
    Div = 3,
    Mod = 4,
    Neg = 5,
    Shl = 6,
    Shr = 7,
    BitAnd = 8,
    BitOr = 9,
    BitXor = 10,
    BitNot = 11,
    Lt = 12,
    Le = 13,
    Gt = 14,
    Ge = 15,
    Eq = 16,
    Ne = 17,
    Count,
};

/// @brief Unary math function sub-id; "Fast*" entries are reduced-precision variants.
enum class MathFunc1 : u8 {
    Sqrt = 0,
    FastSqrt = 31,
    RSqrt = 1,
    FastRSqrt = 32,
    Cbrt = 2,
    FastCbrt = 33,
    Length = 3,
    FastLength = 34,
    Normalize = 4,
    FastNormalize = 35,
    Sin = 5,
    FastSin = 36,
    Cos = 6,
    FastCos = 37,
    SinCos = 7,
    FastSinCos = 38,
    Tan = 8,
    FastTan = 39,
    Asin = 9,
    FastAsin = 40,
    Acos = 10,
    FastAcos = 41,
    Atan = 11,
    FastAtan = 42,
    Exp = 13,
    FastExp = 44,
    Exp2 = 14,
    FastExp2 = 45,
    Log = 15,
    FastLog = 46,
    Log2 = 16,
    FastLog2 = 47,
    Rcp = 17,
    FastRcp = 48,
    Abs = 18,
    Sign = 19,
    Ceil = 20,
    Floor = 21,
    FracUnsigned = 22,
    Frac = 23,
    Saturate = 24,
    All = 49,
    Any = 50,
    IsFinite = 51,
    IsInfinite = 52,
};

f32 mathSign(f32 x) noexcept;

/// @brief `x - floor(x)`, always in `[0,1)` regardless of sign.
f32 mathFracUnsigned(f32 x) noexcept;

/// @brief `x - trunc(x)`, sign-preserving (range `(-1,1)`).
f32 mathFrac(f32 x) noexcept;

/// @brief Evaluate a unary MathFunc1 by raw sub-id. Pushes a fatal issue on unknown ids.
std::optional<f32> mathFunc1Eval(u8 id, f32 x, IssueBag& issues);

/// @brief Two-operand math function sub-id (atan2, min, max, dot, cross, ...).
enum class MathFunc2 : u8 {
    Atan2 = 12,
    FastAtan2 = 43,
    Step = 25,
    Discretize = 26,
    Min = 27,
    Max = 28,
    Dot = 29,
    Cross = 30,
};

/// @brief Three-operand math function sub-id. IDs are engine-verified (lerp/clamp/within).
enum class MathFunc3 : u8 {
    Lerp = 0,
    Clamp = 1,
    Within = 2,
    Count,
};

f32 mathLerp(f32 a, f32 b, f32 t) noexcept;

f32 mathClamp(f32 x, f32 lo, f32 hi) noexcept;

/// @brief `(x >= lo) && (x <= hi)` boolean range check (NOT smoothstep).
f32 mathWithin(f32 x, f32 lo, f32 hi) noexcept;

/// @brief Evaluate a ternary MathFunc3 by raw sub-id. Pushes a fatal issue on unknown ids.
std::optional<f32> mathFunc3Eval(u8 id, f32 a, f32 b, f32 c, IssueBag& issues);

} // namespace whiteout::cornflakes
