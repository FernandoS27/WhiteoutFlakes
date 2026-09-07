// Browser facade. JS owns navigator.gpu, the canvas, input events, and the
// asset fetch pump; WASM owns the renderer and AssetManager. API mirrors
// mdx-m3-viewer's shape so host pages can swap backends.

import { Instance, Model, Scene, TEAM_COLORS, TEAM_COLOR_NAMES,
         EFFECT_EXTENSIONS, isEffectPath,
         MODEL_EXTENSIONS, isModelPath } from './wf-instance.js';
import { pumpAssetNeeds } from './wf-asset-pump.js';
import { prefetchEngineAssets, prefetchShaders } from './wf-prefetch.js';

// Cache-bust the module URL — the ES module map ignores HTTP no-store.
const { default: createModule } = await import(`./wf-core.js?t=${Date.now()}`);

// Web Crypto caps getRandomValues at 64 KiB/call; Emscripten asks for
// ~294 KiB at startup. Chunk the native API once, on module load.
{
    const MAX_BYTES = 65536;
    const native = crypto.getRandomValues.bind(crypto);
    if (!crypto.__wfChunked) {
        crypto.getRandomValues = function chunked(buf) {
            if (!buf || buf.byteLength <= MAX_BYTES) return native(buf);
            const u8 = new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
            for (let off = 0; off < u8.length; off += MAX_BYTES) {
                native(u8.subarray(off, Math.min(off + MAX_BYTES, u8.length)));
            }
            return buf;
        };
        crypto.__wfChunked = true;
    }
}

// Renderer probes each on the C++ side and degrades gracefully when absent.
const REQUESTED_FEATURES = [
    'texture-compression-bc',     // WC3 BLPs decode to BC1/BC3/BC7
    'float32-filterable',         // HDR sampling
    'rg11b10ufloat-renderable',   // R11G11B10F scene target
];

// Re-export so host pages keep `import { WhiteoutViewer, TEAM_COLORS }`.
export { TEAM_COLORS, TEAM_COLOR_NAMES, Instance, Model, Scene,
         EFFECT_EXTENSIONS, isEffectPath, MODEL_EXTENSIONS, isModelPath };

// HD debug-vis modes — keep ordering in sync with basic_viewer's
// kDebugVisLabels (tools/basic_viewer/viewer_ui.cpp).
export const HD_DEBUG_MODES = [
    { value: 0, label: 'Shaded' },
    { value: 1, label: 'Albedo' },
    { value: 2, label: 'World Normal' },
    { value: 3, label: 'LOD Heatmap' },
    { value: 4, label: 'Light Count' },
    { value: 5, label: 'Shading (white)' },
    { value: 6, label: 'Shading (grey)' },
    { value: 7, label: 'Specular Only' },
    { value: 8, label: 'No ORM' },
    { value: 9, label: 'AO Only' },
];

export class WhiteoutViewer {
    constructor(canvas, options = {}) {
        if (!canvas) throw new Error('WhiteoutViewer: canvas required');
        this.canvas = canvas;
        this._module = null;
        this._handle = 0;
        this._raf = 0;
        this._lastTime = 0;
        // HD/SD is auto-detected per model from its MDX material layers
        // (see `_applyPreferredRenderMode`), mirroring basic_viewer's
        // `ViewerApp::LoadModel`: any layer on a non-SD shader means HD.
        // `options.hdMode` is ignored — we always bootstrap in SD and let
        // the first load() flip to HD via setHdMode when the model needs
        // it. The bootstrap mode only covers the engine/shader + DnC
        // prefetch, which resolve fine in SD; per-model deps then fetch
        // under the detected overlay (cascUrl `context=hd/sd`, `_hd.w3mod`
        // direct prefix) once the model has been probed.
        //
        // `options.forceHd` ("Reforged Graphics"): pin every model to the HD
        // pipeline + HD asset overlay regardless of its detected preference
        // (see `setForceHd` / `_applyPreferredRenderMode`). Bootstrap in HD so
        // the very first load fetches HD deps.
        this._forceHd = options.forceHd === true;
        this.hdMode = this._forceHd;
        // Hive's CASC mirror — CORS-enabled, 302 to resolved asset,
        // server-side family expansion. Override for a local proxy.
        // Encode per-segment so `/` stays literal in the query; Hive's
        // path normalizer doesn't always decode %2F back to a separator
        // (observed on sound/ paths). The `context` param tells the
        // backend which mod stack to resolve against: `hd` searches the
        // `_hd.w3mod` overlay, `sd` forces the base/SD stack. Model deps
        // send it so an SD model doesn't get HD textures. Engine/startup
        // assets (DNC, IBL — base content, mode-agnostic) pass
        // `withContext=false` to fetch them with no context at all.
        this.cascUrl = (path, withContext = true) => {
            const url = 'https://www.hiveworkshop.com/casc-contents/?path=' +
                path.split('/').map(encodeURIComponent).join('/');
            if (!withContext) return url;
            return url + (this.hdMode ? '&context=hd' : '&context=sd');
        };
        // Direct-asset prefix skips the /casc-contents/ 302 (1 fewer
        // round-trip per asset). Only used in HD mode — the direct
        // tree only mirrors `_hd.w3mod/` content. SD viewers fall
        // through to cascUrl, which carries `context=sd` so the backend
        // resolves against the base/SD mod stack.
        this.cascDirectAssetBase = this.hdMode
            ? 'https://www.hiveworkshop.com/assets/wc3/war3.w3mod/_hd.w3mod/'
            : null;
        // Firefox wgpu/naga emits slow fragment code for HD PBR; full
        // DPR tips into fragment-bound at zoom-in. Cap at 1 there; opt
        // back in via `viewer.backingPixelRatio = devicePixelRatio`.
        this.backingPixelRatio =
            (navigator.userAgent.indexOf('Firefox/') >= 0) ? 1 : (window.devicePixelRatio || 1);

        // Day-night cycle. `_todHours` mirrors the engine's DncService TOD
        // (engine default 11.5). When `_dayNightAnimate` is on, the render
        // loop advances it so a full 24 h cycle takes 60 s (0.4 h/s) and
        // pushes the value to WASM each frame; otherwise the slider drives it.
        this._todHours = 11.5;
        this._dayNightAnimate = false;

        // Need-path -> original `src`. AssetManager lower-cases every path it
        // surfaces, and a standalone effect is fetched through that queue
        // rather than pushed as bytes, so the pump would otherwise hand a
        // case-folded URL to the solver. Populated by _loadEffectImpl.
        this._needAliases = new Map();
    }

