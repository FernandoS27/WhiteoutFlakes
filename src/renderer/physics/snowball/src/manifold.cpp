#include "snowball/manifold.h"

#include "snowball/partial_polytope.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "snowball/distance.h"

namespace snowball {
namespace {

constexpr f32 kEpsilon = 1.1920929e-07f;

u32 MakeId(i32 indexA, i32 indexB, i32 typeA, i32 typeB) {
    return static_cast<u32>((indexA & 0xFF) | ((indexB & 0xFF) << 8) |
                            ((typeA & 0xFF) << 16) | ((typeB & 0xFF) << 24));
}

f32 Length3(const Vec4& v) {
    const f32 lengthSquared = LengthSquared3(v);
    return lengthSquared > 0.0f ? lengthSquared * Rsqrt(lengthSquared) : 0.0f;
}

Vec4 Normalize3(const Vec4& v) {
    const f32 lengthSquared = LengthSquared3(v);
    return lengthSquared > kEpsilon ? v * Rsqrt(lengthSquared) : Vec4{};
}

Vec4 ToWorld(const Transform& xf, const Vec4& p) {
    return RotationMatrix(xf.rotation).Transform(p) + xf.position;
}

/// The shared shape of every "two round things touch" entry: a witness pair, their radii, and
/// a single contact point placed midway between the two surfaces.
ContactManifold RoundPair(const Vec4& onA, const Vec4& onB, f32 radiusA, f32 radiusB, u32 id) {
    ContactManifold m;
    const Vec4 delta = onB - onA;
    const f32 d = Length3(delta);
    const f32 separation = d - (radiusA + radiusB);
    if (separation > 0.0f) {
        return m;  // separated: no point at all, rather than a point with positive separation
    }
    m.normal = d > kEpsilon ? delta * Rsqrt(LengthSquared3(delta)) : constants::kUnitY;
    m.count = 1;
    m.points[0].point = onA + m.normal * (radiusA + separation * 0.5f);
    m.points[0].separation = separation;
    m.points[0].id = id;
    return m;
}

/// Closest point on a convex proxy to another, via the distance solver.
struct Witness {
    Vec4 onA{};
    Vec4 onB{};
    f32 distance{0.0f};
    Vec4 normal{};
};

Witness ClosestPoints(const Shape& a, const Shape& b, const Transform& xfB = {}) {
    DistanceInput input;
    input.proxyA = SupportProxy(a);
    input.proxyB = SupportProxy(b);
    input.xfB = xfB;
    GjkCache cache;
    const DistanceOutput out = Distance(input, cache);
    return Witness{out.pointA, out.pointB, out.distance, out.normal};
}

// -- face queries and clipping -------------------------------------------------------------

struct Face {
    Vec4 normal{};
    f32 offset{0.0f};
    std::vector<Vec4> vertices;
};

Face FaceOf(const Polytope& p, i32 index, const Transform& xf) {
    Face face;
    const Mtx rotation = RotationMatrix(xf.rotation);
    face.normal = rotation.Transform(p.hull.faces[static_cast<usize>(index)].normal);
    for (const i32 v : p.hull.FaceVertices(index)) {
        face.vertices.push_back(ToWorld(xf, p.hull.vertices[static_cast<usize>(v)]));
    }
    face.offset = Dot3(face.normal, face.vertices.front());
    return face;
}

Face TriangleFace(const Triangle& t) {
    Face face;
    face.normal = Normalize3(t.RawNormal());
    face.vertices = {t.v1, t.v2, t.v3};
    face.offset = Dot3(face.normal, t.v1);
    return face;
}

/// A polygon vertex as it travels through the clipper, carrying where it came from.
///
/// The provenance is not bookkeeping. It becomes the contact's feature id, and that id is the
/// only handle warm starting has on a point from one step to the next. Number the points by
/// their output slot instead and a manifold silently re-matches the moment one corner leaves
/// the clip: every remaining point inherits its neighbour's accumulated impulse, which is not
/// a crash and not a visible glitch, just a contact that stops holding.
struct ClipVertex {
    Vec4 point{};
    i32 vertex{-1};   ///< incident vertex index, or -1 for a point the clip invented
    i32 feature{0};   ///< an invented point's identity: source segment, and the plane that cut it
    i32 edge{0};      ///< what the segment LEAVING this point runs along -- see the key below
};

/// Segment keys: incident edge `i` is `i`, and a segment lying along reference edge `e` is
/// `8 + e`. Four bits each, so a face of more than eight vertices would alias -- a box face has
/// four and authored collision hulls stay far below it.
constexpr i32 kReferenceEdgeKey = 8;

/// Clip a polygon against the side planes of `reference`, each pushed out by `margin`.
///
/// The margin is not cosmetic: a contact point may legitimately sit outside the reference face
/// by the two shapes' skins, and clipping to the bare face would drop it. A box corner hanging
/// past a triangle's hypotenuse therefore comes back up to `marginSum` beyond the edge plane
/// rather than being pulled onto it.
std::vector<ClipVertex> ClipToFace(const std::vector<ClipVertex>& polygon, const Face& reference,
                                   f32 margin) {
    std::vector<ClipVertex> clipped = polygon;
    const auto& ref = reference.vertices;
    for (usize i = 0; i < ref.size() && !clipped.empty(); ++i) {
        const Vec4& a = ref[i];
        const Vec4& b = ref[(i + 1) % ref.size()];
        const Vec4 side = Normalize3(Cross3(b - a, reference.normal));
        if (LengthSquared3(side) <= kEpsilon) {
            continue;
        }
        const f32 limit = Dot3(side, a) + margin;
        const i32 refEdge = static_cast<i32>(i) & 0x07;

        std::vector<ClipVertex> next;
        next.reserve(clipped.size() + 1);
        for (usize k = 0; k < clipped.size(); ++k) {
            const ClipVertex& p = clipped[k];
            const ClipVertex& q = clipped[(k + 1) % clipped.size()];
            const f32 dp = Dot3(side, p.point) - limit;
            const f32 dq = Dot3(side, q.point) - limit;
            if (dp <= 0.0f) {
                next.push_back(p);
            }
            if ((dp < 0.0f && dq > 0.0f) || (dp > 0.0f && dq < 0.0f)) {
                const bool entering = dp > 0.0f;
                ClipVertex crossing;
                crossing.point = p.point + (q.point - p.point) * (dp / (dp - dq));
                // Named by the segment it cuts and the plane that cut it. Two crossings of one
                // reference plane always lie on different incident segments, and one segment cut
                // twice is cut by different planes, so the pair is unique either way.
                crossing.feature = (p.edge & 0x0F) | ((refEdge & 0x0F) << 4);
                // Leaving an entry point the boundary resumes the segment it interrupted;
                // leaving an exit point it runs along the plane just clipped against.
                crossing.edge = entering ? p.edge : (kReferenceEdgeKey + refEdge);
                next.push_back(crossing);
            }
        }
        clipped.swap(next);
    }
    return clipped;
}

/// The incident polygon as the clipper wants it: every vertex still its own feature.
std::vector<ClipVertex> ToClipPolygon(const std::vector<Vec4>& points) {
    std::vector<ClipVertex> out;
    out.reserve(points.size());
    for (usize i = 0; i < points.size(); ++i) {
        out.push_back(ClipVertex{points[i], static_cast<i32>(i), 0, static_cast<i32>(i) & 0x0F});
    }
    return out;
}

/// The feature id a clipped point carries out of the narrowphase.
///
/// The *values* are a free choice; what is bound is that a point keeps the same one for as
/// long as it is the same contact.
u32 ClipId(const ClipVertex& v, i32 referenceIndex, i32 typeB) {
    return v.vertex >= 0 ? MakeId(referenceIndex, v.vertex, 0, typeB)
                         : MakeId(referenceIndex, v.feature, 1, typeB);
}

/// A clipped point that survived the separation test, with where it came from.
struct Candidate {
    Vec4 point{};
    f32 separation{0.0f};
    ClipVertex source{};
};

/// Keep at most four points.
void ReduceTo4(std::vector<Candidate>& points) {
    if (points.size() <= 4) {
        return;
    }
    // A manifold holds four points and a clip can produce more, so some must go. Which ones is
    // a free choice that nonetheless changes how a box sits on a mesh seam, so it is made
    // deliberately: keep the incident face's own corners first and drop the points the clip
    // invented, since a corner carries a feature identity that survives to the next step while
    // an interpolated point does not.
    std::vector<usize> keep;
    for (i32 pass = 0; pass < 2 && keep.size() < 4; ++pass) {
        const bool wantCorner = pass == 0;
        // Corners in order; the points the clip invented are taken from the back, so on a
        // five-point clip it is the last-made crossing that survives.
        for (usize k = 0; k < points.size() && keep.size() < 4; ++k) {
            const usize i = wantCorner ? k : points.size() - 1 - k;
            if ((points[i].source.vertex >= 0) == wantCorner) {
                keep.push_back(i);
            }
        }
    }
    std::sort(keep.begin(), keep.end());

    std::vector<Candidate> kept;
    kept.reserve(keep.size());
    for (const usize i : keep) {
        kept.push_back(points[i]);
    }
    points.swap(kept);
}

/// Build a manifold from an incident polygon clipped onto a reference face.
ContactManifold FromClippedFace(const Face& reference, const std::vector<Vec4>& incident,
                                f32 marginSum, bool flipNormal, i32 referenceIndex) {
    ContactManifold m;
    const std::vector<ClipVertex> clipped =
        ClipToFace(ToClipPolygon(incident), reference, marginSum);
    std::vector<Candidate> points;
    for (const ClipVertex& v : clipped) {
        const f32 depth = Dot3(reference.normal, v.point) - reference.offset;
        const f32 separation = depth - marginSum;
        if (separation > 0.0f) {
            continue;
        }
        // Place the point on the reference plane rather than where the incident face happens
        // to be -- every face-clipped manifold, box/box and triangle/box alike, sits there.
        points.push_back(Candidate{v.point - reference.normal * depth, separation, v});
    }
    if (points.empty()) {
        return m;
    }
    ReduceTo4(points);

    m.normal = flipNormal ? -reference.normal : reference.normal;
    m.count = static_cast<i32>(points.size());
    for (i32 i = 0; i < m.count; ++i) {
        const Candidate& c = points[static_cast<usize>(i)];
        m.points[static_cast<usize>(i)].point = c.point;
        m.points[static_cast<usize>(i)].separation = c.separation;
        m.points[static_cast<usize>(i)].id = ClipId(c.source, referenceIndex, 2);
    }
    return m;
}

/// How far a face plane holds a point set off: the deepest any of them reaches past it.
f32 FaceSeparation(const Face& face, const std::vector<Vec4>& points) {
    f32 separation = constants::kMaxFloat.x;
    for (const Vec4& q : points) {
        separation = std::min(separation, Dot3(face.normal, q) - face.offset);
    }
    return separation;
}

/// The polytope face that separates best from a point set, and by how much.
struct FaceQuery {
    i32 index{-1};
    f32 separation{-constants::kMaxFloat.x};
};

FaceQuery BestFace(const Polytope& p, const Transform& xf, const std::vector<Vec4>& points) {
    FaceQuery best;
    for (i32 f = 0; f < static_cast<i32>(p.hull.faces.size()); ++f) {
        const f32 separation = FaceSeparation(FaceOf(p, f, xf), points);
        if (separation > best.separation) {
            best.separation = separation;
            best.index = f;
        }
    }
    return best;
}

// -- the polytope/polytope separating-axis sweep ---------------------------------------------

/// A candidate separating axis. `indexA`/`indexB` name faces for the two face kinds and edges
/// for the edge kind -- the same two fields, read differently depending on `kind`.
struct SatAxis {
    enum Kind { kNone = 0, kFaceA = 1, kFaceB = 2, kEdge = 3 };
    f32 separation{-constants::kMaxFloat.x};
    i32 indexA{-1};
    i32 indexB{-1};
    i32 kind{kNone};
};

/// What B's faces must beat the incumbent by before the reference frame moves across, and what
/// the edge axis must beat the winning face by before it takes over.
///
/// Both are hysteresis rather than geometry. A box resting square on another separates by the
/// same amount along either shape's face to the last bit, and an unbiased comparison lets the
/// reference flip from step to step -- which renames every feature id the warm-start matcher
/// keys on, so the accumulated impulses reset and the stack sags.
constexpr f32 kFaceBias = 0.98f;
constexpr f32 kEdgeBias = 0.9f;

/// A hull edge as the SAT wants it: one of each twin pair, with the two faces meeting along it.
struct SatEdge {
    i32 index{0};
    Vec4 tail{};
    Vec4 dir{};       ///< head - tail, unnormalised
    Vec4 normal1{};   ///< the face this half-edge winds around
    Vec4 normal2{};   ///< and the one its twin does
};

/// Which half of each twin pair is taken is a free choice: the twin negates the direction and
/// swaps the two normals at the same time, and the Minkowski-face test below is invariant under
/// exactly that pair of flips. The convention here is that twins pair by endpoints and the
/// lower-indexed half-edge is the one kept.
std::vector<SatEdge> SatEdges(const Polytope& p) {
    std::vector<SatEdge> out;
    for (i32 e = 0; e < static_cast<i32>(p.hull.edges.size()); ++e) {
        const HalfEdge& edge = p.hull.edges[static_cast<usize>(e)];
        if (edge.twin < e) {
            continue;  // already taken, or (twin == -1) an open hull with no pair to take
        }
        const Vec4& tail = p.hull.vertices[static_cast<usize>(edge.origin)];
        const HalfEdge& next = p.hull.edges[static_cast<usize>(edge.next)];
        const HalfEdge& twin = p.hull.edges[static_cast<usize>(edge.twin)];
        out.push_back(SatEdge{e, tail,
                              p.hull.vertices[static_cast<usize>(next.origin)] - tail,
                              p.hull.faces[static_cast<usize>(edge.face)].normal,
                              p.hull.faces[static_cast<usize>(twin.face)].normal});
    }
    return out;
}

/// Gregorius's Gauss-map test: two edges only build a face of the Minkowski difference -- and so
/// only carry a candidate separating axis -- if the arcs they sweep on the unit sphere cross.
///
/// Skipping it is not merely slower. Every non-crossing pair still has a cross product, and one
/// of those will out-separate the real axis on a shape of any complexity, leaving a contact
/// normal that points along no surface of either hull.
bool IsMinkowskiFace(f32 d1, f32 d2, f32 d3, f32 d4) {
    // Three sign tests written as one on purpose -- the form also settles the NaN case: a
    // max involving a NaN is not less than zero, so the pair is skipped.
    return std::max(d1 * d4, std::max(d2 * d1, d3 * d4)) < 0.0f;
}

/// One edge pair's candidate axis and separation -- the body of the sweep below, shared with
/// the cached-axis fast path. The safe normalise zeroes a degenerate axis rather than
/// skipping, which makes the separation come out at `-marginSum` -- the sweep never sees that
/// (it skips the pair first), the cached path does.
SatAxis TestEdgePair(const Vec4& centroidA, const SatEdge& ea, i32 ia, const SatEdge& eb,
                     i32 ib, const Polytope& b, const Transform& xfB, f32 marginSum) {
    const Mtx rotation = RotationMatrix(xfB.rotation);
    const Vec4 dirB = rotation.Transform(eb.dir);
    const Vec4 tailB = ToWorld(xfB, eb.tail);
    const Vec4 armB = rotation.Transform(eb.tail - b.centroid);
    const Vec4 cross = Cross3(ea.dir, dirB);
    const f32 lengthSquared = LengthSquared3(cross);
    Vec4 axis = lengthSquared > constants::kZeroSafe.x ? cross * Rsqrt(lengthSquared) : Vec4{};
    // Turn it to point out of A. Which centroid decides is not fixed: whichever gives
    // the longer arm along the axis is the better-conditioned cue, and an edge running
    // close to its own hull's centre gives almost no signal at all.
    const f32 armAlongA = Dot3(ea.tail - centroidA, axis);
    const f32 armAlongB = Dot3(armB, axis);
    const bool flip = std::abs(armAlongB) < std::abs(armAlongA) ? armAlongA < 0.0f
                                                                : armAlongB > 0.0f;
    if (flip) {
        axis = -axis;
    }
    return SatAxis{Dot3(tailB - ea.tail, axis) - marginSum, ia, ib, SatAxis::kEdge};
}

/// The best edge-pair axis, or a `kNone` axis if no pair of edges builds a Minkowski face.
///
/// **Returns the first separating pair it meets**, not the widest -- the sweep bails the
/// moment any axis proves separation, and which pair that is lands in the SAT cache and
/// steers the next step's fast path. Under no separation the sweep runs to the end with a
/// plain `>` and no hysteresis.
SatAxis TestEdges(const Vec4& centroidA, const std::vector<SatEdge>& edgesA, const Polytope& b,
                  const std::vector<SatEdge>& edgesB, const Transform& xfB, f32 marginSum) {
    SatAxis best;
    const Mtx rotation = RotationMatrix(xfB.rotation);
    for (usize ib = 0; ib < edgesB.size(); ++ib) {
        const SatEdge& eb = edgesB[ib];
        const Vec4 dirB = rotation.Transform(eb.dir);
        const Vec4 normalB1 = rotation.Transform(eb.normal1);
        const Vec4 normalB2 = rotation.Transform(eb.normal2);
        const Vec4 tailB = ToWorld(xfB, eb.tail);
        const Vec4 armB = rotation.Transform(eb.tail - b.centroid);
        for (usize ia = 0; ia < edgesA.size(); ++ia) {
            const SatEdge& ea = edgesA[ia];
            if (!IsMinkowskiFace(Dot3(normalB1, ea.dir), Dot3(normalB2, ea.dir),
                                 Dot3(ea.normal1, dirB), Dot3(ea.normal2, dirB))) {
                continue;
            }
            const Vec4 cross = Cross3(ea.dir, dirB);
            const f32 lengthSquared = LengthSquared3(cross);
            if (lengthSquared <= constants::kZeroSafe.x) {
                continue;  // parallel edges span no axis, and their faces already cover the pair
            }
            Vec4 axis = cross * Rsqrt(lengthSquared);
            const f32 armAlongA = Dot3(ea.tail - centroidA, axis);
            const f32 armAlongB = Dot3(armB, axis);
            const bool flip = std::abs(armAlongB) < std::abs(armAlongA) ? armAlongA < 0.0f
                                                                       : armAlongB > 0.0f;
            if (flip) {
                axis = -axis;
            }
            const f32 separation = Dot3(tailB - ea.tail, axis) - marginSum;
            if (separation > 0.0f) {
                return SatAxis{separation, static_cast<i32>(ia), static_cast<i32>(ib),
                               SatAxis::kEdge};
            }
            if (separation > best.separation) {
                best.separation = separation;
                best.indexA = static_cast<i32>(ia);
                best.indexB = static_cast<i32>(ib);
                best.kind = SatAxis::kEdge;
            }
        }
    }
    return best;
}

/// The one-point manifold an edge-pair axis produces.
///
/// One point, not a clipped patch: two crossing edges meet at a point, and a manifold that
/// invented a second would resist a rotation the real contact cannot.
ContactManifold CollideEdges(const Vec4& centroidA, const SatEdge& ea, const SatEdge& eb,
                             const Transform& xfB, f32 marginSum) {
    ContactManifold m;
    const Vec4 headA = ea.tail + ea.dir;
    const Vec4 tailB = ToWorld(xfB, eb.tail);
    const Vec4 headB = ToWorld(xfB, eb.tail + eb.dir);

    const Vec4 cross = Cross3(headA - ea.tail, headB - tailB);
    const f32 lengthSquared = LengthSquared3(cross);
    if (lengthSquared <= constants::kZeroSafe.x) {
        return m;
    }
    Vec4 axis = cross * Rsqrt(lengthSquared);
    // Only A's centroid decides the sign here, where the sweep that chose this axis weighed
    // both. The disagreement is reachable and intentional; it stays to match the reference
    // engine.
    if (Dot3(ea.tail - centroidA, axis) < 0.0f) {
        axis = -axis;
    }

    f32 s = 0.0f;
    f32 t = 0.0f;
    ClosestPointsBetweenSegments(ea.tail, headA, tailB, headB, s, t);
    const Vec4 onA = ea.tail + (headA - ea.tail) * s;
    const Vec4 onB = tailB + (headB - tailB) * t;

    m.count = 1;
    m.normal = axis;
    m.points[0].point = (onA + onB) * 0.5f;
    m.points[0].separation = Dot3(onB - onA, axis) - marginSum;
    m.points[0].id = MakeId(ea.index, eb.index, 1, 1);
    return m;
}

/// The polytope pair's cached-edge collide: `CollideEdges` plus the warm path's consistency
/// test, and nothing else -- no adjacency to check, and every empty outcome (degenerate cross
/// or inconsistency) simply reads as "no points" and sends the caller to the full sweep. The
/// test compares `|separation|` against half the SQUARED witness distance -- a DELIBERATE
/// dimensional mismatch, shared with the triangle path and preserved for compatibility with
/// the reference engine; "fixing" the units would move the fallback threshold and diverge
/// from the pinned behaviour.
ContactManifold CollideEdgesConsistent(const Vec4& centroidA, const SatEdge& ea,
                                       const SatEdge& eb, const Transform& xfB, f32 marginSum,
                                       f32 pairSeparation) {
    ContactManifold m;
    const Vec4 headA = ea.tail + ea.dir;
    const Vec4 tailB = ToWorld(xfB, eb.tail);
    const Vec4 headB = ToWorld(xfB, eb.tail + eb.dir);

    f32 s = 0.0f;
    f32 t = 0.0f;
    ClosestPointsBetweenSegments(ea.tail, headA, tailB, headB, s, t);
    const Vec4 onA = ea.tail + (headA - ea.tail) * s;
    const Vec4 onB = tailB + (headB - tailB) * t;
    if (std::abs(pairSeparation + marginSum) < 0.5f * LengthSquared3(onB - onA)) {
        return m;
    }

    const Vec4 cross = Cross3(headA - ea.tail, headB - tailB);
    const f32 lengthSquared = LengthSquared3(cross);
    if (lengthSquared <= constants::kZeroSafe.x) {
        return m;
    }
    Vec4 axis = cross * Rsqrt(lengthSquared);
    if (Dot3(ea.tail - centroidA, axis) < 0.0f) {
        axis = -axis;
    }

    m.count = 1;
    m.normal = axis;
    m.points[0].point = (onA + onB) * 0.5f;
    m.points[0].separation = Dot3(onB - onA, axis) - marginSum;
    m.points[0].id = MakeId(ea.index, eb.index, 1, 1);
    return m;
}

enum class CachedEdge { Abort, Fallback, Hit };

/// The triangle path's cached-edge collide -- the same construction as above plus the three
/// exits only the warm path has. `Abort` is the load-bearing one: an empty manifold with
/// **no full-SAT fallback**. That is this engine's one DELIBERATE collision defect, preserved
/// for compatibility with the reference engine -- adding the fallback would diverge from the
/// pinned behaviour. With no adjacency refusal in this entry, a degenerate cross product is
/// the only abort left. The consistency test really does compare a length against half a
/// squared length -- the dimensional mismatch is equally DELIBERATE, and it decides which
/// side of the fallback threshold small-scale geometry lands on.
CachedEdge CollideEdgesCached(ContactManifold& m, const PartialPolytope& pp, const SatEdge& ea,
                              const SatEdge& eb, const Transform& xfB, f32 marginSum,
                              f32 pairSeparation) {
    const Vec4 headA = ea.tail + ea.dir;
    const Vec4 tailB = ToWorld(xfB, eb.tail);
    const Vec4 headB = ToWorld(xfB, eb.tail + eb.dir);

    const Vec4 cross = Cross3(headA - ea.tail, headB - tailB);
    const f32 lengthSquared = LengthSquared3(cross);
    if (lengthSquared <= constants::kZeroSafe.x) {
        return CachedEdge::Abort;
    }
    Vec4 axis = cross * Rsqrt(lengthSquared);
    if (Dot3(ea.tail - pp.centroid, axis) < 0.0f) {
        axis = -axis;
    }

    f32 s = 0.0f;
    f32 t = 0.0f;
    ClosestPointsBetweenSegments(ea.tail, headA, tailB, headB, s, t);
    const Vec4 onA = ea.tail + (headA - ea.tail) * s;
    const Vec4 onB = tailB + (headB - tailB) * t;

    // |raw separation along the pair's axis| against half the SQUARED witness distance.
    if (std::abs(pairSeparation + marginSum) < 0.5f * LengthSquared3(onB - onA)) {
        return CachedEdge::Fallback;
    }

    m.count = 1;
    m.normal = axis;
    m.points[0].point = (onA + onB) * 0.5f;
    m.points[0].separation = Dot3(onB - onA, axis) - marginSum;
    m.points[0].id = MakeId(ea.index, eb.index, 1, 1);
    return CachedEdge::Hit;
}

/// Clip a capsule's segment onto a reference face, dropping ends that are not in contact.
ContactManifold CapsuleOnFace(const Face& reference, const Capsule& capsule, f32 marginSum,
                              i32 referenceIndex) {
    ContactManifold m;
    // Clipped to the BARE face, with no margin expansion: a capsule lying across a box face
    // comes back clipped exactly at the face's own edges. The face-face entries expand their
    // clip planes by both skins instead (FromClippedFace) -- another deliberate entry-by-entry
    // disagreement, not one to unify.
    const std::vector<ClipVertex> clipped =
        ClipToFace(ToClipPolygon({capsule.p1, capsule.p2}), reference, 0.0f);
    // ClipToFace treats its input as a closed loop, which for a two-point segment walks it
    // there and back; the duplicates are harmless and removed here.
    std::vector<ClipVertex> unique;
    for (const ClipVertex& v : clipped) {
        const bool seen = std::any_of(unique.begin(), unique.end(), [&](const ClipVertex& other) {
            return LengthSquared3(v.point - other.point) <= 1e-10f;
        });
        if (!seen) {
            unique.push_back(v);
        }
    }

    std::reverse(unique.begin(), unique.end());
    for (const ClipVertex& v : unique) {
        const f32 separation = Dot3(reference.normal, v.point) - reference.offset - marginSum;
        if (separation > 0.0f) {
            continue;  // this end of the capsule is not in contact
        }
        if (m.count == 4) {
            break;
        }
        ContactPoint& point = m.points[static_cast<usize>(m.count)];
        // The point sits one capsule radius below the segment, not one margin
        // sum -- the reference shape's own skin does not displace it.
        point.point = v.point - reference.normal * capsule.radius;
        point.separation = separation;
        point.id = ClipId(v, referenceIndex, 1);
        ++m.count;
    }
    if (m.count > 0) {
        m.normal = reference.normal;
    }
    return m;
}

}  // namespace

// -- round pairs ---------------------------------------------------------------------------

ContactManifold CollideSphereSphere(const Sphere& a, const Sphere& b) {
    return RoundPair(a.centre, b.centre, a.radius, b.radius, MakeId(0, 0, 0, 0));
}

ContactManifold CollideCapsuleSphere(const Capsule& a, const Sphere& b) {
    const f32 t = ClosestPointOnSegment(a.p1, a.p2, b.centre);
    return RoundPair(a.p1 + (a.p2 - a.p1) * t, b.centre, a.radius, b.radius, MakeId(0, 0, 0, 0));
}

ContactManifold CollideCapsuleCapsule(const Capsule& a, const Capsule& b) {
    f32 s = 0.0f;
    f32 t = 0.0f;
    ClosestPointsBetweenSegments(a.p1, a.p2, b.p1, b.p2, s, t);
    return RoundPair(a.p1 + (a.p2 - a.p1) * s, b.p1 + (b.p2 - b.p1) * t, a.radius, b.radius,
                     MakeId(0, 0, 0, 0));
}

// -- polytope pairs ------------------------------------------------------------------------

ContactManifold CollidePolytopeSphere(const Polytope& a, const Sphere& b) {
    const Witness w = ClosestPoints(a, b);
    ContactManifold m;
    const f32 separation = w.distance - (a.margin + b.radius);
    if (separation > 0.0f) {
        return m;
    }
    m.normal = w.normal;
    m.count = 1;
    // The contact sits the full skin depth below the sphere's centre -- not midway between
    // the two surfaces the way the round-pair entries place theirs.
    m.points[0].point = b.centre - w.normal * (a.margin + b.radius);
    m.points[0].separation = separation;
    m.points[0].id = 0;  // there is no feature to name on a sphere
    return m;
}

ContactManifold CollidePolytopeCapsule(const Polytope& a, const Capsule& b) {
    const FaceQuery query = BestFace(a, Transform{}, {b.p1, b.p2});
    const f32 marginSum = a.margin + b.radius;
    if (query.index < 0 || query.separation - marginSum > 0.0f) {
        return {};
    }
    return CapsuleOnFace(FaceOf(a, query.index, Transform{}), b, marginSum, query.index);
}

ContactManifold CollidePolytopePolytope(const Polytope& a, const Polytope& b,
                                        const Transform& xfB, SatCache* cache) {
    std::vector<Vec4> bVertices;
    for (const Vec4& v : b.hull.vertices) {
        bVertices.push_back(ToWorld(xfB, v));
    }
    const std::vector<Vec4> aVertices(a.hull.vertices.begin(), a.hull.vertices.end());
    const f32 marginSum = a.margin + b.margin;
    const f32 halfSlop = 0.5f * constants::kLinearSlop.x;

    // Margined face separations with the supporting opposite-side vertex tracked -- the cache
    // stores the pair, and a fresh support is what every cached-axis revisit measures.
    const auto faceASep = [&](i32 f, i32& support) {
        const Face face = FaceOf(a, f, Transform{});
        f32 separation = constants::kMaxFloat.x;
        support = 0;
        for (usize i = 0; i < bVertices.size(); ++i) {
            const f32 d = Dot3(face.normal, bVertices[i]) - face.offset;
            if (d < separation) {
                separation = d;
                support = static_cast<i32>(i);
            }
        }
        return separation - marginSum;
    };
    const auto faceBSep = [&](i32 f, i32& support) {
        const Face face = FaceOf(b, f, xfB);
        f32 separation = constants::kMaxFloat.x;
        support = 0;
        for (usize i = 0; i < aVertices.size(); ++i) {
            const f32 d = Dot3(face.normal, aVertices[i]) - face.offset;
            if (d < separation) {
                separation = d;
                support = static_cast<i32>(i);
            }
        }
        return separation - marginSum;
    };
    // The clipped face manifold for either reference side; the incident face is the one facing
    // most directly back at the reference.
    const auto faceManifold = [&](bool useA, i32 refIndex) {
        const Polytope& incidentShape = useA ? b : a;
        const Transform incXf = useA ? xfB : Transform{};
        const Face refFace = FaceOf(useA ? a : b, refIndex, useA ? Transform{} : xfB);
        i32 incidentIndex = 0;
        f32 bestDot = constants::kMaxFloat.x;
        for (i32 f = 0; f < static_cast<i32>(incidentShape.hull.faces.size()); ++f) {
            const f32 d = Dot3(FaceOf(incidentShape, f, incXf).normal, refFace.normal);
            if (d < bestDot) {
                bestDot = d;
                incidentIndex = f;
            }
        }
        return FromClippedFace(refFace,
                               FaceOf(incidentShape, incidentIndex, incXf).vertices,
                               marginSum, !useA, refIndex);
    };

    // -- the cached-axis fast path. Same protocol as the triangle collider, one structural
    // difference: a cached collide that makes no points here ALWAYS falls through to the full
    // sweep -- there is no abort exit on this pair. ------------------------------------------
    if (cache != nullptr && cache->type != 0) {
        constexpr f32 kStale = 0.005f;
        if (cache->type == 1) {
            i32 support = 0;
            const f32 separation = faceASep(cache->indexA, support);
            if (separation > 0.0f) {
                return {};  // still separated; the cache keeps its stale value on purpose
            }
            if (std::abs(separation - cache->separation) < kStale) {
                const ContactManifold m = faceManifold(true, cache->indexA);
                if (m.count > 0) {
                    return m;
                }
            }
        } else if (cache->type == 2) {
            i32 support = 0;
            const f32 separation = faceBSep(cache->indexB, support);
            if (separation > 0.0f) {
                return {};
            }
            if (std::abs(separation - cache->separation) < kStale) {
                const ContactManifold m = faceManifold(false, cache->indexB);
                if (m.count > 0) {
                    return m;
                }
            }
        } else {
            const std::vector<SatEdge> cachedEdgesA = SatEdges(a);
            const std::vector<SatEdge> cachedEdgesB = SatEdges(b);
            const i32 ia = cache->indexA;
            const i32 ib = cache->indexB;
            if (ia < static_cast<i32>(cachedEdgesA.size()) &&
                ib < static_cast<i32>(cachedEdgesB.size())) {
                const SatEdge& ea = cachedEdgesA[static_cast<usize>(ia)];
                const SatEdge& eb = cachedEdgesB[static_cast<usize>(ib)];
                const Mtx rotation = RotationMatrix(xfB.rotation);
                const Vec4 dirB = rotation.Transform(eb.dir);
                const Vec4 normalB1 = rotation.Transform(eb.normal1);
                const Vec4 normalB2 = rotation.Transform(eb.normal2);
                if (IsMinkowskiFace(Dot3(normalB1, ea.dir), Dot3(normalB2, ea.dir),
                                    Dot3(ea.normal1, dirB), Dot3(ea.normal2, dirB))) {
                    const SatAxis axis =
                        TestEdgePair(a.centroid, ea, ia, eb, ib, b, xfB, marginSum);
                    if (axis.separation > 0.0f) {
                        return {};
                    }
                    if (std::abs(axis.separation - cache->separation) < kStale) {
                        const ContactManifold m = CollideEdgesConsistent(
                            a.centroid, ea, eb, xfB, marginSum, axis.separation);
                        if (m.count > 0) {
                            return m;
                        }
                    }
                }
            }
        }
    }

    // -- the full sweep: A's faces and then B's into one running best, rather than two
    // independent queries compared at the end: B's have to clear `kFaceBias` times the
    // incumbent plus half a slop. Every exit writes the cache. -------------------------------
    SatAxis best;
    for (i32 f = 0; f < static_cast<i32>(a.hull.faces.size()); ++f) {
        i32 support = 0;
        const f32 separation = faceASep(f, support);
        if (separation > 0.0f) {
            if (cache != nullptr) {
                *cache = SatCache{separation, static_cast<u8>(f), static_cast<u8>(support), 1};
            }
            return {};
        }
        if (separation > best.separation) {
            best = SatAxis{separation, f, support, SatAxis::kFaceA};
        }
    }
    for (i32 f = 0; f < static_cast<i32>(b.hull.faces.size()); ++f) {
        i32 support = 0;
        const f32 separation = faceBSep(f, support);
        if (separation > 0.0f) {
            if (cache != nullptr) {
                *cache = SatCache{separation, static_cast<u8>(support), static_cast<u8>(f), 2};
            }
            return {};
        }
        if (best.separation * kFaceBias + halfSlop < separation) {
            best = SatAxis{separation, support, f, SatAxis::kFaceB};
        }
    }
    if (best.kind == SatAxis::kNone) {
        return {};
    }

    // The edge axes are the half of the SAT a face-only test cannot see. Two boxes crossing at a
    // corner are held apart by an axis perpendicular to both edges and to neither face, and
    // without this a face axis wins by default and reports a fraction of the real overlap.
    const std::vector<SatEdge> edgesA = SatEdges(a);
    const std::vector<SatEdge> edgesB = SatEdges(b);
    const SatAxis edge = TestEdges(a.centroid, edgesA, b, edgesB, xfB, marginSum);
    if (edge.separation > 0.0f) {
        if (cache != nullptr) {
            *cache = SatCache{edge.separation, static_cast<u8>(edge.indexA),
                              static_cast<u8>(edge.indexB), 3};
        }
        return {};
    }

    const bool useA = best.kind == SatAxis::kFaceA;
    ContactManifold m = faceManifold(useA, useA ? best.indexA : best.indexB);

    // Which axis wins is decided against the face manifold's own deepest point, not against the
    // face query that produced it -- a clipped patch routinely holds points shallower than its
    // own axis. The first clause is the fallback for a manifold too small to judge that way.
    f32 deepest = 0.0f;
    for (i32 i = 0; i < m.count; ++i) {
        deepest = std::min(deepest, m.points[static_cast<usize>(i)].separation);
    }
    const bool edgeWins =
        edge.kind == SatAxis::kEdge &&
        ((kEdgeBias * best.separation + halfSlop < edge.separation && m.count < 2) ||
         (halfSlop + kEdgeBias * deepest < edge.separation));
    if (edgeWins) {
        if (cache != nullptr) {
            *cache = SatCache{edge.separation, static_cast<u8>(edge.indexA),
                              static_cast<u8>(edge.indexB), 3};
        }
        return CollideEdges(a.centroid, edgesA[static_cast<usize>(edge.indexA)],
                            edgesB[static_cast<usize>(edge.indexB)], xfB, marginSum);
    }
    if (cache != nullptr) {
        *cache = SatCache{best.separation, static_cast<u8>(best.indexA),
                          static_cast<u8>(best.indexB),
                          static_cast<u8>(best.kind == SatAxis::kFaceA ? 1 : 2)};
    }
    return m;
}

// -- triangle pairs ------------------------------------------------------------------------

ContactManifold CollideTriangleSphere(const Triangle& a, const Sphere& b) {
    const Face face = TriangleFace(a);
    // One-sided: the back of a mesh triangle does not collide, however deep the overlap.
    if (Dot3(face.normal, b.centre) - face.offset < 0.0f) {
        return {};
    }
    const Witness w = ClosestPoints(a, b);
    ContactManifold m;
    // The triangle's own radius is ignored here and a flat 0.01 used instead -- the asymmetry
    // with the capsule entry below is deliberate, and is not ours to tidy up.
    const f32 separation = w.distance - (kTriangleContactMargin + b.radius);
    if (separation > 0.0f) {
        return m;
    }
    m.normal = w.normal;
    m.count = 1;
    m.points[0].point = b.centre - w.normal * (kTriangleContactMargin + b.radius);
    m.points[0].separation = separation;
    m.points[0].id = 0;
    return m;
}

ContactManifold CollideTriangleCapsule(const Triangle& a, const Capsule& b) {
    const Face face = TriangleFace(a);
    const f32 side = std::max(Dot3(face.normal, b.p1), Dot3(face.normal, b.p2)) - face.offset;
    if (side < 0.0f) {
        return {};
    }
    // ...and here the triangle's radius IS used, with no constant added. Same geometry, a
    // different rule, and a sphere and a capsule therefore rest at different heights on the
    // same mesh.
    return CapsuleOnFace(face, b, a.radius + b.radius, 0);
}

/// The triangle's three edges, carrying the two planes that meet along each.
///
/// `normal1` is the face plane and `normal2` the edge plane, and that assignment is not
/// arbitrary: the Gauss-map test pairs `normal2` with B's normals twice, so swapping them tests
/// a different pair of arcs. The planes go in **unnegated** -- negating both of A's normals at
/// once would be an equivalent spelling, since every term of the test is a product of two of
/// them, and the unnegated one is used consistently here.
std::vector<SatEdge> TriangleSatEdges(const PartialPolytope& p) {
    const Vec4 corner[3] = {p.v1, p.v2, p.v3};
    std::vector<SatEdge> out;
    for (i32 k = 0; k < 3; ++k) {
        out.push_back(SatEdge{k, corner[k], corner[(k + 1) % 3] - corner[k],
                              p.planes[0].normal, p.planes[k + 1].normal});
    }
    return out;
}

// The adjacency-modified planes steer the sweep (and an edge plane that wins still generates
// nothing), but there is no admissibility refusal of a generated faceB/edge normal: once an
// axis wins and generates, its direction is never vetoed for crossing into a neighbour.
ContactManifold CollideTrianglePolytope(const Triangle& a, const Polytope& b,
                                        const Transform& xfB, SatCache* cache) {
    const PartialPolytope pp = MakePartialPolytope(a);
    const Vec4& faceNormal = pp.planes[0].normal;

    std::vector<Vec4> bVertices;
    for (const Vec4& v : b.hull.vertices) {
        bVertices.push_back(ToWorld(xfB, v));
    }
    // The cull runs against B's *stored* centroid -- not one recomputed from the hull -- and
    // before the cache is even looked at, so a culled triangle leaves its cache untouched.
    if (Dot3(faceNormal, ToWorld(xfB, b.centroid)) - pp.planes[0].offset < 0.0f) {
        return {};  // the polytope is behind a one-sided face
    }

    const f32 marginSum = kTriangleContactMargin + b.margin;
    // Where the polytope/polytope sweep biases multiplicatively (kFaceBias, kEdgeBias), this one
    // biases by slop alone -- half for a face, a whole one for the edge axis. Two entry points,
    // two hysteresis rules; the difference is deliberate and not to be tidied away.
    const f32 halfSlop = 0.5f * constants::kLinearSlop.x;
    const std::vector<Vec4> aVertices = {a.v1, a.v2, a.v3};

    // The margined separation of partial-polytope plane `k` against B, and B's supporting
    // vertex -- the pair every faceA cache write stores.
    const auto planeSeparation = [&](i32 k, i32& support) {
        f32 separation = constants::kMaxFloat.x;
        support = 0;
        for (usize i = 0; i < bVertices.size(); ++i) {
            const f32 d = pp.planes[static_cast<usize>(k)].Distance(bVertices[i]);
            if (d < separation) {
                separation = d;
                support = static_cast<i32>(i);
            }
        }
        return separation - marginSum;
    };
    const auto faceSeparation = [&](const Face& face, i32& support) {
        f32 separation = constants::kMaxFloat.x;
        support = 0;
        for (usize i = 0; i < aVertices.size(); ++i) {
            const f32 d = Dot3(face.normal, aVertices[i]) - face.offset;
            if (d < separation) {
                separation = d;
                support = static_cast<i32>(i);
            }
        }
        return separation - marginSum;
    };
    // The faceA reference is the *geometric* triangle, and clipping keeps the bare side
    // planes: the adjacency-modified ones steer the sweep, and reusing them here would
    // narrow every patch near a fold.
    const auto collideFaceA = [&]() {
        const Face reference{faceNormal, pp.planes[0].offset, aVertices};
        i32 incidentIndex = 0;
        f32 bestDot = constants::kMaxFloat.x;
        for (i32 f = 0; f < static_cast<i32>(b.hull.faces.size()); ++f) {
            const f32 d = Dot3(FaceOf(b, f, xfB).normal, faceNormal);
            if (d < bestDot) {
                bestDot = d;
                incidentIndex = f;
            }
        }
        return FromClippedFace(reference, FaceOf(b, incidentIndex, xfB).vertices, marginSum,
                               false, 0);
    };

    // -- the cached-axis fast path ------------------------------------------------------------
    //
    // Three exits per type: still separated along the cached axis -> empty, cache untouched;
    // drifted a linearSlop from the stored separation -> fall through to the full sweep;
    // otherwise collide on the cached axis alone. A cached-axis collide that *refuses* (an
    // adjacency plane, a rejected direction, a vanished edge) returns empty with no fallback;
    // one that merely finds no points falls through.
    if (cache != nullptr && cache->type != 0) {
        constexpr f32 kStale = 0.005f;
        if (cache->type == 1) {
            i32 support = 0;
            const f32 separation = planeSeparation(cache->indexA, support);
            if (separation > 0.0f) {
                return {};
            }
            if (std::abs(separation - cache->separation) < kStale) {
                // An edge plane can reject but never generate -- cached or not.
                if (cache->indexA != 0) {
                    return {};
                }
                const ContactManifold m = collideFaceA();
                if (m.count > 0) {
                    return m;
                }
            }
        } else if (cache->type == 2) {
            const Face reference = FaceOf(b, cache->indexB, xfB);
            i32 support = 0;
            const f32 separation = faceSeparation(reference, support);
            if (separation > 0.0f) {
                return {};
            }
            if (std::abs(separation - cache->separation) < kStale) {
                const ContactManifold m =
                    FromClippedFace(reference, aVertices, marginSum, true, cache->indexB);
                if (m.count > 0) {
                    return m;
                }
            }
        } else {
            const std::vector<SatEdge> edgesA = TriangleSatEdges(pp);
            const std::vector<SatEdge> edgesB = SatEdges(b);
            const i32 ia = cache->indexA;
            const i32 ib = cache->indexB;
            if (ia < static_cast<i32>(edgesA.size()) && ib < static_cast<i32>(edgesB.size())) {
                const SatEdge& ea = edgesA[static_cast<usize>(ia)];
                const SatEdge& eb = edgesB[static_cast<usize>(ib)];
                const Mtx rotation = RotationMatrix(xfB.rotation);
                const Vec4 dirB = rotation.Transform(eb.dir);
                const Vec4 normalB1 = rotation.Transform(eb.normal1);
                const Vec4 normalB2 = rotation.Transform(eb.normal2);
                if (IsMinkowskiFace(Dot3(normalB1, ea.dir), Dot3(normalB2, ea.dir),
                                    Dot3(ea.normal1, dirB), Dot3(ea.normal2, dirB))) {
                    const SatAxis axis =
                        TestEdgePair(pp.centroid, ea, ia, eb, ib, b, xfB, marginSum);
                    if (axis.separation > 0.0f) {
                        return {};
                    }
                    if (std::abs(axis.separation - cache->separation) < kStale) {
                        ContactManifold m;
                        switch (CollideEdgesCached(m, pp, ea, eb, xfB, marginSum,
                                                   axis.separation)) {
                            case CachedEdge::Abort:
                                return {};  // the DELIBERATE defect: empty, no full sweep behind it
                            case CachedEdge::Hit:
                                return m;
                            case CachedEdge::Fallback:
                                break;
                        }
                    }
                }
            }
        }
    }

    // -- the full sweep. Every exit from here on writes the cache ------------------------------
    SatAxis best;
    for (i32 k = 0; k < 4; ++k) {
        i32 support = 0;
        const f32 separation = planeSeparation(k, support);
        if (separation > 0.0f) {
            if (cache != nullptr) {
                *cache = SatCache{separation, static_cast<u8>(k), static_cast<u8>(support), 1};
            }
            return {};
        }
        if (best.separation + halfSlop < separation) {
            best = SatAxis{separation, k, support, SatAxis::kFaceA};
        }
    }

    for (i32 f = 0; f < static_cast<i32>(b.hull.faces.size()); ++f) {
        i32 support = 0;
        const f32 separation = faceSeparation(FaceOf(b, f, xfB), support);
        if (separation > 0.0f) {
            if (cache != nullptr) {
                *cache = SatCache{separation, static_cast<u8>(support), static_cast<u8>(f), 2};
            }
            return {};
        }
        if (best.separation + halfSlop < separation) {
            best = SatAxis{separation, support, f, SatAxis::kFaceB};
        }
    }

    const std::vector<SatEdge> edgesA = TriangleSatEdges(pp);
    const std::vector<SatEdge> edgesB = SatEdges(b);
    const SatAxis edge = TestEdges(pp.centroid, edgesA, b, edgesB, xfB, marginSum);
    if (edge.separation > 0.0f) {
        if (cache != nullptr) {
            *cache = SatCache{edge.separation, static_cast<u8>(edge.indexA),
                              static_cast<u8>(edge.indexB), 3};
        }
        return {};
    }

    if (edge.kind == SatAxis::kEdge &&
        best.separation + constants::kLinearSlop.x < edge.separation) {
        if (cache != nullptr) {
            *cache = SatCache{edge.separation, static_cast<u8>(edge.indexA),
                              static_cast<u8>(edge.indexB), 3};
        }
        return CollideEdges(pp.centroid, edgesA[static_cast<usize>(edge.indexA)],
                            edgesB[static_cast<usize>(edge.indexB)], xfB, marginSum);
    }

    if (best.kind == SatAxis::kFaceA) {
        if (cache != nullptr) {
            *cache = SatCache{best.separation, static_cast<u8>(best.indexA),
                              static_cast<u8>(best.indexB), 1};
        }
        // **An edge plane can reject but never generate.** If one of the three won the sweep,
        // the contact belongs to the neighbour across that edge and this triangle yields
        // nothing -- which is what stops a body picking up two contacts as it crosses a seam.
        if (best.indexA != 0) {
            return {};
        }
        return collideFaceA();
    }

    const Face reference = FaceOf(b, best.indexB, xfB);
    if (cache != nullptr) {
        *cache = SatCache{best.separation, static_cast<u8>(best.indexA),
                          static_cast<u8>(best.indexB), 2};
    }
    return FromClippedFace(reference, aVertices, marginSum, true, best.indexB);
}

}  // namespace snowball
