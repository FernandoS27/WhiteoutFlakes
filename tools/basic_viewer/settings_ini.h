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

}
