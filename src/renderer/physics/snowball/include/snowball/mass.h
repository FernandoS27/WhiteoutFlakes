//===----------------------------------------------------------------------===//
// snowball/mass.h -- what a shape weighs and how it resists rotation.
//
// Only the values here are contractual: the computed mass, centre, and tensor feed the solver
// bit-for-bit, so the mass routines that fill this struct must not be "improved". The layout
// is not -- nothing keys off field offsets or struct size, so the ordering stays whatever
// reads best.
//===----------------------------------------------------------------------===//
#pragma once

#include "snowball/common_types.h"
#include "snowball/math.h"
#include "snowball/vec.h"

namespace snowball {

struct MassProperties {
    Mtx inertia{};      ///< symmetric, so the column/row distinction does not bite here
    Vec4 centre{};      ///< centre of mass, local space
    f32 mass{0.0f};
};

}  // namespace snowball
