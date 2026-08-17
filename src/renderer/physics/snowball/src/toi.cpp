#include "snowball/toi.h"

#include <algorithm>
#include <cmath>

namespace snowball {
namespace {

// The band around the target inside which a separation counts as "touching", and the
// tolerance the root-finder solves to. Numerically 0.25 and 0.025 of a linearSlop, but they
// are independent literals here on purpose, not derived constants -- rewriting them in terms
// of the slop would invite retuning values every pinned impact time depends on.
constexpr f32 kTolerance = 0.00125f;
constexpr f32 kRootTolerance = 1.25e-4f;
constexpr i32 kMaxOuterIterations = 20;
constexpr i32 kMaxRootIterations = 50;
constexpr i32 kMaxAxisIterations = 32;

Vec4 Rotate(const Vec4& q, const Vec4& v) { return RotationMatrix(q).Transform(v); }

Vec4 InvRotate(const Vec4& q, const Vec4& v) {
    return Transpose3(RotationMatrix(q)).Transform(v);
}

Vec4 ToWorld(const Transform& xf, const Vec4& p) {
    return RotationMatrix(xf.rotation).Transform(p) + xf.position;
}

/// nlerp with the dot-sign flip: the shorter arc, through the approximate rsqrt.
Vec4 NlerpRotation(const Vec4& q0, const Vec4& q1, f32 t) {
    const f32 dot = (q0.x * q1.x + q0.y * q1.y) + (q0.z * q1.z + q0.w * q1.w);
    const Vec4 target = dot > 0.0f ? q1 : -q1;
    return QuatNormalize(q0 + (target - q0) * t);
}

/// The separation function -- b2SeparationFunction taken to 3D, plus the extra edge-edge
/// type. Everything works on the re-based sweeps the core hands it.
class HomingFunction {
public:
    enum class Type { Points, FaceA, FaceB, EdgeEdge };

    HomingFunction(const DistanceOutput& distance, const GjkCache& cache, const ToiInput& in,
                   const Sweep& sweepA, const Sweep& sweepB, bool allowEdge, f32 t)
        : input_(&in), sweepA_(&sweepA), sweepB_(&sweepB) {
        const Transform xfA = SweepTransform(sweepA, in.localCentreA, t);
        const Transform xfB = SweepTransform(sweepB, in.localCentreB, t);

        if (cache.count == 1) {
            // Two closest vertices: the GJK normal is the axis and no frame is needed.
            type_ = Type::Points;
            axis_ = distance.normal;
            return;
        }

        if (cache.count == 2) {
            if (cache.indexB[0] == cache.indexB[1]) {
                // B degenerated to a vertex, so the feature is A's edge: face-of-A, with the
                // axis carried in A's frame and NOT renormalised on this path (the size-3
                // face path below does renormalise -- a deliberate asymmetry; normalising
                // here would move every separation built on this path).
                type_ = Type::FaceA;
                localPoint_ = InvRotate(xfA.rotation, distance.pointA - xfA.position);
                axis_ = InvRotate(xfA.rotation, distance.normal);
            } else {
                type_ = Type::FaceB;
                localPoint_ = InvRotate(xfB.rotation, distance.pointB - xfB.position);
                axis_ = -InvRotate(xfB.rotation, distance.normal);
            }
            return;
        }

        // Simplex of three. Count the distinct vertices per side: two distinct means the
        // simplex ends on that shape's edge.
        const auto distinct = [](const std::array<i32, 4>& index, i32& other) {
            i32 count = 1;
            other = index[0];
            for (i32 k = 1; k < 3; ++k) {
                if (index[static_cast<usize>(k)] != index[0]) {
                    other = index[static_cast<usize>(k)];
                    ++count;
                }
            }
            // Three distinct values report 3; two report 2 with `other` the second value.
            if (count == 3 && index[1] == index[2]) {
                count = 2;
            }
            return count;
        };
        i32 otherA = 0, otherB = 0;
        const i32 distinctA = distinct(cache.indexA, otherA);
        const i32 distinctB = distinct(cache.indexB, otherB);

        if (distinctA == 2 && distinctB == 2 && allowEdge) {
            const Vec4 edgeA =
                input_->proxyA.Vertex(otherA) - input_->proxyA.Vertex(cache.indexA[0]);
            const Vec4 edgeB =
                input_->proxyB.Vertex(otherB) - input_->proxyB.Vertex(cache.indexB[0]);
            const Vec4 cross =
                Cross3(Rotate(xfB.rotation, edgeB), Rotate(xfA.rotation, edgeA));
            if (LengthSquared3(cross) > constants::kZeroSafe.x) {
                type_ = Type::EdgeEdge;
                localEdgeA_ = edgeA;
                // The sign is fixed once, here, by folding it into the stored edge: the axis
                // recomputed at any later time then agrees with the GJK normal's direction.
                localEdgeB_ = Dot3(distance.normal, cross) < 0.0f ? -edgeB : edgeB;
                localPoint_ = distance.normal;
                return;
            }
            // Degenerate cross: fall through to the face selection below.
        }

        if (distinctA >= distinctB) {
            type_ = Type::FaceA;
            localPoint_ = InvRotate(xfA.rotation, distance.pointA - xfA.position);
            axis_ = Normalize(InvRotate(xfA.rotation, distance.normal));
        } else {
            type_ = Type::FaceB;
            localPoint_ = InvRotate(xfB.rotation, distance.pointB - xfB.position);
            axis_ = Normalize(-InvRotate(xfB.rotation, distance.normal));
        }
    }

