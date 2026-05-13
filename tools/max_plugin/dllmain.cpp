// ============================================================================
// WhiteoutDex — All-in-One Max SDK Plugin (.dlx)
//
// Adapter pattern (live source): MaxSceneAdapter implements both
// IModelDataSource (Build()) and IAnimationSource (Evaluate()), so a single
// SpawnActorFromLiveSource call covers both the static snapshot AND the
// per-frame animation feed. The adapter outlives the renderer because the
// actor's AnimationDriver holds a shared_ptr to it.
//
// MaxScript API:
//   ndxStart()             → Extract scene + open renderer + start sync
//   ndxStop()              → Stop everything + close window
//   ndxRefreshMaterials()  → Re-read material properties (hot reload)
// ============================================================================

#include "max_scene_adapter.h"
#include "render_window.h"
#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/model/model_instance.h" // Actor
#include "renderer/model/model_loader.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "whiteout/flakes/types.h"
#include "windows_sound_emitter.h"

#include <chrono>
#include <filesystem>
#include <memory>

#include <max.h>
#include <maxscript/foundation/numbers.h>
#include <maxscript/macros/define_instantiation_functions.h>
#include <maxscript/maxscript.h>
#include <maxscript/util/listener.h>
#include <maxversion.h>

using namespace whiteout::flakes;

// ============================================================================
// Global state
// ============================================================================
static std::shared_ptr<whiteout::flakes::MaxSceneAdapter> g_adapter;
static whiteout::flakes::renderer::model::Actor* g_actor = nullptr; // borrowed; owned by g_scene
static whiteout::flakes::renderer::SceneManager* g_scene = nullptr;
static whiteout::flakes::renderer::RenderService* g_renderer = nullptr;
static whiteout::flakes::RenderWindow* g_renderWindow = nullptr;
static bool g_running = false;
static HINSTANCE g_hInstance = nullptr;
static DWORD g_lastTimeChangedTick = 0;

// Convert Max time (TimeValue ticks) to milliseconds. Returns 0 if Max
// reports a missing tick rate (rare; tested ndxStart paths).
static i32 MaxTimeToMs(TimeValue t) {
    i32 tpf = GetTicksPerFrame(), fps = GetFrameRate();
    return (tpf > 0 && fps > 0) ? (i32)((f32)t / (f32)tpf * 1000.0f / (f32)fps) : 0;
}

// Push Max's externally-driven time onto the actor + scene clock, then run
// eval+apply on Max's thread. MaxSceneAdapter::Evaluate reads live Max
// scene state (node TMs, modifier params, vertex paint, materials) which is
// only thread-safe to touch from Max's UI thread — the render-thread Tick
// can't do this for us.
static void EvalFromMax(i32 timeMs) {
    if (!g_actor || !g_renderer || !g_scene)
        return;
    g_actor->animation.SetTimeMs(timeMs);
    g_scene->SetAnimationTime(timeMs);
    g_actor->EvaluateAndApply(g_renderer->MakeActorEvalContext());
}

// ============================================================================
// TimeChange callback — Max scrubs the timeline; we re-evaluate the actor.
// ============================================================================
class NdxTimeCallback : public TimeChangeCallback {
public:
    void TimeChanged(TimeValue t) override {
        if (!g_running || !g_renderer || !g_actor)
            return;
        if (!g_renderWindow || !g_renderWindow->IsOpen())
            return;
        g_lastTimeChangedTick = GetTickCount();
        EvalFromMax(MaxTimeToMs(t));
    }
};

static NdxTimeCallback* g_timeCallback = nullptr;

// ============================================================================
// Material polling timer — detects property changes even without timeline scrub
// ============================================================================
static UINT_PTR g_materialTimerId = 0;

static void CALLBACK MaterialPollTimer(HWND, UINT, UINT_PTR, DWORD) {
    if (!g_running || !g_renderer || !g_adapter || !g_actor)
        return;
    if (!g_renderWindow || !g_renderWindow->IsOpen())
        return;

    // Hot-reload check.
    auto result = g_adapter->RefreshMaterials();
    if (result.changed) {
        g_renderer->Loader().UpdateMaterials(g_actor->handle, result.materials, result.textures);
    }

    // Re-evaluate when the timeline is idle — picks up non-animated
    // changes (vertex paint, modifier toggles, visibility flips). Skip
    // when TimeChanged is actively firing so playback isn't fighting a
    // duplicate eval on the same frame.
    DWORD now = GetTickCount();
    if (now - g_lastTimeChangedTick > 1000) {
        Interface* ip = GetCOREInterface();
        if (ip)
            EvalFromMax(MaxTimeToMs(ip->GetTime()));
    }
}

