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

// ----------------------------------------------------------------------------
// Math helpers — minimum needed for setLocation/setRotation/setScale.
// Compose to a column-vector 4x4 (renderer uses Matrix44f, same memory
// layout we ship from JS as 16 floats).
// ----------------------------------------------------------------------------

const _mat = new Float32Array(16);
function _identityMat(out) {
    out[0] = 1;  out[1] = 0;  out[2] = 0;  out[3] = 0;
    out[4] = 0;  out[5] = 1;  out[6] = 0;  out[7] = 0;
    out[8] = 0;  out[9] = 0;  out[10] = 1; out[11] = 0;
    out[12] = 0; out[13] = 0; out[14] = 0; out[15] = 1;
    return out;
}
// Compose a transform matrix from translation, quaternion rotation, and
// non-uniform scale. JS quat = [x, y, z, w].
function _composeTRS(out, t, q, s) {
    const x = q[0], y = q[1], z = q[2], w = q[3];
    const xx = x * x, yy = y * y, zz = z * z;
    const xy = x * y, xz = x * z, yz = y * z;
    const wx = w * x, wy = w * y, wz = w * z;
    const sx = s[0], sy = s[1], sz = s[2];
    out[0]  = (1 - 2 * (yy + zz)) * sx;
    out[1]  = (2 * (xy + wz)) * sx;
    out[2]  = (2 * (xz - wy)) * sx;
    out[3]  = 0;
    out[4]  = (2 * (xy - wz)) * sy;
    out[5]  = (1 - 2 * (xx + zz)) * sy;
    out[6]  = (2 * (yz + wx)) * sy;
    out[7]  = 0;
    out[8]  = (2 * (xz + wy)) * sz;
    out[9]  = (2 * (yz - wx)) * sz;
    out[10] = (1 - 2 * (xx + yy)) * sz;
    out[11] = 0;
    out[12] = t[0];
    out[13] = t[1];
    out[14] = t[2];
    out[15] = 1;
    return out;
}

// Standard Warcraft 3 player team-color palette (0-23). Each entry is
// the sRGB 8-bit team color the ReplaceableTextureManager bakes onto
// the model's TeamColor slot.
// Warcraft III player team-color palette. Hex values lifted verbatim from
// Hive's `ratory_wc3model_preview` markup so the swatch grid sits 1:1 with
// the colours users see on the Hiveworkshop viewer. Indices 0-23 match
// WC3's player slots (Red, Blue, Teal, … Peanut).
export const TEAM_COLORS = [
    [0xff, 0x04, 0x02], [0x00, 0x42, 0xff], [0x1b, 0xe6, 0xba], [0x54, 0x00, 0x81],
    [0xff, 0xfc, 0x00], [0xff, 0x8a, 0x0d], [0x20, 0xc0, 0x00], [0xe4, 0x5b, 0xb0],
    [0x94, 0x96, 0x97], [0x7e, 0xbf, 0xf1], [0x10, 0x62, 0x47], [0x4f, 0x2a, 0x05],
    [0x9c, 0x00, 0x00], [0x00, 0x00, 0xc3], [0x00, 0xeb, 0xff], [0xbd, 0x00, 0xff],
    [0xec, 0xcd, 0x86], [0xf7, 0xa4, 0x8b], [0xc0, 0xff, 0x80], [0xdc, 0xb9, 0xec],
    [0x4f, 0x4f, 0x55], [0xec, 0xf0, 0xff], [0x00, 0x78, 0x1e], [0xa4, 0x6f, 0x33],
];

// Human-readable labels matching Hive's `title=...` chip tooltips.
export const TEAM_COLOR_NAMES = [
    'Red',     'Blue',       'Teal',      'Purple',
    'Yellow',  'Orange',     'Green',     'Pink',
    'Gray',    'Light blue', 'Dark green','Brown',
    'Maroon',  'Navy',       'Turquoise', 'Violet',
    'Wheat',   'Peach',      'Mint',      'Lavender',
    'Coal',    'Snow',       'Emerald',   'Peanut',
];

