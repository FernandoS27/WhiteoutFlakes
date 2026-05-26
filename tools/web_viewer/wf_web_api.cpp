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
#include "web_audio_emitter.h"
#include "whiteout/flakes/enums.h"
#include "whiteout/flakes/event_data.h"
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
    // Frame counter for the LoadEventDataFiles retry throttle. See
    // wf_tick — without this, the page log gets blown out by SLK-miss
    // WARN lines while the JS drain ferries the bytes in.
    uint32_t eventDataRetryTick = 0;
    // Tracked active camera preset (matches the desktop viewer's
    // `activeCameraPresetIdx_` + `UpdateCameraPresetAnimator` pair).
    // Set by wf_camera_activate_preset; re-evaluated each wf_tick so
    // animated cameras (cinematic flythroughs, portrait sweeps, …)
    // update with the animation cursor.
    uint32_t cameraPresetActor = 0;
    int      cameraPresetIdx   = -1;
};

namespace {
// Re-pose an animated camera preset using the actor's current animation
// time + active-sequence range. Mirrors viewer_app.cpp::
// UpdateCameraPresetAnimator — runs every tick so cameras whose pose is
// driven by a track (MDX cinematic cameras) stay synced with the model.
inline void UpdateAnimatedCameraPreset(WfRenderer* h) {
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
} // namespace

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
        // Swap in the Web Audio-backed sound emitter so MDX SND events
        // get marshaled to JS instead of dropping into the null sink.
        // The JS side (web_audio.js / HiveApp) installs the actual
        // AudioContext + PannerNode graph on first user gesture.
        h->renderer.SwapSoundEmitter(
            std::make_unique<whiteout::flakes::web::WebAudioSoundEmitter>());
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
    auto* cp = h->renderer.Scene().ActiveContentProvider();
    if (cp) cp->Pump();
    // Engine SLK tables (Splats / Sounds / spawn-data) load lazily as
    // the JS drain delivers bytes. Throttle the retry to every ~30 frames
    // (~half-second at 60fps) — running it every frame floods the page
    // log with 20+ "[events] WARN: X.slk: not found" lines per tick
    // while the drain catches up, with no upside since the JS drain
    // itself only fires every 10 frames.
    if (cp) {
        // Force-reload until every critical SLK has populated. Sound SLKs
        // depend on the assetMap (DialogueXxxBase.slk) loading first; if
        // they parsed in a tick where the assetMap was still empty the
        // SndEntry filePaths hold raw labels instead of full paths.
        // force=true re-clears + re-parses so the cache always reflects
        // the latest provider state. Stops automatically once the
        // splat tables are populated (`c.loaded` early-returns the
        // LoadEventDataFiles body).
        if ((++h->eventDataRetryTick % 30) == 0) {
            const bool wasLoaded = whiteout::flakes::io::IsSplCachePopulated();
            whiteout::flakes::io::LoadEventDataFiles(cp, /*force=*/true);
            // Eagerly poke every SPL / UBR texture and every SPN child-
            // model path the moment the splat tables first appear.
            // Single-shot is enough on desktop (sync I/O) — but on web
            // it queues every reference into the provider's missing list
            // so the JS drain can ferry bytes in BEFORE the first event
            // fire, eliminating the placeholder-frame for splats and
            // letting child models build cleanly on their first spawn.
            if (!wasLoaded && whiteout::flakes::io::IsSplCachePopulated())
                whiteout::flakes::io::PrefetchEventAssetPaths(cp);
        }
    }
    h->renderer.Scene().Update(dtSeconds);
    h->renderer.Tick(dtSeconds);
    // Re-pose any active animated camera preset using the now-advanced
    // animation cursor. Mirrors the desktop viewer's per-tick
    // UpdateCameraPresetAnimator call in viewer_app.cpp::Tick.
    UpdateAnimatedCameraPreset(h);
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
    // Child-model templates (attachment slots + PE1 emitter children)
    // load through `FrameTicker::UpdateAttachments` / `UpdatePE1`'s
    // per-frame `Templates().GetOrLoadAsync` loop — which under WASM
    // calls ParseAndBuild and walks the child's texture refs onto the
    // FetchContentProvider's missing list. The JS lazy drain ferries
    // those bytes in and subsequent frames' GetOrLoadAsync re-parses
    // cleanly. No separate prefetch needed here.
    return h->renderer.Loader().SpawnUnit(std::string(mdxPath));
}

