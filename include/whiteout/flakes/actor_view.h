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

    /// @brief Render mode the actor's template expects (`HD` if any
    ///        material layer uses a non-zero BLS shaderId, else `SD`).
    ///        Hosts call this after `SpawnUnit` and forward to
    ///        `SettingsView::SetRenderMode` so SD models don't render
    ///        through the HD pipeline (which mis-blends multi-layer SD
    ///        materials) and vice-versa.
    RenderMode PreferredRenderMode() const;

private:
    ActorView(detail::RendererImpl* impl, ActorHandle h) : impl_(impl), handle_(h) {}
    detail::RendererImpl* impl_;
    ActorHandle handle_;
    friend class Renderer;
};

} // namespace whiteout::flakes