    /// The deepest pair along the axis at time `t`, and the separation there. Returns false
    /// only for a degenerate edge-edge cross, which the caller treats as "retry without
    /// edge-edge".
    bool FindMinSeparation(f32 t, f32& separation, i32& indexA, i32& indexB) const {
        const Transform xfA = SweepTransform(*sweepA_, input_->localCentreA, t);
        const Transform xfB = SweepTransform(*sweepB_, input_->localCentreB, t);
        Vec4 axis;
        if (!WorldAxis(xfA, xfB, axis)) {
            return false;
        }
        switch (type_) {
            case Type::Points:
            case Type::EdgeEdge:
                indexA = input_->proxyA.Support(InvRotate(xfA.rotation, axis));
                indexB = input_->proxyB.Support(InvRotate(xfB.rotation, -axis));
                separation = Dot3(ToWorld(xfB, input_->proxyB.Vertex(indexB)) -
                                      ToWorld(xfA, input_->proxyA.Vertex(indexA)),
                                  axis);
                return true;
            case Type::FaceA:
                indexA = -1;
                indexB = input_->proxyB.Support(InvRotate(xfB.rotation, -axis));
                separation = Dot3(ToWorld(xfB, input_->proxyB.Vertex(indexB)) -
                                      ToWorld(xfA, localPoint_),
                                  axis);
                return true;
            case Type::FaceB:
                indexA = input_->proxyA.Support(InvRotate(xfA.rotation, -axis));
                indexB = -1;
                separation = Dot3(ToWorld(xfA, input_->proxyA.Vertex(indexA)) -
                                      ToWorld(xfB, localPoint_),
                                  axis);
                return true;
        }
        return false;
    }

    /// The separation of a fixed pair at time `t` -- the root-finder's function.
    bool Evaluate(i32 indexA, i32 indexB, f32 t, f32& separation) const {
        const Transform xfA = SweepTransform(*sweepA_, input_->localCentreA, t);
        const Transform xfB = SweepTransform(*sweepB_, input_->localCentreB, t);
        Vec4 axis;
        if (!WorldAxis(xfA, xfB, axis)) {
            return false;
        }
        switch (type_) {
            case Type::Points:
            case Type::EdgeEdge:
                separation = Dot3(ToWorld(xfB, input_->proxyB.Vertex(indexB)) -
                                      ToWorld(xfA, input_->proxyA.Vertex(indexA)),
                                  axis);
                return true;
            case Type::FaceA:
                separation = Dot3(ToWorld(xfB, input_->proxyB.Vertex(indexB)) -
                                      ToWorld(xfA, localPoint_),
                                  axis);
                return true;
            case Type::FaceB:
                separation = Dot3(ToWorld(xfA, input_->proxyA.Vertex(indexA)) -
                                      ToWorld(xfB, localPoint_),
                                  axis);
                return true;
        }
        return false;
    }

private:
    static Vec4 Normalize(const Vec4& v) {
        const f32 lengthSquared = LengthSquared3(v);
        return lengthSquared > 0.0f ? v * Rsqrt(lengthSquared) : v;
    }

