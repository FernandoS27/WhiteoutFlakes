#include "snowball/partial_polytope.h"

#include "snowball/math.h"

namespace snowball {
namespace {

/// Unit vector through the engine's reciprocal square root, which every plane normal here goes
/// through rather than a true divide -- the same bits as every other normalisation in the engine.
Vec4 Normalize(const Vec4& v) { return v * Rsqrt(Vec4::Splat(LengthSquared3(v))).x; }

Plane PlaneThrough(const Vec4& normal, const Vec4& point) {
    return Plane{normal, Dot3(point, normal)};
}

/// The edge plane for edge `tail -> tail + edge`, given the neighbour across it.
///
/// The ordering of the tests is the point: the convex case is decided on the neighbour's height
/// above the *face*, not on the edge geometry, so a fold flat to within an epsilon is treated as
/// interior rather than as a silhouette.
Plane EdgePlane(const Vec4& faceNormal, const Vec4& tail, const Vec4& edge, const Vec4& neighbour,
                bool hasNeighbour) {
    if (!hasNeighbour) {
        // Reversed: nothing can be outside it, so an open edge constrains nothing at all.
        return PlaneThrough(-faceNormal, tail);
    }
    const Vec4 arm = neighbour - tail;
    if (Dot3(arm, faceNormal) > kAdjacencyEpsilon) {
        return PlaneThrough(faceNormal, tail);
    }
    return PlaneThrough(Normalize(Cross3(arm, edge)), tail);
}

}  // namespace

PartialPolytope MakePartialPolytope(const Triangle& t) {
    PartialPolytope out;
    out.v1 = t.v1;
    out.v2 = t.v2;
    out.v3 = t.v3;

    const Vec4 e0 = t.v2 - t.v1;
    const Vec4 e1 = t.v3 - t.v2;
    const Vec4 e2 = t.v1 - t.v3;
    const Vec4 normal = Normalize(Cross3(e0, e1));

    out.planes[0] = PlaneThrough(normal, t.v1);
    out.planes[1] = EdgePlane(normal, t.v1, e0, t.adjacent[0], t.hasAdjacent[0]);
    out.planes[2] = EdgePlane(normal, t.v2, e1, t.adjacent[1], t.hasAdjacent[1]);
    out.planes[3] = EdgePlane(normal, t.v3, e2, t.adjacent[2], t.hasAdjacent[2]);
    out.centroid = (t.v1 + t.v2 + t.v3) * constants::kOneThird.x;
    return out;
}

bool CheckAxis(const PartialPolytope& p, const Vec4& axis) {
    const Vec4& faceNormal = p.planes[0].normal;
    if (Dot3(faceNormal, axis) > kAxisFaceCosine) {
        return true;
    }
    // Which edge's region the axis falls in, read off the fan around the centroid. The two tests
    // are ordered, not independent: the second only runs when the first has already ruled out
    // edge 0, so the third is a fallthrough rather than a test of its own.
    const f32 side1 = Dot3(Cross3(p.v1 - p.centroid, axis), faceNormal);
    const f32 side2 = Dot3(Cross3(p.v2 - p.centroid, axis), faceNormal);
    if (side1 >= 0.0f && side2 < 0.0f) {
        return Dot3(p.v2 - p.v1, Cross3(axis, p.planes[1].normal)) >= 0.0f;
    }
    if (side2 >= 0.0f) {
        const f32 side3 = Dot3(Cross3(p.v3 - p.centroid, axis), faceNormal);
        if (side3 < 0.0f) {
            return Dot3(p.v3 - p.v2, Cross3(axis, p.planes[2].normal)) >= 0.0f;
        }
    }
    return Dot3(p.v1 - p.v3, Cross3(axis, p.planes[3].normal)) >= 0.0f;
}

}  // namespace snowball
