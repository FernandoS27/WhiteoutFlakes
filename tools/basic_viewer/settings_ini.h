#pragma once

#include <string>
#include <vector>

namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes {

// `loopNonLoopingPolicy` is a tool-side default: when true, freshly-loaded
// actors should have actor->ignoreNonLooping = true (callers apply this on
// load). `forceHd` is the "Reforged Graphics" tool toggle (force HD for every
// model). Both are viewer-side bools the caller applies after load; the rest of
// the settings live on RenderService. LoadSettingsIni populates them from disk;
// SaveSettingsIni reads them.
void LoadSettingsIni(renderer::RenderService& service, bool& loopNonLoopingPolicy, bool& forceHd);

void SaveSettingsIni(const renderer::RenderService& service, bool loopNonLoopingPolicy,
                     bool forceHd);

// Reads only the keys that must land on RenderSettings *before* the
// render thread spins up — currently `GraphicsDebug` (the validation
// layer is wired in at gfx::CreateDevice time) and `DefaultBackend`
// (test_main consults it when --backend is omitted). Default values
// stay in place when the keys are missing.
void LoadStartupSettingsFromIni(renderer::RenderService& service);

// User overrides for the FileContentProvider, driven by the Settings > IO
// tab. installPath empty means "use the auto-detected Warcraft III path";
// mpqListSet=false means "use FileContentProvider::DefaultMpqList()" (so
// "never opened the IO tab" silently falls back to defaults instead of
// loading zero MPQs).
struct IoPathOverrides {
    std::string installPath;
    bool ignoreCasc = false;
    bool ignoreMpq = false;
    bool mpqListSet = false;
    std::vector<std::string> mpqList;
};

IoPathOverrides LoadIoPathOverrides();
void SaveIoPathOverrides(const IoPathOverrides& overrides);

} // namespace whiteout::flakes