void wf_clear_all(WfRenderer* h) {
    if (!h) return;
    h->renderer.Loader().RequestClearAll();
}

// Drop every live splat decal. Hosts call this on sequence change to
// avoid carrying lingering footstep / blood splats across cuts (the
// desktop viewer does the same — viewer_ui.cpp:482).
void wf_clear_splats(WfRenderer* h) {
    if (!h) return;
    h->renderer.Splats().Clear();
}

// Evict the cached model templates so a subsequent SpawnUnit re-parses
// the MDX. Web loader calls this between iterations so newly-fetched
// texture bytes get picked up; without it the second SpawnUnit reuses
// the iter1 template with its failed-load TextureData placeholders.
void wf_clear_template_cache(WfRenderer* h) {
    if (!h) return;
    h->renderer.Loader().ClearTemplateCache();
}

// ----------------------------------------------------------------------------
// Actor controls — wraps ActorView so the JS Instance class can mirror
// mdx-m3-viewer's per-instance API (setSequence, setTransform, setTeamColor,
// timeScale, detach, etc.).
// ----------------------------------------------------------------------------

void wf_actor_destroy(WfRenderer* h, uint32_t actor) {
    if (!h) return;
    h->renderer.Loader().Destroy(actor);
}

// Push a row-major 4x4 transform straight from JS HEAPF32. The renderer
// expects Matrix44f (column-vector convention) but the bytes are
// identical to a row-major matrix when both sides agree on storage
// order; JS callers compose location/rotation/scale and write 16 floats.
void wf_actor_set_transform(WfRenderer* h, uint32_t actor, const float* m) {
    if (!h || !m) return;
    auto av = h->renderer.Actor(actor);
    if (!av.IsValid()) return;
    whiteout::flakes::Matrix44f mat;
    std::memcpy(&mat, m, sizeof(float) * 16);
    av.SetTransform(mat);
}

void wf_actor_set_sequence(WfRenderer* h, uint32_t actor, int seqIdx) {
    if (!h) return;
    auto av = h->renderer.Actor(actor);
    if (av.IsValid()) av.SetActiveSequence(seqIdx);
}

// Returns the render mode the actor's template prefers (0 = SD,
// 1 = HD). JS calls this after wf_spawn_unit and forwards the result to
// wf_set_render_mode so SD-shader models don't render through the HD
// pipeline (which incorrectly blends multi-layer SD materials).
int wf_actor_preferred_render_mode(WfRenderer* h, uint32_t actor) {
    if (!h) return 0;
    auto av = h->renderer.Actor(actor);
    if (!av.IsValid()) return 0;
    return av.PreferredRenderMode() == whiteout::flakes::RenderMode::HD ? 1 : 0;
}

// Maps mdx-m3-viewer's loop modes (0=never, 1=per-model, 2=always) onto
// our renderer's `ignoreNonLooping` flag. When ignoreNonLooping=true the
// renderer holds the last frame of non-looping sequences (mode 0).
// Modes 1 and 2 both let sequences loop per their MDX flag — we don't
// distinguish them on the renderer side.
void wf_actor_set_loop_mode(WfRenderer* h, uint32_t actor, int mode) {
    if (!h) return;
    auto av = h->renderer.Actor(actor);
    if (av.IsValid()) av.SetIgnoreNonLooping(mode == 0);
}

void wf_actor_set_team_color(WfRenderer* h, uint32_t actor, int r, int g, int b) {
    if (!h) return;
    auto av = h->renderer.Actor(actor);
    if (!av.IsValid()) return;
    auto clamp8 = [](int v) -> uint8_t {
        if (v < 0) return 0;
        if (v > 255) return 255;
        return static_cast<uint8_t>(v);
    };
    av.SetTeamColor(clamp8(r), clamp8(g), clamp8(b));
}

void wf_actor_set_playback_speed(WfRenderer* h, uint32_t actor, float speed) {
    if (!h) return;
    auto av = h->renderer.Actor(actor);
    if (av.IsValid()) av.SetPlaybackSpeed(speed);
}

void wf_actor_set_anim_time(WfRenderer* h, uint32_t actor, int ms) {
    if (!h) return;
    auto av = h->renderer.Actor(actor);
    if (av.IsValid()) av.SetAnimationTimeMs(ms);
}