    // Bring up the WebGPU device, then instantiate the WASM module with
    // `preinitializedWebGPUDevice` so the C++ side shares it.
    async init() {
        const trace = (s) => console.log('[wf]', s);
        if (!navigator.gpu) {
            throw new Error('WebGPU not available on this browser/profile.');
        }
        const device = await this._initDevice(trace);
        await this._initModule(device, trace);
        this._handle = this._module._wf_create();
        if (!this._handle) throw new Error('wf_create returned 0: ' + this._lastErr());
        trace('wf_create handle=0x' + this._handle.toString(16));

        // BLS bundles + engine assets must be in the provider before
        // wf_init reads them via ReadFile.
        trace('prefetching BLS shader bundles…');
        await prefetchShaders(this);
        await prefetchEngineAssets(this);
        trace('assets prefetched (' +
              this._module._wf_provider_count(this._handle) + ' files in provider)');

        this._initRenderer(trace);
        this._installCameraControls();
        this._startLoop();
        return this;
    }

    // ---- init helpers --------------------------------------------------

    async _initDevice(trace) {
        trace('requestAdapter…');
        // Prefer discrete GPU on hybrid systems; browser may ignore on battery.
        const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
        if (!adapter) throw new Error('navigator.gpu.requestAdapter returned null.');
        trace('adapter ok; features=' + Array.from(adapter.features).join(','));

        const requiredFeatures = REQUESTED_FEATURES.filter((f) => adapter.features.has(f));
        trace('requestDevice features=[' + requiredFeatures.join(',') + ']…');
        // Watchdog vs. hung requestDevice (seen on Edge InPrivate).
        const devicePromise = adapter.requestDevice({ requiredFeatures });
        const deviceTimeout = new Promise((_, reject) =>
            setTimeout(() => reject(new Error('adapter.requestDevice timed out at 10 s')), 10000));
        const device = await Promise.race([devicePromise, deviceTimeout]);
        if (!device) throw new Error('adapter.requestDevice returned null.');
        device.addEventListener('uncapturederror', (e) => {
            console.error('[wf] uncaptured GPU error:', e.error);
        });
        device.lost.then((info) => {
            console.error('[wf] device lost:', info.reason, info.message);
        });
        this._device = device;
        trace('device ok');
        return device;
    }

    async _initModule(device, trace) {
        // Surfaces at 1×1 on some browsers without an explicit size.
        const init = this._computeBackingSize();
        this.canvas.width = init.w;
        this.canvas.height = init.h;
        // Surface descriptor needs a CSS selector.
        if (!this.canvas.id) this.canvas.id = 'wf-canvas-' + Math.random().toString(36).slice(2, 10);

        const wasmLog = (s) => console.log('[wasm]', s);
        trace('createModule (wasm fetch+instantiate)…');
        const t0 = performance.now();
        const elapsed = () => (performance.now() - t0).toFixed(0) + ' ms';
        const watchdog = setInterval(() => {
            trace('… still inside createModule after ' + elapsed());
        }, 3000);
        let lastHook = 'none';
        try {
            this._module = await createModule({
                preinitializedWebGPUDevice: device,
                print:    wasmLog,
                printErr: wasmLog,
                locateFile: (path) => { trace('locateFile(' + path + ') @' + elapsed()); lastHook = 'locateFile'; return path; },
                // Custom instantiate sidesteps MIME-type rejection (python
                // http.server doesn't register application/wasm) and
                // streams codegen for a smoother startup than compile(buf).
                instantiateWasm: (imports, success) => {
                    (async () => {
                        // Resolve against this module's URL, not the document
                        // base — when the package is mounted under a subdir
                        // (e.g. /whiteout-js-viewer/), a document-relative
                        // './wf-core.wasm' would mis-resolve to the host root.
                        const url = new URL(
                            './wf-core.wasm?t=' + Date.now(),
                            import.meta.url).href;
                        trace('iw: streaming fetch+compile of ' + url + ' @' + elapsed());
                        const { module, instance } = await WebAssembly.instantiateStreaming(
                            fetch(url, { cache: 'no-store' }), imports);
                        trace('iw: streaming compile+instantiate done @' + elapsed());
                        success(instance, module);
                    })().catch((e) => trace('iw: ERR ' + (e && e.message || e)));
                    return {}; // async signal
                },
                preInit:  [() => { trace('preInit @' + elapsed()); lastHook = 'preInit'; }],
                preRun:   [() => { trace('preRun @' + elapsed()); lastHook = 'preRun'; }],
                onRuntimeInitialized: () => { trace('onRuntimeInitialized @' + elapsed()); lastHook = 'onRuntimeInitialized'; },
                postRun:  [() => { trace('postRun @' + elapsed()); lastHook = 'postRun'; }],
            });
        } finally {
            clearInterval(watchdog);
        }
        trace('createModule resolved in ' + elapsed() + ' (last hook: ' + lastHook + ')');
        // index.html's error decoder reads exception messages from here.
        window.__wfModule = this._module;
        trace('module instantiated');
    }

