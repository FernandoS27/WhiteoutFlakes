#pragma once

// ============================================================================
// The application's own icon as something ImGui can draw: the embedded PNG,
// resampled and packed into the font atlas as a custom rectangle.
//
// The font atlas rather than a texture of its own because the hosts' ImGui
// renderers upload exactly one texture — the atlas — and it is already typed
// sRGB on the GPU, which is what a PNG is. The cost is a lifecycle:
// ApplyImGuiDpiScale clears the atlas, so Bake has to run after it and before
// the backend uploads, and the packed rectangle moves whenever the atlas is
// rebuilt, which is why the UVs are read per frame rather than cached.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <imgui.h>

namespace whiteout::flakes::ui {

/// Decodes the embedded icon, resamples it to a square sized for a ribbon's
/// application tile at @p dpiScale, and packs it into the current font atlas.
/// Call once, after the fonts are set up and before the backend uploads.
void BakeAppIcon(f32 dpiScale);

/// UVs of the baked icon in the atlas as it stands now. False when nothing was
/// baked — the caller falls back to a drawn glyph. Read per frame, never cached.
bool AppIconUV(ImVec2* uv0, ImVec2* uv1);

} // namespace whiteout::flakes::ui