int wf_actor_get_sequence_count(WfRenderer* h, uint32_t actor) {
    if (!h) return 0;
    auto av = h->renderer.Actor(actor);
    if (!av.IsValid()) return 0;
    return static_cast<int>(av.Sequences().size());
}

// ---- Camera presets ---------------------------------------------------------
// Each MDX may embed N named camera setups ("Portrait_Camera",
// "Cinematic_Camera", …). The renderer exposes them per-actor via
// ActorView::CameraPresets(); the desktop viewer's Cameras dropdown is
// populated the same way (see tools/basic_viewer/viewer_app.cpp:546-548).
int wf_actor_camera_preset_count(WfRenderer* h, uint32_t actor) {
    if (!h) return 0;
    auto av = h->renderer.Actor(actor);
    if (!av.IsValid()) return 0;
    return static_cast<int>(av.CameraPresets().size());
}

int wf_actor_camera_preset_name(WfRenderer* h, uint32_t actor, int idx,
                                char* outBuf, int bufCap) {
    if (!h || !outBuf || bufCap <= 0) return 0;
    auto av = h->renderer.Actor(actor);
    if (!av.IsValid()) return 0;
    const auto presets = av.CameraPresets();
    if (idx < 0 || idx >= static_cast<int>(presets.size())) return 0;
    const std::string& s = presets[idx].name;
    const int n = static_cast<int>(std::min<std::size_t>(
        s.size(), static_cast<std::size_t>(bufCap - 1)));
    std::memcpy(outBuf, s.data(), n);
    outBuf[n] = '\0';
    return n;
}

// Activate the actor's preset @ `idx`. `idx < 0` means "Reset" → flip to
// orbital mode + restore the engine's default FoV / clip planes.
// Mirrors tools/basic_viewer/viewer_app.cpp::ActivateCameraPreset: the
// preset's animator (if any) is evaluated once with the actor's current
// animation cursor for the initial pose, and the {actor, idx} is stashed
// on the WfRenderer so wf_tick can re-evaluate the animator each frame
// (`UpdateAnimatedCameraPreset`) as the cursor advances.
void wf_camera_activate_preset(WfRenderer* h, uint32_t actor, int idx) {
    if (!h) return;
    auto cam = h->renderer.Camera();
    if (idx < 0) {
        h->cameraPresetActor = 0;
        h->cameraPresetIdx   = -1;
        cam.SetOrbitalMode();
        cam.SetFovDiagonal(whiteout::flakes::CameraView::kDefaultFovDiagonal);
        cam.SetClip(whiteout::flakes::CameraView::kDefaultNearZ,
                    whiteout::flakes::CameraView::kDefaultFarZ);
        cam.Reset();
        return;
    }
    auto av = h->renderer.Actor(actor);
    if (!av.IsValid()) return;
    const auto presets = av.CameraPresets();
    if (idx >= static_cast<int>(presets.size())) return;
    const auto& p = presets[idx];
    cam.SetFovDiagonal(p.fovDiagonal > 1e-3f
                       ? p.fovDiagonal
                       : whiteout::flakes::CameraView::kDefaultFovDiagonal);
    cam.SetClip(p.zNear, p.zFar);
    cam.SetDirectPose(p.position, p.target, p.staticRoll);

    // Record the active preset BEFORE evaluating the animator so the
    // per-tick path picks up the same state on the next wf_tick.
    h->cameraPresetActor = actor;
    h->cameraPresetIdx   = idx;
    UpdateAnimatedCameraPreset(h);
}

// Copy the i'th sequence name (null-terminated) into the JS-supplied
// buffer. Returns the number of bytes written (excluding the trailing
// NUL) or 0 on error.
int wf_actor_get_sequence_name(WfRenderer* h, uint32_t actor, int idx,
                               char* outBuf, int bufCap) {
    if (!h || !outBuf || bufCap <= 0) return 0;
    auto av = h->renderer.Actor(actor);
    if (!av.IsValid()) return 0;
    const auto seqs = av.Sequences();
    if (idx < 0 || idx >= static_cast<int>(seqs.size())) return 0;
    const std::string& s = seqs[idx].name;
    const int n = static_cast<int>(std::min<std::size_t>(
        s.size(), static_cast<std::size_t>(bufCap - 1)));
    std::memcpy(outBuf, s.data(), n);
    outBuf[n] = '\0';
    return n;
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