    _initRenderer(trace) {
        const selector = '#' + this.canvas.id;
        const selBytes = this._module.lengthBytesUTF8(selector) + 1;
        const selPtr = this._module._malloc(selBytes);
        this._module.stringToUTF8(selector, selPtr, selBytes);
        trace('wf_init(' + selector + ',' + this.canvas.width + 'x' + this.canvas.height + ')…');
        const ok = this._module._wf_init(this._handle, selPtr, this.canvas.width, this.canvas.height);
        this._module._free(selPtr);
        if (!ok) throw new Error('wf_init returned 0: ' + this._lastErr());
        trace('wf_init ok');

        this._module._wf_set_background(this._handle, 24, 56, 96); // moody blue
        // Shadows on by default everywhere except Firefox — wgpu/naga's
        // HD shadow sample is expensive there and makes zoom-in
        // fragment-bound. Both sides go through setShadowsEnabled.
        if (navigator.userAgent.indexOf('Firefox/') >= 0) {
            this._module._wf_set_shadows_enabled(this._handle, 0);
        } else {
            this._module._wf_set_shadows_enabled(this._handle, 1);
        }
        // Bloom on by default (the engine ships it off). HD-only, and the
        // extract/combine bundles it needs are in the prefetch set.
        this._module._wf_set_bloom_enabled(this._handle, 1);
        // Initial render mode honours the constructor's `hdMode` option;
        // load() can still flip it per actor PreferredRenderMode.
        this._module._wf_set_render_mode(this._handle, this.hdMode ? 1 : 0);

        this._resizeObserver = new ResizeObserver(() => this._onResize());
        this._resizeObserver.observe(this.canvas);
    }

    _startLoop() {
        this._lastTime = performance.now();
        this._emaDt = 1 / 60;
        // Enable per-frame profiling via `viewer.profileFrames = true`.
        // GPU-bound shows as small render + big rAF-gap.
        this.profileFrames = false;
        this._profAccum = { ticks: 0, ticksMs: 0, renderMs: 0, gapMs: 0, last: 0 };
        this._loop = (now) => {
            if (!this._handle) return;
            const elapsed = now - this._lastTime;
            const dt = Math.min(0.1, Math.max(0.0, elapsed / 1000));
            this._lastTime = now;
            if (dt > 0) {
                const k = 0.1; // ~10-frame smoothing
                this._emaDt = this._emaDt * (1 - k) + dt * k;
            }
            // 60-second day-night cycle: 24 h / 60 s = 0.4 h per second.
            if (this._dayNightAnimate) {
                this._todHours = (this._todHours + 0.4 * dt) % 24;
                this._module._wf_set_time_of_day(this._handle, this._todHours);
            }
            const prof = this.profileFrames;
            const t0 = prof ? performance.now() : 0;
            this._module._wf_tick(this._handle, dt);
            // After the tick, so the framing query sees this frame's particles.
            this._tickEffectFraming();
            const t1 = prof ? performance.now() : 0;
            this._module._wf_render(this._handle);
            const t2 = prof ? performance.now() : 0;
            this._raf = requestAnimationFrame(this._loop);
            pumpAssetNeeds(this);
            if (prof) this._tickProfile(now, t0, t1, t2, elapsed);
        };
        this._raf = requestAnimationFrame(this._loop);
    }

    _tickProfile(now, t0, t1, t2, elapsed) {
        const p = this._profAccum;
        p.ticks += 1;
        p.ticksMs += (t1 - t0);
        p.renderMs += (t2 - t1);
        p.gapMs += elapsed;
        if (now - p.last < 1000) return;
        const n = Math.max(1, p.ticks);
        console.log('[wf-prof] tick=' + (p.ticksMs / n).toFixed(2)
            + ' ms  render=' + (p.renderMs / n).toFixed(2)
            + ' ms  rAF-gap=' + (p.gapMs / n).toFixed(2)
            + ' ms  (' + n + ' frames)');
        p.ticks = 0; p.ticksMs = 0; p.renderMs = 0; p.gapMs = 0;
        p.last = now;
    }

