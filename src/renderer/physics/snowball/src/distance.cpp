#include "snowball/distance.h"

#include <algorithm>
#include <cmath>

namespace snowball {
namespace {

constexpr i32 kMaxIterations = 20;
constexpr f32 kEpsilon = 1.1920929e-07f;

Vec4 ToWorld(const Transform& xf, const Vec4& p) {
    return RotationMatrix(xf.rotation).Transform(p) + xf.position;
}

Vec4 ToLocal(const Transform& xf, const Vec4& d) {
    // Only a direction, so the translation does not apply; the rotation's inverse is its
    // transpose, which for a column-major matrix is three dot products.
    const Mtx r = RotationMatrix(xf.rotation);
    return Vec4{Dot3(r.c[0], d), Dot3(r.c[1], d), Dot3(r.c[2], d)};
}

struct SimplexVertex {
    Vec4 wA{};
    Vec4 wB{};
    Vec4 w{};
    f32 u{0.0f};
    i32 indexA{0};
    i32 indexB{0};
};

struct Simplex {
    SimplexVertex v[4]{};
    i32 count{0};
};

/// Barycentric weights of the closest point to the origin on a triangle, if it lies inside.
bool ClosestOnTriangle(const Vec4& a, const Vec4& b, const Vec4& c, f32 w[3]) {
    const Vec4 ab = b - a;
    const Vec4 ac = c - a;
    const Vec4 n = Cross3(ab, ac);
    const f32 nn = LengthSquared3(n);
    if (nn <= kEpsilon) {
        return false;
    }
    // Project the origin onto the plane, then take the three sub-triangle areas as weights.
    const f32 t = Dot3(n, a) / nn;
    const Vec4 p = n * t;
    const f32 area = nn;
    const f32 wa = Dot3(Cross3(b - p, c - p), n);
    const f32 wb = Dot3(Cross3(c - p, a - p), n);
    const f32 wc = Dot3(Cross3(a - p, b - p), n);
    if (wa < 0.0f || wb < 0.0f || wc < 0.0f) {
        return false;
    }
    w[0] = wa / area;
    w[1] = wb / area;
    w[2] = wc / area;
    return true;
}

/// Reduce the simplex to the smallest face containing the closest point to the origin, and
/// leave the barycentric weights on the surviving vertices.
void SolveSimplex(Simplex& s) {
    switch (s.count) {
    case 1:
        s.v[0].u = 1.0f;
        return;

    case 2: {
        const Vec4& a = s.v[0].w;
        const Vec4& b = s.v[1].w;
        const f32 t = ClosestPointOnSegment(a, b, Vec4{});
        if (t <= 0.0f) {
            s.count = 1;
            s.v[0].u = 1.0f;
        } else if (t >= 1.0f) {
            s.v[0] = s.v[1];
            s.count = 1;
            s.v[0].u = 1.0f;
        } else {
            s.v[0].u = 1.0f - t;
            s.v[1].u = t;
        }
        return;
    }

    case 3: {
        f32 w[3];
        if (ClosestOnTriangle(s.v[0].w, s.v[1].w, s.v[2].w, w)) {
            s.v[0].u = w[0];
            s.v[1].u = w[1];
            s.v[2].u = w[2];
            return;
        }
        // Outside the triangle: the answer is on one of its edges.
        f32 best = constants::kMaxFloat.x;
        SimplexVertex keep[2];
        i32 keepCount = 1;
        SimplexVertex bestSingle = s.v[0];
        for (i32 i = 0; i < 3; ++i) {
            const SimplexVertex& p = s.v[i];
            const SimplexVertex& q = s.v[(i + 1) % 3];
            const f32 t = ClosestPointOnSegment(p.w, q.w, Vec4{});
            const Vec4 closest = p.w + (q.w - p.w) * t;
            const f32 d = LengthSquared3(closest);
            if (d < best) {
                best = d;
                if (t <= 0.0f) {
                    keepCount = 1;
                    bestSingle = p;
                } else if (t >= 1.0f) {
                    keepCount = 1;
                    bestSingle = q;
                } else {
                    keepCount = 2;
                    keep[0] = p;
                    keep[0].u = 1.0f - t;
                    keep[1] = q;
                    keep[1].u = t;
                }
            }
        }
        if (keepCount == 1) {
            s.v[0] = bestSingle;
            s.v[0].u = 1.0f;
            s.count = 1;
        } else {
            s.v[0] = keep[0];
            s.v[1] = keep[1];
            s.count = 2;
        }
        return;
    }

    default: {
        // Tetrahedron: if the origin is behind every face the shapes overlap and we are done;
        // otherwise the answer lies on whichever face is closest.
        const Vec4 centre =
            (s.v[0].w + s.v[1].w + s.v[2].w + s.v[3].w) * 0.25f;
        constexpr i32 faces[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
        f32 best = constants::kMaxFloat.x;
        Simplex bestSimplex;
        bool outside = false;
        for (const auto& face : faces) {
            const Vec4& a = s.v[face[0]].w;
            const Vec4& b = s.v[face[1]].w;
            const Vec4& c = s.v[face[2]].w;
            Vec4 n = Cross3(b - a, c - a);
            if (Dot3(n, centre - a) > 0.0f) {
                n = -n;
            }
            if (Dot3(n, Vec4{} - a) < 0.0f) {
                continue;  // origin behind this face
            }
            outside = true;
            Simplex trial;
            trial.count = 3;
            for (i32 k = 0; k < 3; ++k) {
                trial.v[k] = s.v[face[k]];
            }
            SolveSimplex(trial);
            Vec4 closest{};
            for (i32 k = 0; k < trial.count; ++k) {
                closest = closest + trial.v[k].w * trial.v[k].u;
            }
            const f32 d = LengthSquared3(closest);
            if (d < best) {
                best = d;
                bestSimplex = trial;
            }
        }
        if (!outside) {
            return;  // origin enclosed: the shapes overlap
        }
        s = bestSimplex;
        return;
    }
    }
}

Vec4 ClosestPoint(const Simplex& s) {
    Vec4 p{};
    for (i32 i = 0; i < s.count; ++i) {
        p = p + s.v[i].w * s.v[i].u;
    }
    return p;
}

}  // namespace

// The geometric helpers below divide rather than routing through Rcp, and that is deliberate:
// the approximate-math contract covers the solver path, not the barycentric weights. An
// approximate reciprocal here would cost the exact 0.5 on a symmetric simplex, turning a
// witness of exactly (0,0,0) into 3e-08 and unsettling every regression value pinned on it.
// Normalisation still goes through Rsqrt, which is where the approximate arithmetic is
// actually observable.
f32 ClosestPointOnSegment(const Vec4& a, const Vec4& b, const Vec4& p) {
    const Vec4 ab = b - a;
    const f32 denom = LengthSquared3(ab);
    if (denom <= kEpsilon) {
        return 0.0f;
    }
    return std::clamp(Dot3(p - a, ab) / denom, 0.0f, 1.0f);
}

void ClosestPointsBetweenSegments(const Vec4& p1, const Vec4& q1, const Vec4& p2, const Vec4& q2,
                                  f32& s, f32& t) {
    const Vec4 d1 = q1 - p1;
    const Vec4 d2 = q2 - p2;
    const Vec4 r = p1 - p2;
    const f32 a = LengthSquared3(d1);
    const f32 e = LengthSquared3(d2);
    const f32 f = Dot3(d2, r);

    if (a <= kEpsilon && e <= kEpsilon) {
        s = t = 0.0f;
        return;
    }
    if (a <= kEpsilon) {
        s = 0.0f;
        t = std::clamp(f / e, 0.0f, 1.0f);
        return;
    }
    const f32 c = Dot3(d1, r);
    if (e <= kEpsilon) {
        t = 0.0f;
        s = std::clamp(-c / a, 0.0f, 1.0f);
        return;
    }

    const f32 b = Dot3(d1, d2);
    const f32 denom = a * e - b * b;
    // Parallel segments leave the denominator at zero. Taking s = 0 there is what makes two
    // overlapping parallel capsules produce one contact point rather than two -- deliberate,
    // and load-bearing: a two-point manifold would resist rolling about the shared axis,
    // which such capsules must stay free to do.
    s = denom > kEpsilon ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
    t = (b * s + f) / e;
    if (t < 0.0f) {
        t = 0.0f;
        s = std::clamp(-c / a, 0.0f, 1.0f);
    } else if (t > 1.0f) {
        t = 1.0f;
        s = std::clamp((b - c) / a, 0.0f, 1.0f);
    }
}

SupportProxy::SupportProxy(const Shape& shape) {
    std::visit(
        [this](const auto& s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, Sphere>) {
                local_[0] = s.centre;
                count_ = 1;
                radius_ = s.radius;
            } else if constexpr (std::is_same_v<T, Capsule>) {
                local_[0] = s.p1;
                local_[1] = s.p2;
                count_ = 2;
                radius_ = s.radius;
            } else if constexpr (std::is_same_v<T, Triangle>) {
                local_[0] = s.v1;
                local_[1] = s.v2;
                local_[2] = s.v3;
                count_ = 3;
                radius_ = s.radius;
            } else if constexpr (std::is_same_v<T, Polytope>) {
                external_ = s.hull.vertices.data();
                count_ = static_cast<i32>(s.hull.vertices.size());
                radius_ = s.margin;
            } else {
                // A mesh never reaches GJK whole; the mesh paths hand this constructor one
                // *triangle* at a time instead, so an empty proxy is the correct answer here.
                count_ = 0;
                radius_ = 0.0f;
            }
        },
        shape);
}

i32 SupportProxy::Support(const Vec4& direction) const {
    i32 best = 0;
    f32 bestDot = Dot3(Vertex(0), direction);
    for (i32 i = 1; i < count_; ++i) {
        const f32 d = Dot3(Vertex(i), direction);
        if (d > bestDot) {
            bestDot = d;
            best = i;
        }
    }
    return best;
}

DistanceOutput Distance(const DistanceInput& input, GjkCache& cache) {
    const SupportProxy& a = input.proxyA;
    const SupportProxy& b = input.proxyB;

    Simplex simplex;
    const auto emit = [&](i32 indexA, i32 indexB) {
        SimplexVertex v;
        v.indexA = indexA;
        v.indexB = indexB;
        v.wA = ToWorld(input.xfA, a.Vertex(indexA));
        v.wB = ToWorld(input.xfB, b.Vertex(indexB));
        v.w = v.wA - v.wB;
        return v;
    };

    // Warm start from the cached simplex where it is still valid; otherwise begin at vertex 0.
    for (i32 i = 0; i < cache.count && i < 4; ++i) {
        if (cache.indexA[static_cast<usize>(i)] >= a.Count() ||
            cache.indexB[static_cast<usize>(i)] >= b.Count()) {
            simplex.count = 0;
            break;
        }
        simplex.v[simplex.count++] =
            emit(cache.indexA[static_cast<usize>(i)], cache.indexB[static_cast<usize>(i)]);
    }
    if (simplex.count == 0) {
        simplex.v[0] = emit(0, 0);
        simplex.count = 1;
    }

    i32 saveA[4], saveB[4];
    bool enclosed = false;
    DistanceOutput out;
    for (out.iterations = 0; out.iterations < kMaxIterations; ++out.iterations) {
        const i32 saveCount = simplex.count;
        for (i32 i = 0; i < saveCount; ++i) {
            saveA[i] = simplex.v[i].indexA;
            saveB[i] = simplex.v[i].indexB;
        }

        SolveSimplex(simplex);
        if (simplex.count == 4) {
            // The origin is enclosed, so the cores overlap and there is no closest *pair* to
            // report -- recovering a penetration depth from here needs EPA, which the manifold
            // entries do not ask for: they resolve an overlap by face query instead. Weight the
            // simplex evenly so the witnesses are at least well defined.
            enclosed = true;
            for (i32 i = 0; i < 4; ++i) {
                simplex.v[i].u = 0.25f;
            }
            break;
        }

        const Vec4 closest = ClosestPoint(simplex);
        if (LengthSquared3(closest) <= kEpsilon * kEpsilon) {
            break;
        }

        const Vec4 direction = -closest;
        const i32 indexA = a.Support(ToLocal(input.xfA, direction));
        const i32 indexB = b.Support(ToLocal(input.xfB, -direction));

        // A repeated vertex pair means the support cannot improve on what we have.
        bool duplicate = false;
        for (i32 i = 0; i < saveCount; ++i) {
            if (saveA[i] == indexA && saveB[i] == indexB) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            break;
        }
        simplex.v[simplex.count++] = emit(indexA, indexB);
    }

    for (i32 i = 0; i < simplex.count; ++i) {
        out.pointA = out.pointA + simplex.v[i].wA * simplex.v[i].u;
        out.pointB = out.pointB + simplex.v[i].wB * simplex.v[i].u;
    }
    const Vec4 delta = out.pointB - out.pointA;
    const f32 lengthSquared = LengthSquared3(delta);
    out.distance = !enclosed && lengthSquared > 0.0f ? lengthSquared * Rsqrt(lengthSquared) : 0.0f;
    out.normal = !enclosed && lengthSquared > 0.0f ? delta * Rsqrt(lengthSquared) : Vec4{};

    cache.count = simplex.count;
    for (i32 i = 0; i < simplex.count; ++i) {
        cache.indexA[static_cast<usize>(i)] = simplex.v[i].indexA;
        cache.indexB[static_cast<usize>(i)] = simplex.v[i].indexB;
    }

    if (input.useRadii) {
        const f32 total = a.Radius() + b.Radius();
        if (out.distance > total && out.distance > kEpsilon) {
            out.distance -= total;
            out.pointA = out.pointA + out.normal * a.Radius();
            out.pointB = out.pointB - out.normal * b.Radius();
        } else {
            // Overlapping once the radii are counted: collapse both witnesses to the midpoint,
            // which is what the manifold entries expect to receive.
            const Vec4 mid = (out.pointA + out.pointB) * 0.5f;
            out.pointA = mid;
            out.pointB = mid;
            out.distance = 0.0f;
        }
    }
    return out;
}

}  // namespace snowball
