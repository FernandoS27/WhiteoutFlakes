#pragma once

// ============================================================================
// WhiteoutFlakes — public enums.
//
// Canonical definitions for renderer-facing enums. Internal renderer code
// pulls these into its own namespaces via `using` aliases (see
// src/renderer/render_target.h, src/renderer/model/model_types.h, etc.) so
// that existing code keeps compiling without changes while consumers of the
// public API see them under `whiteout::flakes::*`.
// ============================================================================

#include "gfx_types.h"
#include "types.h"

namespace whiteout::flakes {

// GfxApi lives in the gfx namespace internally; surface it under the
// public namespace so consumers don't need to dig into ::gfx.
using ::whiteout::flakes::gfx::GfxApi;

enum class RenderMode : u8 {
    SD = 0,
    HD = 1,
};

enum class LightingMode : u8 {
    InGame = 0,
    Glue = 1,
    Dynamic = 2,
};

enum class IblMode : u8 {
    Portrait = 0,
    DayNight = 1,
    Dungeon = 2,
    Sunset = 3,
};

// Roles roughly mirror the internal ActorRole enum — exposed so host code
// (e.g. the Max plugin) can mark an actor as host-driven (External) and
// suppress automatic per-frame evaluation.
enum class ActorRole : u8 {
    Unit = 0,
    External = 1,
    Attachment = 2,
    PE1 = 3,
    SPN = 4,
};

// Filter mode for material layers / particle emitters. Values mirror the
// engine's blend-mode encoding so MDX/PE2 -> renderer mappings stay
// equality-stable.
enum FilterMode {
    FILTER_NONE = 0,
    FILTER_TRANSPARENT = 1,
    FILTER_BLEND = 2,
    FILTER_ADDITIVE = 3,
    FILTER_ADD_ALPHA = 4,
    FILTER_MODULATE = 5,
    FILTER_MODULATE_2X = 6,
};

inline i32 MapFilterMode(i32 raw) {
    if (raw < 0)
        return FILTER_NONE;
    if (raw > 6)
        return FILTER_MODULATE_2X;
    return raw;
}

inline i32 MapPE2BlendMode(i32 blendMode) {
    static constexpr i32 table[] = {FILTER_BLEND, FILTER_ADDITIVE, FILTER_MODULATE,
                                    FILTER_MODULATE_2X, FILTER_TRANSPARENT};
    if (blendMode >= 0 && blendMode < 5)
        return table[blendMode];
    return FILTER_BLEND;
}

enum BoneBillboardFlag : u32 {
    BONE_BILLBOARD_NONE = 0,
    BONE_BILLBOARD_FULL = 1,
    BONE_BILLBOARD_LOCK_X = 2,
    BONE_BILLBOARD_LOCK_Y = 4,
    BONE_BILLBOARD_LOCK_Z = 8,
    BONE_BILLBOARD_CAMERA_ANCHORED = 16,
};

enum MaterialFlags {
    MAT_TWO_SIDED = 1,
    MAT_UNSHADED = 2,
    MAT_UNFOGGED = 4,
    MAT_NO_DEPTH_TEST = 8,
    MAT_NO_DEPTH_SET = 16,
    MAT_CONSTANT_COLOR = 32,
};

} // namespace whiteout::flakes