    // ---- public API ----------------------------------------------------

    // Returns 0 until the loop has run at least one frame.
    getFps() {
        if (!this._emaDt || this._emaDt <= 0) return 0;
        return 1 / this._emaDt;
    }

    // Solver fetches Texture/Particle/ChildModel deps the AssetManager
    // surfaces after load(). Returns one URL or a fallback chain.
    setPathSolver(solver) { this._lazySolver = solver; }

    // Per-dep progress hooks — start when queued, end when resolved.
    setFetchHooks({ start, end } = {}) {
        this._onFetchStart = typeof start === 'function' ? start : null;
        this._onFetchEnd   = typeof end   === 'function' ? end   : null;
    }

    setBackground(r, g, b) {
        if (this._handle) this._module._wf_set_background(this._handle, r | 0, g | 0, b | 0);
    }
    // 0=in-game, 1=glue (portrait), 2=dynamic. See enums.h::LightingMode.
    setLightingMode(mode) {
        if (this._handle) this._module._wf_set_lighting_mode(this._handle, mode | 0);
    }
    setShowGrid(on) {
        if (this._handle) this._module._wf_set_show_grid(this._handle, on ? 1 : 0);
    }
    // Default off on Firefox (3-cascade sample is expensive on wgpu/naga).
    setShadowsEnabled(on) {
        if (this._handle) this._module._wf_set_shadows_enabled(this._handle, on ? 1 : 0);
    }
    // HD-only bloom. On by default (see _initRenderer).
    setBloomEnabled(on) {
        if (this._handle) this._module._wf_set_bloom_enabled(this._handle, on ? 1 : 0);
    }

    // HD debug-vis mode (see HD_DEBUG_MODES below).
    setHdDebugMode(mode) {
        if (this._handle) this._module._wf_set_hd_debug_mode(this._handle, mode | 0);
    }

    // ---- day-night cycle ----------------------------------------------

    // Set the time of day in hours [0..24); wraps. Used by the slider for
    // manual control and as the seed before animating.
    setTimeOfDay(hours) {
        let h = Number(hours);
        if (!Number.isFinite(h)) return;
        h = ((h % 24) + 24) % 24;
        this._todHours = h;
        if (this._handle) this._module._wf_set_time_of_day(this._handle, h);
    }
    // Current time of day in hours — the render loop advances this while
    // animating, so the UI reads it back to keep the slider in sync.
    getTimeOfDay() { return this._todHours; }
    // Toggle the 60-second animated day-night cycle (0.4 h/s in _loop).
    setDayNightAnimate(on) { this._dayNightAnimate = !!on; }
    // IBL probe set: 0=Portrait, 1=DayNight, 2=Dungeon, 3=Sunset. The day/night
    // toggle flips Portrait↔DayNight so HD env reflections cycle with the TOD.
    setIblMode(mode) {
        if (this._handle) this._module._wf_set_ibl_mode(this._handle, mode | 0);
    }

    // Flip HD asset preference at runtime. Updates the flag the
    // `cascUrl` / `cascDirectAssetBase` getters read (so subsequent
    // network fetches route through Hive's `_hd.w3mod` overlay) and
    // mirrors it to the WASM-side RenderMode so the renderer picks
    // the HD pipeline. Does NOT invalidate already-cached assets —
    // call this before any spawn / asset prefetch, or evict the
    // affected paths from the provider yourself.
    setHdMode(on) {
        this.hdMode = !!on;
        this.cascDirectAssetBase = this.hdMode
            ? 'https://www.hiveworkshop.com/assets/wc3/war3.w3mod/_hd.w3mod/'
            : null;
        if (this._handle) {
            this._module._wf_set_render_mode(this._handle, this.hdMode ? 1 : 0);
        }
    }

    // "Reforged Graphics" — force every model into the HD pipeline + HD asset
    // overlay, overriding per-model SD/HD auto-detection. The host should
    // reload the current model after toggling so its deps re-fetch under the
    // new overlay; turning it off lets the next load auto-detect again.
    setForceHd(on) {
        this._forceHd = !!on;
        if (this._forceHd) this.setHdMode(true);
    }

    // Live WebGPU CreateTexture+CreateBuffer bytes (deferred-delete
    // excluded). `__hive.viewer.gpuBytes()` for leak-spotting.
    gpuBytes() {
        if (!this._handle) return 0;
        return this._module._wf_gpu_bytes(this._handle);
    }
    logGpuBytes(tag = '') {
        const b = this.gpuBytes();
        const mb = (b / (1024 * 1024)).toFixed(1);
        console.log(`[wf-gpu] ${tag} live=${mb} MiB (${b} bytes)`);
    }

