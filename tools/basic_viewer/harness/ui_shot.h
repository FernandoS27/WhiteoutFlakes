#pragma once

// ============================================================================
// `--ui-shot <out.png> [--ui-shot-panel <name>]` — the viewer's UI rendered to
// a PNG with no visible window, the basic viewer's twin of ModelExplorer's
// `--panel-shot`. The only check that can see a UI refactor: the draw-trace
// gates never build an ImGui frame.
//
// Panels: main, menu-file, menu-view, menu-debug, menu-tools, menu-language,
// settings-general, settings-{wc3,sc2,wow,d3}[-io], animation, export,
// d3-equip, wow-customize, dialog-mdx, dialog-m3, dialog-gltf, dialog-saveas,
// dialog-saveas-mdl, dialog-m3save, dialog-wem.
//
// The capture is deterministic only once assets and background tasks have
// settled, so the harness pauses the scene and pumps until both are quiet.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <filesystem>
#include <string>

namespace whiteout::flakes {

class ViewerApp;

/// Fixed so a capture does not depend on the display it was taken on.
inline constexpr i32 kUiShotWidth = 1280;
inline constexpr i32 kUiShotHeight = 800;

struct UiShotOptions {
    std::filesystem::path out;
    std::string panel = "main";
};

/// Opens the panel, settles, captures one frame and writes it. Returns 0 on
/// success, 2 for an unusable panel, 3 when the capture or the write fails.
i32 RunUiShot(ViewerApp& app, const UiShotOptions& options);

} // namespace whiteout::flakes