    /// The axis in world space at the given poses. Only edge-edge can fail, and only edge-edge
    /// depends on time at all -- the cross of the two rotated edges is recomputed rather than
    /// carried, which is what keeps the separation honest while both bodies turn.
    bool WorldAxis(const Transform& xfA, const Transform& xfB, Vec4& axis) const {
        switch (type_) {
            case Type::Points:
                axis = axis_;
                return true;
            case Type::FaceA:
                axis = Rotate(xfA.rotation, axis_);
                return true;
            case Type::FaceB:
                axis = Rotate(xfB.rotation, axis_);
                return true;
            case Type::EdgeEdge: {
                const Vec4 cross = Cross3(Rotate(xfB.rotation, localEdgeB_),
                                          Rotate(xfA.rotation, localEdgeA_));
                const f32 lengthSquared = LengthSquared3(cross);
                if (lengthSquared <= constants::kZeroSafe.x) {
                    return false;
                }
                axis = cross * Rsqrt(lengthSquared);
                return true;
            }
        }
        return false;
    }

    const ToiInput* input_;
    const Sweep* sweepA_;
    const Sweep* sweepB_;
    Type type_{Type::Points};
    Vec4 axis_{};         ///< world for Points; a body frame for the face types
    Vec4 localPoint_{};   ///< the fixed witness for the face types; the GJK normal for edge-edge
    Vec4 localEdgeA_{};
    Vec4 localEdgeB_{};   ///< stored pre-negated when the cross opposed the GJK normal
};

}  // namespace

Transform SweepTransform(const Sweep& sweep, const Vec4& localCentre, f32 t) {
    Transform xf;
    xf.rotation = NlerpRotation(sweep.q0, sweep.q, t);
    const Vec4 centre = sweep.c0 + (sweep.c - sweep.c0) * t;
    xf.position = centre - RotationMatrix(xf.rotation).Transform(localCentre);
    return xf;
}

void AdvanceSweep(Sweep& sweep, f32 alpha) {
    const f32 remaining = 1.0f - sweep.alpha0;
    if (!(sweep.alpha0 < alpha) || remaining <= constants::kEpsilon.x) {
        return;
    }
    // `Rcp` (rcpps plus a Newton step), not a true divide -- deliberate, and ulp-pinned.
    const f32 beta = (alpha - sweep.alpha0) * Rcp(remaining);
    sweep.c0 = sweep.c0 + (sweep.c - sweep.c0) * beta;
    sweep.q0 = NlerpRotation(sweep.q0, sweep.q, beta);
    sweep.alpha0 = alpha;
}

namespace {

ToiOutput TimeOfImpactRun(const ToiInput& input, GjkCache& cache) {
    ToiOutput out;

    // Re-base everything on sweep A's start so the arithmetic runs near the origin; the
    // witnesses get the base added back on the way out. Float precision, not correctness --
    // but at world coordinates in the thousands the roots move visibly without it.
    const Vec4 origin = input.sweepA.c0;
    Sweep sweepA = input.sweepA;
    sweepA.c = sweepA.c - origin;
    sweepA.c0 = Vec4{};
    Sweep sweepB = input.sweepB;
    sweepB.c = sweepB.c - origin;
    sweepB.c0 = sweepB.c0 - origin;

    const f32 target =
        std::max(input.proxyA.Radius() + input.proxyB.Radius() - 0.015f, 0.005f);
    f32 t1 = 0.0f;
    bool allowEdge = true;
    i32 counted = 0;
    for (;;) {
        ++out.iterations;

        DistanceInput di;
        di.proxyA = input.proxyA;
        di.proxyB = input.proxyB;
        di.xfA = SweepTransform(sweepA, input.localCentreA, t1);
        di.xfB = SweepTransform(sweepB, input.localCentreB, t1);
        const DistanceOutput distance = Distance(di, cache);
        out.witnessA = distance.pointA + origin;
        out.witnessB = distance.pointB + origin;

        // A distance of exactly zero, or a full four-vertex simplex, is containment: the
        // cores already overlap at t1 and there is no first touch to find.
        if (distance.distance == 0.0f || cache.count == 4) {
            out.state = ToiState::Overlap;
            out.t = t1;
            return out;
        }
        if (distance.distance < target + kTolerance) {
            out.state = ToiState::Touching;
            out.t = t1;
            return out;
        }

        const HomingFunction fcn(distance, cache, input, sweepA, sweepB, allowEdge, t1);
        bool degenerate = false;
        bool advanced = false;
        f32 t2 = 1.0f;
        for (i32 pushBack = 0;;) {
            f32 s2 = 0.0f;
            i32 indexA = 0, indexB = 0;
            if (!fcn.FindMinSeparation(t2, s2, indexA, indexB)) {
                degenerate = true;
                break;
            }
            if (s2 > target + kTolerance) {
                out.state = ToiState::Separated;
                out.t = 1.0f;
                return out;
            }
            if (s2 > target - kTolerance) {
                // The deepest pair only just reaches the target at t2: commit the advance
                // and let the next outer iteration look again from there.
                t1 = t2;
                advanced = true;
                break;
            }

            f32 s1 = 0.0f;
            if (!fcn.Evaluate(indexA, indexB, t1, s1)) {
                degenerate = true;
                break;
            }
            if (s1 < target - kTolerance) {
                out.state = ToiState::Overlap;
                out.t = t1;
                return out;
            }
            if (s1 <= target + kTolerance) {
                out.state = ToiState::Touching;
                out.t = t1;
                return out;
            }

            // This pair crosses the target somewhere in [t1, t2]: root-find it, alternating
            // bisection (even iterations) with false position. The false-position divide is
            // an `Rcp`, not a true divide -- deliberate; see the header note.
            f32 a1 = t1, a2 = t2;
            for (i32 root = 0;;) {
                const f32 t = (root & 1) != 0
                                  ? a1 + (a2 - a1) * (target - s1) * Rcp(s2 - s1)
                                  : (a1 + a2) * 0.5f;
                f32 s = 0.0f;
                if (!fcn.Evaluate(indexA, indexB, t, s)) {
                    degenerate = true;
                    break;
                }
                out.rootIterations = std::max(out.rootIterations, root + 1);
                if (std::abs(s - target) < kRootTolerance) {
                    // Converged: pull the outer candidate time down to the crossing.
                    t2 = t;
                    break;
                }
                if (s > target) {
                    a1 = t;
                    s1 = s;
                } else {
                    a2 = t;
                    s2 = s;
                }
                if (++root == kMaxRootIterations) {
                    break;
                }
            }
            if (degenerate || ++pushBack == kMaxAxisIterations) {
                break;
            }
        }

        if (degenerate) {
            // A vanished edge-edge axis. Retry the whole outer body with edge-edge disabled
            // -- the retry deliberately does NOT count against the iteration cap, and
            // edge-edge is re-armed as soon as one pass survives without it.
            allowEdge = false;
            continue;
        }
        allowEdge = true;
        (void)advanced;  // an exhausted axis loop advances the count with t1 unchanged
        if (++counted == kMaxOuterIterations) {
            out.state = ToiState::Touching;
            out.t = t1;
            return out;
        }
    }
}

}  // namespace

ToiOutput TimeOfImpact(const ToiInput& input, GjkCache* cache) {
    // The core reads and writes the caller's cache when one is offered -- the mesh path keeps
    // one per triangle -- and runs cold otherwise, which is how the convex caller uses it.
    GjkCache local;
    if (cache != nullptr) {
        local = *cache;
    }
    const ToiOutput out = TimeOfImpactRun(input, local);
    if (cache != nullptr) {
        *cache = local;
    }
    return out;
}

}  // namespace snowball