    // mdx-m3-viewer-shape load. `src` is opaque; the solver returns its
    // URL (and the URL of every dep). With no solver, deps resolve
    // relative to `src`'s directory.
    async load(src, pathSolver = null) {
        if (!this._handle) throw new Error('viewer not initialised');

        if (!pathSolver) {
            const baseDir = String(src).substring(0, String(src).lastIndexOf('/') + 1);
            pathSolver = (name) => {
                if (typeof name !== 'string') return null;
                if (name === src) return src;
                return baseDir + name;
            };
        }

        // An effect frames the camera on its particle cloud, which is nowhere
        // near where the next model wants it — a `.pkb` zoomed in hard leaves
        // the following model rendered from inside its boots. Undo only the
        // app's own move; a camera the user placed still survives a switch.
        if (this._cameraFramedByEffect) {
            this.resetCamera();
            this._cameraFramedByEffect = false;
        }

        // mdx-m3-viewer returns Model sync + resolves async; we await so
        // callers can `await load()` while exposing model.whenLoaded().
        const model = new Model(this, src);
        if (!this._models) this._models = [];
        this._models.push(model);
        model._loadPromise = this._loadInternal(src, pathSolver, model);
        await model._loadPromise;
        return model;
    }

    async loadModel(mdxUrl) {
        const model = await this.load(mdxUrl);
        return model.addInstance();
    }

    // mdx-m3-viewer parity — resolves when every model has loaded.
    whenAllLoaded(models, cb) {
        const p = Promise.all(models.map(m => m._loadPromise || Promise.resolve(m)));
        if (typeof cb === 'function') p.then(() => cb(models));
        return p;
    }

    // Defer a frame so the latest paint flushes before readback.
    toBlob(cb, type, quality) {
        requestAnimationFrame(() => this.canvas.toBlob(cb, type, quality));
    }

    resetCamera() {
        if (this._handle) this._module._wf_camera_reset(this._handle);
    }
    // Wheel-equivalent zoom in detent steps; positive pulls the camera in.
    // For on-screen +/- buttons, where there is no wheel to listen to.
    zoomBy(steps) {
        if (this._handle) this._module._wf_camera_zoom(this._handle, steps | 0);
    }
    // Multiplicative zoom — `scale` > 1 pulls in. What a pinch produces.
    zoomScale(scale) {
        if (this._handle && this._module._wf_camera_zoom_scale) {
            this._module._wf_camera_zoom_scale(this._handle, Number(scale) || 1);
        }
    }
    // Wipe live splats — use on sequence change to avoid carryover.
    clearSplats() {
        if (this._handle) this._module._wf_clear_splats(this._handle);
    }

    // mdx-m3-viewer parity — parsers are static here.
    addHandler() { /* intentionally empty */ }
    addScene() { return this.scene; }
    get scene() {
        if (!this._scene) this._scene = new Scene(this);
        return this._scene;
    }

    dispose() {
        if (this._raf) cancelAnimationFrame(this._raf);
        this._raf = 0;
        if (this._resizeObserver) {
            this._resizeObserver.disconnect();
            this._resizeObserver = null;
        }
        if (this._handle && this._module) {
            this._module._wf_destroy(this._handle);
            this._handle = 0;
        }
    }

    // ---- model load internals -----------------------------------------

    async _loadInternal(src, pathSolver, model) {
        const log = (s) => console.log('[wf]', s);
        // Drain any deferred cleanup before spawning.
        this._module._wf_tick(this._handle, 0);
        return this._loadInternalImpl(src, pathSolver, model, log);
    }

    async _loadInternalImpl(src, pathSolver, model, log) {
        if (isEffectPath(src)) return this._loadEffectImpl(src, pathSolver, model, log);
        const M = this._module;

        // Back to the bootstrap mode before the MDX fetch below, so the probe
        // further down is what decides HD and not whatever was loaded last.
        // The mode picks the CASC overlay, so a viewer left in HD (by an
        // earlier HD model, or by a `.pkb`, which pins it) would fetch this
        // model's MDX out of `_hd.w3mod` — and Hive hands back the Reforged
        // variant of a model the user asked for in SD, which then probes HD
        // and pins the viewer for good. Reforged Graphics stays pinned:
        // `_forceHd` IS the baseline when it's on.
        this.setHdMode(this._forceHd);

        // Push the bytes under a stable key so SpawnUnit's provider lookup
        // hits. An `.m3` rides the same route: the loader sniffs the magic
        // rather than the name, and reads the file back through the provider.
        // The solver may hand back a single URL or a fallback chain (local
        // object URL → Hive direct → /casc-contents/), same as the asset pump.
        // A directory pick resolves to a chain, so a bare fetch(chain) would
        // stringify the array into a bogus URL and 404 — try each in order.
        let mdxCands = await Promise.resolve(pathSolver(src));
        if (!mdxCands) throw new Error('pathSolver returned no URL for src: ' + src);
        if (!Array.isArray(mdxCands)) mdxCands = [mdxCands];
        let mdxBytes = null;
        let mdxFrom = '';
        for (const url of mdxCands) {
            if (!url) continue;
            log('load: fetching model ' + url);
            try {
                const r = await fetch(url, { cache: 'no-store' });
                if (!r.ok) continue;
                mdxBytes = new Uint8Array(await r.arrayBuffer());
                mdxFrom = r.url || url;
                break;
            } catch (_) { /* try next candidate */ }
        }
        if (!mdxBytes) throw new Error('fetch model failed for all candidates: ' + src);
        log('load: fetched model ' + mdxFrom);
        const mdxKey = String(src).split(/[\\/]/).pop();
        this._putBytes(mdxKey, mdxBytes);

        // A World of Warcraft `.m2` is not self-contained the way an MDX/M3 is:
        // its skin profiles (the actual drawable geometry) live in sibling
        // `.skin` files that the parser reads *synchronously* during
        // SpawnUnit. The asset pump only runs after spawn, so unless those
        // siblings are already in the provider the parse finds no skin, the
        // model has no geometry, and it renders nothing. Pre-fetch them here.
        if (mdxKey.toLowerCase().endsWith('.m2'))
            await this._prefetchM2Skins(mdxKey, pathSolver, log);

        // PE1 emitter-child-MDX search base. Informational for Hive solvers.
        const srcDir = String(src).substring(0, String(src).lastIndexOf('/') + 1);
        if (srcDir) this._setPe1Base(srcDir.replace(/^\.?\/?/, ''));

        // Texture slots start on a white placeholder; bytes swap in via
        // the per-frame asset-pump as they arrive.
        log('spawn: ' + mdxKey);
        const keyPtr = this._cstr(mdxKey);
        let handle = 0;
        try {
            handle = M._wf_spawn_unit(this._handle, keyPtr);
            // Drop MDX bytes once SpawnUnit's template parse consumed them.
            M._wf_provider_evict(this._handle, keyPtr);
        } finally {
            M._free(keyPtr);
        }
        if (!handle) {
            model.error = new Error('SpawnUnit returned 0');
            throw model.error;
        }
        this._applyPreferredRenderMode(handle);
        model.loaded = true;
        const inst = new Instance(this, model, handle);
        model._instances.push(inst);

        // Start fetches before the next rAF.
        pumpAssetNeeds(this);
        return model;
    }

