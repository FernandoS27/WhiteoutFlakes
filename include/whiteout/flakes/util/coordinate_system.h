#pragma once

/// @file coordinate_system.h
/// @brief Convert points / directions / quaternions / transforms between
///        Blizzard and 3ds Max axis conventions.
///
/// **Blizzard convention**: +X forward, +Y left, +Z up (right-handed).
/// **Max convention**: +X right, +Y forward, +Z up (right-handed).
/// Used by adapters that import data authored in a foreign coord space
/// (the Max plugin's scene adapter, for instance).

#include "../types.h"

#include <cmath>

namespace whiteout::flakes::renderer {

/// @brief Right-handed coordinate-system convention.
///
/// All three are Z-up and right-handed, so each is fully described by where
/// "forward" points; "right" then follows as `forward × up`.
///
/// New entries append. `CoordSpace` is part of the bound `flakes_core` module
/// (see BINDINGS.md) and `WDX_DEFAULT_COORD_SPACE` is pasted after
/// `CoordSpace::`, so renumbering an existing value changes both.
enum class CoordSpace {
    /// Warcraft III and World of Warcraft: +X forward, +Y **left**, +Z up.
    /// (An earlier comment here said "+Y right", which contradicts both the
    /// basis table and `ForwardAxis` — for a right-handed Z-up space with
    /// +X forward, right is −Y.)
    Blizzard,
    /// 3ds Max: −Y forward, +X left, +Z up.
    Max,
    /// StarCraft II and Heroes of the Storm: +Y forward, +X right, +Z up.
    /// A 180° yaw from @ref Max, not the same space — Max's +X is left.
    Sc2,
};

#ifndef WDX_DEFAULT_COORD_SPACE
#define WDX_DEFAULT_COORD_SPACE Blizzard
#endif
/// @brief The renderer's native coordinate space.
///
/// Selected at CMake-configure time via `-DWDX_DEFAULT_COORD_SPACE=…`;
/// defaults to Blizzard. Everything in the renderer (bone matrices,
/// particle samples, camera) is in this space.
inline constexpr CoordSpace kDefaultCoordSpace = CoordSpace::WDX_DEFAULT_COORD_SPACE;

/// @brief Forward unit vector in the given space.
inline Vector3f ForwardAxis(CoordSpace s) {
    switch (s) {
    case CoordSpace::Max:
        return {0.0f, -1.0f, 0.0f};
    case CoordSpace::Sc2:
        return {0.0f, 1.0f, 0.0f};
    case CoordSpace::Blizzard:
        break;
    }
    return {1.0f, 0.0f, 0.0f};
}
/// @brief Forward unit vector in @ref kDefaultCoordSpace.
inline Vector3f DefaultForwardAxis() {
    return ForwardAxis(kDefaultCoordSpace);
}

/// @brief Static helpers for moving geometry data between coordinate spaces.
///
/// Use `ConvertPoint` for positions (basis change applied), `ConvertDirection`
/// for vectors that should ignore translation, `ConvertScale` for non-uniform
/// scale vectors, `ConvertTangent` for tangent-frame `vec4` (with sign in w),
/// and `ConvertQuaternion` for rotations. `ConvertTransform` is the full
/// 4x4 conjugation.
class CoordinateSystem {
public:
    /// @brief Renderer-native space (== @ref kDefaultCoordSpace).
    static constexpr CoordSpace Default() {
        return kDefaultCoordSpace;
    }

    /// @brief Basis matrix that converts vectors *from* @p s to the
    ///        renderer-native space.
    static const Matrix44f& BasisToRef(CoordSpace s);
    /// @brief Basis matrix that converts vectors *from* the renderer-native
    ///        space to @p s.
    static const Matrix44f& BasisFromRef(CoordSpace s);
    /// @brief Combined basis change `from → to`.
    static const Matrix44f& BasisChange(CoordSpace from, CoordSpace to);

    static Vector3f ConvertPoint(CoordSpace from, CoordSpace to, Vector3f v);
    static Vector3f ConvertDirection(CoordSpace from, CoordSpace to, Vector3f v);
    static Vector3f ConvertScale(CoordSpace from, CoordSpace to, Vector3f s);
    static Vector4f ConvertTangent(CoordSpace from, CoordSpace to, Vector4f t);
    static Quaternion ConvertQuaternion(CoordSpace from, CoordSpace to, Quaternion q);
    static Matrix44f ConvertTransform(CoordSpace from, CoordSpace to, const Matrix44f& M);

    /// @name Shortcuts to renderer-native space
    /// @{
    static Vector3f ToDefault(CoordSpace from, Vector3f v) {
        return ConvertPoint(from, Default(), v);
    }
    static Vector3f ToDefaultDir(CoordSpace from, Vector3f v) {
        return ConvertDirection(from, Default(), v);
    }
    static Vector3f ToDefaultScale(CoordSpace from, Vector3f s) {
        return ConvertScale(from, Default(), s);
    }
    static Vector4f ToDefaultTangent(CoordSpace from, Vector4f t) {
        return ConvertTangent(from, Default(), t);
    }
    static Quaternion ToDefault(CoordSpace from, Quaternion q) {
        return ConvertQuaternion(from, Default(), q);
    }
    static Matrix44f ToDefault(CoordSpace from, const Matrix44f& M) {
        return ConvertTransform(from, Default(), M);
    }
    /// @}
};

} // namespace whiteout::flakes::renderer

namespace whiteout::flakes {
using ::whiteout::flakes::renderer::CoordinateSystem;
using ::whiteout::flakes::renderer::CoordSpace;
using ::whiteout::flakes::renderer::DefaultForwardAxis;
using ::whiteout::flakes::renderer::ForwardAxis;
using ::whiteout::flakes::renderer::kDefaultCoordSpace;
} // namespace whiteout::flakes