// ----------------------------------------------------------------------------
// Instance — mdx-m3-viewer's per-actor handle. Wraps one ActorHandle and
// owns its TRS state; pushes a composed Matrix44f to the renderer on every
// mutation. All setters return `this` to mirror mdx-m3-viewer's chaining.
// ----------------------------------------------------------------------------
export class Instance {
    constructor(viewer, model, actorHandle) {
        this._viewer = viewer;
        this._model = model;
        this._handle = actorHandle;
        this._M = viewer._module;
        this._vh = viewer._handle;
        this._location = new Float32Array(3);
        this._rotation = new Float32Array([0, 0, 0, 1]);
        this._scale = new Float32Array([1, 1, 1]);
        this._visible = true;
        this._timeScale = 1;
        this._teamColor = 0;
    }

    // Push current TRS as a 4x4 to the renderer. When hidden, push a
    // zero-scale matrix so the actor stays alive (detach() destroys it)
    // but contributes nothing to the frame.
    _pushTransform() {
        if (!this._handle) return;
        if (this._visible) {
            _composeTRS(_mat, this._location, this._rotation, this._scale);
        } else {
            _identityMat(_mat);
            _mat[0] = _mat[5] = _mat[10] = 0;
        }
        const M = this._M;
        const ptr = M._malloc(64);
        M.HEAPF32.set(_mat, ptr >> 2);
        M._wf_actor_set_transform(this._vh, this._handle, ptr);
        M._free(ptr);
    }

    setLocation(loc) {
        this._location[0] = loc[0]; this._location[1] = loc[1]; this._location[2] = loc[2];
        this._pushTransform();
        return this;
    }
    move(d) {
        this._location[0] += d[0]; this._location[1] += d[1]; this._location[2] += d[2];
        this._pushTransform();
        return this;
    }
    setRotation(q) {
        this._rotation[0] = q[0]; this._rotation[1] = q[1];
        this._rotation[2] = q[2]; this._rotation[3] = q[3];
        this._pushTransform();
        return this;
    }
    setScale(s) {
        if (typeof s === 'number') {
            this._scale[0] = this._scale[1] = this._scale[2] = s;
        } else {
            this._scale[0] = s[0]; this._scale[1] = s[1]; this._scale[2] = s[2];
        }
        this._pushTransform();
        return this;
    }
    setUniformScale(s) {
        this._scale[0] = this._scale[1] = this._scale[2] = +s;
        this._pushTransform();
        return this;
    }
    setTransformation(loc, rot, scale) {
        if (loc) { this._location[0] = loc[0]; this._location[1] = loc[1]; this._location[2] = loc[2]; }
        if (rot) { this._rotation[0] = rot[0]; this._rotation[1] = rot[1];
                   this._rotation[2] = rot[2]; this._rotation[3] = rot[3]; }
        if (scale) {
            if (typeof scale === 'number') {
                this._scale[0] = this._scale[1] = this._scale[2] = scale;
            } else {
                this._scale[0] = scale[0]; this._scale[1] = scale[1]; this._scale[2] = scale[2];
            }
        }
        this._pushTransform();
        return this;
    }
    resetTransformation() {
        this._location[0] = this._location[1] = this._location[2] = 0;
        this._rotation[0] = this._rotation[1] = this._rotation[2] = 0;
        this._rotation[3] = 1;
        this._scale[0] = this._scale[1] = this._scale[2] = 1;
        this._pushTransform();
        return this;
    }

    // mdx-m3-viewer accepts either a sequence index or a name. We resolve
    // names through the actor's Sequences() list.
    setSequence(idOrName) {
        if (!this._handle) return this;
        let idx = idOrName;
        if (typeof idOrName === 'string') {
            idx = this.getSequences().indexOf(idOrName);
            if (idx < 0) return this;
        }
        this._M._wf_actor_set_sequence(this._vh, this._handle, idx | 0);
        return this;
    }
    setSequenceLoopMode(mode) {
        if (this._handle) this._M._wf_actor_set_loop_mode(this._vh, this._handle, mode | 0);
        return this;
    }
    setTeamColor(id) {
        this._teamColor = id | 0;
        const c = TEAM_COLORS[this._teamColor] || TEAM_COLORS[0];
        if (this._handle) this._M._wf_actor_set_team_color(this._vh, this._handle, c[0], c[1], c[2]);
        return this;
    }
    setAnimationTimeMs(ms) {
        if (this._handle) this._M._wf_actor_set_anim_time(this._vh, this._handle, ms | 0);
        return this;
    }