    // Standalone `.pkb` / `.pkfx`: no MDX to fetch and no template to key on.
    // The renderer's CornEffectSource asks the AssetManager for the effect
    // itself, so the bytes arrive through the same needs pump that serves a
    // model's corn-fx dependencies — we only have to spawn and let it drain.
    async _loadEffectImpl(src, pathSolver, model, log) {
        const M = this._module;
        if (!M._wf_spawn_effect) {
            throw new Error('this wf-core build has no wf_spawn_effect export');
        }
        // The effect file is a pump dependency, not something we fetch here,
        // so with no persistent solver installed nothing would ever arrive —
        // a model at least still spawns without one. Adopt this load's solver
        // so `viewer.load(pkbUrl, solver)` works on its own.
        if (!this._lazySolver && pathSolver) this._lazySolver = pathSolver;

        // Corn effects are Reforged-only content, so one always renders
        // through the HD pipeline regardless of the previous model's mode —
        // same rule as basic_viewer's LoadEffectIntoActiveScene.
        this.setHdMode(true);

        const rel = String(src).replaceAll('\\', '/');
        // Textures the effect names resolve against its own directory.
        const dir = rel.substring(0, rel.lastIndexOf('/') + 1);
        if (dir) this._setPe1Base(dir.replace(/^\.?\/?/, ''));
        // The pump will see this path lower-cased; alias it back so a
        // case-sensitive source URL still resolves.
        this._needAliases.set(rel.toLowerCase(), src);

        log('spawn effect: ' + rel);
        const keyPtr = this._cstr(rel);
        let handle = 0;
        try {
            handle = M._wf_spawn_effect(this._handle, keyPtr);
        } finally {
            M._free(keyPtr);
        }
        if (!handle) {
            model.error = new Error('SpawnEffect returned 0: ' + this._lastErr());
            throw model.error;
        }
        model.loaded = true;
        const inst = new Instance(this, model, handle);
        model._instances.push(inst);

        pumpAssetNeeds(this);
        this._armEffectFraming(handle);
        return model;
    }

    // A `.pkb` has no mesh bounds, so the camera can only be framed on the
    // particle cloud once the effect has emitted one — and the .pkb itself
    // is still in flight through the asset pump at this point. Retry from
    // the render loop over a bounded window, warming up first so the AABB
    // reflects the steady-state spread rather than the first puff.
    _armEffectFraming(actorHandle) {
        this._effectFrameActor = actorHandle;
        this._effectFrameTicks = 0;
    }
    _tickEffectFraming() {
        if (!this._effectFrameActor) return;
        const WARMUP_TICKS = 12;
        const MAX_TICKS = 600; // ~10 s at 60 Hz; covers a slow CASC fetch
        if (++this._effectFrameTicks < WARMUP_TICKS) return;
        const M = this._module;
        const framed = !!M._wf_camera_frame_effect
            && M._wf_camera_frame_effect(this._handle, this._effectFrameActor) === 1;
        if (framed) this._cameraFramedByEffect = true;
        if (framed || !M._wf_camera_frame_effect || this._effectFrameTicks >= MAX_TICKS) {
            this._effectFrameActor = 0;
        }
    }

