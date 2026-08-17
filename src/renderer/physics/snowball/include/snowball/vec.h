//===----------------------------------------------------------------------===//
// snowball/vec.h -- the four-lane vector and the engine's shared constants.
//
// Vec4 is four named lanes with 16-byte alignment, and both properties are load-bearing: the
// alignment lets the reciprocal primitives in math.h hand a value straight to an SSE register,
// and `w` is real storage rather than padding -- a quaternion keeps its scalar in it, and
// several structs carry meaning in the fourth lane.
//
// The constants are not conveniences. The engine's arithmetic is built from these exact bit
// patterns in specific associations, and both halves are part of the numeric contract:
// `kOneThird` is the float32 nearest 1/3, spelled as a literal rather than computed, and either
// nudging a value or reassociating the expressions that consume it changes the last bit of
// every volume downstream. Regression baselines pin those exact bits.
//===----------------------------------------------------------------------===//
#pragma once

#include "snowball/common_types.h"

namespace snowball {

/// @brief Four floats, 16-byte aligned. `w` is storage, not padding.
struct alignas(16) Vec4 {
    f32 x{};
    f32 y{};
    f32 z{};
    f32 w{};

    constexpr Vec4() = default;
    constexpr Vec4(f32 x_, f32 y_, f32 z_, f32 w_ = 0.0f) : x(x_), y(y_), z(z_), w(w_) {}

    static constexpr Vec4 Splat(f32 v) { return {v, v, v, v}; }

    friend constexpr bool operator==(const Vec4&, const Vec4&) = default;
};

constexpr Vec4 operator+(const Vec4& a, const Vec4& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
constexpr Vec4 operator-(const Vec4& a, const Vec4& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}
constexpr Vec4 operator*(const Vec4& a, const Vec4& b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}
constexpr Vec4 operator*(const Vec4& a, f32 s) { return a * Vec4::Splat(s); }
constexpr Vec4 operator*(f32 s, const Vec4& a) { return a * Vec4::Splat(s); }
constexpr Vec4 operator-(const Vec4& a) { return {-a.x, -a.y, -a.z, -a.w}; }

/// @brief Three-lane dot product, summed as `(x + y) + z`.
///
/// The association is part of the numeric contract, not a stylistic choice: reassociating the
/// sum changes the last ULP, and every constraint impulse downstream inherits the difference.
constexpr f32 Dot3(const Vec4& a, const Vec4& b) {
    return (a.x * b.x + a.y * b.y) + a.z * b.z;
}

constexpr Vec4 Cross3(const Vec4& a, const Vec4& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x, 0.0f};
}

constexpr f32 LengthSquared3(const Vec4& v) { return Dot3(v, v); }

// -- the constant block --------------------------------------------------------------------

namespace constants {

inline constexpr Vec4 kZero{};
inline constexpr Vec4 kOne = Vec4::Splat(1.0f);
inline constexpr Vec4 kOneHalf = Vec4::Splat(0.5f);
inline constexpr Vec4 kOneThird = Vec4::Splat(0.3333333432674408f);
inline constexpr Vec4 kTwo = Vec4::Splat(2.0f);
inline constexpr Vec4 kThree = Vec4::Splat(3.0f);
inline constexpr Vec4 kPi = Vec4::Splat(3.1415927410125732f);
inline constexpr Vec4 kTwoPi = Vec4::Splat(6.2831854820251465f);
inline constexpr Vec4 kEpsilon = Vec4::Splat(1.1920928955078125e-07f);
inline constexpr Vec4 kMaxFloat = Vec4::Splat(3.4028234663852886e+38f);

/// The guard used instead of dividing by a near-zero determinant: 1000 * FLT_MIN, and neither
/// FLT_MIN nor FLT_EPSILON.
inline constexpr Vec4 kZeroSafe = Vec4::Splat(1.1754943508222875e-35f);

/// Allowed contact penetration, the float32 nearest 0.005. Position correction deliberately
/// leaves this much overlap so contacts stay persistent instead of jittering, and it is the
/// depth at which a body finally comes to rest.
inline constexpr Vec4 kLinearSlop = Vec4::Splat(0.004999999888241291f);

inline constexpr Vec4 kUnitX{1.0f, 0.0f, 0.0f, 0.0f};
inline constexpr Vec4 kUnitY{0.0f, 1.0f, 0.0f, 0.0f};
inline constexpr Vec4 kUnitZ{0.0f, 0.0f, 1.0f, 0.0f};
inline constexpr Vec4 kUnitW{0.0f, 0.0f, 0.0f, 1.0f};

/// Quaternion storage is xyzw with the scalar LAST.
inline constexpr Vec4 kQuatIdentity{0.0f, 0.0f, 0.0f, 1.0f};

}  // namespace constants

}  // namespace snowball
