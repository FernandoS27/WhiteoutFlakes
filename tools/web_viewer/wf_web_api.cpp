// ============================================================================
// wf_web_api.cpp — C facade for the WhiteoutFlakes browser host.
//
// JS instantiates wf-core.{js,wasm}, then drives this facade via `cwrap`/
// `ccall`. The set kept here is the *minimum* needed for the Phase 1
// milestone (clear-color on the canvas); later phases extend it with model
// loading (Phase 3), camera+sequences (Phase 4), and audio (Phase 5).
//
// Conventions:
//   * Every exported symbol is `extern "C"` with integer/pointer args only.
//   * The WfRenderer struct owns: the Renderer (and its View graph) plus
//     the swap-chain target id and a stable copy of the canvas-selector
//     string (EmscriptenSurfaceSourceCanvasHTMLSelector.selector is read
//     lazily by WebGPU; the string must outlive every Configure call).
//   * No globals — every entry takes the WfRenderer* handle.
//
// Phase 1's "clear color" is produced by RenderFrame on an empty scene:
// the renderer clears to SettingsView::BackgroundColorRaw() and presents.
// ============================================================================

#include "io/fetch_content_provider.h"
#include "whiteout/flakes/enums.h"
#include "whiteout/flakes/gfx_types.h"
#include "whiteout/flakes/renderer.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <vector>

using whiteout::flakes::Renderer;
using whiteout::flakes::RenderTargetId;
using whiteout::flakes::gfx::GfxApi;
using whiteout::flakes::io::FetchContentProvider;

namespace {

struct WfRenderer {
    Renderer renderer;
    RenderTargetId target = RenderTargetId{};
    // Selector lifetime: WebGPU's CanvasHTMLSelector source holds a raw
    // `const char*` we hand it through CreateSurface. Keep a copy alive
    // for the surface's lifetime (== the WfRenderer's lifetime here).
    std::string canvasSelector;
    // Web content provider — JS pushes bytes into it via wf_provider_put
    // before wf_init runs. The renderer takes a shared_ptr at SetContentProvider
    // time, and we keep our own ref so wf_provider_put can call Put().
    std::shared_ptr<FetchContentProvider> provider;
    // Snapshot of provider misses produced by the most recent
    // wf_provider_missing_count() call. JS reads it back path-by-path
    // via wf_provider_missing_get(idx, buf, cap).
    std::vector<std::string> lastMissing;
    bool inited = false;
};

// Last facade-level error message — populated by the C++ side when
// something it knows about goes wrong (e.g. Pipeline().InitDevice
// reporting IsDeviceReady=false). The build is -fno-exceptions, so any
// genuine error inside our deps aborts the wasm and JS never sees it
// here. Module is single-threaded so a plain static is fine.
std::string g_lastError;

template <class Fn>
auto Guard(const char* /*tag*/, Fn&& fn) -> decltype(fn()) {
    // No try/catch: under -fno-exceptions nothing can be caught. Kept as
    // a wrapper so call sites stay shaped for a future revival of EH if
    // we ever need it.
    return fn();
}

} // namespace