    // Auto-detect the actor's HD/SD mode from its own MDX material layers
    // and adopt it for this load — the JS analogue of basic_viewer's
    // ViewerApp::LoadModel HD-probe. The WASM side reports HD when any
    // material layer carries a non-zero BLS shaderId (== a non-SD shader,
    // the same test viewer_app.cpp runs on Layer::ShaderType), so the
    // constructor's hdMode is ignored as the mode authority. Routing
    // through setHdMode moves the asset overlay (cascUrl `&hd=1` /
    // `_hd.w3mod` direct prefix) in lockstep with the render pipeline —
    // running an HD pipeline on SD data (or vice-versa) mis-blends
    // multi-layer materials, and fetching deps from the wrong overlay
    // pulls the wrong texture set. This fires before the load's texture
    // pump (see _loadInternalImpl), so every dependent fetch resolves
    // under the detected mode.
    _applyPreferredRenderMode(actorHandle) {
        if (!this._handle || !actorHandle) return;
        // Reforged Graphics pins HD; skip the per-model probe entirely.
        if (this._forceHd) { this.setHdMode(true); return; }
        const M = this._module;
        if (!M._wf_actor_preferred_render_mode) return; // older wasm build
        const hd = M._wf_actor_preferred_render_mode(this._handle, actorHandle) === 1;
        this.setHdMode(hd);
    }

    // ---- WASM helpers (used by sibling modules) -----------------------

    // Allocates a NUL-terminated UTF-8 buffer. Caller MUST free or the
    // WASM heap fragments fast under the asset pump.
    _cstr(s) {
        const M = this._module;
        const bytes = M.lengthBytesUTF8(s) + 1;
        const p = M._malloc(bytes);
        M.stringToUTF8(s, p, bytes);
        return p;
    }
    _putBytes(path, u8) {
        const M = this._module;
        const pathPtr = this._cstr(path);
        const dataPtr = M._malloc(u8.byteLength);
        M.HEAPU8.set(u8, dataPtr);
        try {
            M._wf_provider_put(this._handle, pathPtr, dataPtr, u8.byteLength);
        } finally {
            M._free(dataPtr);
            M._free(pathPtr);
        }
    }
    // Pre-Legion `.m2` models name their skin profiles positionally,
    // `<stem>00.skin` upward; the header's profile count is the manifest, but
    // it isn't known until the base file is parsed, so try a small fixed range
    // and push whatever resolves. Keyed by the exact basename the parser's file
    // system requests (relative to the model, which in the browser has no
    // directory component). Legion+ models name skins by fileDataID in SFID and
    // need a listfile the browser cannot resolve — those still won't render.
    async _prefetchM2Skins(mdxKey, pathSolver, log) {
        const dot = mdxKey.lastIndexOf('.');
        const stem = dot >= 0 ? mdxKey.slice(0, dot) : mdxKey;
        for (let i = 0; i < 4; ++i) {
            const skinName = stem + String(i).padStart(2, '0') + '.skin';
            let cands = await Promise.resolve(pathSolver(skinName));
            if (!cands) continue;
            if (!Array.isArray(cands)) cands = [cands];
            for (const url of cands) {
                if (!url) continue;
                try {
                    const r = await fetch(url, { cache: 'no-store' });
                    if (!r.ok) continue;
                    this._putBytes(skinName, new Uint8Array(await r.arrayBuffer()));
                    log('load: m2 skin ' + skinName);
                    break;
                } catch (_) { /* try next candidate */ }
            }
        }
    }
    _setPe1Base(p) {
        const M = this._module;
        const pathPtr = this._cstr(p);
        try {
            M._wf_set_pe1_base(this._handle, pathPtr);
        } finally {
            M._free(pathPtr);
        }
    }
    // Surfaces the C++ what() string instead of Emscripten's `{excPtr:…}`.
    _lastErr() {
        if (!this._module || !this._module._wf_last_error) return '<no module>';
        const ptr = this._module._wf_last_error();
        return ptr ? this._module.UTF8ToString(ptr) : '<empty>';
    }

    // ---- canvas / camera input ----------------------------------------

    _onResize() {
        if (!this._handle) return;
        const { w, h } = this._computeBackingSize();
        if (w === this.canvas.width && h === this.canvas.height) return;
        this.canvas.width = w;
        this.canvas.height = h;
        this._module._wf_resize(this._handle, w, h);
    }

    // CSS box × backingPixelRatio.
    _computeBackingSize() {
        const dpr = this.backingPixelRatio || 1;
        const cssW = this.canvas.clientWidth || this.canvas.width || 800;
        const cssH = this.canvas.clientHeight || this.canvas.height || 600;
        return {
            w: Math.max(1, Math.floor(cssW * dpr)),
            h: Math.max(1, Math.floor(cssH * dpr)),
        };
    }

