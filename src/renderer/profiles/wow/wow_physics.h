#pragma once

// ============================================================================
// `.phys` rigid-body simulation — the WoW half of Domino's glue contract.
//
// A `.phys` file (or an M2's inline PFDC chunk) is a small ragdoll: one rigid
// body per bone, a run of collision shapes per body, and joints chaining them.
// In practice WoW authors it for **cloth and tassels** — cloaks, tabards,
// beards, chains — not for corpses. `PhysicsBodyType::Dynamic` bodies are the
// swinging segments; there is exactly one kinematic body, which is the anchor
// the animation drives.
//
// The engine underneath is Snowball, our re-implementation of Blizzard's
// Domino (../Domino). What the client does *with* it — the drive, the writeback
// and the conventions at the boundary — is recovered in `DOMINO_GLUE.md` §2-§5
// from a WoW 6.0.1 client, and the section references below point at it.
//
// This stage is compiled only under WDX_ENABLE_PHYSICS.
// ============================================================================

#include "whiteout/flakes/pose_stage.h"

#include <memory>
#include <vector>

namespace whiteout {
namespace m2 {
struct Model;
struct PolytopeShape;
}
} // namespace whiteout

namespace whiteout::flakes::renderer::profiles::wow {

/// @brief Build the `.phys` stage for @p model, or nullptr if it has none.
///
/// Returns nullptr rather than an inert stage when the model carries no
/// physics, no dynamic bodies, or no bone a body can attach to: an empty stage
/// still costs a virtual call and a claim scan every frame, per actor.
///
/// One `snowball::Scene` per actor, which is not the arrangement the client
/// uses and is nevertheless equivalent for this content. WoW runs every model
/// in a single `dmWorld` and separates them with a per-model collision filter
/// whose include mask is **zero** and whose group index is positive and unique
/// (`DOMINO_GLUE.md` §3.3) — so a model's fixtures collide with each other and
/// with nothing else in the world, which is exactly what per-actor isolation
/// gives us for free. If world collision ever arrives, that stops being true
/// and the scenes have to merge.
std::unique_ptr<animation::IPoseStage> CreateWowPhysicsStage(const ::whiteout::m2::Model& model);

/// @brief Palette -> rigid-body frame -> palette, with no simulation in between.
///
/// The stage's two boundary conversions have to be exact inverses, and nothing
/// observable about a finished frame can tell you whether they are: transposing
/// one of them hands every body its own **conjugate** rotation, which is stable,
/// physically plausible, and wrong only in orientation. The rig still hangs,
/// settles and collides; the mesh twists.
///
/// Exposed because it cannot be checked through `Run`. Snowball's joint position
/// pass applies a *full* geometric correction, so even a one-millisecond step
/// moves a seeded rig by more than the error being looked for, and the signal is
/// buried under the settle. Returns @p palette unchanged when the conversions
/// agree.
Matrix44f RoundTripBoneFrame(const Matrix44f& palette, const Vector3f& pivot);

/// @brief A `PLYT` hull's undirected edges, as index pairs into its vertices.
///
/// Twins sit at adjacent indices (`twinOffset` is only ever +1 or -1, in equal
/// numbers), so taking the ones that step *forward* walks every edge exactly
/// once. Out-of-range and degenerate pairs are dropped rather than clamped: a
/// hull whose tables disagree should come out sparse, not folded.
///
/// Exposed for the overlay and for testing. The corpus's `.phys` files carry no
/// box shapes at all and 182 polytopes across 19 of 40 models, so this is the
/// hull path that actually renders — and the one that cannot be reached through
/// `M2ModelAdapter` in a test, every polytope-bearing corpus model being one
/// whose `.skin` profiles are not beside it.
std::vector<::whiteout::u16> WowPolytopeEdges(const ::whiteout::m2::PolytopeShape& hull);

} // namespace whiteout::flakes::renderer::profiles::wow
