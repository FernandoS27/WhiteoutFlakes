#pragma once

namespace whiteout::flakes::renderer { class RenderService; }

namespace whiteout::flakes {

// `loopNonLoopingPolicy` is a tool-side default: when true, freshly-loaded
// actors should have actor->ignoreNonLooping = true (callers apply this on
// load). LoadSettingsIni populates the bool from disk; SaveSettingsIni reads
// it.
void LoadSettingsIni(renderer::RenderService& service,
                     bool& loopNonLoopingPolicy);

void SaveSettingsIni(const renderer::RenderService& service,
                     bool loopNonLoopingPolicy);

// Reads only the keys that must land on RenderSettings *before* the
// render thread spins up — currently `GraphicsDebug` (the validation
// layer is wired in at gfx::CreateDevice time) and `DefaultBackend`
// (test_main consults it when --backend is omitted). Default values
// stay in place when the keys are missing.
void LoadStartupSettingsFromIni(renderer::RenderService& service);

}
