#pragma once

/// @file actor_view.h
/// @brief Lightweight per-actor handle returned by `Renderer::Actor()`.

#include "display.h"
#include "enums.h"
#include "model_data.h"
#include "types.h"

#include <vector>

namespace whiteout::flakes {

namespace detail {
class RendererImpl;
}

/// @brief Opaque handle identifying one actor in the scene.
using ActorHandle = u32;

/// @brief Value-typed view onto one actor.
///
/// Constructed by `Renderer::Actor(handle)`. Cheap to copy; under the
/// hood it's just `{ RendererImpl*, ActorHandle }`. Lifetime mirrors
/// the renderer's — copies become invalid when the actor is removed.
/// @bind no_default_ctor, methods
class ActorView {
public:
    /// @brief `true` if the underlying actor still exists in the scene.
    bool IsValid() const;
    /// @brief The handle this view was constructed with.
    ActorHandle Handle() const {
        return handle_;
    }
    /// @brief Role assigned at spawn time (see @ref ActorRole).
    ActorRole Role() const;

    /// @name Transform / playback / team-color
    /// @{
    Matrix44f Transform() const;
    void SetTransform(const Matrix44f&);

    /// @brief Animation playback rate (`1.0` = nominal).
    f32 PlaybackSpeed() const;
    void SetPlaybackSpeed(f32);

    /// @brief If `true`, non-looping sequences hold their last frame instead
    ///        of restarting from the beginning.
    bool IgnoreNonLooping() const;
    void SetIgnoreNonLooping(bool);

    /// @brief Packed 0x00BBGGRR team colour (low 24 bits used).
    u32 TeamColor() const;
    /// @brief Set the team colour from sRGB byte components.
    void SetTeamColor(u8 r, u8 g, u8 b);

    /// @brief Mark this actor as host-driven so `Renderer::Tick` skips
    ///        evaluating it. Used by the Max plugin, which receives time
    ///        scrubs from Max's timeline and evaluates manually via
    ///        @ref EvaluateAndApply.
    void SetRoleExternal();
    /// @}

    /// @name Animation cursor
    /// @{
    std::vector<SequenceInfo> Sequences() const;
    i32 ActiveSequenceIndex() const;
    void SetActiveSequence(i32);
    i32 AnimationTimeMs() const;
    void SetAnimationTimeMs(i32);
    /// @brief `true` once the actor has an `IAnimationSource` bound (i.e.
    ///        spawn-from-source completed successfully).
    bool HasAnimationSource() const;
    /// @}

    /// @name Layered playback
    ///
    /// `SetActiveSequence` above drives one play and replaces whatever was
    /// running — the whole of Warcraft III's and World of Warcraft's playback
    /// model. StarCraft II actors instead run a stack: an upper body swinging
    /// through an attack over legs that keep walking, each with its own clock
    /// and blend envelope. These four methods are that stack.
    ///
    /// Available on every format, not just `.m3`: a WC3 model asked for two
    /// plays blends them, because the blend happens above the sampler. What is
    /// format-specific is the *default* transition — WC3 and WoW cut, M3
    /// cross-fades over 150 ms — and passing an explicit blend overrides it
    /// either way.
    /// @{

    /// @brief Stack another play on top of whatever is running.
    ///
    /// @param sequence   Index into @ref Sequences.
    /// @param weight     Contribution before the blend envelope multiplies in.
    ///                   Weights are spent from a budget of 1.0, highest
    ///                   priority first, so a full-weight play on top hides
    ///                   the ones below it.
    /// @param speed      Clock rate for this play alone, independent of
    ///                   @ref SetPlaybackSpeed.
    /// @param loop       `false` retires the play when the sequence ends.
    /// @param blendInMs  Fade-in. `0` starts at full weight.
    /// @param blendOutMs Fade-out used when this play stops. `-1` takes the
    ///                   format's default.
    /// @return A handle for @ref StopPlay, or `0` if the actor is gone or has
    ///         no animation source yet.
    u32 Play(i32 sequence, f32 weight, f32 speed, bool loop, i32 blendInMs, i32 blendOutMs);

    /// @brief Fade one play out and retire it. `blendOutMs < 0` uses the
    ///        play's own blend-out. Unknown handles are ignored.
    void StopPlay(u32 playHandle, i32 blendOutMs);

    /// @brief Fade every play out, including the one `SetActiveSequence`
    ///        drives. The actor holds its last pose until something plays.
    void StopAllPlays(i32 blendOutMs);

    /// @brief How many plays are live, blend-outs included.
    i32 PlayCount() const;
    /// @}

    /// @brief Evaluate the animation at the actor's current cursor and
    ///        push the result into the renderer state.
    ///
    /// Used by host-driven actors (Max-plugin timeline scrub). Call
    /// `SetAnimationTimeMs(t)` first if you want a specific time.
    void EvaluateAndApply();

    /// @brief Convenience: `SetAnimationTimeMs(t)` + `EvaluateAndApply()`.
    void EvaluateAt(i32 timeMs);

    /// @name Read-only counts for status displays.
    /// @{
    i32 GeosetCount() const;
    i32 MaterialCount() const;
    i32 CollisionShapeCount() const;
    /// @}

    /// @brief Camera presets attached to this actor's source model.
    std::vector<CameraPreset> CameraPresets() const;

    /// @brief A hint: which Warcraft III frame this actor's template would
    ///        prefer (`HD` if any material layer uses a non-zero BLS shaderId,
    ///        else `SD`). Hosts call this after `SpawnUnit` and forward it to
    ///        `SettingsView::SetRenderMode` so SD models don't render through
    ///        the HD pipeline (which mis-blends multi-layer SD materials) and
    ///        vice-versa.
    ///
    ///        A hint and not a decision: the frame is chosen by the scene's
    ///        @ref ProductId first, and only falls back to the render mode for
    ///        Warcraft III content. A non-WC3 template reports `SD`, which
    ///        carries no meaning for it — the host is free to ignore this.
    RenderMode PreferredRenderMode() const;

    /// @brief Every child-model path this actor's template will
    ///        eventually need: attachment slots (`AttachmentConfig`) and
    ///        legacy PE1 particle emitters (`PE1EmitterConfig`). Hosts
    ///        running an async loader (web build) use this to eagerly
    ///        prefetch the child MDX bytes so the first attachment
    ///        spawn / first PE1 fire lands in a primed cache rather
    ///        than triggering a miss.
    std::vector<std::string> ChildModelPaths() const;

private:
    ActorView(detail::RendererImpl* impl, ActorHandle h) : impl_(impl), handle_(h) {}
    detail::RendererImpl* impl_;
    ActorHandle handle_;
    friend class Renderer;
};

} // namespace whiteout::flakes