    set timeScale(s) {
        this._timeScale = +s;
        if (this._handle) this._M._wf_actor_set_playback_speed(this._vh, this._handle, +s);
    }
    get timeScale() { return this._timeScale; }

    show() { this._visible = true;  this._pushTransform(); return this; }
    hide() { this._visible = false; this._pushTransform(); return this; }
    shown()  { return this._visible; }
    hidden() { return !this._visible; }

    detach() {
        if (!this._handle) return;
        this._M._wf_actor_destroy(this._vh, this._handle);
        this._handle = 0;
        const arr = this._model._instances;
        const i = arr.indexOf(this);
        if (i >= 0) arr.splice(i, 1);
    }

    getSequences() {
        if (!this._handle) return [];
        const M = this._M;
        const n = M._wf_actor_get_sequence_count(this._vh, this._handle);
        if (!n) return [];
        const CAP = 128;
        const buf = M._malloc(CAP);
        const out = [];
        for (let i = 0; i < n; ++i) {
            M._wf_actor_get_sequence_name(this._vh, this._handle, i, buf, CAP);
            out.push(M.UTF8ToString(buf));
        }
        M._free(buf);
        return out;
    }

    // Names of every camera preset baked into the source MDX. Hive's
    // Cameras dropdown is populated from this list (plus a "Reset" row
    // for free-orbit). Static FOV / position / target / clip per preset
    // live on the C++ side; `activateCameraPreset(idx)` applies them.
    getCameraPresets() {
        if (!this._handle) return [];
        const M = this._M;
        const n = M._wf_actor_camera_preset_count(this._vh, this._handle);
        if (!n) return [];
        const CAP = 128;
        const buf = M._malloc(CAP);
        const out = [];
        for (let i = 0; i < n; ++i) {
            M._wf_actor_camera_preset_name(this._vh, this._handle, i, buf, CAP);
            out.push(M.UTF8ToString(buf));
        }
        M._free(buf);
        return out;
    }

    // Activate this actor's preset `idx`, or pass -1 to drop back to
    // the orbital free-camera with the engine's default FoV / clip.
    activateCameraPreset(idx) {
        if (this._handle) this._M._wf_camera_activate_preset(this._vh, this._handle, idx | 0);
        return this;
    }
}

// ----------------------------------------------------------------------------
// Model — the loaded MDX asset. Owns the resolved-and-cached state (the
// renderer caches the template internally once load() has run a successful
// SpawnUnit). `addInstance()` cheaply re-spawns from that template.
// ----------------------------------------------------------------------------
export class Model {
    constructor(viewer, src) {
        this._viewer = viewer;
        this._src = src;
        this._mdxKey = String(src).split(/[\\/]/).pop();
        this._instances = [];
        this.loaded = false;
        this.error = null;
        this._loadPromise = null;
    }

    addInstance() {
        if (!this.loaded) throw new Error('Model.addInstance() before load resolved');
        const M = this._viewer._module;
        const handle = M._wf_spawn_unit(this._viewer._handle, this._viewer._cstr(this._mdxKey));
        if (!handle) throw new Error('addInstance: wf_spawn_unit returned 0');
        const inst = new Instance(this._viewer, this, handle);
        this._instances.push(inst);
        return inst;
    }

    whenLoaded() { return this._loadPromise; }
}

// ----------------------------------------------------------------------------
// Scene — mdx-m3-viewer has many; we have one renderer-side scene. The
// class is a thin housekeeping bucket so the host-side API shape matches.
// ----------------------------------------------------------------------------
export class Scene {
    constructor(viewer) {
        this._viewer = viewer;
        this._instances = [];
    }

