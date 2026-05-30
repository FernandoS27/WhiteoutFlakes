// Lifecycle + content-provider + error surface for the browser facade.
// Settings/camera/actor/assets exports live in sibling TUs; the shared
// `WfRenderer` and helpers are in `wf_web_internal.h`.

#include "wf_web_internal.h"

#include "web_audio_emitter.h"
#include "whiteout/flakes/event_data.h"
#include "whiteout/flakes/gfx_types.h"

#include <cstdio>
#include <new>

using wf_web::WfRenderer;
using wf_web::Guard;
using whiteout::flakes::gfx::GfxApi;
using whiteout::flakes::io::FetchContentProvider;

namespace wf_web {

std::string g_lastError;

void UpdateAnimatedCameraPreset(WfRenderer* h) {
    if (!h || h->cameraPresetIdx < 0) return;
    auto av = h->renderer.Actor(h->cameraPresetActor);
    if (!av.IsValid()) {
        h->cameraPresetIdx = -1;
        return;
    }
    const auto presets = av.CameraPresets();
    if (h->cameraPresetIdx >= static_cast<int>(presets.size())) return;
    const auto& p = presets[h->cameraPresetIdx];
    if (!p.animator) return;

    const auto seqs   = av.Sequences();
    const int  seqIdx = av.ActiveSequenceIndex();
    whiteout::flakes::i32 seqStart = 0;
    whiteout::flakes::i32 seqEnd   = 1 << 30;
    if (seqIdx >= 0 && seqIdx < static_cast<int>(seqs.size())) {
        seqStart = seqs[seqIdx].startMs;
        seqEnd   = seqs[seqIdx].endMs;
    }
    whiteout::flakes::Vector3f pos = p.position;
    whiteout::flakes::Vector3f tgt = p.target;
    whiteout::flakes::f32      roll = p.staticRoll;
    p.animator(pos, tgt, roll, av.AnimationTimeMs(), seqStart, seqEnd);
    h->renderer.Camera().SetDirectPose(pos, tgt, roll);
}

} // namespace wf_web

extern "C" {

WfRenderer* wf_create() {
    return Guard("wf_create", []() -> WfRenderer* {
        auto* h = new WfRenderer{};
        // Install before InitDevice so all IContentProvider::Request
        // hits go through the JS-pushed cache.
        h->provider = std::make_shared<FetchContentProvider>();
        h->renderer.Scene().SetContentProvider(h->provider);
        // Route MDX SND events to web_audio.js (gated on user gesture).
        h->renderer.SwapSoundEmitter(
            std::make_unique<whiteout::flakes::web::WebAudioSoundEmitter>());
        return h;
    });
}

void wf_destroy(WfRenderer* h) {
    if (!h) return;
    if (h->inited) h->renderer.Pipeline().Shutdown();
    delete h;
}

// Device + swap-chain bring-up. JS must have set
// Module.preinitializedWebGPUDevice before WASM instantiation.
int wf_init(WfRenderer* h, const char* canvasSelector, int width, int height) {
    if (!h || !canvasSelector || width <= 0 || height <= 0) return 0;
    if (h->inited) return 1;

    return Guard("wf_init", [&]() -> int {
        h->canvasSelector.assign(canvasSelector);

        auto pipeline = h->renderer.Pipeline();
        std::fprintf(stderr, "[wf] wf_init: calling Pipeline().InitDevice(WebGPU)\n");
        pipeline.InitDevice(GfxApi::WebGPU);
        if (!pipeline.IsDeviceReady()) {
            wf_web::g_lastError =
                "wf_init: Pipeline().InitDevice returned with IsDeviceReady=false";
            std::fprintf(stderr, "[wf] %s\n", wf_web::g_lastError.c_str());
            return 0;
        }
        std::fprintf(stderr, "[wf] wf_init: device ready, creating swap-chain target\n");

        // EMSCRIPTEN backend casts this back to `const char*` selector.
        void* surfaceHandle = static_cast<void*>(const_cast<char*>(h->canvasSelector.c_str()));
        h->target = pipeline.CreateSwapChainTarget(surfaceHandle, width, height);
        pipeline.SetPrimaryTarget(h->target);
        std::fprintf(stderr, "[wf] wf_init: done\n");

        h->inited = true;
        return 1;
    });
}

void wf_resize(WfRenderer* h, int width, int height) {
    if (!h || !h->inited || width <= 0 || height <= 0) return;
    h->renderer.Pipeline().ResizePrimaryTarget(width, height);
}

void wf_tick(WfRenderer* h, float dtSeconds) {
    if (!h || !h->inited) return;
    // FetchContentProvider resolves synchronously inside Request();
    // Pump just runs the callbacks on the render thread.
    auto* cp = h->renderer.Scene().ActiveContentProvider();
    if (cp) cp->Pump();
    // Throttle event-data retries to ~2 Hz — running per-frame floods
    // the log with "X.slk: not found" while JS catches up.
    if (cp) {
        // Skip the retry once all tables populated — force=true
        // bypasses the early-out and re-clears+parses every 30 frames.
        const bool allLoaded =
            whiteout::flakes::io::IsSpnCachePopulated() &&
            whiteout::flakes::io::IsSplCachePopulated() &&
            whiteout::flakes::io::IsUbrCachePopulated() &&
            whiteout::flakes::io::IsSndCachePopulated();
        if (!allLoaded && (++h->eventDataRetryTick % 30) == 0) {
            const bool wasLoaded = whiteout::flakes::io::IsSplCachePopulated();
            whiteout::flakes::io::LoadEventDataFiles(cp, /*force=*/true);
            // Refcounted SPL/UBR/SPN slots — survive animation changes.
            if (!wasLoaded && whiteout::flakes::io::IsSplCachePopulated())
                h->renderer.Assets().PrefetchEventAssets();
        }
    }
    h->renderer.Scene().Update(dtSeconds);
    h->renderer.Tick(dtSeconds);
    wf_web::UpdateAnimatedCameraPreset(h);
}

void wf_render(WfRenderer* h) {
    if (!h || !h->inited) return;
    auto pipeline = h->renderer.Pipeline();
    pipeline.RenderFrame(h->target);
    pipeline.Present(h->target);
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

// JS evicts MDX bytes post-spawn so they don't pile up.
int wf_provider_evict(WfRenderer* h, const char* path) {
    if (!h || !h->provider || !path) return 0;
    return h->provider->Evict(std::string(path)) ? 1 : 0;
}

// Read when an entry returns 0/null. "" = no captured error.
const char* wf_last_error() {
    return wf_web::g_lastError.c_str();
}

} // extern "C"
