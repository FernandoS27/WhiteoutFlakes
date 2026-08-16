// Coordinate-space conversion (Blizzard <-> Max <-> Sc2). The adapters that
// import foreign-authored data lean on these being exact inverses of each
// other, so most tests assert the invariants rather than one hand-picked
// mapping — every space in `kSpaces` gets the round-trip sweep for free.
//
// Invariants alone cannot pin *which* convention a space is, though: a table
// rotated 180° round-trips perfectly and still points every model backwards.
// The axis-semantics case at the bottom is what fixes each space's meaning.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "whiteout/flakes/util/coordinate_system.h"

#include <cmath>

using whiteout::flakes::f32;
using whiteout::flakes::i32;
using whiteout::flakes::Matrix44f;
using whiteout::flakes::Quaternion;
using whiteout::flakes::Vector3f;
using whiteout::flakes::Vector4f;
using whiteout::flakes::renderer::CoordinateSystem;
using whiteout::flakes::renderer::CoordSpace;
using Catch::Approx;

namespace {

constexpr CoordSpace kSpaces[] = {CoordSpace::Blizzard, CoordSpace::Max, CoordSpace::Sc2};

f32 Length(const Vector3f& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

void RequireSameVector(const Vector3f& a, const Vector3f& b, f32 margin = 1e-5f) {
    REQUIRE(a.x == Approx(b.x).margin(margin));
    REQUIRE(a.y == Approx(b.y).margin(margin));
    REQUIRE(a.z == Approx(b.z).margin(margin));
}

void RequireIdentity(const Matrix44f& m, f32 margin = 1e-5f) {
    for (i32 i = 0; i < 4; ++i)
        for (i32 j = 0; j < 4; ++j)
            REQUIRE(m.data[i][j] == Approx(i == j ? 1.0f : 0.0f).margin(margin));
}

// A transform with rotation, non-uniform scale and translation, so a
// conjugation that drops any of them shows up.
Matrix44f MakeTransform() {
    Matrix44f m = Matrix44f::identity();
    const f32 c = std::cos(0.7f), s = std::sin(0.7f);
    m.data[0][0] = 2.0f * c;
    m.data[0][1] = 2.0f * -s;
    m.data[1][0] = 3.0f * s;
    m.data[1][1] = 3.0f * c;
    m.data[2][2] = 4.0f;
    m.data[3][0] = 10.0f;
    m.data[3][1] = -20.0f;
    m.data[3][2] = 30.0f;
    return m;
}

constexpr Vector3f kSample{1.0f, -2.0f, 3.5f};

} // namespace

TEST_CASE("Converting a space to itself is the identity") {
    for (CoordSpace s : kSpaces) {
        RequireSameVector(CoordinateSystem::ConvertPoint(s, s, kSample), kSample);
        RequireSameVector(CoordinateSystem::ConvertDirection(s, s, kSample), kSample);
        RequireSameVector(CoordinateSystem::ConvertScale(s, s, {1.0f, 2.0f, 3.0f}),
                          {1.0f, 2.0f, 3.0f});
        RequireIdentity(CoordinateSystem::BasisChange(s, s));
    }
}

TEST_CASE("Point conversion round-trips") {
    for (CoordSpace from : kSpaces) {
        for (CoordSpace to : kSpaces) {
            const Vector3f there = CoordinateSystem::ConvertPoint(from, to, kSample);
            RequireSameVector(CoordinateSystem::ConvertPoint(to, from, there), kSample);
        }
    }
}

TEST_CASE("Conversion preserves length") {
    // The basis change is a rotation, so nothing may grow or shrink.
    for (CoordSpace from : kSpaces) {
        for (CoordSpace to : kSpaces) {
            const Vector3f v = CoordinateSystem::ConvertPoint(from, to, kSample);
            REQUIRE(Length(v) == Approx(Length(kSample)).margin(1e-5));
        }
    }
}

TEST_CASE("Points and directions convert identically") {
    // The basis matrices carry no translation, so transform_point and
    // transform_normal must agree — a translation creeping in would show here.
    for (CoordSpace from : kSpaces) {
        for (CoordSpace to : kSpaces) {
            RequireSameVector(CoordinateSystem::ConvertPoint(from, to, kSample),
                              CoordinateSystem::ConvertDirection(from, to, kSample));
        }
    }
}

TEST_CASE("Basis matrices are orthonormal") {
    for (CoordSpace from : kSpaces) {
        for (CoordSpace to : kSpaces) {
            const Matrix44f& m = CoordinateSystem::BasisChange(from, to);
            for (i32 i = 0; i < 3; ++i) {
                f32 rowLen = 0.0f;
                for (i32 k = 0; k < 3; ++k)
                    rowLen += m.data[i][k] * m.data[i][k];
                REQUIRE(rowLen == Approx(1.0f).margin(1e-5));
                for (i32 j = i + 1; j < 3; ++j) {
                    f32 dot = 0.0f;
                    for (i32 k = 0; k < 3; ++k)
                        dot += m.data[i][k] * m.data[j][k];
                    REQUIRE(dot == Approx(0.0f).margin(1e-5));
                }
            }
        }
    }
}

TEST_CASE("BasisToRef and BasisFromRef are inverses") {
    for (CoordSpace s : kSpaces)
        RequireIdentity(CoordinateSystem::BasisToRef(s) * CoordinateSystem::BasisFromRef(s));
}

TEST_CASE("Scale conversion permutes magnitudes without sign flips") {
    const Vector3f scale{2.0f, 3.0f, 5.0f};
    for (CoordSpace from : kSpaces) {
        for (CoordSpace to : kSpaces) {
            const Vector3f s = CoordinateSystem::ConvertScale(from, to, scale);
            REQUIRE(s.x > 0.0f);
            REQUIRE(s.y > 0.0f);
            REQUIRE(s.z > 0.0f);
            // Same three magnitudes, possibly reordered.
            REQUIRE(s.x * s.y * s.z == Approx(scale.x * scale.y * scale.z).margin(1e-4));
            RequireSameVector(CoordinateSystem::ConvertScale(to, from, s), scale, 1e-4f);
        }
    }
}

TEST_CASE("Tangent conversion rotates xyz and keeps the handedness sign") {
    // Both conventions are right-handed, so the basis change has a positive
    // determinant and the tangent's w (bitangent sign) must survive it.
    const Vector4f t{0.0f, 1.0f, 0.0f, -1.0f};
    for (CoordSpace from : kSpaces) {
        for (CoordSpace to : kSpaces) {
            const Vector4f out = CoordinateSystem::ConvertTangent(from, to, t);
            RequireSameVector({out.x, out.y, out.z},
                              CoordinateSystem::ConvertDirection(from, to, {t.x, t.y, t.z}));
            REQUIRE(out.w == Approx(t.w).margin(1e-6));
        }
    }
}

TEST_CASE("Quaternion conversion keeps the rotation unit-length and round-trips") {
    const f32 half = 0.6f;
    const f32 s = std::sin(half);
    const Quaternion q{s * 0.0f, s * 0.6f, s * 0.8f, std::cos(half)};

    for (CoordSpace from : kSpaces) {
        for (CoordSpace to : kSpaces) {
            const Quaternion out = CoordinateSystem::ConvertQuaternion(from, to, q);
            const f32 norm =
                std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z + out.w * out.w);
            REQUIRE(norm == Approx(1.0f).margin(1e-5));

            const Quaternion back = CoordinateSystem::ConvertQuaternion(to, from, out);
            REQUIRE(back.x == Approx(q.x).margin(1e-5));
            REQUIRE(back.y == Approx(q.y).margin(1e-5));
            REQUIRE(back.z == Approx(q.z).margin(1e-5));
            REQUIRE(back.w == Approx(q.w).margin(1e-5));
        }
    }
}

