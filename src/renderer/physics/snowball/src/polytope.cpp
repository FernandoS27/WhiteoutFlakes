#include "snowball/polytope.h"

#include <algorithm>
#include <array>

namespace snowball {
namespace {

/// Accumulated second moments of the whole solid about the origin, plus its volume and the
/// first moment. Every face is fanned into triangles and each triangle closed to the origin as
/// a tetrahedron, so a closed hull sums to the solid regardless of how its faces are shaped.
struct Moments {
    f32 volume{0.0f};
    Vec4 firstMoment{};
    f32 covariance[3][3]{};
};

void AccumulateTetrahedron(Moments& m, const Vec4& a, const Vec4& b, const Vec4& c) {
    // Signed six-volume of the tet (0, a, b, c).
    const f32 det = Dot3(a, Cross3(b, c));
    if (det == 0.0f) {
        return;
    }
    m.volume += det;
    m.firstMoment = m.firstMoment + (a + b + c) * det;

    // C = det * A * Ccanon * A^T with A's columns a, b, c, and Ccanon the canonical tetrahedron
    // covariance (2,1,1 / 1,2,1 / 1,1,2) over 120.
    const std::array<Vec4, 3> col{a, b, c};
    constexpr f32 canon[3][3] = {{2.0f, 1.0f, 1.0f}, {1.0f, 2.0f, 1.0f}, {1.0f, 1.0f, 2.0f}};
    for (i32 i = 0; i < 3; ++i) {
        const f32 ai[3] = {col[0].x, col[1].x, col[2].x};
        const f32 aj[3] = {col[0].y, col[1].y, col[2].y};
        const f32 ak[3] = {col[0].z, col[1].z, col[2].z};
        const f32* rows[3] = {ai, aj, ak};
        for (i32 j = 0; j < 3; ++j) {
            f32 sum = 0.0f;
            for (i32 p = 0; p < 3; ++p) {
                for (i32 q = 0; q < 3; ++q) {
                    sum += canon[p][q] * rows[i][p] * rows[j][q];
                }
            }
            m.covariance[i][j] += det * sum;
        }
    }
}

Moments HullMoments(const Hull& hull) {
    Moments m;
    for (i32 f = 0; f < static_cast<i32>(hull.faces.size()); ++f) {
        const std::vector<i32> loop = hull.FaceVertices(f);
        for (usize k = 1; k + 1 < loop.size(); ++k) {
            AccumulateTetrahedron(m, hull.vertices[static_cast<usize>(loop[0])],
                                  hull.vertices[static_cast<usize>(loop[k])],
                                  hull.vertices[static_cast<usize>(loop[k + 1])]);
        }
    }
    return m;
}

}  // namespace

Polytope MakePolytope(std::span<const Vec4> points, f32 scale) {
    Polytope p;
    p.hull = BuildHull(points);
    ScaleHull(p.hull, scale);
    p.margin = kPolytopeMargin;

    const Moments m = HullMoments(p.hull);
    p.volume = m.volume / 6.0f;
    // First moment is sum(det * (a+b+c)); the tet centroid is (a+b+c)/4, so the division by
    // four and the six from the volume fold into this.
    p.centroid = m.volume != 0.0f ? m.firstMoment * (1.0f / (4.0f * m.volume)) : Vec4{};
    return p;
}

Polytope MakeBox(const Vec4& halfExtents, const Transform& xf, f32 scale) {
    const Vec4 h{std::max(halfExtents.x, kMinBoxHalfExtent),
                 std::max(halfExtents.y, kMinBoxHalfExtent),
                 std::max(halfExtents.z, kMinBoxHalfExtent)};
    const Mtx rotation = RotationMatrix(xf.rotation);

    std::array<Vec4, 8> corners{};
    i32 at = 0;
    for (i32 sx = -1; sx <= 1; sx += 2) {
        for (i32 sy = -1; sy <= 1; sy += 2) {
            for (i32 sz = -1; sz <= 1; sz += 2) {
                const Vec4 corner{h.x * static_cast<f32>(sx), h.y * static_cast<f32>(sy),
                                  h.z * static_cast<f32>(sz)};
                corners[static_cast<usize>(at++)] = rotation.Transform(corner) + xf.position;
            }
        }
    }
    return MakePolytope(corners, scale);
}

f32 Volume(const Polytope& p) { return p.volume; }

MassProperties ComputeMass(const Polytope& p, f32 density) {
    const Moments m = HullMoments(p.hull);

    MassProperties out;
    const f32 volume = m.volume / 6.0f;
    out.mass = density * volume;
    out.centre = m.volume != 0.0f ? m.firstMoment * (1.0f / (4.0f * m.volume)) : Vec4{};
    if (m.volume == 0.0f) {
        return out;
    }

    // The covariance carries det, which is six times the volume, and the canonical matrix
    // divides by 120; together that is the 1/120 the tetrahedron formula asks for.
    f32 c[3][3];
    for (i32 i = 0; i < 3; ++i) {
        for (i32 j = 0; j < 3; ++j) {
            c[i][j] = density * m.covariance[i][j] / 120.0f;
        }
    }

    // I = trace(C) * Identity - C.
    const f32 trace = c[0][0] + c[1][1] + c[2][2];
    out.inertia.c[0] = {trace - c[0][0], -c[0][1], -c[0][2], 0.0f};
    out.inertia.c[1] = {-c[1][0], trace - c[1][1], -c[1][2], 0.0f};
    out.inertia.c[2] = {-c[2][0], -c[2][1], trace - c[2][2], 0.0f};
    return out;
}

}  // namespace snowball
