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
//
// The atlas bakes exactly the glyphs used by the shipped `lang/*.ini` catalogs
// (found next to `fontsDir`). `extraGlyphText` (UTF-8) bakes glyphs from text
// that lives outside the catalogs — e.g. the language-picker endonyms, which
// are hardcoded and can use characters (like 體 in 繁體中文) no translation does.
void ApplyImGuiDpiScale(float scale, const std::string& fontsDir = "",
                        const std::string& extraGlyphText = "");

} // namespace whiteout::flakes
