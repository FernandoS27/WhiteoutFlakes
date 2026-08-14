#pragma once

/// @file pose_request.h
/// @brief What to evaluate an animation source at — the parameter object
///        `IAnimationSource::Evaluate` takes.
///
/// A `(sequenceIdx, timeMs)` pair cannot express what the formats after
/// Warcraft III need. World of Warcraft blends two clips per bone and assigns
/// clips per *key bone*; StarCraft II blends an unbounded priority-ordered
/// layer list. Both need a list of clips with per-clip scope and weight.
///
/// **Warcraft III passes exactly one clip and none of the rest of this is
/// live.** The full shape lands anyway, because the alternative is breaking
/// the signature a second time later — and later means four implementations
/// instead of three, with no byte-identical gate to check them against. Every
/// field below is here because a specific engine demands it; none is here for
/// symmetry.
///
/// What the core deliberately does *not* offer is blending two `FrameState`s
/// after the fact. Blending belongs inside the adapter at track-sample time,
/// because that is the only place the semantics are known: SC2 slerps against
/// a running weight budget, WoW slerps toward identity by a hierarchical
/// modificator, and non-bone channels (emission rates, texture transforms,
/// event firing) have no meaningful blend at all.

#include "types.h"

#include <span>

namespace whiteout::flakes {

/// @brief How a clip behaves for a property it has no track for.
enum class ClipMask : u8 {
    /// @brief Contribute nothing and consume no weight budget, leaving it for
    ///        lower-priority layers. StarCraft II's abstain flag.
    Abstain = 0,
    /// @brief Contribute the animref default, which pulls the node toward bind
    ///        pose. The distinction from @ref Abstain is not cosmetic: getting
    ///        it wrong produces a subtly wrong pose rather than an obviously
    ///        broken one.
    FillDefault = 1,
};

/// @brief One clip in a pose request.
struct ClipRef {
    /// @brief Index into the source's sequence table. `-1` ⇒ bind pose.
    i32 sequence = -1;
    /// @brief Local time within that sequence, in ms.
    i32 timeMs = 0;
    /// @brief Blend weight against the other clips in the request.
    f32 weight = 1.0f;
    /// @brief Per-clip playback rate. Per-*bone* in WoW and per-*layer* in
    ///        SC2; both collapse to per-clip once @ref rootNode scopes it.
    f32 speed = 1.0f;
    bool loop = true;
    ClipMask mask = ClipMask::FillDefault;
    /// @brief The subtree this clip drives; `-1` ⇒ the whole skeleton.
    ///
    /// Expresses WoW's per-key-bone assignment without a per-bone weight
    /// array — a clip drives a subtree, exactly as `UpdateBonesSeq` propagates
    /// a sequence pointer down to bones that do not own one.
    i32 rootNode = -1;
};

/// @brief A host-supplied transform applied to one node after its local TRS is
///        composed and before the parent multiply.
///
/// Half of the entire "post-track modifier" surface the renderer owes: turret
/// aim, look-at and IK are host concerns in both WoW and SC2, so the renderer
/// provides the hook and not the solver.
struct NodeOverride {
    i32 node = -1;
    Matrix44f m = Matrix44f::identity();
    /// @brief Replace the sampled local transform rather than compose with it.
    bool replace = false;
};

/// @brief Everything an animation source needs to produce one `FrameState`.
struct PoseRequest {
    /// @brief The clips to evaluate. Warcraft III always passes exactly one.
    ///
    /// A view, not storage: the caller owns the clips and must keep them alive
    /// across the call. See @ref OneClip.
    std::span<const ClipRef> clips;

    /// @brief Wall-clock time for global-sequence tracks, in ms. `-1` ⇒ use
    ///        the primary clip's `timeMs`.
    ///
    /// Outside the clip list because global-sequence tracks never participate
    /// in blending in either engine — WoW skips the secondary block entirely
    /// when a track carries a global sequence id.
    i32 globalTimeMs = -1;

    /// @brief World-space root transform of the actor being evaluated.
    Matrix44f world = Matrix44f::identity();

    /// @brief Camera position in world space, for camera-anchored billboards.
    Vector3f cameraPos = {0.0f, 0.0f, 0.0f};

    /// @brief Per-node transforms applied after local TRS composition. Inert
    ///        for Warcraft III.
    std::span<const NodeOverride> overrides;

    /// @brief Nodes the host drives entirely; the sampler skips them. Inert
    ///        for Warcraft III.
    std::span<const i32> externallyDriven;

    /// @brief The first clip, or a bind-pose `ClipRef` when there are none.
    ///
    /// What every single-clip adapter reads. "No clips" and "sequence -1" are
    /// the same request, so they resolve to the same value rather than making
    /// each adapter handle an empty span.
    ClipRef PrimaryClip() const {
        return clips.empty() ? ClipRef{} : clips.front();
    }

    /// @brief Build a one-clip request over a clip the caller owns.
    ///
    /// @p clip must outlive the request — `clips` is a view. The rvalue
    /// overload is deleted rather than left to dangle, so
    /// `OneClip(ClipRef{...})` is a compile error instead of a read of a
    /// destroyed temporary.
    static PoseRequest OneClip(const ClipRef& clip) {
        PoseRequest r;
        r.clips = std::span<const ClipRef>(&clip, 1);
        return r;
    }
    static PoseRequest OneClip(ClipRef&&) = delete;
};

} // namespace whiteout::flakes
