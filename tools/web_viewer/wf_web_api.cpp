// ============================================================================
// wf_web_api.cpp — C facade for the WhiteoutFlakes browser host.
//
// JS instantiates wf-core.{js,wasm}, then drives this facade via
// `cwrap` / `ccall`. Exports cover the full host surface: device +
// canvas init, model spawn / actor controls, settings, camera, sound,
// and the AssetManager push pipeline (needs queue + apply).
//
// Conventions:
//   * Every exported symbol is `extern "C"` with integer/pointer args.
//   * The WfRenderer struct owns: the Renderer (and its View graph)
//     plus the swap-chain target id and a stable copy of the canvas-
//     selector string (EmscriptenSurfaceSourceCanvasHTMLSelector reads
//     the selector lazily; the string must outlive every Configure
//     call).
//   * No globals — every entry takes the WfRenderer* handle.
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
#include <span>
#include <string>
#include <string_view>
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
    // Snapshot of AssetManager needs produced by the most recent
    // wf_assets_needs_count() call. JS reads each entry's (kind, path)
    // via wf_assets_needs_get_kind/_get_path.
    struct AssetNeed { int kind; std::string path; };
    std::vector<AssetNeed> lastNeeds;
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

// Drop the bytes for @p path from the provider's in-memory cache.
// JS calls this after a spawn so the MDX bytes (only needed during
// the synchronous ParseAndBuild request that fires inside SpawnUnit)
// don't pile up across many model loads.
int wf_provider_evict(WfRenderer* h, const char* path) {
    if (!h || !h->provider || !path) return 0;
    return h->provider->Evict(std::string(path)) ? 1 : 0;
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
        // Force-reload only while at least one critical SLK table is
        // still empty. Once SPN / SPL / UBR / SND have all populated,
        // STOP — otherwise force=true bypasses the `c.loaded`
        // early-return inside LoadEventDataFiles and re-clears +
        // re-parses every SLK every 30 frames forever, churning
        // hundreds of map entries + strings each pass and bleeding
        // heap ~10KB/frame at idle.
        const bool allLoaded =
            whiteout::flakes::io::IsSpnCachePopulated() &&
            whiteout::flakes::io::IsSplCachePopulated() &&
            whiteout::flakes::io::IsUbrCachePopulated() &&
            whiteout::flakes::io::IsSndCachePopulated();
        if (!allLoaded && (++h->eventDataRetryTick % 30) == 0) {
            const bool wasLoaded = whiteout::flakes::io::IsSplCachePopulated();
            whiteout::flakes::io::LoadEventDataFiles(cp, /*force=*/true);
            // First time the splat tables appear: Acquire AssetManager
            // slots for every SPL/UBR texture and SPN child-model.
            // Slots stay refcounted for the rest of the session so the
            // host pump fetches them eagerly and they survive animation
            // changes.
            if (!wasLoaded && whiteout::flakes::io::IsSplCachePopulated())
                h->renderer.Assets().PrefetchEventAssets();
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

// Light-rig preset: 0 = InGame (engine-runtime), 1 = Glue (loading-screen /
// portrait), 2 = Dynamic (DNC). Bad values clamp to InGame.
void wf_set_lighting_mode(WfRenderer* h, int mode) {
    if (!h) return;
    using LM = whiteout::flakes::LightingMode;
    LM lm = (mode == 1) ? LM::Glue : (mode == 2) ? LM::Dynamic : LM::InGame;
    h->renderer.Settings().SetLightingMode(lm);
}

// Ground reference grid.
void wf_set_show_grid(WfRenderer* h, int on) {
    if (!h) return;
    auto df = h->renderer.Settings().GetDisplayFlags();
    df.showGrid = (on != 0);
    h->renderer.Settings().SetDisplayFlags(df);
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

// ---- AssetManager bridge ----------------------------------------------------
// JS pumps the needs queue with wf_assets_needs_count() + _get(i, buf, cap),
// then fetches each path and pushes bytes back via wf_assets_apply(kind,
// path, ptr, len, ext). The needs list is buffered C++-side until JS
// asks for it — DrainNeeds clears the buffer, so a follow-up _get(i)
// call reads from the snapshot stashed on WfRenderer.
//
// Kind values mirror AssetsView::Kind: 0 = Texture, 1 = Particle, 2 = ChildModel.

int wf_assets_needs_count(WfRenderer* h) {
    if (!h) return 0;
    // Snapshot from the AssetManager into our per-renderer cache.
    h->lastNeeds.clear();
    h->renderer.Assets().DrainNeeds(
        [&](whiteout::flakes::AssetsView::Kind k, std::string_view path) {
            h->lastNeeds.push_back({static_cast<int>(k), std::string(path)});
        });
    return static_cast<int>(h->lastNeeds.size());
}

int wf_assets_needs_get_kind(WfRenderer* h, int index) {
    if (!h) return -1;
    if (index < 0 || index >= static_cast<int>(h->lastNeeds.size())) return -1;
    return h->lastNeeds[index].kind;
}

int wf_assets_needs_get_path(WfRenderer* h, int index, char* outBuf, int bufCap) {
    if (!h || !outBuf || bufCap <= 0) return 0;
    if (index < 0 || index >= static_cast<int>(h->lastNeeds.size())) return 0;
    const std::string& s = h->lastNeeds[index].path;
    const int n = static_cast<int>(std::min<std::size_t>(
        s.size(), static_cast<std::size_t>(bufCap - 1)));
    std::memcpy(outBuf, s.data(), n);
    outBuf[n] = '\0';
    return n;
}

int wf_assets_apply(WfRenderer* h, int kind, const char* path,
                    const void* bytes, int len, const char* foundExt) {
    if (!h || !path || !bytes || len <= 0) return 0;
    if (kind < 0 || kind > 2) return 0;
    const auto k = static_cast<whiteout::flakes::AssetsView::Kind>(kind);
    std::span<const std::uint8_t> span(
        static_cast<const std::uint8_t*>(bytes), static_cast<std::size_t>(len));
    return h->renderer.Assets().ApplyAsset(
        k, std::string_view(path), span,
        foundExt ? std::string_view(foundExt) : std::string_view{}) ? 1 : 0;
}

// Live GPU bytes currently allocated by the WebGPU backend (sum of every
// outstanding CreateTexture + CreateBuffer that hasn't been drained from
// the deferred-delete queue). Diagnostic — JS pulls into the overlay /
// console to spot leaks across model loads.
double wf_gpu_bytes(WfRenderer* h) {
    if (!h) return 0.0;
    // Return as double — JS uses 64-bit floats and emscripten's i64
    // marshaling is awkward; double exactly represents integers up to 2^53
    // which covers any plausible VRAM total.
    return static_cast<double>(h->renderer.Pipeline().LiveGpuBytes());
}

// Diagnostics — JS pulls these into the page overlay during dev.
int wf_assets_stat(WfRenderer* h, int which) {
    if (!h) return 0;
    auto s = h->renderer.Assets().GetStats();
    switch (which) {
        case 0: return static_cast<int>(s.liveSlots);
        case 1: return static_cast<int>(s.loadedSlots);
        case 2: return static_cast<int>(s.pendingNeeds);
        case 3: return static_cast<int>(s.totalAcquires);
        case 4: return static_cast<int>(s.totalReleases);
        case 5: return static_cast<int>(s.totalApplies);
        case 6: return static_cast<int>(s.totalApplyMisses);
        default: return 0;
    }
}

} // extern "C"
