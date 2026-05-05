#pragma once

// Project-wide basic types. Mirrors the convention used by WhiteoutLib so that
// types flow naturally across the API boundary between WhiteoutDex and
// WhiteoutLib. The canonical definitions live in <whiteout/common_types.h>;
// this header just re-exports them into the WhiteoutDex namespace.

#include <whiteout/common_types.h>

#include <cstddef>

namespace WhiteoutDex {

using whiteout::u8;
using whiteout::u16;
using whiteout::u32;
using whiteout::u64;

using whiteout::i8;
using whiteout::i16;
using whiteout::i32;
using whiteout::i64;

using whiteout::f16;
using whiteout::f32;
using whiteout::f64;

using usize = std::size_t;
using isize = std::ptrdiff_t;

using whiteout::snorm;
using whiteout::unorm;
using whiteout::snorm8;
using whiteout::snorm16;
using whiteout::snorm32;
using whiteout::unorm8;
using whiteout::unorm16;
using whiteout::unorm32;

using whiteout::fixed_point;
using whiteout::fixed8_5;
using whiteout::fixed16_8;
using whiteout::fixed16_9;
using whiteout::fixed16_11;
using whiteout::fixed32_16;

} // namespace WhiteoutDex
