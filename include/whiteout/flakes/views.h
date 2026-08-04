#pragma once

/// @file views.h
/// @brief Sub-service view classes returned by `Renderer`.
///
/// Each view is a lightweight handle pointing at the renderer's internal
/// state. They expose a curated set of operations on a single subsystem
/// (pipeline, camera, settings, …) and forward into the corresponding
/// internal classes (`RenderPipeline`, `Camera`, `RenderSettings`, …).
/// Views don't own the impl; they're invalid after the `Renderer` is
/// destroyed. Constructed exclusively by `Renderer` accessors.

#include "content_provider.h"
#include "display.h"
#include "enums.h"
#include "model_data.h"
#include "model_source.h"
#include "types.h"
#include "util/replaceable_paths.h" // Tileset

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace whiteout::flakes {

/// @brief Opaque handle for a renderer-owned actor. `0` = invalid.
using ActorHandle = u32;

/// @brief Handle for a camera in the scene's camera set. `0` is the default
///        camera that always exists; additional cameras (for extra viewports
///        onto the scene) are created with `Renderer::CreateCamera`.
using CameraHandle = u32;

namespace detail {
class RendererImpl;
class AssetPreloadState;
}

/// @brief Swap-chain + frame-submit surface.
/// @bind no_default_ctor, methods
class PipelineView {
public:
    /// @brief Create the gfx device. Must succeed before any other
    ///        pipeline call. Picks the platform-default backend if
    ///        @p api is `Vulkan` on non-Windows.
    void InitDevice(GfxApi);
    bool IsDeviceReady() const;
    /// @brief Create a swap-chain target from a host window handle.
    /// @param hwnd Platform-specific: HWND on Windows, pre-created
    ///             VkSurfaceKHR (cast through `uintptr_t`) on Linux/macOS.
    RenderTargetId CreateSwapChainTarget(void* hwnd, i32 width, i32 height);
    /// @brief Create a headless (off-screen) target with no swap-chain.
    ///        RenderFrame / RenderViewport draw into it; Present no-ops on it.
    ///        Read the result back via the frame-capture ring. Use for thumbnail
    ///        / render-to-texture viewports that aren't shown in a window.
    RenderTargetId CreateOffscreenTarget(i32 width, i32 height);
    /// @brief Destroy a target (swap-chain or off-screen) and free its GPU
    ///        memory. Safe to call on any target id; clears primary if it was.
    void DestroyTarget(RenderTargetId);
    /// @brief Mark a target as the main one (frame stats track this one).
    void SetPrimaryTarget(RenderTargetId);
    /// @brief Resize the primary target's swap-chain.
    void ResizePrimaryTarget(i32 width, i32 height);
    /// @brief Render the scene's default camera to @p t.
    void RenderFrame(RenderTargetId t);
    /// @brief Render the scene seen through @p camera into target @p t. The
    ///        unit of multi-viewport rendering: call once per (camera, target)
    ///        pair in a frame, then Present the windowed targets. Cameras come
    ///        from the scene's camera set (`Renderer::CreateCamera`).
    void RenderViewport(RenderTargetId t, CameraHandle camera);
    /// @brief Present @p t to the window. No-op for headless targets.
    void Present(RenderTargetId t);
    /// @brief Tear down the gfx device. Idempotent.
    void Shutdown();
    /// @brief Last-frame stats (geoset / texture / node / particle /
    ///        segment counts).
    /// @bind rename=FrameStats
    FrameStats GetFrameStats() const;
    /// @brief Live GPU bytes currently allocated. WebGPU backend tracks
    ///        every CreateTexture / CreateBuffer and subtracts on
    ///        deferred-delete drain — diagnostic for memory growth.
    ///        Returns 0 on backends without tracking.
    u64 LiveGpuBytes() const;

    /// @brief Read a target's composited colour back as tightly-packed RGBA8,
    ///        row-major from the top-left.
    ///
    /// How a GUI shows the renderer somewhere it cannot hand over a window:
    /// a preview thumbnail, a docked panel the toolkit composites itself, an
    /// off-screen export. Render a viewport into a target, read it here, give
    /// the pixels to the toolkit as a texture.
    ///
    /// Works on every backend this library exposes — D3D11, D3D12, Vulkan and
    /// WebGPU each implement it. Row padding, and the BGRA ordering a swap
    /// chain usually has, are dealt with here so the result is always tight
    /// RGBA8.
    ///
    /// Synchronous: it submits and waits for the copy, so it costs a stall per
    /// call. That is the right trade for a panel that updates when something
    /// changes and the wrong one for a viewport running at full rate — give
    /// that a window of its own via @ref CreateSwapChainTarget.
    ///
    /// Returns an empty vector if @p t is unknown, if the target was resized
    /// out from under @p width / @p height, or if the source is multisampled.
    /// @p width and @p height report the size read.
    /// @bind skip — `std::vector<u8>` alongside two out-params does not fit
    ///              the generator's one-return convention. Hand-written in
    ///              bindings/c/whiteout_flakes_shims.cpp.
    std::vector<u8> ReadbackTarget(RenderTargetId t, i32& width, i32& height);


private:
    explicit PipelineView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

/// @brief Scene-clock + content-provider surface.
/// @bind no_default_ctor, methods
class SceneView {
public:
    /// @brief Master animation clock the renderer ticks (ms).
    i32 AnimationTimeMs() const;
    void SetAnimationTimeMs(i32);
    /// @brief Advance per-frame scene state (DNC, deferred deletes, …).
    void Update(f32 dt);
    /// @brief Set the directory PE1 emitter child-MDX paths are resolved
    ///        against. Hosts typically pass the parent of the loaded model.
    void SetPE1BasePath(const std::filesystem::path&);

    /// @brief Set the root the renderer searches for engine-shipped assets
    ///        — the BLS `shaders/` pack and `pso_trace.bin`.
    ///
    /// Defaults to the executable's directory, which is only correct when
    /// the host *is* an executable shipped beside those files. Hosts loaded
    /// as a shared library — language bindings, the Max plugin — must point
    /// this at a real directory **before** `PipelineView::InitDevice`, or
    /// shader acquisition fails and device init aborts.
    ///
    /// No-op for scenes running on a host-supplied content provider: those
    /// resolve engine assets however the host's provider chooses to.
    void SetEngineAssetRoot(const std::filesystem::path& root);
    /// @brief Point this scene's content provider at an installed Warcraft
    ///        III, so archive paths resolve out of its CASC storage.
    ///
    /// The counterpart to @ref StorageBrowser: browse an install, then hand the
    /// same root here and `LoaderView::SpawnUnit` reads the paths the browser
    /// returns. Loose files on disk keep working — CASC is consulted for what
    /// is not found beside @ref SetPE1BasePath.
    ///
    /// No-op for scenes running on a host-supplied content provider, which
    /// resolve however the host chooses.
    void SetCascInstallPath(const std::string& root);

    /// @brief Point this scene's content provider at one or more MPQ
    ///        archives, replacing any it already had.
    ///
    /// The MPQ counterpart to @ref SetCascInstallPath, and the other half of
    /// browsing with @ref StorageBrowser: hand it the archive you opened and
    /// the paths the browser returns become loadable. Paths are resolved in
    /// list order, so a patch archive belongs before the base one.
    ///
    /// Requires a build with MPQ support (the crate's `mpq` feature); without
    /// it this is a no-op and archive paths will not resolve.
    /// @bind skip — a `std::vector<std::string>` parameter is a shape neither
    ///              emitter models. Hand-written as a (pointer, count) pair in
    ///              bindings/c/whiteout_flakes_shims.cpp.
    void SetMpqList(const std::vector<std::string>& archives);

    /// @brief Prefer the HD (Reforged) asset chain over SD when resolving
    ///        archive paths.
    ///
    /// A CASC path exists in several mod layers; the provider tries them in
    /// order, and which order is right depends on the model. Set this from
    /// the actor's @ref ActorView::PreferredRenderMode, or textures resolve
    /// to the wrong layer and a Reforged model renders with classic art.
    void SetHdMode(bool enabled);

    /// @brief The renderer's installed content provider (`nullptr` until
    ///        the host attaches one). Used by adapters that need to read
    ///        extra files referenced by the model.
    IContentProvider* ActiveContentProvider();

    /// @brief Install a host-owned content provider, replacing the
    ///        default disk-backed one. The web build calls this with a
    ///        FetchContentProvider before InitDevice runs so BLS / DNC
    ///        / IBL loads find their bytes via JS-pushed buffers.
    ///        Ownership is shared — the renderer keeps the provider
    ///        alive while it's active.
    void SetContentProvider(std::shared_ptr<IContentProvider> provider);

private:
    explicit SceneView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

/// @brief Free-orbit + scripted camera surface.
/// @bind no_default_ctor, methods
class CameraView {
public:
    /// @brief Camera control mode.
    /// @bind
    enum class Mode {
        Orbital, ///< Pitch/yaw/distance + target follow.
        Direct,  ///< Position/target/roll set by a `CameraPreset` animator.
    };

    /// @brief Re-center on the world origin in orbital mode.
    void Reset();
    void SetPitch(f32);
    void SetYaw(f32);
    void SetDistance(f32);
    void SetTarget(f32 x, f32 y, f32 z);
    /// @brief Apply a mouse-drag rotation (orbital mode).
    void Rotate(i32 dx, i32 dy);
    /// @brief Apply a mouse-drag pan (orbital mode).
    void Pan(i32 dx, i32 dy);
    /// @brief Apply a wheel-step zoom.
    void Zoom(i32 wheelDelta);
    /// @brief Apply a continuous-axis zoom (middle-mouse drag).
    void ZoomSmooth(f32 factor);
    /// @brief Switch from `Direct` back to `Orbital`.
    void SetOrbitalMode();
    /// @brief Diagonal field of view (radians).
    void SetFovDiagonal(f32);
    /// @brief Near + far clip planes.
    void SetClip(f32 nz, f32 fz);
    /// @brief Switch to `Direct` mode and set the pose.
    void SetDirectPose(Vector3f pos, Vector3f target, f32 roll);
    /// @bind rename=Mode
    Mode GetMode() const;
    Vector3f GetTarget() const;
    /// @bind rename=Distance
    f32 GetDistance() const;

    /// @brief Scale factor: distance-units per wheel-detent in `Zoom`.
    static const f32 kFactorRelDist;
    static const f32 kDefaultFovDiagonal;
    static const f32 kDefaultNearZ;
    static const f32 kDefaultFarZ;

private:
    explicit CameraView(detail::RendererImpl* impl, CameraHandle cam = 0)
        : impl_(impl), cam_(cam) {}
    detail::RendererImpl* impl_;
    CameraHandle cam_; ///< which camera in the scene's set this view drives
    friend class Renderer;
};

/// @brief Display flags, post-processing tunables, and persistent host
///        knobs that survive across runs.
/// @bind no_default_ctor, methods
class SettingsView {
public:
    /// @bind rename=DisplayFlags
    DisplayFlags GetDisplayFlags() const;
    void SetDisplayFlags(const DisplayFlags&);
    /// @brief Returns `true` once (then resets) whenever the render mode
    ///        was changed since the last call. Hosts gate model
    ///        re-spawn / material refresh on this.
    bool ConsumeRenderModeDirty();

    /// @bind rename=LightingMode
    LightingMode GetLightingMode() const;
    void SetLightingMode(LightingMode);

    /// @brief Background colour as packed 0x00BBGGRR.
    u32 BackgroundColorRaw() const;
    void SetBackgroundColor(u8 r, u8 g, u8 b);

    /// @bind rename=TonemapExposure
    f32 GetTonemapExposure() const;
    void SetTonemapExposure(f32);

    /// @bind rename=IblMode
    IblMode GetIblMode() const;
    void SetIblMode(IblMode);

    /// @brief HD-shader debug mode (0 = off, 1..7 = visualisations).
    i32 HdDebugMode() const;
    void SetHdDebugMode(i32);

    /// @brief GTAO quality preset (0 = Low, 1 = Medium, 2 = High).
    u32 AoQuality() const;
    void SetAoQuality(u32);

    /// @brief LOD override (`-1` = auto-pick by screen size, `0..3` = forced).
    i32 LodOverride() const;
    void SetLodOverride(i32);

    void SetRenderMode(RenderMode);

private:
    explicit SettingsView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

/// @brief Spawn / refresh / clear actors.
/// @bind no_default_ctor, methods
class LoaderView {
public:
    /// @brief Spawn an actor from a path resolvable by the content provider.
    /// @return New actor handle, or `0` on failure.
    ActorHandle SpawnUnit(const std::string& path);
    /// @brief Spawn an actor from a custom @ref IModelSource.
    /// @param source            Implementation that produces the static
    ///                          snapshot and per-frame animation.
    /// @param initialTransform  World transform to start at.
    ActorHandle SpawnUnitFromSource(std::shared_ptr<IModelSource> source,
                                    const Matrix44f& initialTransform = Matrix44f::identity());
    /// @brief Spawn a standalone corn effect (`.pkb` / `.pkfx`) as its own
    ///        actor, with no model around it.
    ///
    /// A corn effect is normally reached through a model: an MDX's `CORN`
    /// chunk names the `.pkb` and the loader stages an emitter for it. This
    /// plays one on its own — the same thing the viewer does when you open a
    /// `.pkb` directly.
    ///
    /// Equivalent to `SpawnUnitFromSource` with the library's
    /// `CornEffectSource`, and exists because bindings cannot implement
    /// @ref IModelSource themselves. The actor carries a single always-on
    /// emitter and one looping placeholder sequence to keep it ticking; the
    /// effect's own runtime decides whether it loops or plays once.
    ///
    /// @param path Resolvable by the content provider, exactly like
    ///        @ref SpawnUnit.
    ActorHandle SpawnEffect(const std::string& path);

    /// @brief Replace material + texture data for an already-spawned actor
    ///        without re-loading the rest of it (hot-reload from the Max
    ///        plugin's vertex-paint / material-property polling).
    void UpdateMaterials(ActorHandle handle, const std::vector<MaterialData>& materials,
                         const std::vector<TextureData>& textures);
    /// @brief Defer-destroy every actor currently in the scene.
    void RequestClearAll();

    /// @brief Destroy a single actor by handle. Used by JS wrappers
    ///        that expose mdx-m3-viewer-style `instance.detach()` /
    ///        `instance.hide()` semantics.
    void Destroy(ActorHandle handle);

private:
    explicit LoaderView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

/// @brief Keeps a batch of preloaded assets resident.
///
/// Returned by `AssetsView::Preload` / `PreloadDirectory`. Every asset in
/// the batch holds a reference for as long as this object is alive; when it
/// dies (or `Release()` is called) the references drop and the assets are
/// freed once nothing else uses them. Move-only — the batch has one owner.
///
/// Preloading only *asks* for the assets: the paths go onto the renderer's
/// needs queue and the host's usual asset pump fetches them, so the bytes
/// land a frame or more later. `Ready()` / `LoadedCount()` say how far
/// along that is.
///
/// @code
/// // Pin every texture the FX folder holds for as long as the effect
/// // editor is open.
/// auto pinned = r.Assets().PreloadDirectory(AssetsView::Kind::Texture,
///                                           "Textures/FX", true);
/// @endcode
///
/// Safe to outlive the `Renderer`, unlike the views: a batch whose renderer
/// (or GPU device) is already gone simply has nothing left to release.
/// @bind skip — move-only owned return value, not modelled by the emitters.
class AssetPreload {
public:
    AssetPreload();
    ~AssetPreload();

    AssetPreload(AssetPreload&&) noexcept;
    AssetPreload& operator=(AssetPreload&&) noexcept;
    AssetPreload(const AssetPreload&) = delete;
    AssetPreload& operator=(const AssetPreload&) = delete;

    /// @brief How many assets this batch holds references to.
    std::size_t Count() const;
    /// @brief How many of them have finished loading.
    std::size_t LoadedCount() const;
    /// @brief `true` once every asset in the batch has loaded. An empty
    ///        batch is trivially ready.
    bool Ready() const;
    /// @brief The paths held, normalised (lowercase, `/` separators).
    std::vector<std::string> Paths() const;
    /// @brief Drop the references now rather than at destruction.
    void Release();

private:
    explicit AssetPreload(std::unique_ptr<detail::AssetPreloadState> state);
    std::unique_ptr<detail::AssetPreloadState> state_;
    friend class AssetsView;
};

/// @brief Push-based asset registry view.
///
/// Renderer subsystems call into `AssetManager` directly to Acquire
/// slots; the host uses this view to:
///   * pump the needs queue (paths the renderer wants the host to fetch),
///   * push fetched bytes back in via `ApplyAsset`.
///
/// Stays valid for the renderer's lifetime; cheap to copy.
/// @bind no_default_ctor, methods
class AssetsView {
public:
    /// @brief Asset categories the manager tracks. Keep in sync with
    ///        `renderer::assets::AssetKind`.
    /// @bind
    enum class Kind : u8 {
        Texture    = 0,
        Particle   = 1,
        ChildModel = 2,
    };

    /// @brief Fired once per unique path the renderer Acquired since
    ///        the last drain. Use this to schedule fetches host-side.
    using NeededFn = std::function<void(Kind, std::string_view path)>;

    /// @brief Drain the buffered needs queue. Safe to call any time;
    ///        typically once per tick / animation frame.
    void DrainNeeds(const NeededFn& cb);

    /// @brief Push the bytes fetched for @p path. The manager decodes /
    ///        parses according to @p kind and queues the result for
    ///        the next `Commit` (which the FrameTicker pumps on the
    ///        render thread).
    /// @return `true` if a slot existed for @p path AND decode succeeded.
    bool ApplyAsset(Kind kind, std::string_view path,
                    std::span<const u8> bytes, std::string_view foundExt = {});

    /// @brief Snapshot diagnostic counters.
    /// @bind value_object
    struct Stats {
        std::size_t liveSlots        = 0;
        std::size_t loadedSlots      = 0;
        std::size_t pendingNeeds     = 0;
        std::size_t totalAcquires    = 0;
        std::size_t totalReleases    = 0;
        std::size_t totalApplies     = 0;
        std::size_t totalApplyMisses = 0;
    };
    /// @bind rename=Stats
    Stats GetStats() const;

    /// @brief Acquire slots for every SPL/UBR texture and SPN child-model
    ///        referenced by the loaded event-data SLKs. The slots are
    ///        held by the event-data cache for the rest of the session,
    ///        so the host pump fetches them eagerly and they survive
    ///        animation changes. Call after `LoadEventDataFiles` finishes
    ///        populating the splat tables.
    void PrefetchEventAssets();

    /// @brief Per-actor variant — only Acquire slots for the
    ///        SPL/UBR/SPN/FPT events the actor's template actually
    ///        references. Use this instead of `PrefetchEventAssets()`
    ///        on backends where every Acquire costs a network fetch
    ///        (web viewer hitting Hive's CASC mirror); the global
    ///        variant would pull in every entry in the SLK tables
    ///        regardless of whether the loaded model fires them.
    void PrefetchEventAssetsForActor(ActorHandle actor);

    /// @brief True iff a Texture slot for @p path is already loaded.
    ///        Used by hosts that want to dedup texture decode work
    ///        across models (e.g. the Max plugin's live adapter).
    bool IsTextureCached(std::string_view path) const;

    /// @brief Pin a known set of assets in memory.
    ///
    /// Each path is Acquired once (duplicates collapse) and the whole set
    /// is handed back as an @ref AssetPreload that holds the references
    /// until it dies. Use it for assets you know are about to be needed —
    /// an ability's splat textures, a unit's team-colour variants — so
    /// they're resident before the first frame that draws them, and stay
    /// resident when the last model using them is unloaded.
    /// @bind skip — returns a move-only owned object.
    AssetPreload Preload(Kind kind, std::span<const std::string> paths);

    /// @brief Pin every asset of @p kind under @p directory.
    ///
    /// The active content provider enumerates the directory (CASC, MPQ and
    /// the loose-file base path) and the listing is filtered to the
    /// extensions @p kind can decode. Enumerating an archive costs a full
    /// walk of its manifest — do this once per directory, not per frame.
    /// Returns an empty batch when the provider can't enumerate (the web
    /// fetch provider) or the directory holds nothing of that kind.
    /// @param directory Provider-relative, e.g. `"Textures/FX"`; empty
    ///                  means everything the provider knows about.
    /// @param recursive Include nested subdirectories.
    /// @bind skip — returns a move-only owned object.
    AssetPreload PreloadDirectory(Kind kind, std::string_view directory, bool recursive = true);

private:
    explicit AssetsView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

/// @brief Day-night-cycle service.
/// @bind no_default_ctor, methods
class DncView {
public:
    /// @brief `true` once the host has set a unit-MDL path and the DNC
    ///        service has loaded successfully.
    bool IsValid() const;
    /// @brief Time of day in hours (0..@ref GetHoursPerDay).
    /// @bind rename=TimeOfDay
    f32 GetTimeOfDay() const;
    void SetTimeOfDay(f32);
    /// @brief TOD playback rate (`0` = paused; `1` = real-time-equivalent).
    /// @bind rename=TodScale
    f32 GetTodScale() const;
    void SetTodScale(f32);
    /// @brief Length of a full DNC cycle in hours (typically `24`).
    /// @bind rename=HoursPerDay
    f32 GetHoursPerDay() const;
    /// @brief Currently-loaded DNC unit-MDL path.
    /// @bind skip — the C emitter moves string returns into the wrapper,
    ///              which a `const&` return can't satisfy. Hand-written in
    ///              bindings/c/whiteout_flakes_shims.cpp under the same
    ///              symbol the generator would have produced.
    const std::string& UnitMdlPath() const;
    /// @brief Replace the DNC unit-MDL path (re-loads from the content provider).
    void SetUnitMdl(const std::string&);
    /// @brief Advance the DNC clock by @p dt seconds.
    void Advance(f32 dt);

private:
    explicit DncView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

/// @brief Cascade-shadow-map service.
/// @bind no_default_ctor, methods
class ShadowView {
public:
    bool IsValid() const;
    bool IsEnabled() const;
    void SetEnabled(bool on);
    ShadowParams Params() const;
    void SetParams(const ShadowParams&);

private:
    explicit ShadowView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

/// @brief Splat-decal service (engine-spawned SPL/UBR events).
/// @bind no_default_ctor, methods
class SplatView {
public:
    /// @brief Remove every live splat. Hosts call this on sequence
    ///        change to avoid carrying decay-only splats across cuts.
    void Clear();

private:
    explicit SplatView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

/// @brief Replaceable-texture / tileset switching.
/// @bind no_default_ctor, methods
class ReplaceablesView {
public:
    /// @brief Returns `true` once when the tileset has changed since
    ///        last call (hosts use this to invalidate cached swatches).
    bool ConsumeDirty();
    /// @brief Switch the active tileset; in-scene replaceables re-resolve
    ///        on the next frame.
    void SetTileset(Tileset);

private:
    explicit ReplaceablesView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

/// @brief Transport controls for the scene clock — one switch that governs
///        model animation and every effect system alike.
///
/// A frame advances in two halves: `SceneView::Update` moves the animation
/// clocks, `Renderer::Tick` moves the particle, PE1, ribbon and corn effects
/// simulations. Both read the state set here, so a host cannot pause one and
/// leave the other running.
///
/// @code
/// r.Playback().Pause();     // freeze mid-animation, particles hang in air
/// r.Playback().Play();      // carry on from that instant
/// r.Playback().Restart();   // rewind to the top and play
/// r.Playback().Stop();      // rewind to the top and hold
/// @endcode
/// @bind no_default_ctor, methods
class PlaybackView {
public:
    /// @brief Current transport state.
    /// @bind rename=State
    PlaybackState GetState() const;
    /// @brief Set the transport state directly. Entering @ref
    ///        PlaybackState::Stopped rewinds; the other transitions do not.
    void SetState(PlaybackState);

    /// @brief Resume (or continue) advancing time.
    void Play();
    /// @brief Hold time at the current instant. Resuming continues from here.
    void Pause();
    /// @brief Rewind to the start and hold. Animation cursors return to zero
    ///        and transient effect state — live particles, ribbon trails,
    ///        PE1 instances, corn effects runtimes — is discarded, so nothing is
    ///        left hanging in mid-air.
    void Stop();
    /// @brief Rewind to the start and play. Equivalent to `Stop()` then
    ///        `Play()`, and the usual way to replay a one-shot effect.
    void Restart();

    /// @brief Drop transient effect state so the effect systems replay from
    ///        the cursor's current position.
    ///
    /// For hosts that move the clock themselves — a timeline scrub — rather
    /// than through this view. Particles, ribbons and corn effects are forward
    /// simulations with no seek: jumping the animation cursor leaves them
    /// mid-flight from wherever they were, which reads as the effects
    /// ignoring the scrub. This restarts them. The clock is untouched.
    void ResyncEffects();

    /// @brief `true` when time is not advancing (paused *or* stopped).
    bool IsPaused() const;

    /// @brief Multiplier applied to every advancing frame. Survives a
    ///        pause/resume, so slow-motion is not lost on Play().
    /// @bind rename=TimeScale
    f32 GetTimeScale() const;
    void SetTimeScale(f32);

private:
    explicit PlaybackView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

} // namespace whiteout::flakes
