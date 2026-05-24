// wf-viewer.js — WhiteoutFlakes browser facade (ES module).
//
// Phase 1: WebGPU device handoff + canvas surface + clear-color rAF loop.
// JS owns: navigator.gpu, the canvas, every input event. WASM owns: the
// renderer, the swap chain, every GPU command. Bytes flow via
// `_wf_*` exports (ccall-shape — see exports.txt for the symbol list).
//
// Phase 3 will add loadModel(url); Phase 4 the pointer-event camera
// forwarding; Phase 5 the Web Audio sound emitter. Each adds methods on
// `WhiteoutViewer` without changing this shape.

// Dynamic import with a cache-buster query so each page load picks up
// the latest wf-core.js. Static `import` is cached by URL across reloads
// in Chromium even with hard-reload, which masks JS-level fixes (e.g.
// the chunked-getRandomValues patch in wf-core.js or the pre.js
// runtime patch). The static-import shape was confusing during bring-up.
const { default: createModule } = await import(`./wf-core.js?t=${Date.now()}`);

// Web Crypto enforces a 65,536-byte cap on getRandomValues per call.
// Emscripten's libc startup asks for ~294 KB in a single shot during
// runtime init (likely a thread/stack guard seed buffer), which trips
// QuotaExceededError. Wrap the native API once, on module load, with a
// version that loops in 64 KB chunks. Idempotent — only patches once.
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

// Features we ask the GPUDevice for. The renderer probes each via
// device.HasFeature on the C++ side and degrades gracefully when absent.
const REQUESTED_FEATURES = [
    'texture-compression-bc',     // WC3 BLPs decode to BC1/BC3/BC7
    'float32-filterable',         // HDR sampling
    'rg11b10ufloat-renderable',   // R11G11B10F scene target
];

export class WhiteoutViewer {
    constructor(canvas) {
        if (!canvas) throw new Error('WhiteoutViewer: canvas required');
        this.canvas = canvas;
        this._module = null;
        this._handle = 0;
        this._raf = 0;
        this._lastTime = 0;
    }

