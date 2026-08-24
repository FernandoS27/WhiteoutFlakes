#pragma once

// ============================================================================
// RenderSettings — application-tunable knobs that influence rendering.
//
// Pure data: atomics for things UI/render threads both touch, plain values for
// things only the render path reads. No GPU resources, no scene state, no
// pipeline behavior. The pipeline reads from here every frame; settings never
// reach back into the pipeline.
//
// IBL mode + render mode raise a one-shot dirty flag; the pipeline polls it
// and reacts on the next frame.
// ============================================================================

#include <functional>
#include "render_target.h"             // DisplayFlags, RenderMode, LightingMode, IblMode
#include "whiteout/flakes/gfx_types.h" // gfx::GfxApi
#include "whiteout/flakes/types.h"

#include <atomic>
#include <cstring>
#include <string>

namespace whiteout::flakes::renderer {

class RenderSettings {
public:
    RenderSettings() = default;

    // ---- Display flags (what to draw) ----
    DisplayFlags GetDisplayFlags() const {
        DisplayFlags df;
        df.showGrid = showGrid_;
        df.showParticles = showParticles_;
        df.showRibbons = showRibbons_;
        df.showCollisions = showCollisions_;
        df.showLights = showLights_;
        df.showEvents = showEvents_;
        df.renderMode = renderMode_;
        return df;
    }
    void SetDisplayFlags(const DisplayFlags& f) {
        showGrid_ = f.showGrid;
        showParticles_ = f.showParticles;
        showRibbons_ = f.showRibbons;
        showCollisions_ = f.showCollisions;
        showLights_ = f.showLights;
        showEvents_ = f.showEvents;
        SetRenderMode(f.renderMode);
    }

    bool ShowGrid() const {
        return showGrid_;
    }
    bool ShowParticles() const {
        return showParticles_;
    }
    bool ShowRibbons() const {
        return showRibbons_;
    }
    bool ShowCollisions() const {
        return showCollisions_;
    }
    bool ShowLights() const {
        return showLights_;
    }
    bool ShowEvents() const {
        return showEvents_;
    }

    // ---- Physics body overlay ----
    //
    // Deliberately *not* in DisplayFlags: that struct is the host-facing display
    // contract, and these are debug views in the same class as HdDebugMode and
    // LodOverride, which take the same direct-setter route.
    bool ShowPhysicsDynamic() const {
        return showPhysicsDynamic_;
    }
    bool ShowPhysicsKinematic() const {
        return showPhysicsKinematic_;
    }
    bool ShowPhysicsStatic() const {
        return showPhysicsStatic_;
    }
    bool ShowAnyPhysicsBodies() const {
        return showPhysicsDynamic_ || showPhysicsKinematic_ || showPhysicsStatic_;
    }
    /// Cloth is its own category rather than a fourth body kind: a cloth
    /// particle is not a rigid body, its constraints are not colliders, and it
    /// draws a mesh of links where the other three draw one shape each. On the
    /// same rig the two overlays sit on top of each other, so they toggle apart.
    /// @brief Subdivide each frame's physics step instead of taking one step
    ///        per rendered frame.
    ///
    /// A simulation setting, not a debug overlay, and on by default: the
    /// shipped one-step-per-frame cadence lets a fast collider pass straight
    /// through cloth, and a game covers that by retuning the numbers in its
    /// data editor rather than by stepping finer. Off reproduces the shipped
    /// cadence. Honoured today by the SC2/Heroes cloth stage.
    bool PhysicsSubstepping() const {
        return physicsSubstepping_;
    }
    void SetPhysicsSubstepping(bool v) {
        physicsSubstepping_ = v;
    }

    bool ShowPhysicsCloth() const {
        return showPhysicsCloth_;
    }
    void SetShowPhysicsCloth(bool v) {
        showPhysicsCloth_ = v;
    }
    void SetShowPhysicsDynamic(bool v) {
        showPhysicsDynamic_ = v;
    }
    void SetShowPhysicsKinematic(bool v) {
        showPhysicsKinematic_ = v;
    }
    void SetShowPhysicsStatic(bool v) {
        showPhysicsStatic_ = v;
    }