// ============================================================================
// Helpers
// ============================================================================
static void NdxCleanup() {
    if (g_materialTimerId) {
        KillTimer(nullptr, g_materialTimerId);
        g_materialTimerId = 0;
    }
    if (g_timeCallback) {
        Interface* ip = GetCOREInterface();
        if (ip)
            ip->UnRegisterTimeChangeCallback(g_timeCallback);
        delete g_timeCallback;
        g_timeCallback = nullptr;
    }
    g_running = false;
    if (g_renderer) {
        g_renderer->Loader().RequestClearAll();
    }
    if (g_renderWindow) {
        g_renderWindow->Close();
        delete g_renderWindow;
        g_renderWindow = nullptr;
    }
    // Order: actors die with the renderer's scene-clear/release; release the
    // renderer next so its non-owning scene pointer goes inert; drop the
    // adapter shared_ptr (the actor's AnimationDriver was the last other
    // holder, so this destroys the adapter); finally tear down scene.
    g_actor = nullptr;
    if (g_renderer) {
        delete g_renderer;
        g_renderer = nullptr;
    }
    g_adapter.reset();
    if (g_scene) {
        delete g_scene;
        g_scene = nullptr;
    }
    mprintf(_M("WhiteoutDex: === STOPPED ===\n"));
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInstance = hInst;
        DisableThreadLibraryCalls(hInst);
    }
    if (reason == DLL_PROCESS_DETACH) {
        NdxCleanup();
    }
    return TRUE;
}

// ============================================================================
// Max Plugin Descriptor
// ============================================================================
class WhiteoutDexExtractorClassDesc : public ClassDesc2 {
public:
    i32 IsPublic() override {
        return FALSE;
    }
    void* Create(BOOL) override {
        return nullptr;
    }
    const MCHAR* ClassName() override {
        return _M("WhiteoutDexExtractor");
    }
#if MAX_PRODUCT_YEAR_NUMBER >= 2022
    const MCHAR* NonLocalizedClassName() override {
        return _M("WhiteoutDexExtractor");
    }
#endif
    SClass_ID SuperClassID() override {
        return GUP_CLASS_ID;
    }
    Class_ID ClassID() override {
        return Class_ID(0x4e444558, 0x45585452);
    }
    const MCHAR* Category() override {
        return _M("WhiteoutDex");
    }
    const MCHAR* InternalName() override {
        return _M("WhiteoutDexExtractor");
    }
    HINSTANCE HInstance() override {
        return g_hInstance;
    }
};

static WhiteoutDexExtractorClassDesc g_classDesc;

extern "C" {
__declspec(dllexport) const MCHAR* LibDescription() {
    return _M("WhiteoutDex All-in-One Preview");
}
__declspec(dllexport) i32 LibNumberClasses() {
    return 1;
}
__declspec(dllexport) ClassDesc* LibClassDesc(i32 i) {
    return (i == 0) ? &g_classDesc : nullptr;
}
__declspec(dllexport) ULONG LibVersion() {
    return VERSION_3DSMAX;
}
}

// ============================================================================
// ndxStart() — collect scene via adapter, load into renderer
// Returns extraction time in ms, or -1 on error
// ============================================================================

