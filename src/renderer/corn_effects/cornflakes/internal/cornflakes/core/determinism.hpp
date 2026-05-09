#pragma once

/// @file
/// @brief Compile-time guard asserting that determinism flags were applied to this TU.

#ifndef CORNFLAKES_DETERMINISM_FLAGS_APPLIED
#error                                                                                             \
    "cornflakes/core/determinism.hpp included in a translation unit that is not linked against cornflakes::determinism"
#endif

static_assert(CORNFLAKES_DETERMINISM_FLAGS_APPLIED == 1,
              "CORNFLAKES_DETERMINISM_FLAGS_APPLIED must be 1");

namespace whiteout::cornflakes::core {

inline constexpr bool kDeterminismFlagsApplied = true;

}
