#pragma once

// ============================================================================
// The frame loop's, the input's and the camera's tuning, named. Values a
// behaviour depends on rather than a layout: changing one changes what the
// viewer does, so each says what it is for.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <chrono>
#include <string_view>

namespace whiteout::flakes::tuning {

// ---- Input ----
/// Camera zoom per scroll-wheel notch.
inline constexpr f64 kScrollZoomStep = 30.0;

// ---- Frame loop ----
/// The parent-clock delta handed to the ticker is clamped to this, so a stall
/// (a load, a breakpoint) does not fast-forward every emitter.
inline constexpr i32 kMaxParentClockStepMs = 100;
/// Headless runs that capture (`--export-anim`) tick this many frames of this
/// length first, so assets and a `.pkb`'s particle cloud settle.
inline constexpr i32 kHeadlessWarmupTicks = 24;
inline constexpr f32 kHeadlessTickSeconds = 0.016f;

// ---- Walk drift ----
/// Sequences whose name contains this move the model along the camera's X.
inline constexpr std::string_view kWalkSequenceToken = "walk";
/// Their speed when the sequence names none.
inline constexpr f32 kDefaultWalkSpeed = 100.0f;

// ---- Sequence switch ----
/// A switch onto a sequence named with one of these keeps the splats alive:
/// they are the ones that finish a death.
inline constexpr std::string_view kSplatKeepingSequenceTokens[] = {"decay", "dissipate"};

// ---- Standalone effects ----
/// A `.pkb` has no bounds to frame until its sim has run: the camera waits this
/// many ticks before reframing on the particle cloud, and stops trying after
/// the second count.
inline constexpr i32 kEffectReframeWarmupTicks = 12;
inline constexpr i32 kEffectReframeMaxTicks = 90;
/// The provisional pose until then: three-quarter view like
/// FrameCameraToModel's, from a moderate distance.
inline constexpr f32 kEffectPreviewYawOffset = 0.785398f;
inline constexpr f32 kEffectPreviewPitch = 0.6f;
inline constexpr f32 kEffectPreviewDistance = 100.0f;

// ---- Camera presets ----
/// The end a preset animator samples against when the active sequence has no
/// range: far enough never to wrap.
inline constexpr i32 kOpenEndedSequenceEndMs = 1 << 30;
/// A preset whose diagonal FOV is at or under this carries none.
inline constexpr f32 kPresetFovUnset = 1e-3f;

// ---- Storage Explorer ----
/// Its ini section is rewritten once its state has held still this long.
inline constexpr f32 kExplorerStateSettleSeconds = 1.0f;

// ---- Storage opens ----
/// How often a task waiting on another caller's storage open looks again.
inline constexpr std::chrono::milliseconds kStorageOpenPollInterval{25};
/// Directory levels above a loose `.m2` searched for a listfile and TACT keys.
inline constexpr i32 kNearbyKeySearchLevels = 5;

} // namespace whiteout::flakes::tuning
