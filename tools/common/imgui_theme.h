#pragma once

// Shared ImGui style for the WhiteoutFlakes hosts (basic_viewer + Max plugin).
// Mirrors the "Dark Ruda" theme (Raikiri / ImThemes) used by WhiteoutTex so
// the three companion tools render with the same look.

namespace whiteout::flakes {

// Applies the theme to the current ImGui context. Call once after
// ImGui::CreateContext() and before NewFrame.
void ApplyImGuiTheme();

} // namespace whiteout::flakes
