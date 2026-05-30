#pragma once

// Shared types between the wf_web_* TUs. Internal to the web facade;
// JS sees only the extern "C" exports.

#include "io/fetch_content_provider.h"
#include "whiteout/flakes/renderer.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wf_web {

struct WfRenderer {
    whiteout::flakes::Renderer     renderer;
    whiteout::flakes::RenderTargetId target = whiteout::flakes::RenderTargetId{};
    // Owned copy — the surface descriptor holds the const char* lazily.
    std::string canvasSelector;
    // JS pushes bytes via wf_provider_put; renderer holds a shared_ptr.
    std::shared_ptr<whiteout::flakes::io::FetchContentProvider> provider;
    // Snapshot of the AssetManager needs queue for JS pumping.
    struct AssetNeed { int kind; std::string path; };
    std::vector<AssetNeed> lastNeeds;
    bool inited = false;
    // LoadEventDataFiles retry throttle (see wf_tick).
    std::uint32_t eventDataRetryTick = 0;
    // Mirrors desktop viewer's activeCameraPresetIdx_ + animator pair.
    std::uint32_t cameraPresetActor = 0;
    int           cameraPresetIdx   = -1;
};

// Last facade-level error. -fno-exceptions, so genuine deps errors
// abort wasm; only deliberate writes show up here.
extern std::string g_lastError;

// Wrapper kept for shape — no try/catch under -fno-exceptions.
template <class Fn>
auto Guard(const char* /*tag*/, Fn&& fn) -> decltype(fn()) {
    return fn();
}

// Re-pose an active animated camera preset against the actor's
// current animation cursor. Mirrors viewer_app.cpp's per-tick call.
void UpdateAnimatedCameraPreset(WfRenderer* h);

} // namespace wf_web
