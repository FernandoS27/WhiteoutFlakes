#pragma once

/// @file event_data.h
/// @brief POD descriptors for MDX event objects (`SPN` / `SPL` / `UBR` /
///        `SND`) and the global lookup tables they live in.
///
/// SPN spawns a child model. SPL spawns a splat-style projected billboard.
/// UBR spawns an ubersplat (animated splat). SND triggers a sound effect.
/// Each entry type is read from a corresponding game-data SLK and
/// indexed by event id; renderer subsystems + the sound emitter look
/// them up via the `FindXxx` helpers after the host has called
/// `LoadEventDataFiles` once.

#include "types.h"

#include <string>
#include <string_view>
#include <vector>

namespace whiteout::flakes::io {
class IContentProvider;
}

namespace whiteout::flakes::io {

/// @brief SPN — spawn another model as a child of the firing actor.
struct SpnEntry {
    std::string modelPath; ///< MDX/MDL referenced by event id.
};

/// @brief SPL — short-lived projected billboard splat.
struct SplEntry {
    std::string file; ///< Texture (BLP / DDS / TGA).
    f32 scale = 1.0f;
    f32 startC[4] = {1, 1, 1, 1}; ///< RGBA tint at spawn.
    f32 midC[4] = {1, 1, 1, 1};   ///< RGBA tint mid-life.
    f32 endC[4] = {1, 1, 1, 1};   ///< RGBA tint at end of life.
    i32 columns = 1;              ///< Sprite-sheet columns.
    i32 rows = 1;                 ///< Sprite-sheet rows.
    f32 lifespan = 0.0f;          ///< Seconds until decay starts.
    f32 decay = 0.0f;             ///< Seconds spent fading out.
    i32 uvLifeStart = 0;          ///< First sprite frame during lifespan.
    i32 uvLifeEnd = 0;            ///< Last sprite frame during lifespan.
    i32 lifespanRepeat = 1;       ///< Sprite-sheet repeats over lifespan.
    i32 uvDecayStart = 0;         ///< First sprite frame during decay.
    i32 uvDecayEnd = 0;           ///< Last sprite frame during decay.
    i32 decayRepeat = 1;          ///< Sprite-sheet repeats over decay.
    i32 blendMode = 0;            ///< @ref FilterMode.
};

/// @brief UBR — ubersplat (multi-stage projected splat with birth/pause/decay).
struct UbrEntry {
    std::string file;
    f32 scale = 1.0f;
    f32 c[3][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}}; ///< RGBA at birth / pause / decay.
    f32 birthTime = 0.0f;
    f32 pauseTime = 0.0f;
    f32 decay = 0.0f;
    i32 blendMode = 0; ///< @ref FilterMode.
};

/// @brief SND — sound effect with random per-fire file selection +
///        distance attenuation.
struct SndEntry {
    std::vector<std::string> filePaths; ///< One is picked at random per fire.
    f32 volume = 1.0f;
    f32 minDistance = 0.0f;    ///< Inside this radius volume = 1.
    f32 maxDistance = 0.0f;    ///< Outside this radius volume = 0.
    f32 distanceCutoff = 0.0f; ///< Beyond this radius the sound isn't played at all.
};

/// @brief One-shot load of the event-data SLKs (UnitData, SplatData, etc.).
/// @param cp     Content provider used to read the SLKs.
/// @param force  When `true`, re-load even if a prior call succeeded.
void LoadEventDataFiles(IContentProvider* cp, bool force = false);

/// @name Event-id → entry lookups (`nullptr` if not found)
/// @{
const SpnEntry* FindSpn(std::string_view id);
const SplEntry* FindSpl(std::string_view id);
const UbrEntry* FindUbr(std::string_view id);
const SndEntry* FindSnd(std::string_view id);
/// @}

/// @brief Walk every cached SPL / UBR texture path and every SPN child-
///        model path, issuing a `Request` for each through @p cp. Each
///        miss is recorded in the provider's missing list — the web
///        build's JS drain then ferries the bytes in BEFORE the
///        corresponding event fires, so the splat/spawn shows up
///        textured rather than as a placeholder on first appearance.
///        Idempotent — re-requesting a path the provider already has is
///        a no-op cache hit.
void PrefetchEventAssetPaths(IContentProvider* cp);

/// @name "Has the cache for this entry-kind been populated yet?"
///
/// Used by the event-emitter pool to decide whether a `Find* == nullptr`
/// is a *permanent* miss (cache fully loaded but no row for this id) or
/// a *transient* miss (cache still loading — web build SLKs arrive async
/// via the JS lazy drain). The pool latches `resolutionFailed = true` on
/// permanent misses to avoid log spam; transient misses let the event
/// re-attempt on subsequent fires.
/// @{
bool IsSpnCachePopulated();
bool IsSplCachePopulated();
bool IsUbrCachePopulated();
bool IsSndCachePopulated();
/// @}

} // namespace whiteout::flakes::io

namespace whiteout::flakes {
using ::whiteout::flakes::io::SndEntry;
using ::whiteout::flakes::io::SplEntry;
using ::whiteout::flakes::io::SpnEntry;
using ::whiteout::flakes::io::UbrEntry;
} // namespace whiteout::flakes
