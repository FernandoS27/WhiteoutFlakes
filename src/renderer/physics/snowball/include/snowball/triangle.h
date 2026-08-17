//===----------------------------------------------------------------------===//
// snowball/triangle.h -- one face of a mesh, as the narrowphase sees it.
//
// A triangle is a shape but never a fixture: it is produced on demand while colliding against
// a mesh or a height field, never authored. It carries a radius like any other shape, and the
// triangle entry points do not agree about whether to use it -- that margin asymmetry is
// deliberate and part of the engine's numeric contract, not something to tidy up.
//
// **A triangle pulled out of a mesh knows its neighbours, and that is load-bearing.** The three
// `adjacent` vertices are the far corners of the triangles across each edge, and `hasAdjacent`
// says whether each one is real. `MakePartialPolytope` turns them into the edge planes the
// SAT runs against, which is the whole internal-edge fix: without them a body sliding across a
// mesh catches on every seam it crosses, because each triangle is free to push along its own
// edge normal into a neighbour that is really there.
//
// The identity fields are diagnostic: nothing in the collision path reads them. They let a
// test check that a height field numbered its neighbours correctly at the border, where the
// clamped positions alone cannot tell a correct answer from a plausible one.
//===----------------------------------------------------------------------===//
#pragma once

#include <array>

#include "snowball/common_types.h"
#include "snowball/vec.h"

namespace snowball {

struct Triangle {
    Vec4 v1{};
    Vec4 v2{};
    Vec4 v3{};
    f32 radius{0.0f};

    /// The far vertex of the triangle across edge k, where edge 0 is v1->v2, 1 is v2->v3 and 2
    /// is v3->v1. Only meaningful where the matching `hasAdjacent` is set.
    std::array<Vec4, 3> adjacent{};
    std::array<bool, 3> hasAdjacent{};

    /// Stable per-mesh identities: the three corners, then the three neighbours. Diagnostic
    /// only -- the collider never reads them.
    std::array<i32, 3> vertexId{{-1, -1, -1}};
    std::array<i32, 3> adjacentId{{-1, -1, -1}};

    /// Index within the mesh that produced it, and the high half of every contact id the mesh
    /// path emits -- which is what keeps two neighbouring triangles' features from colliding in
    /// the warm-start table.
    i32 index{-1};

    u16 material{0};

    /// @brief The outward face normal, by the winding (v2-v1) x (v3-v1). Not normalised.
    ///
    /// Triangles are one-sided: only the side this points to generates contacts.
    Vec4 RawNormal() const { return Cross3(v2 - v1, v3 - v1); }
};

constexpr Vec4 Centroid(const Triangle& t) {
    return (t.v1 + t.v2 + t.v3) * constants::kOneThird.x;
}

}  // namespace snowball
