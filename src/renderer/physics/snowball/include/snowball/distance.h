//===----------------------------------------------------------------------===//
// snowball/distance.h -- GJK: closest points between two convex shapes.
//
// Every shape reduces to a point set plus a radius -- a sphere is one point, a capsule two, a
// triangle three, a polytope its hull vertices -- and the solver works on the *cores*, folding
// the radii back in at the end. That is why the proxy vertex count doubles as a shape-type
// tag, and why a shape tagged wrongly reads its vertices from the wrong place and returns
// plausible garbage.
//
// The warm-start cache is the interesting part rather than an optimisation: feeding the last
// simplex back cuts the search to about one iteration for a pair that has barely moved, and
// the contact solver depends on it every step.
//===----------------------------------------------------------------------===//
#pragma once

#include <array>

#include "snowball/common_types.h"
#include "snowball/shape.h"
#include "snowball/transform.h"

namespace snowball {

/// @brief A shape as the distance solver sees it: core vertices and a radius.
class SupportProxy {
public:
    SupportProxy() = default;
    explicit SupportProxy(const Shape& shape);

    i32 Count() const { return count_; }
    f32 Radius() const { return radius_; }
    const Vec4& Vertex(i32 i) const { return external_ ? external_[i] : local_[i]; }

    /// @brief The vertex furthest along `direction`, and its index.
    i32 Support(const Vec4& direction) const;

private:
    std::array<Vec4, 3> local_{};
    const Vec4* external_{nullptr};
    i32 count_{0};
    f32 radius_{0.0f};
};

/// @brief The simplex a previous query ended on, fed back to start the next one near the answer.
///
/// The explicit `count` is load-bearing: a cache laid out as index arrays terminated by a
/// sentinel, with no count field, reads its all-zero value as a valid four-vertex simplex of
/// vertex 0 and warm-starts from nonsense. With the count, a value-initialised cache is
/// simply empty and cannot express that trap.
struct GjkCache {
    i32 count{0};
    std::array<i32, 4> indexA{};
    std::array<i32, 4> indexB{};
};

struct DistanceInput {
    SupportProxy proxyA;
    SupportProxy proxyB;
    Transform xfA;
    Transform xfB;
    /// Subtract the two radii and move the witnesses onto the surfaces. Off measures the cores.
    bool useRadii{false};
};

struct DistanceOutput {
    Vec4 pointA{};
    Vec4 pointB{};
    Vec4 normal{};
    f32 distance{0.0f};
    i32 iterations{0};
};

/// @brief Closest points between two convex shapes. `cache` is read and then overwritten.
DistanceOutput Distance(const DistanceInput& input, GjkCache& cache);

/// @brief The closest point on a segment to `p`, as a parameter in [0, 1].
f32 ClosestPointOnSegment(const Vec4& a, const Vec4& b, const Vec4& p);

/// @brief Closest points between two segments, as parameters in [0, 1].
///
/// Parallel segments are the degenerate case and resolve to the first segment's start, which
/// is what makes two overlapping parallel capsules report a *single* contact point rather than
/// a line of them -- deliberate, and part of the contact contract rather than a bug to fix.
void ClosestPointsBetweenSegments(const Vec4& p1, const Vec4& q1, const Vec4& p2, const Vec4& q2,
                                  f32& s, f32& t);

}  // namespace snowball