    _installCameraControls() {
        const c = this.canvas;
        // Focusable + no touch-pan so wheel/gesture captures don't scroll
        // or bounce-zoom the host page on a phone.
        c.style.touchAction = 'none';
        c.tabIndex = 0;

        // Live pointers, in down order. A mouse contributes exactly one; a
        // touchscreen contributes one per finger, which is what separates
        // one-finger orbit from two-finger pan/pinch.
        const pointers = new Map();
        let dragging = 0;    // 0=none, 1=rotate, 2=pan
        let lastX = 0, lastY = 0;
        // Two-finger gesture state: centroid + spread at the last move.
        let gestureX = 0, gestureY = 0, gestureSpan = 0;
        let lastTapMs = 0;

        const touches = () => [...pointers.values()];
        const centroid = (pts) => {
            let x = 0, y = 0;
            for (const p of pts) { x += p.x; y += p.y; }
            return { x: x / pts.length, y: y / pts.length };
        };
        const span = (pts) => Math.hypot(pts[0].x - pts[1].x, pts[0].y - pts[1].y);
        // Seed the gesture baseline so the first move after a finger goes
        // down or up measures against the new configuration, not the old.
        const armGesture = () => {
            const pts = touches();
            if (pts.length < 2) return;
            const cen = centroid(pts);
            gestureX = cen.x; gestureY = cen.y;
            gestureSpan = span(pts);
        };

        c.addEventListener('pointerdown', (e) => {
            c.setPointerCapture(e.pointerId);
            pointers.set(e.pointerId, { x: e.clientX, y: e.clientY, type: e.pointerType });
            if (pointers.size === 1) {
                // Mouse: left orbits, every other button pans (matching the
                // desktop viewer). Touch: a lone finger orbits.
                dragging = (e.pointerType !== 'mouse' || e.button === 0) ? 1 : 2;
                lastX = e.clientX; lastY = e.clientY;
            } else {
                dragging = 0; // two-finger gesture takes over from here
                armGesture();
            }
            e.preventDefault();
        });

        c.addEventListener('pointermove', (e) => {
            if (!this._handle) return;
            const p = pointers.get(e.pointerId);
            if (!p) return;
            p.x = e.clientX; p.y = e.clientY;

            if (pointers.size >= 2) {
                // Pinch to zoom, slide to pan — both read off the same two
                // fingers, so a diagonal gesture does a bit of each.
                const pts = touches().slice(0, 2);
                const cen = centroid(pts);
                const sp = span(pts);
                // Ignore sub-pixel jitter; a stationary pinch shouldn't drift.
                if (gestureSpan > 0 && sp > 0 && Math.abs(sp - gestureSpan) > 0.5) {
                    this.zoomScale(sp / gestureSpan);
                }
                const dx = cen.x - gestureX;
                const dy = cen.y - gestureY;
                if (dx || dy) {
                    this._module._wf_camera_pan(this._handle, -Math.round(dx), Math.round(dy));
                }
                gestureX = cen.x; gestureY = cen.y; gestureSpan = sp;
                e.preventDefault();
                return;
            }

            if (!dragging) return;
            const dx = e.clientX - lastX;
            const dy = e.clientY - lastY;
            lastX = e.clientX; lastY = e.clientY;
            if (dragging === 1) this._module._wf_camera_rotate(this._handle, dx, dy);
            // Negate dx for grab-and-slide pan; +dy is already down-on-screen.
            else                this._module._wf_camera_pan(this._handle, -dx, dy);
        });

        const endDrag = (e) => {
            pointers.delete(e.pointerId);
            try { c.releasePointerCapture(e.pointerId); } catch (_) {}
            if (pointers.size >= 2) {
                armGesture(); // dropped to a different pair — rebaseline
            } else if (pointers.size === 1) {
                // Lifting one of two fingers hands control back to orbit,
                // starting from where the remaining finger actually is.
                const [only] = touches();
                lastX = only.x; lastY = only.y;
                dragging = 1;
                gestureSpan = 0;
            } else {
                dragging = 0;
                gestureSpan = 0;
            }
        };
        c.addEventListener('pointerup',     endDrag);
        c.addEventListener('pointercancel', endDrag);
        // NB: no pointerleave handler. With pointer capture the pointer never
        // truly leaves, and a touch that grazes the canvas edge fires it —
        // which used to drop the drag mid-gesture.

        // Wheel zoom — 16 detents/notch, scroll-up zooms in.
        const ZOOM_STEPS_PER_NOTCH = 16;
        c.addEventListener('wheel', (e) => {
            if (!this._handle) return;
            e.preventDefault();
            const notches = Math.max(1, Math.round(Math.abs(e.deltaY) / 100));
            const steps = -Math.sign(e.deltaY) * notches * ZOOM_STEPS_PER_NOTCH;
            this._module._wf_camera_zoom(this._handle, steps);
        }, { passive: false });

        c.addEventListener('dblclick', () => {
            if (this._handle) this._module._wf_camera_reset(this._handle);
        });
        // Double-tap resets too — a touchscreen synthesises dblclick only
        // erratically, and it's the one recovery gesture worth having when
        // a pinch has flung the camera somewhere useless.
        c.addEventListener('pointerup', (e) => {
            if (e.pointerType === 'mouse' || pointers.size) return;
            const now = e.timeStamp || performance.now();
            if (now - lastTapMs < 300 && this._handle) {
                this._module._wf_camera_reset(this._handle);
                lastTapMs = 0;
            } else {
                lastTapMs = now;
            }
        });
        // Suppress context menu — interrupts right-drag pan, and a long
        // press on a touchscreen would otherwise pop it mid-gesture.
        c.addEventListener('contextmenu', (e) => e.preventDefault());
    }
}
