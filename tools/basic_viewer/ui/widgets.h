#pragma once

// ============================================================================
// The small widgets the viewer's panels share. Each wraps the ImGui calls a
// panel used to spell out by hand, in the same order, so moving a panel onto
// one draws the same pixels.
// ============================================================================

#include "color_pack.h"
#include "whiteout/flakes/types.h"

#include <imgui.h>

#include <span>
#include <string>

namespace whiteout::flakes::ui {

// ---- Forms ----

/// ImGui::Combo over localisation keys, translated each frame.
bool KeyCombo(const char* label, i32& index, std::span<const char* const> keys);

/// An RGB colour edit over 8-bit channels. @p round rounds back to 8 bits
/// rather than truncating — each caller keeps the conversion its setting
/// was saved with.
bool ColorEditRgb8(const char* label, tools::Rgb8& color, ImGuiColorEditFlags flags = 0, bool round = false);

/// One path setting: the text field, a browse button, a reset (or clear)
/// button and a label. True when the path was committed: edited and left,
/// browsed to, or reset to @p resetTo.
struct PathRowSpec {
    const char* inputId;
    /// Folder picker when empty; otherwise a file picker with this filter,
    /// e.g. {"Listfile", "csv,txt"}.
    const char* filterName = nullptr;
    const char* filterSpec = nullptr;
    const char* browseKey;
    const char* resetKey;
    /// Already translated, or a game name.
    const char* label;
    /// Room left for the buttons and the label.
    f32 reserve;
};
bool PathRow(const PathRowSpec& spec, std::string& path, const std::string& resetTo);

/// "(?)" after the previous item, its tooltip wrapped.
void HelpMarker(const char* text);
/// A wrapped line in @p colour: a warning the user can act on.
void Warn(ImU32 colour, const char* text);
inline constexpr ImU32 kWarnAmber = IM_COL32(230, 170, 60, 255);
inline constexpr ImU32 kWarnRed = IM_COL32(230, 90, 80, 255);

/// "<caption>: <detail>" in the error colour: a failed save or attach, said in
/// the dialog that caused it.
void ErrorText(const char* caption, const char* detail);

} // namespace whiteout::flakes::ui
