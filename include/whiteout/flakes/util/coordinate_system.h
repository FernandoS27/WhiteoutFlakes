#pragma once

// ============================================================================
// WhiteoutFlakes — coordinate-system helpers.
//
// Convert between Blizzard / Max axis conventions. Used by adapters that
// import data authored in foreign coord spaces.
// ============================================================================

#include "../types.h"

#include <cmath>

namespace whiteout::flakes::renderer {

enum class CoordSpace {
    Blizzard,
    Max,

};

#ifndef WDX_DEFAULT_COORD_SPACE
  #define WDX_DEFAULT_COORD_SPACE Blizzard
#endif
inline constexpr CoordSpace kDefaultCoordSpace = CoordSpace::WDX_DEFAULT_COORD_SPACE;

inline Vector3f ForwardAxis(CoordSpace s) {

    return (s == CoordSpace::Blizzard) ? Vector3f{1.0f, 0.0f, 0.0f}
                                       : Vector3f{0.0f, -1.0f, 0.0f};
}
inline Vector3f DefaultForwardAxis() { return ForwardAxis(kDefaultCoordSpace); }

class CoordinateSystem {
public:
    static constexpr CoordSpace Default() { return kDefaultCoordSpace; }

    static const Matrix44f& BasisToRef  (CoordSpace s);
    static const Matrix44f& BasisFromRef(CoordSpace s);
    static const Matrix44f& BasisChange (CoordSpace from, CoordSpace to);

    static Vector3f   ConvertPoint     (CoordSpace from, CoordSpace to, Vector3f v);
    static Vector3f   ConvertDirection (CoordSpace from, CoordSpace to, Vector3f v);
    static Vector3f   ConvertScale     (CoordSpace from, CoordSpace to, Vector3f s);
    static Vector4f   ConvertTangent   (CoordSpace from, CoordSpace to, Vector4f t);
    static Quaternion ConvertQuaternion(CoordSpace from, CoordSpace to, Quaternion q);
    static Matrix44f  ConvertTransform (CoordSpace from, CoordSpace to, const Matrix44f& M);

    static Vector3f   ToDefault(CoordSpace from, Vector3f v)              { return ConvertPoint    (from, Default(), v); }
    static Vector3f   ToDefaultDir(CoordSpace from, Vector3f v)           { return ConvertDirection(from, Default(), v); }
    static Vector3f   ToDefaultScale(CoordSpace from, Vector3f s)         { return ConvertScale    (from, Default(), s); }
    static Vector4f   ToDefaultTangent(CoordSpace from, Vector4f t)       { return ConvertTangent  (from, Default(), t); }
    static Quaternion ToDefault(CoordSpace from, Quaternion q)            { return ConvertQuaternion(from, Default(), q); }
    static Matrix44f  ToDefault(CoordSpace from, const Matrix44f& M)      { return ConvertTransform(from, Default(), M); }
};

}  // namespace whiteout::flakes::renderer

namespace whiteout::flakes {
using ::whiteout::flakes::renderer::CoordSpace;
using ::whiteout::flakes::renderer::CoordinateSystem;
using ::whiteout::flakes::renderer::ForwardAxis;
using ::whiteout::flakes::renderer::DefaultForwardAxis;
using ::whiteout::flakes::renderer::kDefaultCoordSpace;
}
