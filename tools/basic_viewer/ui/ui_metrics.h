#pragma once

// ============================================================================
// Sizes and colours the viewer's UI is laid out with, named by what they size.
//
// Widths are pixels at 100 % display scale: ApplyImGuiDpiScale scales fonts and
// style spacing but not these (BASIC_VIEWER_REFACTOR_PLAN.md B7).
// ============================================================================

#include "whiteout/flakes/types.h"

namespace whiteout::flakes::ui {

inline constexpr const char* kWindowTitle = "WhiteoutFlakes";
inline constexpr i32 kDefaultWindowWidth = 1024;
inline constexpr i32 kDefaultWindowHeight = 768;

/// Toolbar and tab strip: one frame height plus this.
inline constexpr f32 kStripPadding = 8.0f;

// ---- Modal dialogs ----
inline constexpr f32 kDialogButtonWidth = 120.0f;
inline constexpr f32 kDialogCancelWidth = 80.0f;

struct Rgba {
    f32 r, g, b, a;
};
/// A failed save or attach, said in the dialog that caused it.
inline constexpr Rgba kErrorText = {1.0f, 0.45f, 0.35f, 1.0f};

// ---- Toolbar ----
inline constexpr f32 kSequenceComboWidth = 220.0f;
inline constexpr f32 kCameraComboWidth = 140.0f;
inline constexpr f32 kSkinComboWidth = 160.0f;
inline constexpr f32 kLightingComboWidth = 120.0f;
inline constexpr f32 kCustomizeSliderWidth = 140.0f;

// ---- Diablo III equip popup ----
inline constexpr f32 kRegistryProgressWidth = 320.0f;
inline constexpr f32 kOutfitPresetComboWidth = 150.0f;
inline constexpr f32 kOutfitPresetNameWidth = 120.0f;
inline constexpr f32 kOutfitItemComboWidth = 230.0f;
inline constexpr f32 kOutfitDyeComboWidth = 90.0f;
inline constexpr f32 kWardrobeSetComboWidth = 180.0f;
inline constexpr f32 kWardrobeItemComboWidth = 130.0f;
inline constexpr f32 kWardrobeLookComboWidth = 150.0f;

// ---- Settings window ----
inline constexpr f32 kSettingsWindowWidth = 660.0f;
inline constexpr f32 kSettingsWindowHeight = 560.0f;
inline constexpr f32 kSettingsProfileListWidth = 170.0f;
/// The sliders, drags and combos on a settings page.
inline constexpr f32 kSettingsFieldWidth = 180.0f;
/// The Day/Night model and variant combos, whose entries are long names.
inline constexpr f32 kSettingsWideFieldWidth = 220.0f;
/// Room a path row leaves for its buttons and label (a negative item width).
inline constexpr f32 kPathRowReserve = 180.0f;
/// The same for the "add MPQ" row, which has one button.
inline constexpr f32 kAddMpqRowReserve = 140.0f;

// ---- Save As ----
inline constexpr f32 kTextureFormatComboWidth = 180.0f;

// ---- Animation capture dialog ----
inline constexpr f32 kCaptureWindowWidth = 720.0f;
inline constexpr f32 kCaptureWindowHeight = 780.0f;
/// Indent of the Output and Advanced sections under their headers.
inline constexpr f32 kCaptureSectionIndent = 12.0f;
/// The recipe preset and output format combos.
inline constexpr f32 kCapturePresetComboWidth = 220.0f;
/// A clip's sequence combo, and the fill mode's.
inline constexpr f32 kCaptureClipComboWidth = 190.0f;
inline constexpr f32 kCaptureRepeatsWidth = 70.0f;
inline constexpr f32 kCaptureSpeedWidth = 80.0f;
/// The millisecond and count drags: hold, blend, trims, fps, sheet columns,
/// pre-roll, frame step.
inline constexpr f32 kCaptureFieldWidth = 140.0f;
inline constexpr f32 kCaptureDurationWidth = 90.0f;
/// The camera preset and resolution combos.
inline constexpr f32 kCaptureWideComboWidth = 200.0f;
/// The orbit drags and the crop padding.
inline constexpr f32 kCaptureOrbitFieldWidth = 110.0f;
inline constexpr f32 kCaptureResolutionFieldWidth = 96.0f;
inline constexpr f32 kCaptureColorWidth = 160.0f;
/// The output folder and name template.
inline constexpr f32 kCapturePathWidth = 340.0f;
/// "Add clips": the search field and the list under it.
inline constexpr f32 kCaptureSearchWidth = 260.0f;
inline constexpr f32 kCaptureAddListWidth = 300.0f;
inline constexpr f32 kCaptureAddListHeight = 260.0f;
/// The footer: Preview on the left, Export and Close from this far off the right edge.
inline constexpr f32 kCaptureActionsReserve = 200.0f;
inline constexpr f32 kCaptureExportButtonWidth = 110.0f;

// ---- Animation window ----
inline constexpr f32 kAnimationWindowWidth = 560.0f;
inline constexpr f32 kAnimationWindowHeight = 420.0f;
/// Stretch weights of the track table's sequence, sub-track and weight columns.
inline constexpr f32 kTrackSequenceWeight = 0.40f;
inline constexpr f32 kTrackSubtrackWeight = 0.30f;
inline constexpr f32 kTrackWeightWeight = 0.18f;
/// Wide enough for the "Loop" header, not just the checkbox under it.
inline constexpr f32 kTrackLoopColumnWidth = 42.0f;
inline constexpr f32 kTrackRemoveColumnWidth = 24.0f;

} // namespace whiteout::flakes::ui
