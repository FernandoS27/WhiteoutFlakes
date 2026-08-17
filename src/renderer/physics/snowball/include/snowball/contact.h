//===----------------------------------------------------------------------===//
// snowball/contact.h -- a pair that persists, and the identity that carries its impulse.
//
// The narrowphase produces a manifold for one instant. This is what makes a *sequence* of them
// into one contact: the manifold is rebuilt every step, and each new point inherits the
// accumulated normal impulse of the old point sharing its feature id. That inheritance is what
// lets eight iterations hold a stack which would otherwise sag, so an engine that gets it
// wrong produces soft, sinking contacts rather than an obvious failure.
//
// Two properties of the key are worth stating because both are easy to lose:
//
//   * **The id is matched, never the index.** The matcher scans the old manifold for an
//     unclaimed point of equal id, so a manifold that emits its points in a different order
//     still warm starts correctly -- and one whose ids track the output *slot* never notices
//     that the third point of this step is the second point of the last one. That is why the
//     ids the narrowphase emits name incident geometry rather than array position.
//   * **The compare covers eight bytes** -- the four-byte feature id plus four bytes that are
//     always written zero. Leave those as uninitialised padding and points match at random,
//     warm-starting each contact from another one's impulse. The id here is a single 64-bit
//     value for the same reason: there is nothing beside it left to forget to write.
//
// Friction accumulates per *manifold*, not per point: one vector for the whole contact patch,
// carried across wholesale rather than matched point by point -- so it survives even a step in
// which every individual point fails to match and restarts from zero.
//===----------------------------------------------------------------------===//
#pragma once

#include <array>
#include <vector>

#include "snowball/common_types.h"
#include "snowball/distance.h"
#include "snowball/manifold.h"
#include "snowball/transform.h"

namespace snowball {

/// A manifold point as it lives between steps: the geometry the narrowphase produced, plus the
/// impulse the solver accumulated on it and the identity the two are joined by.
struct ManifoldPoint {
    Vec4 point{};
    f32 separation{0.0f};
    f32 normalImpulse{0.0f};
    u64 id{0};
    bool isNew{true};
};

struct Manifold {
    i32 count{0};
    Vec4 normal{};
    std::array<ManifoldPoint, 4> points{};

    /// Accumulated friction for the whole patch. Per manifold, not per point, by design.
    Vec4 frictionImpulse{};

    /// Identifies this allocation, so "the same manifold, not a fresh one" is answerable. Zero
    /// until the contact first touches.
    u64 token{0};
};

/// @brief Carry `previous`'s accumulated impulses onto `fresh`, matching on feature id.
///
/// Runs as the tail of every manifold rebuild, and is the only mechanism warm starting has. A
/// point that finds no match starts from zero and is flagged new; an old point may be claimed
/// only once, so duplicate ids cannot double-count.
void MatchManifold(const Manifold& previous, Manifold& fresh);

/// One queried triangle's persistent state. An entry survives as long as its triangle keeps
/// answering the query, and its warmth is the point -- three consumers share it and never
/// touch each other's slice:
///
///   * `cache` warm-starts the TOI core AND the discrete sphere/capsule colliders;
///   * `sat` belongs to the discrete polytope collider alone (the write-on-full,
///     never-refresh-on-hit protocol in manifold.cpp);
///   * `axis` is TOI-only: the last separated result's witness direction, quantised, so a
///     separated triangle can be dismissed with two support calls instead of an advance.
struct MeshTriangleEntry {
    i32 triangle{-1};
    GjkCache cache{};              ///< `count == 0` is cold, which is what a fresh entry carries
    SatCache sat{};                ///< `type == 0` is cold; the rest of it starts stale on purpose
    std::array<i16, 3> axis{};     ///< `normalize(wB - wA) * 32767`, truncated; zero when unset
};

/// Everything a mesh contact remembers between steps besides its manifolds: the fattened box
/// its entries were queried under, and the entries themselves, ascending by triangle index.
/// Empty (and never touched) on a convex contact.
struct MeshContactState {
    bool primed{false};
    Aabb cached{};   ///< 1.5x-fattened query box, in the mesh's own frame
    std::vector<MeshTriangleEntry> entries;
};

/// @brief The pair itself. Existing and touching are different states.
///
/// Proxies are fattened by +/-0.1, so a contact exists well before the shapes meet -- roughly a
/// fifth of a unit early. Conflating the two double-counts every near miss, which is why
/// `manifolds` stays empty rather than holding a zero-point manifold.
///
/// For a mesh contact the mesh is **always** body A -- pair creation orders fixtures by
/// descending shape type and asserts it -- and `mesh` carries the triangle buffer. Mesh-mesh
/// pairs are never created at all.
struct Contact {
    i32 bodyA{-1};
    i32 bodyB{-1};
    i32 fixtureA{-1};
    i32 fixtureB{-1};
    f32 friction{0.0f};
    f32 restitution{0.0f};
    bool touching{false};
    std::vector<Manifold> manifolds;
    MeshContactState mesh;

    /// The convex record's warm axis, consumed only when both fixtures are polytopes -- the
    /// sphere and capsule colliders are closed-form and keep no per-pair state, so this cache
    /// is the only warm data a convex contact carries besides the manifold itself.
    SatCache sat{};
};

/// @brief The material mixing rules, Box2D's: geometric mean for friction, max for restitution.
///
/// Note that both rules are the identity when the two fixtures share a material, so a scene
/// that never mixes materials exercises neither -- only differing inputs can tell these apart
/// from any other symmetric combination.
f32 MixFriction(f32 a, f32 b);
f32 MixRestitution(f32 a, f32 b);

}  // namespace snowball
