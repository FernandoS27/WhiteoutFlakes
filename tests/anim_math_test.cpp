// ============================================================================
// Shared animation kernels.
//
// ComposePivotSRT was extracted from two byte-identical copies (MDX's
// Vec3QuatScaleToMatrix44f and M2's M2BoneLocal). The values below are pinned
// as literals computed by hand from the composition it claims to perform —
// calling either former copy to generate them would only prove the move was
// self-consistent, not that it was correct.
// ============================================================================

#include "renderer/animation/anim_math.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace whiteout;
using namespace whiteout::flakes;
using namespace whiteout::flakes::renderer::animation;
using Catch::Approx;

namespace {

// Row-vector transform: p' = p * M.
Vector3f Apply(const Matrix44f& m, const Vector3f& p) {
    return {p.x * m.data[0][0] + p.y * m.data[1][0] + p.z * m.data[2][0] + m.data[3][0],
            p.x * m.data[0][1] + p.y * m.data[1][1] + p.z * m.data[2][1] + m.data[3][1],
            p.x * m.data[0][2] + p.y * m.data[1][2] + p.z * m.data[2][2] + m.data[3][2]};
}

const Quaternion kIdentity{0.0f, 0.0f, 0.0f, 1.0f};

} // namespace

TEST_CASE("ComposePivotSRT is p' = ((p - pivot) * S * R) + pivot + t", "[animmath]") {
    SECTION("identity everything is the identity transform") {
        const Matrix44f m = ComposePivotSRT({0, 0, 0}, kIdentity, {1, 1, 1}, {0, 0, 0});
        const Vector3f p = Apply(m, {2, 3, 4});
        REQUIRE(p.x == Approx(2.0f));
        REQUIRE(p.y == Approx(3.0f));
        REQUIRE(p.z == Approx(4.0f));
    }

    SECTION("translation moves the point") {
        const Matrix44f m = ComposePivotSRT({5, 0, 0}, kIdentity, {1, 1, 1}, {0, 0, 0});
        REQUIRE(Apply(m, {1, 1, 1}).x == Approx(6.0f));
    }

    SECTION("scale acts about the pivot, not the origin") {
        // pivot at x=10, scale 2: the point at x=12 is 2 away from the pivot,
        // so it lands 4 away — at x=14. Scaling about the origin would give 24.
        const Matrix44f m = ComposePivotSRT({0, 0, 0}, kIdentity, {2, 2, 2}, {10, 0, 0});
        REQUIRE(Apply(m, {12, 0, 0}).x == Approx(14.0f));
    }

    SECTION("the pivot itself is fixed under scale") {
        const Matrix44f m = ComposePivotSRT({0, 0, 0}, kIdentity, {3, 3, 3}, {7, 2, 0});
        const Vector3f p = Apply(m, {7, 2, 0});
        REQUIRE(p.x == Approx(7.0f));
        REQUIRE(p.y == Approx(2.0f));
    }

    SECTION("rotation acts about the pivot") {
        // 90 degrees about Z, pivot at (1,0,0). The point (2,0,0) is one unit
        // along +X from the pivot and must swing to one unit along +Y: (1,1,0).
        const f32 s = std::sqrt(0.5f);
        const Quaternion rz90{0.0f, 0.0f, s, s};
        const Matrix44f m = ComposePivotSRT({0, 0, 0}, rz90, {1, 1, 1}, {1, 0, 0});
        const Vector3f p = Apply(m, {2, 0, 0});
        REQUIRE(p.x == Approx(1.0f).margin(1e-5));
        REQUIRE(p.y == Approx(1.0f).margin(1e-5));
        REQUIRE(p.z == Approx(0.0f).margin(1e-5));
    }

    SECTION("translation is applied after the pivot is restored") {
        // Scale about the pivot, then translate — the translation must not be
        // scaled, which is what putting it in T(pivot + t) guarantees.
        const Matrix44f m = ComposePivotSRT({100, 0, 0}, kIdentity, {2, 2, 2}, {10, 0, 0});
        REQUIRE(Apply(m, {12, 0, 0}).x == Approx(114.0f));
    }
}
