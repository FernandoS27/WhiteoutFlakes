#pragma once

// ============================================================================
// Everything a command line asks of the full viewer app: open it (hidden unless
// the mode is the interactive viewer), load the documents, run the scripted
// work the mode names — clip list, UI shot, conversions, animation capture —
// or hand over to the frame loop.
// ============================================================================

#include "cli/cli_options.h"
#include "whiteout/flakes/gfx_types.h"

namespace whiteout::flakes::renderer {
class RenderService;
class SceneManager;
} // namespace whiteout::flakes::renderer

namespace whiteout::flakes::cli {

/// Runs the viewer for @p mode (anything IsGateHarness does not own) and
/// returns the process exit code. Some modes end the process themselves.
i32 RunViewer(renderer::RenderService& renderer, renderer::SceneManager& scene,
              const CliOptions& options, RunMode mode, gfx::GfxApi backend);

} // namespace whiteout::flakes::cli
