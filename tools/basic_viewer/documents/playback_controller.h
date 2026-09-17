#pragma once

// ============================================================================
// Playback of the active document: the transport, the sequence the dropdown
// picks, the plays layered under it, the model's global loops, camera presets,
// walk drift and the deferred framing of a standalone effect.
//
// Everything sits on the focus actor's ClipPlaylist — the renderer's own
// layering surface, the one `ActorView::Play` exposes to an embedder — and on
// the document's scene clock, which every profile's animation, particles,
// ribbons and corn-fx advance on. Nothing here is per-format.
// ============================================================================

#include "documents/document.h"
#include "whiteout/flakes/types.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace whiteout::flakes::renderer {
class RenderService;
namespace model {
struct Actor;
}
} // namespace whiteout::flakes::renderer

namespace whiteout::flakes {

class DocumentManager;

class PlaybackController {
public:
    PlaybackController(renderer::RenderService& service, DocumentManager& documents);

    renderer::model::Actor* FocusActor() const;

    // ---- Transport ----
    bool IsPaused() const;
    void SetPaused(bool paused);
    /// Back to frame zero and playing, whatever the transport was.
    void RestartPlayback();

    // ---- Sequences ----
    const std::vector<SequenceInfo>& Sequences() const;
    const std::vector<std::string>& SequenceNames() const;
    /// The focus actor's active sequence, 0 when there is none.
    i32 ActiveSequence() const;
    /// Play sequence @p index on the focus actor. Switching clears the splats a
    /// death left, unless the new sequence is one that finishes a death.
    void SelectSequence(i32 index);
    /// Re-read @p hero's sequence table into @p state. @p resetSelection when the
    /// indices changed meaning (a fresh model, a detach that renumbered) — the
    /// tracks and silenced loops go with the selection. An attach only appends,
    /// so those survive it; the playlist does not (a Bind rebuilt it).
    void RefreshSequences(DocumentState& state, renderer::model::Actor* hero, bool resetSelection);

    // ---- Layered plays ----
    const std::vector<AnimTrack>& Tracks() const;
    /// A track on the sequence the dropdown shows. False without an animated
    /// focus actor.
    bool AddTrack();
    /// Weight, speed and loop retune the live play; a new sequence or sub-track
    /// restarts it.
    void SetTrack(usize index, const AnimTrackInfo& info);
    void RemoveTrack(usize index);
    /// Re-play every track after something rebuilt the playlist, which leaves
    /// each handle naming nothing.
    void ReassertTracks();

    // ---- Global loops ----
    struct GlobalLoop {
        i32 sequence = 0;
        std::string name;
        bool enabled = true;
    };
    std::vector<GlobalLoop> GlobalLoops() const;
    void SetGlobalLoopEnabled(i32 sequence, bool on);

    // ---- Camera presets ----
    const std::vector<CameraPreset>& CameraPresets() const;
    std::optional<i32> ActiveCameraPreset() const;
    bool CameraLocked() const;
    /// A model camera, or the free orbital camera when empty or out of range.
    void ActivateCameraPreset(std::optional<i32> index);
    /// Keep an animated preset tracking the sequence.
    void UpdateCameraPresetAnimator();

    // ---- Host policy ----
    bool LoopNonLooping() const {
        return loopNonLooping_;
    }
    /// Freshly loaded actors play non-looping sequences on repeat when on.
    void SetLoopNonLooping(bool on);

    // ---- Load ----
    /// Sequences, framing, presets and walk drift for a model just spawned into
    /// @p state's scene.
    void OnModelLoaded(DocumentState& state, renderer::model::Actor* hero,
                       const std::filesystem::path& path);
    /// The same for a standalone effect: a placeholder sequence, a provisional
    /// camera and a deferred reframe once its particle cloud has developed.
    void OnEffectLoaded(DocumentState& state, renderer::model::Actor* hero);

    // ---- Frame stages ----
    /// Push the model along the camera's X on a walk sequence (orbital camera
    /// only), and take the push back off when the sequence changes.
    void AdvanceWalkDrift(f32 dt);
    /// The parent-clock step for the ticker, from the scene's animation time.
    f32 ParentClockStep();
    /// Reframe a freshly loaded `.pkb` once its particles exist.
    void AdvanceEffectReframe();

private:
    DocumentState& State();
    const DocumentState& State() const;
    void PublishGlobalLoops();

    renderer::RenderService& service_;
    DocumentManager& documents_;
    bool loopNonLooping_ = true;
};

} // namespace whiteout::flakes