TEST_CASE("Transform conversion round-trips") {
    const Matrix44f m = MakeTransform();
    for (CoordSpace from : kSpaces) {
        for (CoordSpace to : kSpaces) {
            const Matrix44f there = CoordinateSystem::ConvertTransform(from, to, m);
            const Matrix44f back = CoordinateSystem::ConvertTransform(to, from, there);
            for (i32 i = 0; i < 4; ++i)
                for (i32 j = 0; j < 4; ++j)
                    REQUIRE(back.data[i][j] == Approx(m.data[i][j]).margin(1e-4));
        }
    }
}

TEST_CASE("Converting the identity transform yields the identity") {
    for (CoordSpace from : kSpaces)
        for (CoordSpace to : kSpaces)
            RequireIdentity(
                CoordinateSystem::ConvertTransform(from, to, Matrix44f::identity()));
}

TEST_CASE("The ToDefault shortcuts agree with the explicit conversions") {
    const CoordSpace def = CoordinateSystem::Default();
    REQUIRE(def == whiteout::flakes::renderer::kDefaultCoordSpace);

    for (CoordSpace s : kSpaces) {
        RequireSameVector(CoordinateSystem::ToDefault(s, kSample),
                          CoordinateSystem::ConvertPoint(s, def, kSample));
        RequireSameVector(CoordinateSystem::ToDefaultDir(s, kSample),
                          CoordinateSystem::ConvertDirection(s, def, kSample));
    }
}