    // ------------------------------------------------------------------
    // Bring-up. Awaits adapter+device, then instantiates the WASM module
    // with `preinitializedWebGPUDevice` set so the C++ side's
    // emscripten_webgpu_get_device() returns this same device.
    // ------------------------------------------------------------------
    // Quick sanity check: can Chrome compile ANY wasm? An 8-byte module
    // (magic + version, no sections) is the smallest valid wasm. If this
    // hangs, the browser's wasm compiler itself is wedged and our 1.7 MB
    // module isn't the cause.
    static async smokeTestWasm() {
        const t = performance.now();
        const ms = () => (performance.now() - t).toFixed(0) + ' ms';
        const minimal = new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00]);
        const compileP = WebAssembly.compile(minimal);
        const timeoutP = new Promise((_, rj) =>
            setTimeout(() => rj(new Error('minimal wasm compile timed out at 5 s')), 5000));
        const mod = await Promise.race([compileP, timeoutP]);
        return 'minimal wasm OK @' + ms() + ' (module=' + (mod ? 'present' : 'null') + ')';
    }

    async init() {
        // Run the browser-level smoke test first so we can distinguish a
        // Chrome-WebAssembly bug from a bug in our module's bytecode.
        const trace0 = (s) => {
            console.log('[wf]', s);
            const el = document.getElementById('log');
            if (el) {
                const ln = document.createElement('div');
                ln.style.color = '#aaa';
                ln.textContent = '[wf] ' + s;
                el.appendChild(ln); el.scrollTop = el.scrollHeight;
            }
        };
        try {
            trace0('smoke-testing WebAssembly.compile…');
            trace0(await WhiteoutViewer.smokeTestWasm());
        } catch (e) {
            trace0('SMOKE FAIL: ' + (e && e.message || e));
            throw e;
        }
        // Dual log: console.log for stack traces / object inspection,
        // plus a DOM hook so the on-page #log shows progress even when
        // devtools can't attach (e.g. when WASM is hung on a blocking
        // wait that's also frozen the JS main loop between awaits).
        const trace = (s) => {
            console.log('[wf]', s);
            const el = document.getElementById('log');
            if (el) {
                const ln = document.createElement('div');
                ln.style.color = '#888';
                ln.textContent = '[wf] ' + s;
                el.appendChild(ln);
                el.scrollTop = el.scrollHeight;
            }
        };
        if (!navigator.gpu) {
            throw new Error('WebGPU not available on this browser/profile.');
        }
        trace('requestAdapter…');
        const adapter = await navigator.gpu.requestAdapter();
        if (!adapter) throw new Error('navigator.gpu.requestAdapter returned null.');
        trace('adapter ok; features=' + Array.from(adapter.features).join(','));

        const supported = adapter.features;
        const requiredFeatures = REQUESTED_FEATURES.filter((f) => supported.has(f));
        trace('requestDevice features=[' + requiredFeatures.join(',') + ']…');
        // Race against a watchdog so a hung requestDevice surfaces as an
        // error instead of an indefinite freeze (seen on Edge InPrivate).
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

        // Ensure the canvas's drawing buffer matches its CSS box. Without
        // this WebGPU surfaces at 1×1 by default on some browsers.
        const dpr = window.devicePixelRatio || 1;
        const cssW = this.canvas.clientWidth || this.canvas.width || 800;
        const cssH = this.canvas.clientHeight || this.canvas.height || 600;
        this.canvas.width = Math.max(1, Math.floor(cssW * dpr));
        this.canvas.height = Math.max(1, Math.floor(cssH * dpr));

        // Give the canvas an id so we can hand a CSS selector to the
        // surface descriptor. JS owns the string; we also keep a copy in
        // WASM heap inside wf_init.
        if (!this.canvas.id) this.canvas.id = 'wf-canvas-' + Math.random().toString(36).slice(2, 10);
        const selector = '#' + this.canvas.id;

        // Route WASM stderr to BOTH devtools and the on-page log so the
        // C++ `[wgpu] Init: …` printfs show without needing devtools.
        const wasmLog = (s, color) => {
            console.log('[wasm]', s);
            const el = document.getElementById('log');
            if (el) {
                const ln = document.createElement('div');
                ln.style.color = color;
                ln.textContent = '[wasm] ' + s;
                el.appendChild(ln);
                el.scrollTop = el.scrollHeight;
            }
        };
        // Note: we deliberately do NOT pass `canvas:` to createModule. Doing
        // so makes Emscripten try to wire up its legacy GL/SDL canvas
        // helpers which can conflict with the WebGPU surface flow; the
        // canvas selector is communicated via `wf_init` instead.
        //
        // Multi-stage trace so we can pin down which step of module
        // startup is stalling: pre-fetch, post-instantiate, pre-main,
        // or post-main. Each fires from a different Emscripten hook.
        trace('createModule (wasm fetch+instantiate)…');
        const t0 = performance.now();
        const elapsed = () => (performance.now() - t0).toFixed(0) + ' ms';
        // Watchdog: surfaces a hint after 10s if we haven't progressed,
        // so the page tells us "still pending" instead of looking dead.
        const watchdog = setInterval(() => {
            trace('… still inside createModule after ' + elapsed());
        }, 3000);
        let lastHook = 'none';
        try {
            this._module = await createModule({
                preinitializedWebGPUDevice: device,
                print:    (s) => wasmLog(s, '#9c9'),
                printErr: (s) => wasmLog(s, '#fc8'),
                locateFile: (path) => { trace('locateFile(' + path + ') @' + elapsed()); lastHook = 'locateFile'; return path; },
                // Override the fetch + compile + instantiate so each
                // sub-step is observable. This also bypasses any
                // compileStreaming MIME-type rejection — Python's
                // http.server doesn't register application/wasm by default.
                instantiateWasm: (imports, success) => {
                    (async () => {
                        trace('iw: streaming fetch+compile of ./wf-core.wasm @' + elapsed());
                        // `instantiateStreaming` uses a different Chromium
                        // code path than `compile(arrayBuffer)`: it can
                        // start codegen before the body is fully downloaded
                        // and dispatches more aggressively across worker
                        // threads. On the engines where `compile(buf)`
                        // blocks the main thread (V8 has had this on Edge
                        // with mid-sized modules), streaming often runs
                        // through without freezing.
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
        // Expose the Module instance so the on-page error decoder
        // (index.html describe()) can call getExceptionMessage(excPtr).
        window.__wfModule = this._module;
        trace('module instantiated');

        this._handle = this._module._wf_create();
        if (!this._handle) {
            throw new Error('wf_create returned 0: ' + this._lastErr());
        }
        trace('wf_create handle=0x' + this._handle.toString(16));

        // Phase 2: prefetch the WGSL BLS shader bundles and push them
        // into the FetchContentProvider before wf_init runs. The
        // renderer's BLS cache queries the provider during
        // InitBlsShaders; these bundles MUST be in-memory by that point
        // or InitDevice will report incomplete BLS state and bail.
        trace('prefetching BLS shader bundles…');
        await this._prefetchShaders();
        trace('shaders prefetched (' +
              this._module._wf_provider_count(this._handle) + ' files in provider)');

        // Pass the selector by allocating a NUL-terminated UTF-8 buffer
        // in the module heap. wf_init copies it into the WfRenderer.
        const selBytes = this._module.lengthBytesUTF8(selector) + 1;
        const selPtr = this._module._malloc(selBytes);
        this._module.stringToUTF8(selector, selPtr, selBytes);
        trace('wf_init(' + selector + ',' + this.canvas.width + 'x' + this.canvas.height + ')…');
        const ok = this._module._wf_init(this._handle, selPtr, this.canvas.width, this.canvas.height);
        this._module._free(selPtr);
        if (!ok) throw new Error('wf_init returned 0: ' + this._lastErr());
        trace('wf_init ok');

        // A vivid color so the smoke test is obviously WASM-driven, not
        // a default-cleared canvas.
        this._module._wf_set_background(this._handle, 24, 56, 96); // moody blue

        // Resize tracking. Canvas resize → recompute drawing buffer →
        // tell WASM to reconfigure the surface.
        this._resizeObserver = new ResizeObserver(() => this._onResize());
        this._resizeObserver.observe(this.canvas);

        // Pointer + wheel input → CameraView. Left-drag rotates,
        // middle/right-drag pans, wheel zooms, double-click resets.
        this._installCameraControls();

        this._lastTime = performance.now();
        this._loop = (now) => {
            if (!this._handle) return;
            const dt = Math.min(0.1, Math.max(0.0, (now - this._lastTime) / 1000));
            this._lastTime = now;
            this._module._wf_tick(this._handle, dt);
            this._module._wf_render(this._handle);
            this._raf = requestAnimationFrame(this._loop);
        };
        this._raf = requestAnimationFrame(this._loop);
        return this;
    }

    _onResize() {
        if (!this._handle) return;
        const dpr = window.devicePixelRatio || 1;
        const cssW = this.canvas.clientWidth || 1;
        const cssH = this.canvas.clientHeight || 1;
        const w = Math.max(1, Math.floor(cssW * dpr));
        const h = Math.max(1, Math.floor(cssH * dpr));
        if (w === this.canvas.width && h === this.canvas.height) return;
        this.canvas.width = w;
        this.canvas.height = h;
        this._module._wf_resize(this._handle, w, h);
    }

    setBackground(r, g, b) {
        if (this._handle) this._module._wf_set_background(this._handle, r | 0, g | 0, b | 0);
    }

    _installCameraControls() {
        const c = this.canvas;
        // Make the canvas focusable so it can capture wheel events without
        // scrolling the page, and stop the browser's default touch-pan.
        c.style.touchAction = 'none';
        c.tabIndex = 0;
        let dragging = 0; // 0=none, 1=rotate, 2=pan
        let lastX = 0, lastY = 0;

        c.addEventListener('pointerdown', (e) => {
            c.setPointerCapture(e.pointerId);
            // Left button = rotate, middle/right = pan.
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
            // Pan: negate so dragging the canvas moves the scene WITH
            // the cursor (grab-and-slide), matching every other 3D viewer.
            else                this._module._wf_camera_pan(this._handle, -dx, -dy);
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

        // Wheel zoom — preventDefault stops page scroll. CameraView's
        // Zoom takes an integer wheel-delta in detents; we use 3 detents
        // per notch by default so a single scroll click moves a useful
        // distance, and negate the sign so scroll-up zooms in to match
        // every other 3D viewer.
        const ZOOM_STEPS_PER_NOTCH = 16;
        c.addEventListener('wheel', (e) => {
            if (!this._handle) return;
            e.preventDefault();
            const notches = Math.max(1, Math.round(Math.abs(e.deltaY) / 100));
            const steps = -Math.sign(e.deltaY) * notches * ZOOM_STEPS_PER_NOTCH;
            this._module._wf_camera_zoom(this._handle, steps);
        }, { passive: false });

        // Double-click resets to the default orbit pose.
        c.addEventListener('dblclick', () => {
            if (this._handle) this._module._wf_camera_reset(this._handle);
        });

        // Right-click context menu would interrupt right-drag pan.
        c.addEventListener('contextmenu', (e) => e.preventDefault());
    }

    // ------------------------------------------------------------------
    // Asset prefetch — Phase 2 plumbing.
    //
    // Fetches every WGSL BLS bundle the renderer asks for during
    // InitBlsShaders, plus the optional probes/SLKs the warning trail
    // showed during Phase 1 bring-up. Pushes each file's bytes into the
    // FetchContentProvider via _wf_provider_put. All fetches in parallel.
    // ------------------------------------------------------------------
    async _prefetchShaders() {
        const VS = ['foliage','gritty_hd','hd','imgui','popcornfx','sd_highspec',
                    'sd_on_hd','sprite','terrain','toon_hd'];
        const PS = ['crystal','distortion','foliage','gritty_hd','hd','imgui',
                    'popcornfx','sd','sd_on_hd','sprite','terrain','tonemap','toon_hd'];
        // The renderer's BLS cache resolves the api-subdir at lookup time, so
        // we push under the SAME path it queries: `shaders/webgpu/<stage>/<name>.bls`.
        const paths = [
            ...VS.map(n => `shaders/webgpu/vs/${n}.bls`),
            ...PS.map(n => `shaders/webgpu/ps/${n}.bls`),
        ];
        await Promise.all(paths.map(p => this._fetchAndPut(p)));
    }

    async _fetchAndPut(path) {
        const r = await fetch('./' + path, { cache: 'no-store' });
        if (!r.ok) { console.warn('[wf] prefetch FAIL ' + path + ' status=' + r.status); return; }
        const buf = new Uint8Array(await r.arrayBuffer());
        // Allocate in WASM heap and copy in.
        const pathBytes = this._module.lengthBytesUTF8(path) + 1;
        const pathPtr = this._module._malloc(pathBytes);
        this._module.stringToUTF8(path, pathPtr, pathBytes);
        const dataPtr = this._module._malloc(buf.byteLength);
        this._module.HEAPU8.set(buf, dataPtr);
        this._module._wf_provider_put(this._handle, pathPtr, dataPtr, buf.byteLength);
        this._module._free(pathPtr);
        this._module._free(dataPtr);
    }

    // Pull the C-side captured exception what() string. Used by error
    // messages so the on-page log shows the real C++ failure instead of
    // Emscripten's `{excPtr: ...}` placeholder.
    _lastErr() {
        if (!this._module || !this._module._wf_last_error) return '<no module>';
        const ptr = this._module._wf_last_error();
        return ptr ? this._module.UTF8ToString(ptr) : '<empty>';
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
}
