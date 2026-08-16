#pragma once

/// @file pose_stage.h
/// @brief Per-actor corrections that run on the sampled pose before it reaches
///        the skinning palette.
///
/// StarCraft II runs terrain IK and turret aiming inside the model update,
/// between the skeleton and the palette (design B10). They cannot live in the
/// sampler: they need per-actor state across frames (a rate-limited IK goal, a
/// turret's slerp cursor) and per-frame inputs no model file contains (ground
/// height, an aim target). `IModelSource::Evaluate` is `const` and stateless
/// precisely so one adapter can serve many actors, so this is where the
/// stateful, host-fed half goes.
///
/// **Ordering is part of the contract.** Solvers first — they correct the
/// animated pose — and physics last, because it *consumes* the corrected pose.
/// `CreatePoseStages` appends in that order and the runner never reorders.
///
/// **A stage that writes a bone owns its descendants.** IK re-composes its
/// chain root-first; a future ragdoll drives every bone it claims. There is no
/// hidden re-composition pass afterwards to paper over a stage that moved a
/// parent and left its children behind. That is a bug in the stage.
///
/// The list, the context and the claim storage are all plural and ordered from
/// day one. Physics is a known future stage (design §7.4), and widening a
/// single-solver virtual into a list later would re-break every implementation
/// — the same argument that made `ClipPlaylist` take N plays before anything
/// played more than one.
///
/// It is a *public* header, unlike the `src/renderer/animation/` location the
/// design sketched, for one forced reason: `IModelSource::CreatePoseStages` is
/// a virtual on a public interface hosts implement, so its parameter type has
/// to be reachable from `model_source.h`.

#include "whiteout/flakes/frame_state.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace whiteout::flakes::renderer::animation {

/// @brief A bone a stage or the host has taken over.
///
/// The seam ragdoll needs, built now for the solvers' benefit even though
/// neither IK nor the turret claims a bone (SC2 marks IK chains with bit 5,
/// which explicitly does *not* suppress sampling — only look-at, turret-by-actor
/// and ragdoll use the bit-0 claim). Writing it costs one struct; retrofitting
/// a writer that is not the host would touch `PoseRequest`, `Actor` and every
/// caller.
struct BoneClaim {
    i32 node = -1;
    /// @brief Model-space transform to force. Echoed into the next frame's
    ///        `PoseRequest::overrides` so the sampler skips this bone.
    Matrix44f transform = Matrix44f::identity();
};

/// @brief Everything a stage may read that is not in the model.
///
/// The widening point. Gravity, collision queries and pause state land here
/// when physics arrives; `IPoseStage::Run` and `CreatePoseStages` should never
/// need another signature.
struct PoseStageContext {
    /// @brief Parent index per node, `-1` for a root.
    ///
    /// A span of what the actor already keeps rather than a `SkeletonData`
    /// pointer: nothing assembles one of those per frame, and neither current
    /// stage reads more than the hierarchy. A stage needing inverse binds can
    /// widen this — that is what the context is for.
    std::span<const i32> nodeParents;
    /// @brief Real frame delta. The IK goal's rate limit and any future
    ///        integrator are in *real* time, not animation time — a paused
    ///        actor's feet still settle.
    i32 frameDtMs = 0;
    Matrix44f world = Matrix44f::identity();

    /// @brief Ground height under a model-space point.
    ///
    /// Host policy, not renderer policy: the viewer registers a flat plane, a
    /// game host would query its terrain. Returns false for "no surface", which
    /// the IK stage turns into its documented fallback rather than a guess.
    /// @param up   accept a surface at most this far above @p pos
    /// @param down …and this far below
    std::function<bool(const Vector3f& pos, f32 up, f32 down, f32& outZ)> queryGround;

    /// @brief Model-space point this actor's turrets should aim at. Empty
    ///        releases them back to their animation.
    std::optional<Vector3f> aimTarget;
};

/// @brief One correction, instantiated per actor because it owns state.
class IPoseStage {
public:
    virtual ~IPoseStage() = default;

    /// @brief Correct @p fs in place.
    ///
    /// Called between `Evaluate` and `ApplyFrameState`, so whatever this writes
    /// is what the palette is built from — there is exactly one palette build
    /// per frame here, unlike SC2's build-twice (accepted deviation, §7.4.2:
    /// attachments see the current frame's correction rather than a stale one).
    virtual void Run(model::FrameState& fs, const PoseStageContext& ctx) = 0;

    /// @brief Bones this stage took over this frame, for the next frame's
    ///        `PoseRequest`. Empty for every stage that composes a delta on top
    ///        of the animation rather than replacing it — which is both of the
    ///        current ones.
    virtual std::vector<BoneClaim> Claims() const {
        return {};
    }
};

using PoseStageList = std::vector<std::unique_ptr<IPoseStage>>;

} // namespace whiteout::flakes::renderer::animation