TEST_CASE("Forward axes are unit vectors") {
    for (CoordSpace s : kSpaces)
        REQUIRE(Length(whiteout::flakes::renderer::ForwardAxis(s)) == Approx(1.0f).margin(1e-6));

    RequireSameVector(whiteout::flakes::renderer::DefaultForwardAxis(),
                      whiteout::flakes::renderer::ForwardAxis(CoordinateSystem::Default()));
}

TEST_CASE("Each space's axes mean what its documentation says") {
    // The one case the invariants above cannot cover. Round-trips, lengths and
    // inverses all hold just as well for a table that is 180° out, which would
    // render every StarCraft II model facing backwards — visible, but only if
    // someone happens to know which way a Marine should face.
    //
    // Renderer-native is Blizzard: +X forward, +Y left, +Z up (right-handed,
    // so right == forward × up == −Y).
    const CoordSpace ref = CoordSpace::Blizzard;
    constexpr Vector3f kFwdRef{1.0f, 0.0f, 0.0f};
    constexpr Vector3f kRightRef{0.0f, -1.0f, 0.0f};
    constexpr Vector3f kUpRef{0.0f, 0.0f, 1.0f};

    SECTION("Blizzard is the reference") {
        RequireSameVector(CoordinateSystem::ToDefaultDir(ref, {1.0f, 0.0f, 0.0f}), kFwdRef);
        RequireSameVector(CoordinateSystem::ToDefaultDir(ref, {0.0f, -1.0f, 0.0f}), kRightRef);
    }

    SECTION("Sc2 is -Y forward, +X left") {
        // Measured off the shipped skeletons, not taken from a wiki: Zergling
        // names its own legs `… Front` / `… Rear` / `… Left` / `… Right`, and
        // they rest at −Y / +Y / +X / −X respectively. `Ref_Head` sits at −Y on
        // Marine, SCV and Zealot alike.
        //
        // This used to assert +Y forward / +X right, which is the 180° yaw of
        // the truth and drew every SC2 model back-to-front. All three axes are
        // asserted, not just forward: forward alone still admits a mirrored
        // basis, which would flip every model's handedness.
        RequireSameVector(CoordinateSystem::ToDefaultDir(CoordSpace::Sc2, {0.0f, -1.0f, 0.0f}),
                          kFwdRef);
        RequireSameVector(CoordinateSystem::ToDefaultDir(CoordSpace::Sc2, {-1.0f, 0.0f, 0.0f}),
                          kRightRef);
        RequireSameVector(CoordinateSystem::ToDefaultDir(CoordSpace::Sc2, {0.0f, 0.0f, 1.0f}),
                          kUpRef);
    }

    SECTION("Max is -Y forward, and Sc2 shares its basis") {
        RequireSameVector(CoordinateSystem::ToDefaultDir(CoordSpace::Max, {0.0f, -1.0f, 0.0f}),
                          kFwdRef);
        RequireSameVector(CoordinateSystem::ToDefaultDir(CoordSpace::Max, {1.0f, 0.0f, 0.0f}),
                          {0.0f, 1.0f, 0.0f});
        // Not a shortcut — a measured coincidence. SC2's exporter was a 3ds Max
        // plugin and kept Max's axes, so the change between the two spaces is
        // the identity. Pinned so that a future space added by analogy ("Sc2 is
        // surely a yaw off Max") has to argue with the assertion.
        for (i32 i = 0; i < 3; ++i)
            for (i32 j = 0; j < 3; ++j)
                REQUIRE(CoordinateSystem::BasisChange(CoordSpace::Max, CoordSpace::Sc2).data[i][j] ==
                        Approx(i == j ? 1.0f : 0.0f).margin(1e-6));
    }

    SECTION("Forward axes agree with the basis tables") {
        for (CoordSpace s : kSpaces) {
            RequireSameVector(
                CoordinateSystem::ToDefaultDir(s, whiteout::flakes::renderer::ForwardAxis(s)),
                kFwdRef);
        }
    }
}
