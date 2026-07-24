#pragma once

// Shared ImGui style for the WhiteoutFlakes hosts (basic_viewer + Max plugin).
// Mirrors the "Dark Ruda" theme (Raikiri / ImThemes) used by WhiteoutTex so
// the three companion tools render with the same look.

#include <string>

namespace whiteout::flakes {

// Applies the theme to the current ImGui context. Call once after
// ImGui::CreateContext() and before NewFrame.
void ApplyImGuiTheme();

// Rebuilds the font atlas at the host's monitor DPI scale and reapplies the
// scaled style sizes. `fontsDir` (UTF-8) is a directory holding the bundled
// Noto Sans fonts (NotoSansSC-Medium.ttf + NotoSansKR-Medium.ttf); when they're
// present the atlas covers every UI language (Latin+accents, Cyrillic, CJK,
// kana, Hangul), otherwise it falls back to the embedded Latin-only Roboto.
// The engine ImGui renderer bakes one static atlas, so the CJK ranges are
// pre-baked here. Call once after ApplyImGuiTheme() — ScaleAllSizes is not
// idempotent.
void ApplyImGuiDpiScale(float scale, const std::string& fontsDir = "");

} // namespace whiteout::flakes
