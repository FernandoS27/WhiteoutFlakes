#pragma once

// ============================================================================
// One open file (a tab) and everything the host keeps about it.
//
// The scene — actors, camera, clock, render mode — lives in the RenderService
// under `scene`. DocumentState is the host's half: which actor the toolbar
// drives, the sequence table it shows, the layered plays, the camera preset and
// the walk drift. A new per-tab field is one member here; switching tabs moves
// nothing, because each document keeps its own.
// ============================================================================

#include "render_target.h"
#include "whiteout/flakes/display.h"
#include "whiteout/flakes/types.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace whiteout::flakes::renderer {
using SceneId = u32;
}

namespace whiteout::flakes {

/// One extra play the Animation window layers under the sequence dropdown.
struct AnimTrackInfo {
    i32 sequence = 0;
    /// Every sub-track of the sequence when empty, which is what a plain play does.
    std::optional<i32> subtrack;
    f32 weight = 1.0f;
    f32 speed = 1.0f;
    bool loop = true;
};

struct AnimTrack {
    AnimTrackInfo info;
    /// The playlist handle it was started with. One the playlist already retired
    /// reads back as "not found", and the track restarts — which is what makes a
    /// non-looping track re-armable.
    u32 handle = 0;
};

struct CameraPresetState {
    std::vector<CameraPreset> presets;
    /// The free orbital camera when empty.
    std::optional<i32> active;
    /// A live preset drives the camera every frame, so the user cannot orbit.
    bool locked = false;
};

/// The offset the walk cycle has pushed the model and its camera along X.
struct WalkDrift {
    i32 prevSequence = -1;
    f32 accumulated = 0.0f;
};

struct DocumentState {
    std::filesystem::path modelPath;
    /// The actor the sequence dropdown, the team colour and the exports act on.
    u32 focusActor = 0;
    std::vector<SequenceInfo> sequences;
    /// `sequences[i].name`, kept beside them because ImGui combos and the export
    /// recipe take a list of names. Written only with `sequences`.
    std::vector<std::string> sequenceNames;
    std::vector<AnimTrack> tracks;
    /// Global loops the user switched off, by sequence index. Cleared whenever
    /// the sequence table is rebuilt, since the indices then mean nothing.
    std::vector<i32> silencedGlobals;
    CameraPresetState camera;
    WalkDrift walkDrift;
    /// A standalone `.pkb` has no bounds to frame until its sim has run: the
    /// ticks since the load while the reframe is pending.
    std::optional<i32> effectReframeTicks;
    /// The last animation-time sample, so the ticker's parent delta needs no
    /// second clock.
    i32 lastParentTimeMs = 0;
};

struct Document {
    renderer::SceneId scene = 0;
    /// The tab label: the file's stem.
    std::string title;
    DocumentState state;
};

} // namespace whiteout::flakes
