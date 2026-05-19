#pragma once

// Shared ImGui style for the WhiteoutFlakes hosts (basic_viewer + Max plugin).
// Mirrors the "Dark Ruda" theme (Raikiri / ImThemes) used by WhiteoutTex so
// the three companion tools render with the same look.

namespace whiteout::flakes {

// Applies the theme to the current ImGui context. Call once after
// ImGui::CreateContext() and before NewFrame.
void ApplyImGuiTheme();

// Applies the host's monitor DPI scale (1.0 = 96 DPI, 1.5 = 144 DPI, …) to the
// current ImGui style: glyphs rasterise at the scaled pixel size via
// style.FontScaleDpi, and absolute padding/spacing/rounding values are
// multiplied by `scale`. Call once after ApplyImGuiTheme() — ScaleAllSizes is
// not idempotent.
void ApplyImGuiDpiScale(float scale);

} // namespace whiteout::flakes
