#pragma once

// ============================================================================
// WhiteoutFlakes — per-actor view handle.
//
// Lightweight, value-typed handle the host uses to mutate one actor in the
// scene without needing access to the internal Actor type. Returned by
// Renderer::Actor(handle).
// ============================================================================

#include "types.h"
#include "enums.h"
#include "display.h"
#include "model_data.h"

#include <vector>

namespace whiteout::flakes {

namespace detail { class RendererImpl; }

using ActorHandle = u32;

class ActorView {
public:
    bool        IsValid() const;
    ActorHandle Handle() const { return handle_; }
    ActorRole   Role() const;

    // Transform / playback / team color.
    Matrix44f Transform() const;
    void      SetTransform(const Matrix44f&);

    f32   PlaybackSpeed() const;
    void  SetPlaybackSpeed(f32);

    bool  IgnoreNonLooping() const;
    void  SetIgnoreNonLooping(bool);

    u32   TeamColor() const;
    void  SetTeamColor(u8 r, u8 g, u8 b);

    // For host-driven actors (Max plugin) so the renderer's auto-evaluation
    // pass skips them.
    void  SetRoleExternal();

    // Animation cursor.
    std::vector<SequenceInfo> Sequences() const;
    i32   ActiveSequenceIndex() const;
    void  SetActiveSequence(i32);
    i32   AnimationTimeMs() const;
    void  SetAnimationTimeMs(i32);
    bool  HasAnimationSource() const;

    // Per-actor evaluate-and-apply (max_plugin timeline scrub). Reads the
    // cursor state already on the actor; call SetAnimationTimeMs first if
    // you want to evaluate at a specific time.
    void  EvaluateAndApply();

    // Convenience: SetAnimationTimeMs(t) + EvaluateAndApply().
    void  EvaluateAt(i32 timeMs);

    // Read-only counts for status displays.
    i32  GeosetCount() const;
    i32  MaterialCount() const;
    i32  CollisionShapeCount() const;

    std::vector<CameraPreset> CameraPresets() const;

private:
    ActorView(detail::RendererImpl* impl, ActorHandle h) : impl_(impl), handle_(h) {}
    detail::RendererImpl* impl_;
    ActorHandle           handle_;
    friend class Renderer;
};

}  // namespace whiteout::flakes
