#include "snowball/mesh_contact.h"

#include <algorithm>
#include <cmath>

#include "snowball/manifold.h"
#include "snowball/shape.h"

namespace snowball {
namespace {

f32 SignedArea(const Vec4& normal, const Vec4& a, const Vec4& b) {
    return Dot3(Cross3(a, b), normal);
}

/// Cluster normals are renormalised through the engine's reciprocal square root, not a divide.
Vec4 Normalize(const Vec4& v) { return v * Rsqrt(Vec4::Splat(LengthSquared3(v))).x; }

/// Every triangle the mesh paths collide carries this radius -- neither triangle accessor
/// sets one, so each loop stamps it onto its local copy before colliding. The sphere and
/// polytope entries ignore it (their 0.01 is hard-coded); the capsule entry READS it, and
/// forgetting the stamp rests a capsule exactly 0.01 too low on every mesh.
constexpr f32 kMeshTriangleRadius = 0.01f;

}  // namespace

void ReducePoints(Manifold& manifold, std::span<const ManifoldPoint> points) {
    manifold.count = 0;
    if (points.empty()) {
        return;
    }
    const Vec4& normal = manifold.normal;

    // 1. The deepest point. Every stage uses the same 1% bias, so a tie is broken by whichever
    //    candidate came first rather than by the last one to arrive.
    i32 best = 0;
    f32 bestSeparation = constants::kMaxFloat.x;
    for (usize i = 0; i < points.size(); ++i) {
        if (points[i].separation * kReduceBias < bestSeparation) {
            bestSeparation = points[i].separation;
            best = static_cast<i32>(i);
        }
    }
    manifold.points[0] = points[static_cast<usize>(best)];
    manifold.count = 1;

    // 2. The point farthest from it.
    i32 second = 0;
    f32 bestDistance = -constants::kMaxFloat.x;
    for (usize i = 0; i < points.size(); ++i) {
        const f32 d = LengthSquared3(points[i].point - manifold.points[0].point);
        if (d * kReduceBias > bestDistance) {
            bestDistance = d;
            second = static_cast<i32>(i);
        }
    }
    if (bestDistance < kReduceTolerance) {
        return;
    }
    manifold.points[1] = points[static_cast<usize>(second)];
    manifold.count = 2;

    // 3. The point farthest off the line the first two make, by area rather than by distance.
    const Vec4 arm1 = manifold.points[1].point - manifold.points[0].point;
    i32 third = 0;
    f32 bestArea = -constants::kMaxFloat.x;
    for (usize i = 0; i < points.size(); ++i) {
        const f32 area =
            std::abs(SignedArea(normal, arm1, points[i].point - manifold.points[0].point));
        if (area * kReduceBias > bestArea) {
            bestArea = area;
            third = static_cast<i32>(i);
        }
    }
    if (bestArea < kReduceTolerance) {
        return;
    }
    manifold.points[2] = points[static_cast<usize>(third)];
    manifold.count = 3;

    // 4. The point lying most outside that triangle, measured in barycentric terms so that a
    //    point far away but inside cannot win.
    const Vec4 p1 = manifold.points[0].point;
    const Vec4 p2 = manifold.points[1].point;
    const Vec4 p3 = manifold.points[2].point;
    const f32 denominator = SignedArea(normal, arm1, p3 - p1);
    const f32 inverse = 1.0f / denominator;
    i32 fourth = 0;
    f32 bestOutside = constants::kMaxFloat.x;
    for (usize i = 0; i < points.size(); ++i) {
        const Vec4& p = points[i].point;
        const f32 u = SignedArea(normal, arm1, p - p1) * inverse;
        const f32 v = SignedArea(normal, p3 - p2, p - p2) * inverse;
        const f32 w = SignedArea(normal, p1 - p3, p - p3) * inverse;
        const f32 outside = std::min(v, std::min(w, u));
        if (outside * kReduceBias < bestOutside) {
            bestOutside = outside;
            fourth = static_cast<i32>(i);
        }
    }
    if (bestOutside <= 0.0f && std::abs(denominator * bestOutside) >= kReduceTolerance) {
        manifold.points[3] = points[static_cast<usize>(fourth)];
        manifold.count = 4;
    }
}

std::vector<Manifold> ReduceManifolds(std::span<const Manifold> fresh,
                                      std::span<const Manifold> previous,
                                      const Vec4& meshRotation) {
    std::vector<Manifold> out;
    if (fresh.empty()) {
        return out;
    }

    // -- seed: distinct normals, more than 5 degrees apart, at most three of them ------------
    std::vector<Vec4> clusters;
    clusters.push_back(fresh[0].normal);
    for (usize i = 1; i < fresh.size() && clusters.size() < kMaxMeshManifolds; ++i) {
        f32 closest = -constants::kMaxFloat.x;
        for (const Vec4& c : clusters) {
            closest = std::max(closest, Dot3(c, fresh[i].normal));
        }
        if (closest < kClusterCosine) {
            clusters.push_back(fresh[i].normal);
        }
    }

    // -- refine: Lloyd's algorithm, four passes at most, stopping as soon as nothing moves ---
    std::vector<i32> assignment(fresh.size(), -1);
    for (i32 pass = 0; pass < 4; ++pass) {
        std::vector<Vec4> sum(clusters.size());
        bool moved = false;
        for (usize i = 0; i < fresh.size(); ++i) {
            i32 best = 0;
            f32 bestDot = -constants::kMaxFloat.x;
            for (usize k = 0; k < clusters.size(); ++k) {
                // A cluster that collapsed to zero is skipped rather than removed, so the
                // remaining ones keep their indices.
                if (LengthSquared3(clusters[k]) == 0.0f) {
                    continue;
                }
                const f32 d = Dot3(clusters[k], fresh[i].normal);
                if (d > bestDot) {
                    bestDot = d;
                    best = static_cast<i32>(k);
                }
            }
            sum[static_cast<usize>(best)] = sum[static_cast<usize>(best)] + fresh[i].normal;
            if (assignment[i] != best) {
                assignment[i] = best;
                moved = true;
            }
        }
        if (!moved) {
            break;
        }
        for (usize k = 0; k < clusters.size(); ++k) {
            const f32 length = LengthSquared3(sum[k]);
            clusters[k] = length > constants::kZeroSafe.x ? Normalize(sum[k]) : constants::kZero;
        }
    }

    // -- gather: every point of every manifold, into its cluster ------------------------------
    std::vector<std::vector<ManifoldPoint>> gathered(clusters.size());
    for (usize i = 0; i < fresh.size(); ++i) {
        const Manifold& m = fresh[i];
        auto& bucket = gathered[static_cast<usize>(assignment[i])];
        for (i32 p = 0; p < m.count; ++p) {
            bucket.push_back(m.points[static_cast<usize>(p)]);
        }
    }

    // -- reduce: one manifold per non-empty cluster -------------------------------------------
    for (usize k = 0; k < clusters.size(); ++k) {
        if (gathered[k].empty()) {
            continue;
        }
        Manifold m;
        m.normal = clusters[k];
        ReducePoints(m, gathered[k]);
        out.push_back(m);
    }

    // -- warm start: match each new patch to an old one by normal, then by point id -----------
    const Mtx toWorld = RotationMatrix(meshRotation);
    std::vector<bool> claimed(previous.size(), false);
    for (Manifold& m : out) {
        const Vec4 worldNormal = toWorld.Transform(m.normal);
        for (usize j = 0; j < previous.size(); ++j) {
            if (claimed[j] || Dot3(worldNormal, previous[j].normal) <= kClusterCosine) {
                continue;
            }
            claimed[j] = true;
            MatchManifold(previous[j], m);
            break;
        }
    }
    return out;
}

namespace {

/// The shared tail of all three entry points: collide every queried triangle in the mesh's own
/// frame, keep the manifolds that touched, reduce them, then lift the survivors into the world.
///
/// Stamping the triangle index into the high half of each id happens here rather than in the
/// narrowphase, because the narrowphase does not know which triangle it was handed. Feature
/// ids only ever meet other ids produced by this same path, so their values are free to
/// choose; what matters is that the stamp is uniform across every point of a manifold, which
/// is what keeps warm-start matching from pairing a point with its namesake on a neighbouring
/// triangle.
template <typename Collide>
std::vector<Manifold> CollideMesh(const TriangleSource& mesh,
                                  std::span<const MeshTriangleEntry> entries,
                                  const Transform& xfMesh, std::span<const Manifold> previous,
                                  Collide&& collide) {
    std::vector<Manifold> touched;
    for (usize i = 0; i < entries.size(); ++i) {
        Triangle triangle = mesh.FullTriangle(entries[i].triangle);
        triangle.radius = kMeshTriangleRadius;
        const ContactManifold hit = collide(triangle, i);
        if (hit.count <= 0) {
            continue;
        }
        Manifold m;
        m.count = hit.count;
        m.normal = hit.normal;
        for (i32 p = 0; p < hit.count; ++p) {
            const ContactPoint& source = hit.points[static_cast<usize>(p)];
            ManifoldPoint& point = m.points[static_cast<usize>(p)];
            point.point = source.point;
            point.separation = source.separation;
            point.id = static_cast<u64>(source.id) |
                       (static_cast<u64>(static_cast<u32>(triangle.index))
                        << 32);
        }
        touched.push_back(m);
    }

    std::vector<Manifold> out = ReduceManifolds(touched, previous, xfMesh.rotation);
    const Mtx rotation = RotationMatrix(xfMesh.rotation);
    for (Manifold& m : out) {
        m.normal = rotation.Transform(m.normal);
        for (i32 p = 0; p < m.count; ++p) {
            ManifoldPoint& point = m.points[static_cast<usize>(p)];
            point.point = rotation.Transform(point.point) + xfMesh.position;
        }
    }
    return out;
}

}  // namespace

std::vector<Manifold> CollideMeshSphere(const TriangleSource& mesh,
                                        std::span<const MeshTriangleEntry> entries,
                                        const Sphere& sphere, const Transform& xfMesh,
                                        std::span<const Manifold> previous) {
    return CollideMesh(mesh, entries, xfMesh, previous,
                       [&](const Triangle& t, usize) { return CollideTriangleSphere(t, sphere); });
}

std::vector<Manifold> CollideMeshCapsule(const TriangleSource& mesh,
                                         std::span<const MeshTriangleEntry> entries,
                                         const Capsule& capsule, const Transform& xfMesh,
                                         std::span<const Manifold> previous) {
    return CollideMesh(mesh, entries, xfMesh, previous, [&](const Triangle& t, usize) {
        return CollideTriangleCapsule(t, capsule);
    });
}

std::vector<Manifold> CollideMeshPolytope(const TriangleSource& mesh,
                                          std::span<MeshTriangleEntry> entries,
                                          const Polytope& polytope, const Transform& xfShape,
                                          const Transform& xfMesh,
                                          std::span<const Manifold> previous, bool invalidateSat) {
    return CollideMesh(mesh, entries, xfMesh, previous, [&](const Triangle& t, usize i) {
        // The TOI advance teleported the body; whatever axis separated it before the advance
        // says nothing now. Cooling happens per triangle as it is visited: clearing the type
        // byte marks the cache cold before the SAT reads it.
        if (invalidateSat) {
            entries[i].sat.type = 0;
        }
        return CollideTrianglePolytope(t, polytope, xfShape, &entries[i].sat);
    });
}

// -- the triangle buffer and the mesh half of continuous collision ---------------------------

namespace {

/// The axis quantisation pair: the store scale, and the load scale kept as an exact literal
/// rather than re-derived as `1/32767`, so the decode is bit-stable across builds.
constexpr f32 kAxisStore = 32767.0f;
constexpr f32 kAxisLoad = 0.000030518509f;

/// A cached axis is trusted only while the pair closes by less than this over the step.
/// linearSlop, reused -- and compared against zero separation, not the core's target.
constexpr f32 kAxisApproach = 0.005f;

Vec4 Rotate(const Vec4& q, const Vec4& v) { return RotationMatrix(q).Transform(v); }

Vec4 InvRotate(const Vec4& q, const Vec4& v) {
    return Transpose3(RotationMatrix(q)).Transform(v);
}

Vec4 ToWorld(const Transform& xf, const Vec4& p) {
    return RotationMatrix(xf.rotation).Transform(p) + xf.position;
}

}  // namespace

void UpdateTriangleBuffer(MeshContactState& state, const TriangleSource& mesh,
                          const Aabb& localBounds) {
    // The reuse check, xyz lanes only: while the query box still fits inside the fattened one,
    // the previous triangle set AND its warm state stand.
    if (state.primed && state.cached.lower.x <= localBounds.lower.x &&
        state.cached.lower.y <= localBounds.lower.y &&
        state.cached.lower.z <= localBounds.lower.z &&
        localBounds.upper.x <= state.cached.upper.x &&
        localBounds.upper.y <= state.cached.upper.y &&
        localBounds.upper.z <= state.cached.upper.z) {
        return;
    }
    state.primed = true;

    // centre +/- 1.5x the half extent, associated exactly as written: 1.5 applied to the
    // half extent (0.5 * (upper - lower)). Reassociating changes the box's last bits, and
    // with them the step on which a moving body re-queries.
    const Vec4 centre = (localBounds.lower + localBounds.upper) * 0.5f;
    const Vec4 reach = ((localBounds.upper - localBounds.lower) * 0.5f) * 1.5f;
    state.cached = Aabb{centre - reach, centre + reach};

    std::vector<i32> indices;
    mesh.Query(state.cached, indices);

    // Sorted merge -- both sides ascend by triangle index, which `TriangleSource::Query`
    // guarantees; an unsorted query would silently shed warm state here. A survivor keeps
    // its whole entry; a newcomer starts with a cold cache and a zero axis.
    std::vector<MeshTriangleEntry> merged;
    merged.reserve(indices.size());
    auto old = state.entries.begin();
    for (const i32 triangle : indices) {
        while (old != state.entries.end() && old->triangle < triangle) {
            ++old;
        }
        if (old != state.entries.end() && old->triangle == triangle) {
            merged.push_back(*old);
            ++old;
        } else {
            MeshTriangleEntry fresh;
            fresh.triangle = triangle;
            merged.push_back(fresh);
        }
    }
    state.entries = std::move(merged);
}

ToiOutput MeshTimeOfImpact(MeshContactState& state, const TriangleSource& mesh,
                           const Sweep& sweepMesh, const Vec4& localCentreMesh,
                           const SupportProxy& convex, const Vec4& convexCentroid,
                           const Sweep& sweepConvex, const Vec4& localCentreConvex) {
    ToiOutput out;
    out.state = ToiState::Separated;
    out.t = constants::kMaxFloat.x;   // "no hit" until some triangle proves otherwise

    // The convex shape's centroid at sweep start, in the mesh's own frame -- what the
    // back-face cull tests every triangle against.
    const Vec4 originMesh = sweepMesh.c0 - Rotate(sweepMesh.q0, localCentreMesh);
    const Vec4 originConvex = sweepConvex.c0 - Rotate(sweepConvex.q0, localCentreConvex);
    const Vec4 centroidLocal = InvRotate(
        sweepMesh.q0, Rotate(sweepConvex.q0, convexCentroid) + originConvex - originMesh);

    for (MeshTriangleEntry& entry : state.entries) {
        Triangle tri = mesh.PlainTriangle(entry.triangle);
        tri.radius = kMeshTriangleRadius;
        const SupportProxy triProxy{Shape{tri}};

        // Early-out 1: the cached separating axis, when there is one. Separation is measured
        // support-to-support at both sweep endpoints -- endpoint-only on purpose, mid-sweep
        // rotation is deliberately ignored -- and the skip needs "separated at both ends,
        // closing slower than the slop".
        const Vec4 axis{static_cast<f32>(entry.axis[0]) * kAxisLoad,
                        static_cast<f32>(entry.axis[1]) * kAxisLoad,
                        static_cast<f32>(entry.axis[2]) * kAxisLoad};
        if (Dot3(axis, axis) > 0.0f) {
            const auto separation = [&](f32 t) {
                const Transform xfA = SweepTransform(sweepMesh, localCentreMesh, t);
                const Transform xfB = SweepTransform(sweepConvex, localCentreConvex, t);
                const Vec4 supportA =
                    triProxy.Vertex(triProxy.Support(InvRotate(xfA.rotation, axis)));
                const Vec4 supportB =
                    convex.Vertex(convex.Support(InvRotate(xfB.rotation, -axis)));
                return Dot3(axis, ToWorld(xfB, supportB) - ToWorld(xfA, supportA));
            };
            const f32 sepEnd = separation(1.0f);
            if (sepEnd > 0.0f) {
                const f32 sepStart = separation(0.0f);
                if (sepStart > 0.0f && sepEnd - sepStart >= -kAxisApproach) {
                    continue;
                }
            }
        }

        // Early-out 2: triangles are one-sided. The normal goes through the approximate
        // rsqrt before the sign test, which matters exactly once: a degenerate triangle's
        // NaN compares false and is NOT culled, and the core sorts it out instead.
        const Vec4 raw = tri.RawNormal();
        const Vec4 normal = raw * Rsqrt(Vec4::Splat(LengthSquared3(raw))).x;
        if (Dot3(normal, centroidLocal - tri.v1) <= 0.0f) {
            continue;
        }

        ToiInput input;
        input.proxyA = triProxy;
        input.proxyB = convex;
        input.sweepA = sweepMesh;
        input.sweepB = sweepConvex;
        input.localCentreA = localCentreMesh;
        input.localCentreB = localCentreConvex;
        const ToiOutput o = TimeOfImpact(input, &entry.cache);

        // The axis write-back happens whatever the state: a separated result with a real
        // witness pair stores its direction, anything else clears the slot.
        entry.axis = {0, 0, 0};
        if (o.state == ToiState::Separated) {
            const Vec4 d = o.witnessB - o.witnessA;
            const f32 lengthSquared = LengthSquared3(d);
            if (lengthSquared > constants::kZeroSafe.x) {
                const Vec4 scaled = d * Rsqrt(Vec4::Splat(lengthSquared)).x * kAxisStore;
                entry.axis = {
                    static_cast<i16>(std::clamp(scaled.x, -kAxisStore, kAxisStore)),
                    static_cast<i16>(std::clamp(scaled.y, -kAxisStore, kAxisStore)),
                    static_cast<i16>(std::clamp(scaled.z, -kAxisStore, kAxisStore))};
            }
        }

        if (o.state == ToiState::Overlap) {
            // Overlap on ANY triangle wins instantly: return mid-scan, discarding an
            // already-found smaller touching t and every remaining triangle. Deliberate --
            // overlap means "resolve in place, do not advance", callers rely on that, and
            // the regression suite pins the mid-scan result. Do not fold this into min-t.
            return o;
        }
        if (o.state == ToiState::Touching && o.t < out.t) {
            out = o;
        }
    }
    return out;
}

}  // namespace snowball
