#pragma once

/// @file enums.h
/// @brief Canonical definitions for renderer-facing public enums.
///
/// Internal renderer code pulls these into its own namespaces via `using`
/// aliases so existing code keeps compiling without changes, while
/// consumers of the public API see them under `whiteout::flakes::*`.

#include "gfx_types.h"
#include "types.h"

namespace whiteout::flakes {

using ::whiteout::flakes::gfx::GfxApi;

/// @brief Material-rendering path: legacy fixed-function-style (SD) vs
///        Reforged PBR (HD).
/// @bind
enum class RenderMode : u8 {
    SD = 0, ///< Standard-Definition: classic WC3 materials.
    HD = 1, ///< High-Definition: Reforged PBR materials with IBL.
};

/// @brief Transport state of a scene's playback clock.
///
/// `Paused` and `Stopped` both hold time still; they differ in where the
/// clock sits. Pausing keeps the current instant, so resuming continues
/// mid-animation with particles still in flight. Stopping rewinds to the
/// start and discards transient effect state, so resuming plays the scene
/// from the top.
/// @bind
enum class PlaybackState : u8 {
    Playing = 0, ///< Time advances normally (scaled by the scene's time scale).
    Paused = 1,  ///< Time held at the current instant.
    Stopped = 2, ///< Time held at zero, transient effect state discarded.
};

/// @brief Light-rig preset selectable from the host UI.
/// @bind
enum class LightingMode : u8 {
    InGame = 0,  ///< Engine-runtime rig (game look).
    Glue = 1,    ///< Loading-screen / portrait rig (stronger key light).
    Dynamic = 2, ///< Animated DNC-driven rig.
};

/// @brief Image-Based-Lighting probe set selection.
/// @bind
enum class IblMode : u8 {
    Portrait = 0,
    DayNight = 1,
    Dungeon = 2,
    Sunset = 3,
};

/// @brief Role of an actor within the scene.
///
/// Exposed to host code (e.g. the Max plugin) so it can mark an actor as
/// host-driven (`External`) and suppress automatic per-frame evaluation.
/// @bind
enum class ActorRole : u8 {
    Unit = 0,       ///< Top-level model the engine animates.
    External = 1,   ///< Host pushes time + evaluates manually.
    Attachment = 2, ///< Slot-bound child of another actor.
    PE1 = 3,        ///< Legacy PE1 sub-emitter actor.
    SPN = 4,        ///< Engine-spawned (SND / SPL / SPN events).
};

/// @brief Blend mode for material layers and particle emitters.
///
/// Values mirror the engine's encoding so MDX/PE2 → renderer mappings
/// stay equality-stable across format versions.
///
/// NOT bound — same trailing-segment naming problem as `gfx::Format`:
/// `FILTER_MODULATE_2X` becomes the invalid identifier `2X`. Unreachable
/// from the bound surface anyway. See BINDINGS.md.
enum FilterMode {
    FILTER_NONE = 0,
    FILTER_TRANSPARENT = 1,
    FILTER_BLEND = 2,
    FILTER_ADDITIVE = 3,
    FILTER_ADD_ALPHA = 4,
    FILTER_MODULATE = 5,
    FILTER_MODULATE_2X = 6,
};

/// @brief Clamp a raw integer from disk into the @ref FilterMode range
///        (out-of-range values fold to the nearest endpoint).
inline i32 MapFilterMode(i32 raw) {
    if (raw < 0)
        return FILTER_NONE;
    if (raw > 6)
        return FILTER_MODULATE_2X;
    return raw;
}

/// @brief Convert PE2's (Reforged ParticleEmitter2) blend-mode enum to
///        the renderer's @ref FilterMode.
///
/// PE2's enum order differs from the material-layer order; this table
/// re-aligns them. Unknown inputs fall back to @ref FILTER_BLEND.
inline i32 MapPE2BlendMode(i32 blendMode) {
    static constexpr i32 table[] = {FILTER_BLEND, FILTER_ADDITIVE, FILTER_MODULATE,
                                    FILTER_MODULATE_2X, FILTER_TRANSPARENT};
    if (blendMode >= 0 && blendMode < 5)
        return table[blendMode];
    return FILTER_BLEND;
}

/// @brief Bone-flag bits controlling per-bone billboarding.
///
/// Bits are OR-ed at evaluation time. `FULL` overrides per-axis locks;
/// `CAMERA_ANCHORED` makes the billboard track the camera position
/// instead of its forward axis.
enum BoneBillboardFlag : u32 {
    BONE_BILLBOARD_NONE = 0,
    BONE_BILLBOARD_FULL = 1,
    BONE_BILLBOARD_LOCK_X = 2,
    BONE_BILLBOARD_LOCK_Y = 4,
    BONE_BILLBOARD_LOCK_Z = 8,
    BONE_BILLBOARD_CAMERA_ANCHORED = 16,
};

/// @brief Per-material flag bits driving render-state setup.
enum MaterialFlags {
    MAT_TWO_SIDED = 1,
    MAT_UNSHADED = 2,
    MAT_UNFOGGED = 4,
    MAT_NO_DEPTH_TEST = 8,
    MAT_NO_DEPTH_SET = 16,
    MAT_CONSTANT_COLOR = 32,
};

} // namespace whiteout::flakes