    // Accepts either a Model (creates a fresh Instance via addInstance)
    // or an existing Instance (just tracks it).
    addInstance(modelOrInstance) {
        const inst = modelOrInstance instanceof Model
            ? modelOrInstance.addInstance() : modelOrInstance;
        if (inst && this._instances.indexOf(inst) < 0) this._instances.push(inst);
        return inst;
    }

    removeInstance(inst) {
        const i = this._instances.indexOf(inst);
        if (i >= 0) this._instances.splice(i, 1);
        if (inst) inst.detach();
    }

    clear() {
        // Destroy each tracked actor; cheaper than wf_clear_all when the
        // scene only owns a subset of the renderer's actors.
        for (const inst of this._instances) {
            if (inst._handle) this._viewer._module._wf_actor_destroy(this._viewer._handle, inst._handle);
            inst._handle = 0;
        }
        this._instances.length = 0;
    }
}

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
        // Also prefetch the assets wf_init's DNC service + IBL probe
        // loader will read. Those run inside InitDevice, BEFORE any
        // loadModel call, so they have to be in the cache by now.
        await this._prefetchEngineAssets();
        trace('assets prefetched (' +
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
        // Start in SD mode. Each successful load() probes the actor's
        // PreferredRenderMode (HD if any layer carries a non-zero BLS
        // shaderId) and flips the global setting accordingly. Driving the
        // HD pipeline against SD materials silently mis-blends multi-layer
        // SD textures (the reddish wash on classic Arthas, etc.).
        this._module._wf_set_render_mode(this._handle, 0);

        // Resize tracking. Canvas resize → recompute drawing buffer →
        // tell WASM to reconfigure the surface.
        this._resizeObserver = new ResizeObserver(() => this._onResize());
        this._resizeObserver.observe(this.canvas);

        // Pointer + wheel input → CameraView. Left-drag rotates,
        // middle/right-drag pans, wheel zooms, double-click resets.
        this._installCameraControls();

        this._lastTime = performance.now();
        this._lazyTick = 0;
        this._loop = (now) => {
            if (!this._handle) return;
            const dt = Math.min(0.1, Math.max(0.0, (now - this._lastTime) / 1000));
            this._lastTime = now;
            this._module._wf_tick(this._handle, dt);
            this._module._wf_render(this._handle);
            this._raf = requestAnimationFrame(this._loop);
            // Pump the AssetManager needs queue. SpawnUnit + frame_ticker
            // both surface paths the renderer wants; we drain once per
            // frame and fire fetches in parallel. Covers Texture,
            // Particle (.pkb), and ChildModel kinds end-to-end.
            this._pumpAssetNeeds();
        };
        this._raf = requestAnimationFrame(this._loop);
        return this;
    }

    // Install a path solver the renderer uses to fetch assets the
    // AssetManager surfaces on its needs queue (Texture / Particle /
    // ChildModel slots). Called by the host app (e.g. HiveApp) after
    // the user picks a directory.
    setPathSolver(solver) {
        this._lazySolver = solver;
    }

    // Drain the AssetManager's needs queue and fetch each path. Each
    // need is attempted at most once per solver (deduped via
    // `_attemptedAssets`) — the renderer surfaces a need the first
    // time it Acquires a path, never again, so this is naturally
    // small (one entry per unique texture path the model uses).
    _pumpAssetNeeds() {
        if (!this._handle) return;
        const M = this._module;
        if (!M._wf_assets_needs_count) return; // older wasm without bridge
        const n = M._wf_assets_needs_count(this._handle);
        if (!n) return;
        if (!this._inflightAssets) this._inflightAssets = new Map();
        const CAP = 512;
        const buf = M._malloc(CAP);
        try {
            for (let i = 0; i < n; ++i) {
                const kind = M._wf_assets_needs_get_kind(this._handle, i);
                M._wf_assets_needs_get_path(this._handle, i, buf, CAP);
                const path = M.UTF8ToString(buf);
                const dedupKey = kind + ':' + path;
                // Dedupe ONLY in-flight fetches, not lifetime ones. When
                // a slot gets released (e.g. model switch destroys the
                // previous actor) and the next model re-Acquires the
                // same path, the path lands on the needs queue again —
                // we MUST re-fetch because the slot's GPU resources
                // were torn down with the old slot. A lifetime-dedup
                // (the old `_attemptedAssets` Set) caused every shared
                // texture between two sequentially-loaded models to
                // stay on the placeholder forever.
                if (this._inflightAssets.has(dedupKey)) continue;
                if (!this._lazySolver) continue;
                const p = this._fetchAndApplyAsset(this._lazySolver, kind, path)
                    .catch(() => {})
                    .finally(() => { this._inflightAssets.delete(dedupKey); });
                this._inflightAssets.set(dedupKey, p);
            }
        } finally {
            M._free(buf);
        }
    }

    // Fetch the bytes for a needed asset and push them into the
    // AssetManager via _wf_assets_apply. Mirrors _fetchDep's
    // extension-synonym walk (.tif ↔ .dds ↔ .blp, etc.) so the
    // server can serve whichever shape it has on disk.
    async _fetchAndApplyAsset(pathSolver, kind, relPath) {
        if (this._onFetchStart) this._onFetchStart(relPath);
        try {
            return await this._fetchAndApplyAssetImpl(pathSolver, kind, relPath);
        } finally {
            if (this._onFetchEnd) this._onFetchEnd(relPath);
        }
    }

    async _fetchAndApplyAssetImpl(pathSolver, kind, relPath) {
        const fwd = relPath.replaceAll('\\', '/');
        const slash = fwd.lastIndexOf('/');
        const dot = fwd.lastIndexOf('.');
        const stemFull = dot > 0 ? fwd.slice(0, dot) : fwd;
        const stemBase = slash >= 0 ? stemFull.slice(slash + 1) : stemFull;
        const origExt  = dot > 0 ? fwd.slice(dot).toLowerCase() : '';

        // Extension synonyms by kind. Texture slots route freely between
        // .blp / .dds / .tga / .png / .tif (the renderer's adapter doesn't
        // care which format the server serves). Particle (.pkb) often
        // ships as .pkfx in newer Reforged builds — try both. Model
        // synonyms cover .mdx ↔ .mdl.
        const TEX = ['.blp', '.dds', '.tga', '.png', '.tif'];
        const MDL = ['.mdx', '.mdl'];
        const PRT = ['.pkb', '.pkfx'];
        let exts;
        if (TEX.includes(origExt))      exts = [origExt, ...TEX.filter(e => e !== origExt)];
        else if (MDL.includes(origExt)) exts = [origExt, ...MDL.filter(e => e !== origExt)];
        else if (PRT.includes(origExt)) exts = [origExt, ...PRT.filter(e => e !== origExt)];
        else                            exts = [origExt];

        const stems = [stemFull, stemBase].filter((s, i, a) => a.indexOf(s) === i);

        for (const stem of stems) {
            for (const ext of exts) {
                const candName = stem + ext;
                let url;
                try { url = await Promise.resolve(pathSolver(candName)); }
                catch (_) { continue; }
                if (!url) continue;
                try {
                    const r = await fetch(url, { cache: 'no-store' });
                    if (!r.ok) continue;
                    const bytes = new Uint8Array(await r.arrayBuffer());
                    const applied = this._applyAsset(kind, relPath, bytes, ext);
                    if (applied) return true;
                    // Apply rejected — decode failed. Try next candidate
                    // rather than declaring success on bad bytes.
                } catch (_) { /* try next */ }
            }
        }
        // Log kind so it's easier to triage Texture vs Particle vs
        // ChildModel misses against the on-disk content layout.
        const kindName = ['Texture', 'Particle', 'ChildModel'][kind] || ('k=' + kind);
        console.warn('[wf] asset MISS (' + kindName + ', all candidates failed): ' + relPath);
        return false;
    }

    _applyAsset(kind, path, u8, foundExt) {
        const M = this._module;
        const pathPtr = this._cstr(path);
        const extPtr  = this._cstr(foundExt || '');
        const dataPtr = M._malloc(u8.byteLength);
        M.HEAPU8.set(u8, dataPtr);
        try {
            return !!M._wf_assets_apply(
                this._handle, kind, pathPtr, dataPtr, u8.byteLength, extPtr);
        } finally {
            M._free(dataPtr);
        }
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
    // ------------------------------------------------------------------
    // Hive-compatible load entry point — mirrors mdx-m3-viewer's API.
    //
    //   load(src, pathSolver) -> Promise<Model>
    //
    // `src` is opaque (typically a logical asset name) and is handed
    // straight to `pathSolver` to obtain the MDX URL. Every dependency
    // the renderer subsequently requests (textures, child-MDX, sounds)
    // is also routed through `pathSolver`. The host page therefore owns
    // all URL resolution; this viewer never assumes a particular CDN
    // layout. If `pathSolver` is omitted, `src` is treated as a URL and
    // dependencies are resolved relative to its directory — the same
    // ergonomic shortcut mdx-m3-viewer provides.
    //
    // The returned Promise resolves once the renderer has spawned the
    // actor with all its dependencies satisfied; rejects with a
    // descriptive error if any iteration can't find a required asset.
    // ------------------------------------------------------------------
    async load(src, pathSolver = null) {
        if (!this._handle) throw new Error('viewer not initialised');

        // Default solver: src/name is a URL or relative path the page
        // server can resolve. Dependencies are resolved against the
        // src's directory.
        if (!pathSolver) {
            const baseDir = String(src).substring(0, String(src).lastIndexOf('/') + 1);
            pathSolver = (name) => {
                // Bare filenames (no slash) → assume same directory as src.
                if (typeof name !== 'string') return null;
                if (name === src) return src;
                if (name.indexOf('/') < 0 && name.indexOf('\\') < 0) return baseDir + name;
                return baseDir + name;
            };
        }

        // mdx-m3-viewer's contract: load() returns the Model synchronously
        // and the Model resolves its load asynchronously. To stay
        // promise-friendly while still exposing model.whenLoaded(), we
        // build the Model up front, stash the in-flight Promise on it, and
        // await the whole thing here so callers can `await viewer.load()`.
        const model = new Model(this, src);
        if (!this._models) this._models = [];
        this._models.push(model);
        model._loadPromise = this._loadInternal(src, pathSolver, model);
        await model._loadPromise;
        return model;
    }

    // Convenience wrapper — load + auto-create the first Instance. Mirrors
    // mdx-m3-viewer's `instance = model.addInstance()` shorthand for the
    // very common single-instance use case (and our older smoke harness).
    async loadModel(mdxUrl) {
        const model = await this.load(mdxUrl);
        return model.addInstance();
    }

    // Resolve a list of in-flight Model loads to completion. Mirrors
    // mdx-m3-viewer's `viewer.whenAllLoaded(models, cb)` — supports both
    // the callback form (legacy) and a Promise return for `await`.
    whenAllLoaded(models, cb) {
        // Wait for spawn AND any background texture stream. The texture
        // stream runs after `_loadPromise` resolves (so the caller's
        // `await viewer.load()` returns once the model is visible) —
        // whenAllLoaded chains both so callers that need a fully
        // textured model can await it explicitly.
        const p = Promise.all(models.map(async m => {
            await (m._loadPromise || Promise.resolve());
            if (m._texStream) await m._texStream;
            return m;
        }));
        if (typeof cb === 'function') p.then(() => cb(models));
        return p;
    }

    // mdx-m3-viewer's snapshot API. Defers a rAF so the latest in-flight
    // frame has flushed before reading pixels.
    toBlob(cb, type, quality) {
        requestAnimationFrame(() => this.canvas.toBlob(cb, type, quality));
    }

    // Public wrappers around the camera C facade for UI button handlers
    // (Cameras → Reset). The pointer/wheel installer in init() drives the
    // same C entries; these just give app code a named hook.
    resetCamera() {
        if (this._handle) this._module._wf_camera_reset(this._handle);
    }

    // Drop every live splat (footstep / blood / etc). Use on sequence
    // change so a previous animation's decals don't carry over.
    clearSplats() {
        if (this._handle) this._module._wf_clear_splats(this._handle);
    }

    // Probe the freshly-spawned actor for its preferred render mode and
    // flip the global SettingsView::RenderMode accordingly. Mirrors the
    // desktop viewer's `Settings().SetRenderMode(hero->PreferredRenderMode())`
    // call after spawn (tools/basic_viewer/viewer_app.cpp). The wf side
    // returns 1 for HD (any layer with a non-zero BLS shaderId), 0 for SD.
    _applyPreferredRenderMode(actorHandle) {
        if (!this._handle || !actorHandle) return;
        const M = this._module;
        if (!M._wf_actor_preferred_render_mode) return; // older wasm build
        const mode = M._wf_actor_preferred_render_mode(this._handle, actorHandle);
        M._wf_set_render_mode(this._handle, mode);
    }

    // No-op compat with mdx-m3-viewer's handler-registration API. Our
    // build statically knows how to parse MDX/BLP/MDL.
    addHandler() { /* intentionally empty */ }

    // Single scene by design; addScene() returns the singleton so host
    // code that calls `viewer.addScene()` keeps working.
    addScene() { return this.scene; }

    get scene() {
        if (!this._scene) this._scene = new Scene(this);
        return this._scene;
    }

    async _loadInternal(src, pathSolver, model) {
        const M = this._module;
        // Dual-channel logger so the iteration trace shows up on the
        // on-page log AND in devtools console. We need it visible
        // wherever the user is looking.
        const log = (s) => {
            console.log('[wf]', s);
            const el = document.getElementById('log');
            if (el) {
                const ln = document.createElement('div');
                ln.style.color = '#9cf';
                ln.textContent = '[wf] ' + s;
                el.appendChild(ln);
                el.scrollTop = el.scrollHeight;
            }
        };

        // Step the engine once so any cleanup deferred from before the
        // load (e.g. a previous model's actors flagged via detach()) is
        // applied before we spawn the new actor. The render loop keeps
        // running through the load — we no longer destroy+respawn
        // between iterations (refresh_template uses UpdateMaterials to
        // stream textures onto the live actor instead), so the eviction
        // race that motivated the old rAF pause is gone.
        M._wf_tick(this._handle, 0);
        return this._loadInternalImpl(src, pathSolver, model, log);
    }

    async _loadInternalImpl(src, pathSolver, model, log) {
        const M = this._module;

        // Resolve the MDX URL through pathSolver, fetch it, push bytes
        // under a stable in-provider key. The C side calls SpawnUnit
        // with that same key, so the renderer's provider lookups for
        // the MDX itself always hit.
        const mdxUrl = await Promise.resolve(pathSolver(src));
        if (!mdxUrl) throw new Error('pathSolver returned no URL for src: ' + src);
        log('load: fetching MDX ' + mdxUrl);
        const mdxResp = await fetch(mdxUrl, { cache: 'no-store' });
        if (!mdxResp.ok) throw new Error('fetch MDX failed: ' + mdxResp.status);
        const mdxBytes = new Uint8Array(await mdxResp.arrayBuffer());
        const mdxKey = String(src).split(/[\\/]/).pop();   // basename as the key
        this._putBytes(mdxKey, mdxBytes);

        // PE1 emitter child-MDX paths resolve against this base, same
        // convention as the desktop viewer (parent of the loaded
        // model). For Hive-style pathSolvers we don't actually know a
        // filesystem-shape base; the renderer just hands the bare
        // PE1-referenced names to pathSolver during iteration, so the
        // base path is informational only here.
        const srcDir = String(src).substring(0, String(src).lastIndexOf('/') + 1);
        if (srcDir) this._setPe1Base(srcDir.replace(/^\.?\/?/, ''));

        // One-shot spawn. The renderer Acquires AssetManager slots for
        // every file-backed texture during the MDX parse; each slot
        // starts on the shared white placeholder and the path goes
        // onto the needs queue. The per-frame pump in this._loop
        // drains the queue, fetches each path, and pushes bytes via
        // wf_assets_apply — the slot's texHandle swaps in
        // transparently the next frame. No refresh-tick dance, no
        // re-parse, no handle churn.
        log('spawn: ' + mdxKey);
        const handle = M._wf_spawn_unit(this._handle, this._cstr(mdxKey));
        if (!handle) {
            model.error = new Error('SpawnUnit returned 0');
            throw model.error;
        }
        this._applyPreferredRenderMode(handle);
        model.loaded = true;
        const inst = new Instance(this, model, handle);
        model._instances.push(inst);

        // Kick the needs-pump immediately so the first fetches start
        // overlapping with the page resuming the rAF loop. The
        // per-frame pump in _loop covers subsequent ticks (corn-fx
        // and other runtime-discovered assets).
        this._pumpAssetNeeds();
        return model;
    }

    // Install host-side hooks that fire once per logical dep — start
    // when the dep is queued, end when it resolves (success OR failure).
    // The host uses these to drive a determinate progress bar from
    // (completed / total) where total grows as new deps surface from
    // background fetchers and the AssetManager pump.
    setFetchHooks({ start, end } = {}) {
        this._onFetchStart = typeof start === 'function' ? start : null;
        this._onFetchEnd   = typeof end   === 'function' ? end   : null;
    }

    // Helpers to keep loadModel readable.
    _cstr(s) {
        const M = this._module;
        const bytes = M.lengthBytesUTF8(s) + 1;
        const p = M._malloc(bytes);
        M.stringToUTF8(s, p, bytes);
        // Caller responsibility to free — but for short-lived ABI calls
        // we leak intentionally on the WASM heap (a few hundred bytes per
        // SpawnUnit attempt is fine for a viewer session).
        return p;
    }
    _putBytes(path, u8) {
        const M = this._module;
        const pathPtr = this._cstr(path);
        const dataPtr = M._malloc(u8.byteLength);
        M.HEAPU8.set(u8, dataPtr);
        M._wf_provider_put(this._handle, pathPtr, dataPtr, u8.byteLength);
        M._free(dataPtr);
        // pathPtr deliberately not freed (cstr cleanup story above).
    }
    _setPe1Base(p) {
        this._module._wf_set_pe1_base(this._handle, this._cstr(p));
    }

    // Prefetch the engine-wide assets wf_init reads before any model
    // is loaded: the default IBL probe and the DNC unit MDX. These are
    // OUR engine's prerequisites, not the host page's content.
    //
    // Tries two locations in order, falling through on miss:
    //   1. `engineAssetRoot` (default `./`) — useful for dev / committed
    //      assets sitting next to the viewer page.
    //   2. `/casc/<path>` — the wf_casc_server route, which streams from a
    //      live WC3 install. Same shape Hiveworkshop's CASC delivery uses.
    // Override `engineAssetRoot` (string, with trailing slash) before
    // calling init() — e.g. for a CDN that pins these to a versioned path.
    async _prefetchEngineAssets() {
        const root = this.engineAssetRoot || './';
        const ENGINE = [
            'Environment/EnvironmentMap/Portraits/PortraitDefault_IBL.dds',
            'Environment/DNC/DNCLordaeron/DNCLordaeronUnit/DNCLordaeronUnit.mdx',
            'Environment/DNC/DNCLordaeron/DNCLordaeronUnit/DNCLordaeronUnit.mdl',
        ];
        // These assets ride the legacy FetchContentProvider (read by the
        // renderer's init-time code via ContentProvider::ReadFile rather
        // than the slot-based AssetManager). We fetch them eagerly and
        // push the bytes through _putBytes so the synchronous ReadFile
        // inside the renderer's init path always hits.
        const tryFetch = async (url) => {
            try {
                const r = await fetch(url, { cache: 'no-store' });
                if (!r.ok) return null;
                return new Uint8Array(await r.arrayBuffer());
            } catch (_) { return null; }
        };
        await Promise.all(ENGINE.map(async (p) => {
            let bytes = await tryFetch(root + p);
            if (!bytes) bytes = await tryFetch('/casc/' + p);
            if (bytes) this._putBytes(p, bytes);
        }));
    }

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
