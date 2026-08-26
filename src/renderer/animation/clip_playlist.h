#pragma once

/// @file clip_playlist.h
/// @brief Per-actor playback state: N logical plays with their own cursors and
///        blend envelopes, projected into the `ClipRef` span `PoseRequest`
///        carries.
///
/// Warcraft III and World of Warcraft each play exactly one sequence at a time
/// and cut hard between them, so playback state was one index plus one start
/// time living on `Actor::Cursor`. StarCraft II does not work that way: an
/// actor runs an unbounded, priority-ordered stack of plays, each with its own
/// clock, weight and blend-in/out envelope, and a plain sequence switch is a
/// cross-fade rather than a cut.
///
/// This class is that capability, factored out of `Actor::Advance` so all three
/// games share one implementation. The single-play hard-cut path is a *verbatim*
/// port — `WindowSequence` below is the old arithmetic character for character,
/// including the `elapsed < 0` clamp and the roll-forward-by-whole-durations
/// loop — because Warcraft III and WoW are gated byte-identical across this
/// change and "cleaning up" the rounding would break them.
///
/// What stays outside: the actor clock itself. `Actor::cursor.actorTimeMs`
/// accumulates `dt * playbackSpeed` and is read by child actors as their
/// ancestor clock, so it is actor state, not playback state. This class takes
/// `nowMs` in that domain and never advances it.

#include "whiteout/flakes/display.h"
#include "whiteout/flakes/pose_request.h"
#include "whiteout/flakes/types.h"

#include <span>
#include <vector>

namespace whiteout::flakes::renderer::animation {

/// @brief One logical play — what the host asks for, before it is expanded
///        into whatever layers the format needs.
///
/// A play is *not* a layer. One M3 play of a split-body sequence becomes
/// several sampler layers (one per sub-track container), all sharing this
/// play's identity so the blender takes at most one contribution from it. That
/// expansion is adapter-internal; this struct is the request.
struct PlayDesc {
    i32 sequence = -1;
    /// @brief Weight before the blend envelope multiplies in.
    f32 weight = 1.0f;
    f32 speed = 1.0f;
    bool loop = true;
    /// @brief Exempt from the covered-play cull.
    bool persistent = false;
    i32 blendInMs = 0;
    /// @brief `-1` ⇒ the transition policy's blend-out.
    i32 blendOutMs = -1;
    ClipMask mask = ClipMask::FillDefault;
    i32 rootNode = -1;
    /// @brief One sub-track of the sequence, or `-1` for all of them. See
    ///        @ref ClipRef::subtrack.
    i32 subtrack = -1;
};

using PlayHandle = u32;
inline constexpr PlayHandle kInvalidPlay = 0;

/// @brief Where a raw elapsed time lands inside a sequence.
struct SequenceWindow {
    /// @brief Sequence-local sample time, `seq.startMs` included.
    i32 frameMs = 0;
    /// @brief Whole loops consumed by this call.
    i32 cycles = 0;
    /// @brief How far to advance the play's start time so `elapsed` stays in
    ///        `[0, duration)`.
    i32 startShift = 0;
    /// @brief A non-looping sequence reached its end.
    bool ended = false;
};

/// @brief The Warcraft III / WoW windowing arithmetic, extracted unchanged.
///
/// Takes plain integers rather than a `SequenceInfo` so the layered API can
/// window a play that was asked to stop at the end of a sequence the file marks
/// as looping, and so the byte-identity test can drive it directly.
///
/// @param forceLoop `Actor::ignoreNonLooping` — wraps a non-looping sequence
///        anyway, which corn-fx single-shot effects rely on.
SequenceWindow WindowSequence(i32 startMs, i32 endMs, bool nonLooping, i32 elapsed,
                              bool forceLoop);

inline SequenceWindow WindowSequence(const SequenceInfo& seq, i32 elapsed, bool forceLoop) {
    return WindowSequence(seq.startMs, seq.endMs, seq.nonLooping, elapsed, forceLoop);
}

/// @brief The playback stack for one actor.
class ClipPlaylist {
public:
    void SetTransitionPolicy(const TransitionPolicy& p) {
        policy_ = p;
    }
    const TransitionPolicy& Policy() const {
        return policy_;
    }

    // ---- the simple surface: one sequence, what the dropdown drives ----

    /// @brief Request a sequence. Deliberately lazy: the switch is *noticed* by
    ///        the next @ref Advance, exactly as the old cursor's
    ///        `rawIdx != prevActiveSequence` test was, so setting the same
    ///        index twice between frames still starts nothing.
    ///
    /// The index is stored raw (unbounded); @ref Advance does the bounding.
    /// Hosts pass out-of-range values and rely on the wrap.
    void SetActiveSequence(i32 raw) {
        requestedSequence_ = raw;
    }
    i32 ActiveSequenceIndex() const {
        return requestedSequence_;
    }

    // ---- the layered surface ----

    PlayHandle Play(const PlayDesc& desc, i32 nowMs);
    /// @param blendOutMs `-1` ⇒ the play's own blend-out, else the policy's.
    void Stop(PlayHandle h, i32 blendOutMs, i32 nowMs);
    void StopAll(i32 blendOutMs, i32 nowMs);

