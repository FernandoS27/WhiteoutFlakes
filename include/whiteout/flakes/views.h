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

namespace detail {
class RendererImpl;
}

/// @brief Swap-chain + frame-submit surface.
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
    /// @brief Mark a target as the main one (frame stats track this one).
    void SetPrimaryTarget(RenderTargetId);
    /// @brief Resize the primary target's swap-chain.
    void ResizePrimaryTarget(i32 width, i32 height);
    /// @brief Render the scene to @p t.
    void RenderFrame(RenderTargetId t);
    /// @brief Present @p t to the window.
    void Present(RenderTargetId t);
    /// @brief Tear down the gfx device. Idempotent.
    void Shutdown();
    /// @brief Last-frame stats (geoset / texture / node / particle /
    ///        segment counts).
    FrameStats GetFrameStats() const;
    /// @brief Live GPU bytes currently allocated. WebGPU backend tracks
    ///        every CreateTexture / CreateBuffer and subtracts on
    ///        deferred-delete drain — diagnostic for memory growth.
    ///        Returns 0 on backends without tracking.
    u64 LiveGpuBytes() const;

private:
    explicit PipelineView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

/// @brief Scene-clock + content-provider surface.
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
class CameraView {
public:
    /// @brief Camera control mode.
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
    Mode GetMode() const;
    Vector3f GetTarget() const;
    f32 GetDistance() const;

    /// @brief Scale factor: distance-units per wheel-detent in `Zoom`.
    static const f32 kFactorRelDist;
    static const f32 kDefaultFovDiagonal;
    static const f32 kDefaultNearZ;
    static const f32 kDefaultFarZ;

private:
    explicit CameraView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

/// @brief Display flags, post-processing tunables, and persistent host
///        knobs that survive across runs.
class SettingsView {
public:
    DisplayFlags GetDisplayFlags() const;
    void SetDisplayFlags(const DisplayFlags&);
    /// @brief Returns `true` once (then resets) whenever the render mode
    ///        was changed since the last call. Hosts gate model
    ///        re-spawn / material refresh on this.
    bool ConsumeRenderModeDirty();

    LightingMode GetLightingMode() const;
    void SetLightingMode(LightingMode);

    /// @brief Background colour as packed 0x00BBGGRR.
    u32 BackgroundColorRaw() const;
    void SetBackgroundColor(u8 r, u8 g, u8 b);

    f32 GetTonemapExposure() const;
    void SetTonemapExposure(f32);

    IblMode GetIblMode() const;
    void SetIblMode(IblMode);

    /// @brief HD-shader debug mode (0 = off, 1..7 = visualisations).
    i32 HdDebugMode() const;
    void SetHdDebugMode(i32);

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

/// @brief Push-based asset registry view.
///
/// Renderer subsystems call into `AssetManager` directly to Acquire
/// slots; the host uses this view to:
///   * pump the needs queue (paths the renderer wants the host to fetch),
///   * push fetched bytes back in via `ApplyAsset`.
///
/// Stays valid for the renderer's lifetime; cheap to copy.
class AssetsView {
public:
    /// @brief Asset categories the manager tracks. Keep in sync with
    ///        `renderer::assets::AssetKind`.
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
    struct Stats {
        std::size_t liveSlots        = 0;
        std::size_t loadedSlots      = 0;
        std::size_t pendingNeeds     = 0;
        std::size_t totalAcquires    = 0;
        std::size_t totalReleases    = 0;
        std::size_t totalApplies     = 0;
        std::size_t totalApplyMisses = 0;
    };
    Stats GetStats() const;

    /// @brief Acquire slots for every SPL/UBR texture and SPN child-model
    ///        referenced by the loaded event-data SLKs. The slots are
    ///        held by the event-data cache for the rest of the session,
    ///        so the host pump fetches them eagerly and they survive
    ///        animation changes. Call after `LoadEventDataFiles` finishes
    ///        populating the splat tables.
    void PrefetchEventAssets();

    /// @brief True iff a Texture slot for @p path is already loaded.
    ///        Used by hosts that want to dedup texture decode work
    ///        across models (e.g. the Max plugin's live adapter).
    bool IsTextureCached(std::string_view path) const;

private:
    explicit AssetsView(detail::RendererImpl* impl) : impl_(impl) {}
    detail::RendererImpl* impl_;
    friend class Renderer;
};

/// @brief Day-night-cycle service.
class DncView {
public:
    /// @brief `true` once the host has set a unit-MDL path and the DNC
    ///        service has loaded successfully.
    bool IsValid() const;
    /// @brief Time of day in hours (0..@ref GetHoursPerDay).
    f32 GetTimeOfDay() const;
    void SetTimeOfDay(f32);
    /// @brief TOD playback rate (`0` = paused; `1` = real-time-equivalent).
    f32 GetTodScale() const;
    void SetTodScale(f32);
    /// @brief Length of a full DNC cycle in hours (typically `24`).
    f32 GetHoursPerDay() const;
    /// @brief Currently-loaded DNC unit-MDL path.
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

} // namespace whiteout::flakes
