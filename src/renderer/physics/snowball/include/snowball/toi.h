//===----------------------------------------------------------------------===//
// snowball/toi.h -- time of impact: when, within one step, two sweeps first touch.
//
// Conservative advancement in the style of b2TimeOfImpact, taken to 3D: a separation function
// linearises the gap along an axis, and root-finding alternates bisection with false position.
// Three details are deliberate parts of the engine's numeric contract, not defects to correct:
//
//   * **A fourth separation type.** Alongside points, faceA and faceB there is edge-edge,
//     picked when the GJK simplex ends on an edge of each shape. The axis is the cross of
//     the two edges recomputed at every trial time -- and when that cross degenerates, the
//     whole outer iteration is retried with edge-edge disabled and does not count against
//     the iteration cap.
//   * **The tolerances are this engine's, not stock.** target = max(rA + rB - 0.015, 0.005)
//     with a band of +/-0.00125 and a root tolerance of 1.25e-4; caps of 20 outer / 50 root /
//     32 axis iterations. Every pinned impact time depends on these exact numbers.
//   * **The false position step divides through `Rcp`.** The approximate reciprocal (rcpps
//     plus a Newton step), not a true divide -- and so does the sweep advance's `beta`.
//     Swapping in an exact divide moves the roots by ulps and every regression value pinned
//     downstream of them.
//
// The overlap state reports the trial time it was discovered at, not zero -- a distinction
// that only matters to a caller that rescales, which is exactly what ProcessToiCandidate does.
//===----------------------------------------------------------------------===//
#pragma once

#include "snowball/common_types.h"
#include "snowball/distance.h"
#include "snowball/transform.h"

namespace snowball {

/// How far a body's centre of mass moves and turns across one step. `alpha0` is the fraction
/// of the step already consumed: a sweep that has been advanced to its first impact starts
/// there rather than at zero, and every later time-of-impact answer is rescaled through it.
///
/// The local centre is deliberately NOT stored here: the body already owns one, and a second
/// copy is how the two drift apart.
struct Sweep {
    Vec4 c{};                                  ///< centre of mass at the sweep END
    Vec4 q{constants::kQuatIdentity};          ///< rotation at the sweep END
    Vec4 c0{};                                 ///< centre of mass at the sweep START
    Vec4 q0{constants::kQuatIdentity};         ///< rotation at the sweep START
    f32 alpha0{0.0f};
};

/// @brief The pose at fraction `t` of the sweep: nlerp for the rotation -- with the dot-sign
/// flip and the approximate-rsqrt normalise -- and a lerp for the centre, with the body origin
/// re-derived from the local centre.
Transform SweepTransform(const Sweep& sweep, const Vec4& localCentre, f32 t);

/// @brief Move the sweep's START to fraction `alpha`, leaving the end where it is.
///
/// A no-op unless `alpha0 < alpha` and at least FLT_EPSILON of the step remains: the first
/// guard stops the start moving backwards, the second keeps `beta`'s reciprocal away from a
/// vanishing remainder. `beta` goes through `Rcp`, not a true divide (see the header note).
void AdvanceSweep(Sweep& sweep, f32 alpha);

/// The per-body sub-step budget: a body that has hit this many impacts in one step is
/// finalised at the step end and gives up. Reset every step the body qualifies as fast.
inline constexpr i32 kMaxToiSubSteps = 12;

/// The most static contacts one candidate considers per sub-step. Collection simply stops at
/// the cap, so an over-detailed static neighbourhood silently loses candidates.
inline constexpr i32 kMaxToiContacts = 32;

/// The TOI island's position pass: up to 20 iterations at a 0.75 baumgarte, against the main
/// solve's 0.2 -- an impact is pushed out in a few sub-steps or not at all.
inline constexpr i32 kToiPositionIterations = 20;
inline constexpr f32 kToiBaumgarte = 0.75f;

/// A time of impact this close to the step end snaps to it (1 - 1.192e-5; the exact float32
/// bit pattern 0x3F7FFF38 is the contract, not a rounding of anything simpler).
inline constexpr f32 kToiSnapToOne = 0.99998808f;

/// The TOI island's velocity clamps: one sub-integrate may move a body at most 2 units and
/// turn it at most pi/4 -- an eighth of a turn, a deliberately tight rotation budget that
/// every fast-rotation trajectory is pinned against. Widening it is a behaviour change.
inline constexpr f32 kMaxTranslation = 2.0f;
inline constexpr f32 kMaxRotation = 0.7853981852531433f;  // pi/4 as float32 (0x3F490FDB)

enum class ToiState : i32 {
    Touching = 0,   ///< first contact found at `t`
    Separated = 1,  ///< never closer than the target across the whole sweep; `t` is 1
    Overlap = 2,    ///< already deeper than the target at `t` -- the sweep started too late
};

struct ToiInput {
    SupportProxy proxyA;
    SupportProxy proxyB;
    Sweep sweepA;
    Sweep sweepB;
    Vec4 localCentreA{};
    Vec4 localCentreB{};
};

struct ToiOutput {
    ToiState state{ToiState::Separated};
    f32 t{1.0f};
    Vec4 witnessA{};      ///< closest points at the last distance query, in world space
    Vec4 witnessB{};
    i32 iterations{0};        ///< outer iterations, degenerate retries included
    i32 rootIterations{0};    ///< the worst single root-find
};

/// @brief When, in [0, 1] across the two sweeps, the shapes first come within `target` of
/// each other. Always solves the full interval; callers rescale through their own `alpha0`.
///
/// `cache` warm-starts the distance queries and receives their final simplex. The convex
/// caller deliberately passes none and starts cold every call, but the mesh path keeps one
/// cache **per triangle** across sub-steps and steps, which is what makes a body sliding
/// along terrain re-answer each triangle in one iteration.
ToiOutput TimeOfImpact(const ToiInput& input, GjkCache* cache = nullptr);

}  // namespace snowball