    /// @brief Retune a live play in place. False when the handle is gone.
    ///
    /// Separate from stop-and-replay because that would restart the clock: a
    /// host dragging a blend weight would rewind the animation on every frame
    /// of the drag. Sequence and sub-track are *not* here — changing either is
    /// a different animation and should restart.
    bool Retune(PlayHandle h, f32 weight, f32 speed, bool loop);

    // ---- global loops ----

    /// @brief The sequences that play by themselves, continuously, over
    ///        everything else — @ref SequenceInfo::alwaysPlays.
    ///
    /// Not `Play` calls, because they are not requests: they belong to the
    /// model and have to come back after anything that rebuilds the stack.
    /// The next @ref Advance reconciles the live set with this one, so a host
    /// that wants one of them silenced passes the list without it rather than
    /// hunting for its handle.
    ///
    /// StarCraft II does exactly this at `M3AnimState` init and again after
    /// every `.m3a` merge, which here is `AnimationDriver::Bind` both times.
    void SetGlobalSequences(std::vector<i32> sequences);
    const std::vector<i32>& GlobalSequences() const {
        return globalSeqs_;
    }

    /// @brief Advance every play's cursor and envelope, retire what finished,
    ///        and rebuild the clip span.
    void Advance(i32 nowMs, std::span<const SequenceInfo> seqs, bool forceLoop);

    /// @brief Global-loop overlays first, then the host's plays newest-first.
    ///        Empty only when nothing is playing.
    std::span<const ClipRef> Clips() const {
        return clips_;
    }

    /// @brief The primary play's sample time — what `AnimationDriver::TimeMs`
    ///        reports and event dispatch keys off.
    i32 PrimaryTimeMs() const {
        return primaryTimeMs_;
    }
    /// @brief Scrub. Re-bases the primary play's start so the next @ref Advance
    ///        recomputes this same frame instead of overwriting it.
    void SetPrimaryTimeMs(i32 frameMs, i32 nowMs, std::span<const SequenceInfo> seqs);

    /// @brief Wrap counter of the primary play. Corn-fx single-shot emitters
    ///        re-fire on each increment.
    i32 SequenceCycle() const {
        return sequenceCycle_;
    }

    bool Empty() const {
        return plays_.empty();
    }
    std::size_t PlayCount() const {
        return plays_.size();
    }

private:
    enum class Phase : u8 { BlendIn, Steady, BlendOut };

    struct PlayState {
        PlayDesc desc;
        PlayHandle handle = kInvalidPlay;
        /// @brief Actor-clock time this play's sequence window starts at.
        ///        Rolls forward by whole durations as the play loops.
        i32 startTimeMs = 0;
        /// @brief Actor-clock time the play began, never rolled.
        ///
        /// Separate from @ref startTimeMs precisely because that one rolls:
        /// the unwrapped elapsed a track-duration modulus needs cannot be
        /// recovered once the window start has moved.
        i32 originTimeMs = 0;
        i32 cycles = 0;
        Phase phase = Phase::Steady;
        i32 phaseStartMs = 0;
        f32 phaseStartWeight = 0.0f;
        /// @brief Blend envelope, 0..1. Multiplies `desc.weight`.
        f32 envelope = 1.0f;
        /// @brief Set when the switch that spawned this play is the one the
        ///        simple surface tracks, so `SetActiveSequence` can find it.
        bool primary = false;
        /// @brief Started by @ref SetGlobalSequences rather than by a host, so
        ///        the reconcile owns it and neither surface may retire it.
        bool global = false;
        /// @brief Held at the top of the stack, above every host play.
        ///
        /// A global loop whose sub-tracks all run concurrent only keys the
        /// properties it owns and abstains on the rest, so it can only ever
        /// contribute from *above*: sampled below a full-body play, the opaque
        /// container underneath it has already claimed the whole weight budget
        /// with default-fills and the overlay is never reached. A global that
        /// is not concurrent is the model's own animation rather than an
        /// overlay and stays at the bottom, where an ordinary play buries it.
        bool overlay = false;
    };

    void RetireCovered(std::span<const SequenceInfo> seqs);
    /// @brief Bring the live global plays in line with @ref globalSeqs_.
    void ReconcileGlobals(i32 nowMs, std::span<const SequenceInfo> seqs);
    /// @brief Leading overlay plays — where a host play has to be inserted so
    ///        it lands below them.
    std::size_t OverlayCount() const;
    PlayHandle PlayAt(const PlayDesc& desc, i32 nowMs, std::size_t at);
    PlayState* Primary();
    const PlayState* Primary() const;

    std::vector<PlayState> plays_;
    std::vector<ClipRef> clips_;
    TransitionPolicy policy_;
    PlayHandle nextHandle_ = 1;

    std::vector<i32> globalSeqs_;
    bool globalsDirty_ = false;

    i32 requestedSequence_ = 0;
    i32 acknowledgedSequence_ = -1;
    i32 primaryTimeMs_ = 0;
    i32 sequenceCycle_ = 0;
};

} // namespace whiteout::flakes::renderer::animation