    // ---- Pose stages (terrain IK, turret) ----
    // Off by default, and StarCraft II does the same thing: IK is gated on a
    // world flag rather than per model, because a solver with no world to
    // query has nothing to solve against. Off also means the pose is exactly
    // what the sampler produced, which is what keeps the byte-identical gates
    // meaningful.
    bool PoseSolversEnabled() const {
        return poseSolvers_;
    }
    void SetPoseSolversEnabled(bool v) {
        poseSolvers_ = v;
    }

    // Host-supplied ground height under a model-space point, for terrain IK.
    // Renderer policy stops at "call whatever the host registered": the viewer
    // installs a flat plane, a game host would sample its terrain. Unset means
    // no IK runs at all rather than IK against a guessed surface.
    using GroundQuery = std::function<bool(const Vector3f& pos, f32 up, f32 down, f32& outZ)>;
    const GroundQuery& GetGroundQuery() const {
        return groundQuery_;
    }
    void SetGroundQuery(GroundQuery q) {
        groundQuery_ = std::move(q);
    }

    // ---- Render mode (HD vs SD) ----
    // App sets the mode based on what it loaded; pipeline polls the dirty
    // flag to know when to rebuild PSOs / IBL state.
    RenderMode GetRenderMode() const {
        return renderMode_;
    }
    void SetRenderMode(RenderMode m) {
        if (renderMode_ != m) {
            renderMode_ = m;
            MarkRenderModeDirty();
        }
    }
    // "The frame changed, re-stage what depends on it" — which since P5 is a
    // change of *profile*, not only of mode. A scene's ProductId selects the
    // profile directly, so setting it fires this too; the flag keeps its
    // render-mode name because it is bound
    // (whiteout_flakes_FlakesSettingsView_ConsumeRenderModeDirty) and renaming
    // it would break the C ABI for a spelling.
    void MarkRenderModeDirty() {
        renderModeDirty_ = true;
    }
    bool ConsumeRenderModeDirty() {
        return renderModeDirty_.exchange(false);
    }

    // Route the SD shading path through the HDR scene target + tonemap
    // instead of straight onto the LDR swap chain. Keeps authentic SD
    // shading (single RTV, no G-buffer/PBR) but lets additive / team-color
    // geosets accumulate in float and roll off through the tonemap rather
    // than clipping to opaque white. Off = classic direct-to-swap-chain SD
    // (pixel-identical to before). HD mode always renders to HDR and ignores
    // this flag.
    bool SceneHdrInSd() const {
        return sceneHdrInSd_.load();
    }
    void SetSceneHdrInSd(bool on) {
        sceneHdrInSd_.store(on);
    }

    // Parse `.m2` models without reading their `.anim` siblings, and read one
    // the first time a sequence is played — what the WoW client does. A
    // character model can ship a hundred `.anim` files and several megabytes of
    // keys, and a viewer showing one animation needs one of them.
    //
    // Read when a model is loaded, so flipping it affects the next load, not
    // the models already in the scene. Off by default: the eager parse is what
    // every byte-identical gate was recorded against, and a lazy load moves
    // file reads onto the frame that first plays a sequence.
    bool M2LazyAnimations() const {
        return m2LazyAnimations_.load();
    }
    void SetM2LazyAnimations(bool on) {
        m2LazyAnimations_.store(on);
    }

