#include "whiteout/flakes/util/coordinate_system.h"

#include <array>
#include <cassert>
#include <cmath>

namespace whiteout::flakes::renderer {

namespace {

struct SpaceAxes {
    Vector3f xAxis;
    Vector3f yAxis;
    Vector3f zAxis;
};

constexpr SpaceAxes kSpaceAxes[] = {

    {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},

    {{0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
};
constexpr usize kSpaceCount = sizeof(kSpaceAxes) / sizeof(kSpaceAxes[0]);

std::array<Matrix44f, kSpaceCount> g_toRef;
std::array<Matrix44f, kSpaceCount> g_fromRef;
std::array<std::array<Matrix44f, kSpaceCount>, kSpaceCount> g_basisChange;
std::array<std::array<f32, kSpaceCount>, kSpaceCount> g_detSign;

Matrix44f MakeToRef(const SpaceAxes& s) {
    Matrix44f m = Matrix44f::identity();
    m.data[0][0] = s.xAxis.x;
    m.data[0][1] = s.xAxis.y;
    m.data[0][2] = s.xAxis.z;
    m.data[1][0] = s.yAxis.x;
    m.data[1][1] = s.yAxis.y;
    m.data[1][2] = s.yAxis.z;
    m.data[2][0] = s.zAxis.x;
    m.data[2][1] = s.zAxis.y;
    m.data[2][2] = s.zAxis.z;
    return m;
}

Matrix44f Transpose3x3(const Matrix44f& m) {
    Matrix44f r = Matrix44f::identity();
    for (i32 i = 0; i < 3; ++i)
        for (i32 j = 0; j < 3; ++j)
            r.data[i][j] = m.data[j][i];
    return r;
}

f32 Det3x3(const Matrix44f& m) {
    return m.data[0][0] * (m.data[1][1] * m.data[2][2] - m.data[1][2] * m.data[2][1]) -
           m.data[0][1] * (m.data[1][0] * m.data[2][2] - m.data[1][2] * m.data[2][0]) +
           m.data[0][2] * (m.data[1][0] * m.data[2][1] - m.data[1][1] * m.data[2][0]);
}

bool InitBasisTables() {
    for (usize s = 0; s < kSpaceCount; ++s) {
        g_toRef[s] = MakeToRef(kSpaceAxes[s]);
        g_fromRef[s] = Transpose3x3(g_toRef[s]);
    }
    for (usize f = 0; f < kSpaceCount; ++f) {
        for (usize t = 0; t < kSpaceCount; ++t) {
            g_basisChange[f][t] = g_toRef[f] * g_fromRef[t];
            f32 d = Det3x3(g_basisChange[f][t]);
            g_detSign[f][t] = (d >= 0.0f) ? 1.0f : -1.0f;
        }
    }
    return true;
}

[[maybe_unused]] const bool g_basisInit = InitBasisTables();

inline usize Ix(CoordSpace s) {
    return static_cast<usize>(s);
}

} // namespace

const Matrix44f& CoordinateSystem::BasisToRef(CoordSpace s) {
    assert(Ix(s) < kSpaceCount);
    return g_toRef[Ix(s)];
}

const Matrix44f& CoordinateSystem::BasisFromRef(CoordSpace s) {
    assert(Ix(s) < kSpaceCount);
    return g_fromRef[Ix(s)];
}

const Matrix44f& CoordinateSystem::BasisChange(CoordSpace from, CoordSpace to) {
    assert(Ix(from) < kSpaceCount && Ix(to) < kSpaceCount);
    return g_basisChange[Ix(from)][Ix(to)];
}

Vector3f CoordinateSystem::ConvertPoint(CoordSpace from, CoordSpace to, Vector3f v) {
    if (from == to)
        return v;
    return whiteout::transform_point(v, g_basisChange[Ix(from)][Ix(to)]);
}

Vector3f CoordinateSystem::ConvertDirection(CoordSpace from, CoordSpace to, Vector3f v) {
    if (from == to)
        return v;
    return whiteout::transform_normal(v, g_basisChange[Ix(from)][Ix(to)]);
}

Vector3f CoordinateSystem::ConvertScale(CoordSpace from, CoordSpace to, Vector3f s) {
    if (from == to)
        return s;
    const Matrix44f& M = g_basisChange[Ix(from)][Ix(to)];

    f32 in[3] = {s.x, s.y, s.z};
    f32 out[3] = {0, 0, 0};
    for (i32 j = 0; j < 3; ++j) {
        for (i32 i = 0; i < 3; ++i) {
            f32 v = M.data[i][j];
            if (v != 0.0f) {
                assert(std::abs(std::abs(v) - 1.0f) < 1e-5f &&
                       "ConvertScale requires axis-aligned basis");
                out[j] = in[i] * std::abs(v);
            }
        }
    }
    return {out[0], out[1], out[2]};
}

Vector4f CoordinateSystem::ConvertTangent(CoordSpace from, CoordSpace to, Vector4f t) {
    if (from == to)
        return t;
    Vector3f xyz = ConvertDirection(from, to, Vector3f{t.x, t.y, t.z});

    f32 w = t.w * g_detSign[Ix(from)][Ix(to)];
    return {xyz.x, xyz.y, xyz.z, w};
}

Quaternion CoordinateSystem::ConvertQuaternion(CoordSpace from, CoordSpace to, Quaternion q) {
    if (from == to)
        return q;

    Vector3f v = ConvertDirection(from, to, Vector3f{q.x, q.y, q.z});
    f32 w = q.w * g_detSign[Ix(from)][Ix(to)];
    return {v.x, v.y, v.z, w};
}

Matrix44f CoordinateSystem::ConvertTransform(CoordSpace from, CoordSpace to, const Matrix44f& M) {
    if (from == to)
        return M;

    return g_basisChange[Ix(to)][Ix(from)] * M * g_basisChange[Ix(from)][Ix(to)];
}

} // namespace whiteout::flakes::renderer