extern "C" {

// ----------------------------------------------------------------------------
// Lifecycle.
// ----------------------------------------------------------------------------

WfRenderer* wf_create() {
    return Guard("wf_create", []() -> WfRenderer* {
        auto* h = new WfRenderer{};
        // Install the FetchContentProvider before InitDevice runs.
        // SceneManager's default FileContentProvider becomes inactive;
        // any IContentProvider::Request from the renderer (BLS shader
        // cache, IBL probe loader, DNC service, ...) hits our in-memory
        // cache that JS populated via wf_provider_put.
        h->provider = std::make_shared<FetchContentProvider>();
        h->renderer.Scene().SetContentProvider(h->provider);
        return h;
    });
}

void wf_provider_put(WfRenderer* h, const char* path, const uint8_t* data, int len) {
    if (!h || !h->provider || !path || !data || len < 0) return;
    h->provider->Put(std::string(path),
                     std::vector<uint8_t>(data, data + static_cast<std::size_t>(len)));
}

int wf_provider_count(WfRenderer* h) {
    if (!h || !h->provider) return 0;
    return static_cast<int>(h->provider->CachedFileCount());
}

// JS reads this when a wf_* entry returns 0/null. Empty string == no
// captured error since the last successful call.
const char* wf_last_error() {
    return g_lastError.c_str();
}

void wf_destroy(WfRenderer* h) {
    if (!h) return;
    if (h->inited) {
        h->renderer.Pipeline().Shutdown();
    }
    delete h;
}

// ----------------------------------------------------------------------------
// One-shot device + swap-chain bring-up.
//
// JS must have set `Module.preinitializedWebGPUDevice` to a GPUDevice before
// the WASM module was instantiated. webgpu_init.cpp's __EMSCRIPTEN__ path
// retrieves it via emscripten_webgpu_get_device().
// ----------------------------------------------------------------------------

int wf_init(WfRenderer* h, const char* canvasSelector, int width, int height) {
    if (!h || !canvasSelector || width <= 0 || height <= 0) return 0;
    if (h->inited) return 1;

    return Guard("wf_init", [&]() -> int {
        h->canvasSelector.assign(canvasSelector);

        auto pipeline = h->renderer.Pipeline();
        std::fprintf(stderr, "[wf] wf_init: calling Pipeline().InitDevice(WebGPU)\n");
        pipeline.InitDevice(GfxApi::WebGPU);
        if (!pipeline.IsDeviceReady()) {
            g_lastError = "wf_init: Pipeline().InitDevice returned with IsDeviceReady=false";
            std::fprintf(stderr, "[wf] %s\n", g_lastError.c_str());
            return 0;
        }
        std::fprintf(stderr, "[wf] wf_init: device ready, creating swap-chain target\n");

        // The void* is reinterpreted per-backend; the EMSCRIPTEN branch of
        // webgpu_swap_chain.cpp casts it back to `const char*` selector.
        void* surfaceHandle = static_cast<void*>(const_cast<char*>(h->canvasSelector.c_str()));
        h->target = pipeline.CreateSwapChainTarget(surfaceHandle, width, height);
        pipeline.SetPrimaryTarget(h->target);
        std::fprintf(stderr, "[wf] wf_init: done\n");

        h->inited = true;
        return 1;
    });
}

// ----------------------------------------------------------------------------
// Per-frame + resize.
// ----------------------------------------------------------------------------

void wf_resize(WfRenderer* h, int width, int height) {
    if (!h || !h->inited || width <= 0 || height <= 0) return;
    h->renderer.Pipeline().ResizePrimaryTarget(width, height);
}

void wf_tick(WfRenderer* h, float dtSeconds) {
    if (!h || !h->inited) return;
    // Drive completion callbacks for any pending IContentProvider::Request
    // submitted by the renderer. Our FetchContentProvider resolves reads
    // synchronously inside Request(), so Pump() just runs the callbacks
    // here on the render thread (where bls cache state, etc. mutate).
    if (auto* cp = h->renderer.Scene().ActiveContentProvider()) cp->Pump();
    h->renderer.Scene().Update(dtSeconds);
    h->renderer.Tick(dtSeconds);
}

void wf_render(WfRenderer* h) {
    if (!h || !h->inited) return;
    auto pipeline = h->renderer.Pipeline();
    pipeline.RenderFrame(h->target);
    pipeline.Present(h->target);
}

// ----------------------------------------------------------------------------
// Phase-1 convenience: tweak the clear color from JS so the smoke test
// can verify the WASM end-to-end path (and not just a black canvas the
// browser would show anyway).
// ----------------------------------------------------------------------------

// Material path: 0 = SD (classic WC3 materials), 1 = HD (Reforged PBR
// with IBL). Default in DisplayFlags is SD; the web viewer flips to HD
// after wf_init for HD models. Bad values are clamped to SD.
void wf_set_render_mode(WfRenderer* h, int mode) {
    if (!h) return;
    h->renderer.Settings().SetRenderMode(
        mode == 1 ? whiteout::flakes::RenderMode::HD
                  : whiteout::flakes::RenderMode::SD);
}

void wf_set_background(WfRenderer* h, int r, int g, int b) {
    if (!h) return;
    auto clamp8 = [](int v) -> uint8_t {
        if (v < 0) return 0;
        if (v > 255) return 255;
        return static_cast<uint8_t>(v);
    };
    h->renderer.Settings().SetBackgroundColor(clamp8(r), clamp8(g), clamp8(b));
}

// ----------------------------------------------------------------------------
// Camera — orbital controls forwarded from JS pointer/wheel events.
// CameraView already implements orbit math; we just hand it deltas.
// ----------------------------------------------------------------------------

void wf_camera_rotate(WfRenderer* h, int dx, int dy) {
    if (!h) return;
    h->renderer.Camera().Rotate(dx, dy);
}

void wf_camera_pan(WfRenderer* h, int dx, int dy) {
    if (!h) return;
    h->renderer.Camera().Pan(dx, dy);
}

void wf_camera_zoom(WfRenderer* h, int wheelDelta) {
    if (!h) return;
    h->renderer.Camera().Zoom(wheelDelta);
}

void wf_camera_reset(WfRenderer* h) {
    if (!h) return;
    h->renderer.Camera().Reset();
}

// ----------------------------------------------------------------------------
// Model loading — exposes LoaderView so JS can spawn / clear actors and
// iterate over the provider's missing-paths to satisfy texture / child-MDX
// dependencies discovered during SpawnUnit.
// ----------------------------------------------------------------------------

void wf_set_pe1_base(WfRenderer* h, const char* basePath) {
    if (!h || !basePath) return;
    h->renderer.Scene().SetPE1BasePath(std::filesystem::path(basePath));
}

uint32_t wf_spawn_unit(WfRenderer* h, const char* mdxPath) {
    if (!h || !mdxPath) return 0;
    return h->renderer.Loader().SpawnUnit(std::string(mdxPath));
}

void wf_clear_all(WfRenderer* h) {
    if (!h) return;
    h->renderer.Loader().RequestClearAll();
}

// Evict the cached model templates so a subsequent SpawnUnit re-parses
// the MDX. Web loader calls this between iterations so newly-fetched
// texture bytes get picked up; without it the second SpawnUnit reuses
// the iter1 template with its failed-load TextureData placeholders.
void wf_clear_template_cache(WfRenderer* h) {
    if (!h) return;
    h->renderer.Loader().ClearTemplateCache();
}

// ---- Missing-paths retrieval ------------------------------------------------
// JS calls wf_provider_missing_count() after SpawnUnit fails / partially
// resolves to learn how many files the renderer requested that weren't in
// the cache. wf_provider_missing_get(i, buf, cap) copies the i'th path
// into JS's heap buffer (null-terminated). wf_provider_missing_clear()
// drains the list so the next SpawnUnit attempt records a fresh set.

int wf_provider_missing_count(WfRenderer* h) {
    if (!h || !h->provider) return 0;
    // Peek without taking; JS may want to inspect first, then drain via
    // explicit get-by-index calls. We snapshot via TakeMissing and immediately
    // put it back so the count is stable across the subsequent get calls.
    auto snap = h->provider->TakeMissing();
    const int n = static_cast<int>(snap.size());
    // Stash for subsequent get-by-index calls in a per-renderer cache.
    h->lastMissing = std::move(snap);
    return n;
}

int wf_provider_missing_get(WfRenderer* h, int index, char* outBuf, int bufCap) {
    if (!h || !outBuf || bufCap <= 0) return 0;
    if (index < 0 || index >= static_cast<int>(h->lastMissing.size())) return 0;
    const std::string& s = h->lastMissing[index];
    const int n = static_cast<int>(std::min<std::size_t>(s.size(), static_cast<std::size_t>(bufCap - 1)));
    std::memcpy(outBuf, s.data(), n);
    outBuf[n] = '\0';
    return n;
}

} // extern "C"
