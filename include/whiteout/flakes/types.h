#pragma once

// ============================================================================
// WhiteoutFlakes — public math / scalar types.
//
// This header is the canonical public surface for the value types every
// consumer of WhiteoutFlakesLib needs: scalar aliases, vectors, matrices, and
// a small Rect. The definitions live in WhiteoutLib (whiteout/common_types.h
// and whiteout/vector_types.h) and in src/renderer/render_target.h; this
// header re-exports them under the public `whiteout::flakes` namespace.
// ============================================================================

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include <cstddef>

namespace whiteout::flakes {

using whiteout::f16;
using whiteout::f32;
using whiteout::f64;
using whiteout::i16;
using whiteout::i32;
using whiteout::i64;
using whiteout::i8;
using whiteout::u16;
using whiteout::u32;
using whiteout::u64;
using whiteout::u8;

using usize = std::size_t;
using isize = std::ptrdiff_t;

using whiteout::snorm;
using whiteout::snorm16;
using whiteout::snorm32;
using whiteout::snorm8;
using whiteout::unorm;
using whiteout::unorm16;
using whiteout::unorm32;
using whiteout::unorm8;

using whiteout::fixed16_11;
using whiteout::fixed16_8;
using whiteout::fixed16_9;
using whiteout::fixed32_16;
using whiteout::fixed8_5;
using whiteout::fixed_point;

using whiteout::Matrix44f;
using whiteout::Quaternion;
using whiteout::Vector2f;
using whiteout::Vector3f;
using whiteout::Vector4f;

struct Rect {
    i32 left;
    i32 top;
    i32 right;
    i32 bottom;
};

} // namespace whiteout::flakes
