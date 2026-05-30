// Browser facade. JS owns navigator.gpu, the canvas, input events, and the
// asset fetch pump; WASM owns the renderer and AssetManager. API mirrors
// mdx-m3-viewer's shape so host pages can swap backends.

import { Instance, Model, Scene, TEAM_COLORS, TEAM_COLOR_NAMES } from './wf-instance.js';
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
export { TEAM_COLORS, TEAM_COLOR_NAMES, Instance, Model, Scene };

export class WhiteoutViewer {
    constructor(canvas) {
        if (!canvas) throw new Error('WhiteoutViewer: canvas required');
        this.canvas = canvas;
        this._module = null;
        this._handle = 0;
        this._raf = 0;
        this._lastTime = 0;
        // Hive's CASC mirror — CORS-enabled, 302 to resolved asset,
        // server-side family expansion. Override for a local proxy.
        this.cascUrl = (path) =>
            'https://www.hiveworkshop.com/casc-contents/?path=' +
            encodeURIComponent(path);
        // Direct-asset prefix skips the /casc-contents/ 302 (1 fewer
        // round-trip per asset). HD tree by default since modern
        // Reforged content lives there; SD/locale fall through to cascUrl.
        this.cascDirectAssetBase =
            'https://www.hiveworkshop.com/assets/wc3/war3.w3mod/_hd.w3mod/';
        // Firefox wgpu/naga emits slow fragment code for HD PBR; full
        // DPR tips into fragment-bound at zoom-in. Cap at 1 there; opt
        // back in via `viewer.backingPixelRatio = devicePixelRatio`.
        this.backingPixelRatio =
            (navigator.userAgent.indexOf('Firefox/') >= 0) ? 1 : (window.devicePixelRatio || 1);
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
                        trace('iw: streaming fetch+compile of ./wf-core.wasm @' + elapsed());
                        const url = './wf-core.wasm?t=' + Date.now();
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
        // Firefox HD shadows make zoom-in fragment-bound — opt out by
        // default; re-enable via setShadowsEnabled(true).
        if (navigator.userAgent.indexOf('Firefox/') >= 0) {
            this._module._wf_set_shadows_enabled(this._handle, 0);
        }
        // SD start; load() flips to HD per actor PreferredRenderMode.
        this._module._wf_set_render_mode(this._handle, 0);

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
            const prof = this.profileFrames;
            const t0 = prof ? performance.now() : 0;
            this._module._wf_tick(this._handle, dt);
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
        const M = this._module;

        // Push MDX bytes under a stable key so SpawnUnit's provider lookup hits.
        const mdxUrl = await Promise.resolve(pathSolver(src));
        if (!mdxUrl) throw new Error('pathSolver returned no URL for src: ' + src);
        log('load: fetching MDX ' + mdxUrl);
        const mdxResp = await fetch(mdxUrl, { cache: 'no-store' });
        if (!mdxResp.ok) throw new Error('fetch MDX failed: ' + mdxResp.status);
        const mdxBytes = new Uint8Array(await mdxResp.arrayBuffer());
        const mdxKey = String(src).split(/[\\/]/).pop();
        this._putBytes(mdxKey, mdxBytes);

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

    // Flip global RenderMode to match the actor's preferred path; HD
    // pipeline on SD data mis-blends multi-layer textures.
    _applyPreferredRenderMode(actorHandle) {
        if (!this._handle || !actorHandle) return;
        const M = this._module;
        if (!M._wf_actor_preferred_render_mode) return; // older wasm build
        const mode = M._wf_actor_preferred_render_mode(this._handle, actorHandle);
        M._wf_set_render_mode(this._handle, mode);
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
        // Focusable + no touch-pan so wheel captures don't scroll the page.
        c.style.touchAction = 'none';
        c.tabIndex = 0;
        let dragging = 0; // 0=none, 1=rotate, 2=pan
        let lastX = 0, lastY = 0;

        c.addEventListener('pointerdown', (e) => {
            c.setPointerCapture(e.pointerId);
            dragging = (e.button === 0) ? 1 : 2;
            lastX = e.clientX; lastY = e.clientY;
            e.preventDefault();
        });
        c.addEventListener('pointermove', (e) => {
            if (!dragging || !this._handle) return;
            const dx = e.clientX - lastX;
            const dy = e.clientY - lastY;
            lastX = e.clientX; lastY = e.clientY;
            if (dragging === 1) this._module._wf_camera_rotate(this._handle, dx, dy);
            // Negate dx for grab-and-slide pan; +dy is already down-on-screen.
            else                this._module._wf_camera_pan(this._handle, -dx, dy);
        });
        const endDrag = (e) => {
            if (dragging) {
                try { c.releasePointerCapture(e.pointerId); } catch (_) {}
                dragging = 0;
            }
        };
        c.addEventListener('pointerup',     endDrag);
        c.addEventListener('pointercancel', endDrag);
        c.addEventListener('pointerleave',  endDrag);

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
        // Suppress context menu — interrupts right-drag pan.
        c.addEventListener('contextmenu', (e) => e.preventDefault());
    }
}
