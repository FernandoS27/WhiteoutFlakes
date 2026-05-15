#pragma once

/// @file display.h
/// @brief Display flags, camera-preset descriptors, sequence metadata,
///        and frame-stats value types used by the renderer pipeline +
///        settings surface.

#include "enums.h"
#include "shadow_params.h"
#include "types.h"

#include <functional>
#include <string>

namespace whiteout::flakes {

/// @brief Opaque handle for a swap-chain or off-screen render target.
using RenderTargetId = u32;

/// @brief Toggles for what the renderer draws each frame.
///
/// Persisted by the host as a single block; defaults shown match the
/// out-of-the-box viewer experience.
struct DisplayFlags {
    bool showGrid = true;        ///< Reference grid on the ground plane.
    bool showParticles = true;   ///< MDX particle emitters.
    bool showRibbons = true;     ///< MDX ribbon emitters.
    bool showCollisions = false; ///< Debug overlay for collision shapes.
    bool showLights = false;     ///< Debug overlay for light positions.
    bool showEvents = true;      ///< MDX event objects (SPN / SPL / SND).
    RenderMode renderMode = RenderMode::SD; ///< Material path (SD vs HD).
};

/// @brief Per-frame statistics aggregated by the pipeline.
///
/// Names mirror the internal `RenderPipeline::GetFrameStats` out
/// parameters. Hosts surface these in title-bar / overlay HUDs.
struct FrameStats {
    i32 geosets = 0;
    i32 textures = 0;
    i32 nodes = 0;
    i32 particles = 0;
    i32 segments = 0;
};

/// @brief Scripted camera the host may select for a loaded model.
///
/// Mirrors the engine's MDX camera-chunk semantics: a static pose
/// (`position` / `target` / FoV / clip planes / roll) plus an optional
/// per-frame animator that overrides them based on the active sequence's
/// time cursor.
struct CameraPreset {
    /// UTF-8 preset name as authored by the artist (used by host UI
    /// dropdowns); was `std::wstring` historically but normalised to UTF-8
    /// so the same string flows from the MDX adapter through the UI
    /// without per-platform wide-char roundtrips.
    std::string name;

    /// `true` if this preset should drive the camera every frame (i.e.
    /// the user can't free-orbit while it's active).
    bool isLive = false;

    Vector3f position{0.f, 0.f, 0.f}; ///< World-space camera position.
    Vector3f target{0.f, 0.f, 0.f};   ///< World-space look-at point.
    f32 fovDiagonal = 0.95f;          ///< Diagonal field of view (radians).
    f32 zNear = 1.0f;                 ///< Near clip plane.
    f32 zFar = 10000.0f;              ///< Far clip plane.
    f32 staticRoll = 0.0f;            ///< Roll around the view axis (radians).

    /// @name Derived orbital pose
    /// @{ Convenience values populated by the MDX adapter for free-camera
    /// fallback (`pitch`/`yaw` in radians, `distance` from `target`).
    f32 pitch = 0.0f;
    f32 yaw = 0.0f;
    f32 distance = 100.0f;
    /// @}

    /// Optional animator: when set, the host calls it every frame with
    /// the current animation time and the active sequence's [start,end]
    /// range, and the animator mutates `pos` / `target` / `roll` in place.
    std::function<void(Vector3f& pos, Vector3f& target, f32& roll, i32 timeMs, i32 seqStart,
                       i32 seqEnd)>
        animator;
};

/// @brief Description of one animation sequence (`SEQS` chunk entry).
///
/// Times are in milliseconds (matches MDX's internal unit). `moveSpeed`
/// is the artist-authored locomotion speed in world-units / second,
/// used by hosts to drive walk-cycle drift along the camera axis.
struct SequenceInfo {
    std::string name;
    i32 startMs = 0;
    i32 endMs = 0;
    f32 moveSpeed = 0.0f;
    bool nonLooping = false;
};

} // namespace whiteout::flakes