def_visible_primitive(ndxStart, "ndxStart");
Value* ndxStart_cf(Value** /*arg_list*/, i32 count) {
    check_arg_count(ndxStart, 0, count);
    auto start = std::chrono::high_resolution_clock::now();

    if (g_running)
        NdxCleanup();

    // Host owns SceneManager + RenderService + RenderWindow.
    g_scene = new whiteout::flakes::renderer::SceneManager();
    g_renderer = new whiteout::flakes::renderer::RenderService(*g_scene);
    g_renderWindow = new whiteout::flakes::RenderWindow(*g_renderer);
    if (!g_renderWindow->Open(800, 600)) {
        mprintf(_M("WhiteoutDex: ERROR - Could not open renderer window\n"));
        delete g_renderWindow;
        g_renderWindow = nullptr;
        delete g_renderer;
        g_renderer = nullptr;
        delete g_scene;
        g_scene = nullptr;
        return Integer::intern(-1);
    }

    // Make renderer window float above Max. GA_ROOT walks up to a top-level
    // window so GWLP_HWNDPARENT doesn't accidentally re-parent the window
    // under a child viewport panel (which would clip the title bar to "W").
    Interface* ip = GetCOREInterface();
    HWND ndxWnd = FindWindowW(L"WhiteoutDexRendererClass", nullptr);
    if (ndxWnd && ip) {
        HWND maxHwnd = ip->GetMAXHWnd();
        HWND maxRoot = GetAncestor(maxHwnd, GA_ROOT);
        SetWindowLongPtrW(ndxWnd, GWLP_HWNDPARENT, (LONG_PTR)(maxRoot ? maxRoot : maxHwnd));
    }

    // PE1 base path = directory containing the loaded .max file. Set BEFORE
    // adapter collection so PE1 child-model paths in the scene resolve.
    if (const MCHAR* maxFile = ip->GetCurFilePath().data(); maxFile && maxFile[0]) {
        std::wstring wp(maxFile);
        auto pos = wp.find_last_of(L'\\');
        if (pos != std::wstring::npos)
            wp = wp.substr(0, pos + 1);
        g_scene->SetPE1BasePath(std::filesystem::path(wp));
    }

    // Audio: same Windows-native ISoundEmitter the standalone exe uses.
    // Borrows the scene's content provider for CASC/MPQ lookup so SND
    // EventObjects play through the host OS during preview. Without
    // this, the renderer's default null emitter drops every fire.
    g_renderer->SwapSoundEmitter(
        std::make_unique<whiteout::flakes::WindowsSoundEmitter>(g_scene->ActiveContentProvider()));

    // ---- Build the live adapter ----
    g_adapter = std::make_shared<whiteout::flakes::MaxSceneAdapter>();
    // Cross-model dedup: skip BLP/CASC decode for textures that other models
    // already uploaded. SpawnActorFromLiveSource sets this too, but we set
    // it now so CollectScene's incidental texture reads also benefit.
    g_adapter->SetTextureCacheQuery(
        [](std::string_view k) { return g_renderer->Textures().IsCachedShared(k); });

    // Walk the Max scene at t=0 for stable bind-pose extraction.
    TimeValue savedTime = ip->GetTime();
    ip->SetTime(0, FALSE);
    mprintf(_M("WhiteoutDex: Collecting scene...\n"));
    g_adapter->CollectScene();

    // ---- One-call spawn ----
    // Pulls all static data via IModelDataSource::Build, builds an inline
    // ModelTemplate, registers attachment + PE1 configs, and binds the
    // actor's AnimationDriver to the adapter (which is also an
    // IAnimationSource). Replaces ~15 lines of GetX + LoadModel + SetX boilerplate.
    mprintf(_M("WhiteoutDex: Loading model...\n"));
    g_actor = g_renderer->Loader().SpawnUnitFromSource(g_adapter);
    if (!g_actor) {
        mprintf(_M("WhiteoutDex: ERROR - SpawnUnitFromSource failed\n"));
        NdxCleanup();
        return Integer::intern(-1);
    }
    // Max scrubs Max's timeline; the renderer's per-frame ticker must skip
    // its own evaluation pass and let EvalFromMax push the cursor instead.
    g_actor->role = whiteout::flakes::renderer::model::ActorRole::External;
    g_renderer->Settings().SetRenderMode(g_actor->PreferredRenderMode());

    // Camera presets are no longer plumbed into the renderer — Max owns its
    // own viewport so MaxSceneAdapter::GetCameraPresets() is currently unused
    // by the plugin. Re-add a Max-side preset UI if needed.

    // Restore Max's time and run the initial eval so the model is visible
    // before TimeChanged starts firing.
    ip->SetTime(savedTime, FALSE);
    EvalFromMax(MaxTimeToMs(ip->GetTime()));

    // Hook Max's timeline + start the polling timer for hot-reload.
    g_timeCallback = new NdxTimeCallback();
    ip->RegisterTimeChangeCallback(g_timeCallback);
    g_running = true;
    g_materialTimerId = SetTimer(nullptr, 0, 500, MaterialPollTimer);

    auto end = std::chrono::high_resolution_clock::now();
    i32 ms = (i32)std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // Diagnostic readout from the actor's render-side counts.
    mprintf(_M("\nWhiteoutDex: === STARTED in %d ms ===\n"), ms);
    mprintf(_M("  %d geosets, %d materials\n"), (i32)g_actor->render.gpuGeosets.size(),
            (i32)g_actor->render.gpuMaterials.size());
    mprintf(_M("  %d collisions\n"), (i32)g_actor->render.collisionShapes.size());

    return Integer::intern(ms);
}

// ============================================================================
// ndxStop() — stop sync, close renderer
// ============================================================================

def_visible_primitive(ndxStop, "ndxStop");
Value* ndxStop_cf(Value** /*arg_list*/, i32 count) {
    check_arg_count(ndxStop, 0, count);
    NdxCleanup();
    return &ok;
}

// ============================================================================
// ndxRefreshMaterials() — force re-read of material properties
// Returns true if anything changed
// ============================================================================

def_visible_primitive(ndxRefreshMaterials, "ndxRefreshMaterials");
Value* ndxRefreshMaterials_cf(Value** /*arg_list*/, i32 count) {
    check_arg_count(ndxRefreshMaterials, 0, count);
    if (!g_running || !g_renderer || !g_adapter || !g_actor)
        return &false_value;

    auto result = g_adapter->RefreshMaterials();
    if (result.changed) {
        g_renderer->Loader().UpdateMaterials(g_actor->handle, result.materials, result.textures);
        mprintf(_M("WhiteoutDex: Materials refreshed (%d materials, %d textures)\n"),
                (i32)result.materials.size(), (i32)result.textures.size());
        return &true_value;
    }
    return &false_value;
}
