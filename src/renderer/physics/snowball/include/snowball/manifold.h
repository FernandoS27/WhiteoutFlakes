//===----------------------------------------------------------------------===//
// snowball/manifold.h -- the nine narrowphase entry points.
//
// Nine, not four: the small-shape cases are each written out rather than routed through one
// generic path. That matters because the entries do **not** agree with each other about
// margins, and those disagreements are deliberate behaviour, not accidents to unify:
//
//   * The triangle/sphere entry ignores the triangle's own radius and applies a hard-coded
//     0.01, while triangle/capsule uses the radius and applies no constant. So a sphere and a
//     capsule resting on the same mesh settle at *different heights*. Keep it that way.
//   * Two fully overlapping parallel capsules produce a **single** contact point where the true
//     contact region is a segment. A two-point manifold would resist rolling about the shared
//     axis; one does not.
//
// Feature ids only have to be *distinct and stable* -- the exact values are a free choice --
// but stability is load-bearing: the id is the key the warm-start matcher carries accumulated
// impulse across on, so an entry that emits the same contacts in a different order warm-starts
// each one from another point's impulse.
//===----------------------------------------------------------------------===//
#pragma once

#include <array>

#include "snowball/common_types.h"
#include "snowball/shape.h"
#include "snowball/transform.h"

namespace snowball {

/// What the triangle/sphere and triangle/polytope entries add regardless of the triangle's own
/// radius. Deliberately not shared with triangle/capsule, which uses the radius instead.
inline constexpr f32 kTriangleContactMargin = 0.01f;

struct ContactPoint {
    Vec4 point{};
    f32 separation{0.0f};
    u32 id{0};
};

struct ContactManifold {
    i32 count{0};
    Vec4 normal{};
    std::array<ContactPoint, 4> points{};
};

/// The polytope SAT's warm axis: which axis won -- or separated -- last time, and at what
/// separation. The update protocol is asymmetric on purpose: **written on every full SAT
/// run, never refreshed on a hit.** A hit re-derives the separation along the cached axis
/// fresh; only drifting more than a linearSlop from the stored value forces the full sweep
/// again. `type` 0 is empty -- the only cold marker; the other fields start stale and mean
/// nothing until type says otherwise.
struct SatCache {
    f32 separation{0.0f};
    u8 indexA{0};
    u8 indexB{0};
    u8 type{0};   ///< 0 empty, 1 faceA, 2 faceB, 3 edge pair
};

ContactManifold CollideSphereSphere(const Sphere& a, const Sphere& b);
ContactManifold CollideCapsuleSphere(const Capsule& a, const Sphere& b);
ContactManifold CollideCapsuleCapsule(const Capsule& a, const Capsule& b);
ContactManifold CollidePolytopeSphere(const Polytope& a, const Sphere& b);
ContactManifold CollidePolytopeCapsule(const Polytope& a, const Capsule& b);

/// With a warm `cache` this runs the same cached-axis fast path as the triangle collider --
/// but with one structural difference: a cached-axis collide that produces no points here
/// ALWAYS falls back to the full sweep. The empty-manifold-no-fallback defect belongs to the
/// triangle path alone (see CollideTrianglePolytope).
ContactManifold CollidePolytopePolytope(const Polytope& a, const Polytope& b,
                                        const Transform& xfB, SatCache* cache = nullptr);

/// Triangles are one-sided: only the side the winding normal points to generates contacts.
ContactManifold CollideTriangleSphere(const Triangle& a, const Sphere& b);
ContactManifold CollideTriangleCapsule(const Triangle& a, const Capsule& b);

/// With a warm `cache` this runs the cached-axis fast path first, and it carries this engine's
/// one DELIBERATE collision defect: a cached **edge** axis whose pair goes degenerate inside
/// the staleness band returns an empty manifold with NO full-SAT fallback. Intentional,
/// preserved for compatibility with the reference engine; adding the fallback diverges from
/// the pinned behaviour. Null cache = always the full sweep, no writes -- the direct
/// narrowphase entry.
///
/// There is deliberately no admissibility check on the winning axis: a generated faceB or edge
/// normal is never refused for pointing into a neighbouring triangle's territory. The
/// adjacency-modified planes still steer the sweep -- and one of them winning yields nothing
/// -- but once an axis generates a manifold, no test second-guesses its direction.
ContactManifold CollideTrianglePolytope(const Triangle& a, const Polytope& b,
                                        const Transform& xfB, SatCache* cache = nullptr);

}  // namespace snowball