    // Sort transparent `.m2` geometry back-to-front by camera distance.
    //
    // The client does not: `CM2Scene::BeginDraw` passes 0.0 as the sort
    // distance for every geo batch and a real one only for particles and
    // ribbons, so `SortTransparent`'s distance key ties across all geometry and
    // the order falls through to priorityPlane, then materialLayer, then blend
    // mode. Off (the default) reproduces that.
    //
    // On restores our own distance sort, which is strictly better looking in a
    // scene holding several transparent models — the client falls through to a
    // raw `CM2Model*` there — at the cost of no longer matching it.
    bool M2DistanceSortGeometry() const {
        return m2DistanceSortGeometry_.load();
    }
    void SetM2DistanceSortGeometry(bool on) {
        m2DistanceSortGeometry_.store(on);
    }

    // Let a `.m2`'s own light blocks light it, the way CM2Scene::SelectLights
    // feeds them to every model in range. Off leaves only the environment key
    // light, which is what a model with no lights sees anyway — so this only
    // changes torches, braziers and the handful of creatures that glow.
    bool M2ModelLights() const {
        return m2ModelLights_.load();
    }
    void SetM2ModelLights(bool on) {
        m2ModelLights_.store(on);
    }

    // ---- Debug visualization ----

    // Route every odd-indexed geoset through UnlitShading instead of the
    // active WC3 model, so one frame contains draws from two shading models.
    //
    // Not a "render everything unlit" switch, and the difference matters. A
    // global one keeps exactly one model live per frame, which leaves
    // SurfacePass's open/close transition never taken twice and the
    // `key.model` sort term a no-op — the multi-model seam would ship
    // unexercised. Per-geoset is what actually proves it, and it is why this
    // is a debug toggle rather than a mode.
    //
    // Off by default: on, it changes what the frame draws, so every
    // byte-identical gate is recorded and checked with it off.
    bool DebugUnlitOddGeosets() const {
        return debugUnlitOddGeosets_.load();
    }
    void SetDebugUnlitOddGeosets(bool on) {
        debugUnlitOddGeosets_.store(on);
    }

    i32 HdDebugMode() const {
        return hdDebugMode_.load();
    }
    void SetHdDebugMode(i32 m) {
        hdDebugMode_.store(m);
    }

    i32 LodOverride() const {
        return lodOverride_.load();
    }
    void SetLodOverride(i32 l) {
        lodOverride_.store(l);
    }

    // ---- Ambient occlusion (GTAO) ----
    // Gates the HD-mode GTAO pass between the G-buffer close and the
    // tonemap. Off skips both the main and apply draws — the AO buffer
    // is allocated either way (cheap, 1 byte per pixel) so the flip is
    // free of resize work. SD mode ignores this flag.
    bool AoEnabled() const {
        return aoEnabled_.load();
    }
    void SetAoEnabled(bool on) {
        aoEnabled_.store(on);
    }

    // GTAO quality preset — index into gtao::Quality (Low=0, Medium=1,
    // High=2). The pipeline forwards this into the service each frame
    // so the user can A/B presets without a restart.
    u32 AoQuality() const {
        return aoQuality_.load();
    }
    void SetAoQuality(u32 q) {
        aoQuality_.store(q);
    }

