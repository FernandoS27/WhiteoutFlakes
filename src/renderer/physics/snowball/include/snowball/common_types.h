//===----------------------------------------------------------------------===//
// snowball/common_types.h -- the fixed-width vocabulary every other header speaks.
//
// Struct sizes and field widths in this engine are contractual, and a type whose width is
// decided by the toolchain is a type that can quietly stop honouring them. A height sample is
// six bytes because its three fields are 16 bits each; a mesh node's bounds are `int16` because
// authored assets quantise them to that width; a contact id is eight bytes because contact
// matching compares exactly eight. None of those are "an integer" -- they are a specific number
// of bits, and writing them as `int` or `short` states the width in a way the compiler is free
// to reinterpret and a reader has to guess at.
//
// So the rule here is: every scalar in the public surface and in the implementation names its
// own width. `i32` rather than `int`, `f32` rather than `float`, `u16` rather than
// `unsigned short`. The aliases are exact -- `f32` *is* `float` -- so this changes nothing the
// compiler emits and everything about what the source claims. The C shim in bindings/ is the
// one exception and keeps `<stdint.h>` spellings, because it has to parse as C.
//
// Deliberately minimal: no `f16`, and no `snorm`/`unorm`/`fixed_point` templates. Nothing in
// the engine stores a normalised integer: the two quantised representations it does have
// (height samples, mesh node bounds) both scale by a runtime factor rather than a compile-time
// one, so a fixed-point template would not fit them and an unused copy of one here would only
// invite the wrong reading.
//===----------------------------------------------------------------------===//
#pragma once

#include <cstddef>
#include <cstdint>

namespace snowball {

// -- integers ------------------------------------------------------------------------------

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

// -- floating point ------------------------------------------------------------------------

/// IEEE 754 binary32. The engine's working type: every vector lane, every constant, every
/// impulse. `f64` appears only where a test needs headroom to judge an `f32` result.
using f32 = float;
using f64 = double;

// -- sizes and indices ---------------------------------------------------------------------

/// For container subscripts and byte counts, where the width is the platform's by definition
/// and pinning it to 32 or 64 bits would be the unclear choice rather than the clear one.
using usize = std::size_t;
using isize = std::ptrdiff_t;

// The aliases above are load-bearing rather than decorative -- a `float` that is not binary32
// voids every bit-exact guarantee the engine makes -- so they are checked rather than assumed.
static_assert(sizeof(f32) == 4, "snowball requires a 32-bit float");
static_assert(sizeof(f64) == 8, "snowball requires a 64-bit double");

}  // namespace snowball