    // Bent-normal IBL boost strength. 0 = pass disabled (no-op). Tiny
    // values (~0.05) recover some of the indirect-light energy lost when
    // GTAO darkens cavities; bigger values overdrive the model. Stored
    // as raw bits in an atomic<u32> because std::atomic<f32> isn't a
    // standard specialisation pre-C++20 across all our toolchains.
    f32 AoBentBoost() const {
        const u32 bits = aoBentBoost_.load();
        f32 v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    void SetAoBentBoost(f32 v) {
        u32 bits;
        std::memcpy(&bits, &v, sizeof(bits));
        aoBentBoost_.store(bits);
    }

    // ---- M3 deferred local lights (Sc2Heroes profile) ----
    // Gates the DeferredLights pass. The pass also self-disables when the
    // frame collected no lights, so leaving this on costs nothing for a
    // model without any.
    bool DeferredLightsEnabled() const {
        return deferredLightsEnabled_.load();
    }
    void SetDeferredLightsEnabled(bool on) {
        deferredLightsEnabled_.store(on);
    }

    // Scripted debug point light, fed straight into the DeferredLights pass
    // beside the collected M3 lights. Exists for the render gate: corpus
    // models rarely ship lights (shipped M3 data is mostly empty), so the
    // scenario needs a light it fully controls. Position and range are in
    // renderer units (post-WorldScale).
    bool DebugPointLightEnabled() const {
        return dbgPointLightOn_.load();
    }
    Vector3f DebugPointLightPos() const {
        return {loadF32(dbgPointLightPosX_), loadF32(dbgPointLightPosY_),
                loadF32(dbgPointLightPosZ_)};
    }
    Vector3f DebugPointLightColor() const {
        return {loadF32(dbgPointLightColR_), loadF32(dbgPointLightColG_),
                loadF32(dbgPointLightColB_)};
    }
    f32 DebugPointLightRange() const {
        return loadF32(dbgPointLightRange_);
    }
    void SetDebugPointLight(bool on, const Vector3f& pos, const Vector3f& color, f32 range) {
        storeF32(dbgPointLightPosX_, pos.x);
        storeF32(dbgPointLightPosY_, pos.y);
        storeF32(dbgPointLightPosZ_, pos.z);
        storeF32(dbgPointLightColR_, color.x);
        storeF32(dbgPointLightColG_, color.y);
        storeF32(dbgPointLightColB_, color.z);
        storeF32(dbgPointLightRange_, range);
        dbgPointLightOn_.store(on);
    }

    // ---- Bloom (HD-only) ----
    // Master enable. Off ⇒ PostProcessService::RunBloom is a no-op.
    bool BloomEnabled() const {
        return bloomEnabled_.load();
    }
    void SetBloomEnabled(bool on) {
        bloomEnabled_.store(on);
    }
    // Threshold + intensity + saturation float bits stored in atomic<u32>
    // — same trick as AoBentBoost (std::atomic<f32> isn't portable).
    f32 BloomThreshold() const {
        const u32 bits = bloomThreshold_.load();
        f32 v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    void SetBloomThreshold(f32 v) {
        u32 bits;
        std::memcpy(&bits, &v, sizeof(bits));
        bloomThreshold_.store(bits);
    }
    f32 BloomIntensity() const {
        const u32 bits = bloomIntensity_.load();
        f32 v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    void SetBloomIntensity(f32 v) {
        u32 bits;
        std::memcpy(&bits, &v, sizeof(bits));
        bloomIntensity_.store(bits);
    }
    f32 BloomSaturation() const {
        const u32 bits = bloomSaturation_.load();
        f32 v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    void SetBloomSaturation(f32 v) {
        u32 bits;
        std::memcpy(&bits, &v, sizeof(bits));
        bloomSaturation_.store(bits);
    }

    // ---- Refraction particles (WoW) ----
    // On by default, unlike every other post-process toggle here: a refraction
    // emitter draws nothing at all without the pass, so off is not a cheaper
    // look but a missing effect. 439 emitters across 366 shipped `.m2` carry it.
    bool RefractionEnabled() const {
        return refractionEnabled_.load();
    }
    void SetRefractionEnabled(bool on) {
        refractionEnabled_.store(on);
    }
    // Draw the distortion buffer instead of the distorted scene. The client has
    // the same switch (`showRefractionBuffer`); it is the only way to tell an
    // empty mask from a mask whose gradient happens to be flat.
    bool RefractionDebugMask() const {
        return refractionDebugMask_.load();
    }
    void SetRefractionDebugMask(bool on) {
        refractionDebugMask_.store(on);
    }

    // ---- Multi-texture particles (WoW) ----
    // On by default, and for the same reason refraction is: 23 253 emitters
    // across 5 239 shipped `.m2` combine three textures, and off is not a
    // cheaper look but their first layer alone at half the brightness. Kept as
    // a switch because that fallback is the only A/B the render gate has —
    // nothing else can tell "the combiner ran" from "the combiner ran and
    // happened to look like one layer".
    bool MultiTexParticlesEnabled() const {
        return multiTexParticlesEnabled_.load();
    }
    void SetMultiTexParticlesEnabled(bool on) {
        multiTexParticlesEnabled_.store(on);
    }

    // ---- Depth of field (HD-only) ----
    // Master enable. Off (and a focal distance of 0) ⇒ DofService::Run is a
    // no-op. Mirrors WC3's per-camera GetDepthOfFieldEnabled gate.
    bool DofEnabled() const {
        return dofEnabled_.load();
    }
    void SetDofEnabled(bool on) {
        dofEnabled_.store(on);
    }
    // Float bits in atomic<u32>, same trick as the bloom knobs above.
    //   FocusDistance — linear view-Z (scene units) kept in focus; 0 disables.
    //   FocusScale    — circle-of-confusion ramp (WC3 CameraSetDepthOfFieldScale).
    //   MaxBlurSize   — max gather radius in px (WC3 default 10).
    //   RadiusScale   — spiral ring spacing / sample density (WC3 default 1).
    f32 DofFocusDistance() const {
        return loadF32(dofFocusDistance_);
    }
    void SetDofFocusDistance(f32 v) {
        storeF32(dofFocusDistance_, v);
    }
    f32 DofFocusScale() const {
        return loadF32(dofFocusScale_);
    }
    void SetDofFocusScale(f32 v) {
        storeF32(dofFocusScale_, v);
    }
    f32 DofMaxBlurSize() const {
        return loadF32(dofMaxBlurSize_);
    }
    void SetDofMaxBlurSize(f32 v) {
        storeF32(dofMaxBlurSize_, v);
    }
    f32 DofRadiusScale() const {
        return loadF32(dofRadiusScale_);
    }
    void SetDofRadiusScale(f32 v) {
        storeF32(dofRadiusScale_, v);
    }
    bool DofFarFieldOnly() const {
        return dofFarFieldOnly_.load();
    }
    void SetDofFarFieldOnly(bool on) {
        dofFarFieldOnly_.store(on);
    }

    // ---- Lighting / clear color ----
    LightingMode GetLightingMode() const {
        return static_cast<LightingMode>(lightingMode_.load());
    }
    void SetLightingMode(LightingMode m) {
        lightingMode_.store(static_cast<u8>(m));
    }

    u32 BackgroundColorRaw() const {
        return backgroundColor_.load();
    }
    void SetBackgroundColor(u8 r, u8 g, u8 b) {
        backgroundColor_.store(u32(r) | (u32(g) << 8) | (u32(b) << 16));
    }

    // ---- IBL ----
    // Pipeline polls ConsumeIblModeDirty() each frame; if set, it reloads
    // probe textures based on GetIblMode().
    IblMode GetIblMode() const {
        return iblMode_;
    }
    void SetIblMode(IblMode m) {
        iblMode_ = m;
        MarkIblModeDirty();
    }
    // Ask for a reload of the current mode's probes without changing it. The
    // probes are Warcraft III environment maps, so the first apply waits for a
    // session to load that game's content — see RenderService::
    // EnsureWc3GameData, which is what calls this.
    void MarkIblModeDirty() {
        iblModeDirty_ = true;
    }
    bool ConsumeIblModeDirty() {
        return iblModeDirty_.exchange(false);
    }

    // ---- Tonemap ----
    f32 GetTonemapExposure() const {
        return tonemapExposure_;
    }
    void SetTonemapExposure(f32 e) {
        tonemapExposure_ = e;
    }

    // ---- Graphics debug ----
    // Mirrored as an init-time flag: read once at device creation in
    // RenderPipeline::InitDevice and passed into gfx::CreateDevice,
    // which routes it to the per-backend validation layer (Vulkan
    // VK_LAYER_KHRONOS_validation, d3d11 DEBUG flag, d3d12 debug layer
    // + InfoQueue break-on-error). Changing it mid-run has no effect
    // until the next process start — the host persists it to .ini so
    // the next launch picks the new value up.
    bool GraphicsDebug() const {
        return graphicsDebug_;
    }
    void SetGraphicsDebug(bool v) {
        graphicsDebug_ = v;
    }

    // ---- Default GFX backend ----
    // The host (basic_viewer) reads this when no `--backend` argument
    // was given on the command line. RenderSettings stores it so the
    // existing INI save/load + Settings-window infrastructure can be
    // reused; the renderer itself never inspects it (its backend is
    // already fixed by the time RenderPipeline::InitDevice runs).
    gfx::GfxApi DefaultBackend() const {
        return defaultBackend_;
    }
    void SetDefaultBackend(gfx::GfxApi b) {
        defaultBackend_ = b;
    }

    // ---- Preferred GFX device ----
    // Exact-match name of the physical adapter the host wants the
    // selected backend to open (compared verbatim against the names
    // returned by gfx::EnumerateDevices). Empty (the default) means
    // "let the backend pick — highest VRAM / discrete over integrated".
    // Plumbed in InitDevice via gfx::SetPreferredDevice before
    // gfx::CreateDevice. Takes effect on the next process launch.
    const std::string& PreferredDevice() const {
        return preferredDevice_;
    }
    void SetPreferredDevice(std::string name) {
        preferredDevice_ = std::move(name);
    }

private:
    // Bit-cast helpers for the float-in-atomic<u32> knobs (atomic<f32> isn't
    // portable). Used by the depth-of-field accessors above.
    static f32 loadF32(const std::atomic<u32>& a) {
        const u32 bits = a.load();
        f32 v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    static void storeF32(std::atomic<u32>& a, f32 v) {
        u32 bits;
        std::memcpy(&bits, &v, sizeof(bits));
        a.store(bits);
    }

    // Display flags — plain bools; readers tolerate single-byte tearing.
    bool showGrid_ = true;
    bool showParticles_ = true;
    bool showRibbons_ = true;
    bool showCollisions_ = false;
    bool showLights_ = false;
    bool showEvents_ = true;
    bool showPhysicsDynamic_ = false;
    bool showPhysicsKinematic_ = false;
    bool showPhysicsStatic_ = false;
    bool showPhysicsCloth_ = false;
    bool physicsSubstepping_ = true;
    bool poseSolvers_ = false;
    GroundQuery groundQuery_;

    // Render mode + dirty flag.
    RenderMode renderMode_ = RenderMode::SD;
    std::atomic<bool> renderModeDirty_{false};
    std::atomic<bool> sceneHdrInSd_{false};
    std::atomic<bool> m2LazyAnimations_{false};
    std::atomic<bool> m2DistanceSortGeometry_{false};
    std::atomic<bool> m2ModelLights_{true};

    // Debug + LOD.
    std::atomic<i32> hdDebugMode_{0};
    std::atomic<bool> debugUnlitOddGeosets_{false};
    std::atomic<i32> lodOverride_{0};

    // Ambient occlusion (HD-mode GTAO). On by default — the user can
    // disable from the settings menu if they don't want the pass.
    std::atomic<bool> aoEnabled_{true};

    // Quality preset (Low=0, Medium=1, High=2). High by default — the
    // extra slice/step taps comfortably fit the budget on every backend
    // we ship, and the lower presets exist mainly as a fallback.
    std::atomic<u32> aoQuality_{2};

    // Bent-normal IBL boost (float bits in u32 — atomic<f32> isn't
    // portable). 0 disables the boost pass.
    std::atomic<u32> aoBentBoost_{0};

    // M3 deferred local lights + the gate's scripted debug point light.
    // Float knobs as raw bits, same as everything above.
    std::atomic<bool> deferredLightsEnabled_{true};
    std::atomic<bool> dbgPointLightOn_{false};
    std::atomic<u32> dbgPointLightPosX_{0};
    std::atomic<u32> dbgPointLightPosY_{0};
    std::atomic<u32> dbgPointLightPosZ_{0};
    std::atomic<u32> dbgPointLightColR_{0x3F800000u}; // 1.0f
    std::atomic<u32> dbgPointLightColG_{0x3F800000u}; // 1.0f
    std::atomic<u32> dbgPointLightColB_{0x3F800000u}; // 1.0f
    std::atomic<u32> dbgPointLightRange_{0x42C80000u}; // 100.0f

    // Bloom — defaults match the engine's RegisterBloom (BL_BLOOM_D=0
    // off, threshold=1.0, intensity=1.25, saturation=1.0). Floats stored
    // as raw bits in an atomic<u32>; helper bit-casts are below.
    std::atomic<bool> bloomEnabled_{false};
    std::atomic<u32> bloomThreshold_{0x3F800000u}; // 1.0f
    std::atomic<u32> bloomIntensity_{0x3FA00000u}; // 1.25f
    std::atomic<u32> bloomSaturation_{0x3F800000u}; // 1.0f

    // Depth of field — off by default (the host supplies a focal distance).
    // Defaults mirror WC3: maxBlurSize=10, radiusScale=1, focusScale=1.
    std::atomic<bool> refractionEnabled_{true};
    std::atomic<bool> refractionDebugMask_{false};
    std::atomic<bool> multiTexParticlesEnabled_{true};
    std::atomic<bool> dofEnabled_{false};
    std::atomic<u32> dofFocusDistance_{0};            // 0.0f — disables the pass
    std::atomic<u32> dofFocusScale_{0x3F800000u};     // 1.0f
    std::atomic<u32> dofMaxBlurSize_{0x41200000u};    // 10.0f
    std::atomic<u32> dofRadiusScale_{0x3F800000u};    // 1.0f
    std::atomic<bool> dofFarFieldOnly_{false};

    // Lighting + clear color.
    std::atomic<u8> lightingMode_{static_cast<u8>(LightingMode::InGame)};
    std::atomic<u32> backgroundColor_{0x00453A35u};

    // IBL. The world default has to be the day/night pair, not the portrait
    // probe: `CWorldFrameWar3` loads `[DayEnvironmentMap]`/`[NightEnvironmentMap]`
    // for the map's tileset, while `[PortraitEnvironmentMap]` belongs to
    // `CPortraitButton` alone. It also matters more than it looks — the HD DNC
    // rigs carry `ambientIntensity = 0`, so on the plain-HD shader this probe
    // is the entire ambient term, and the portrait probe is ~30% dimmer than
    // Lordaeron Summer's day map (mean luma 0.17 vs 0.25) and never varies
    // with time of day.
    IblMode iblMode_ = IblMode::DayNight;
    // Starts clean: the first apply is triggered by the Warcraft III content
    // gate, not by device init (see MarkIblModeDirty).
    std::atomic<bool> iblModeDirty_{false};

    // Tonemap exposure.
    f32 tonemapExposure_ = 1.0f;

    // Graphics-debug (validation layers). Off by default — turning it
    // on costs frame time and noise. Set before InitDevice runs.
    bool graphicsDebug_ = false;

    // Host-only: default backend when --backend is not on the command
    // line. D3D12 matches the long-standing test_main default on
    // Windows; macOS prefers Metal (D3D12 isn't available there);
    // every other platform falls back to Vulkan (which is the only
    // backend the gfx_factory builds elsewhere).
#if defined(_WIN32)
    gfx::GfxApi defaultBackend_ = gfx::GfxApi::D3D12;
#elif defined(__APPLE__)
    gfx::GfxApi defaultBackend_ = gfx::GfxApi::Metal;
#else
    gfx::GfxApi defaultBackend_ = gfx::GfxApi::Vulkan;
#endif

    // Host-only: preferred physical device name. Empty = default pick.
    std::string preferredDevice_;
};

} // namespace whiteout::flakes::renderer
