// hive_app.js — UI shell that imitates hiveworkshop.com's "View in 3D" panel
// on top of WhiteoutViewer. The page owns: directory picking, model
// enumeration, sidebar populating (Models / Team colors / Background /
// Animations / Cameras), and the pathSolver chain (local picked-dir
// index → CASC server fallback).
//
// JS API summary:
//   const app = new HiveApp();
//   await app.start();   // boots WhiteoutViewer + wires UI
// Subsequent UI interactions are event-driven.
//
// DOM contract — see tools/web_viewer/index.html. Mirrors Hive's
// `ratory_wc3model_preview` template (block / block-container /
// block-header / block-body + `color-chip` swatches). Team-color and
// background chips are PRE-RENDERED in the HTML with their `title`
// tooltips and `data-player-number` / `data-bg` attributes so the
// markup is greppable; this file just wires click handlers on top.

// Top-level dynamic import with a per-load cache-buster — defeats the
// browser's ES module map (keyed by URL) so wf-viewer.js edits actually
// take effect on a soft reload. The HTTP-layer `Cache-Control: no-store`
// stops the network layer from caching, but the module map is a separate
// in-page cache that survives identical URLs across this same document.
const { WhiteoutViewer, TEAM_COLORS } =
    await import('./wf-viewer.js?t=' + Date.now());
const { WebAudioBridge } =
    await import('./web_audio.js?t=' + Date.now());

export class HiveApp {
    constructor() {
        this.canvas       = document.getElementById('wf-canvas');
        this.modelList    = document.getElementById('model-list');
        this.animList     = document.getElementById('anim-list');
        this.cameraList   = document.getElementById('camera-list');
        this.teamSwatches = document.getElementById('team-swatches');
        this.bgSwatches   = document.getElementById('bg-swatches');
        this.bgPicker     = document.getElementById('bg-picker');
        this.openDirBtn   = document.getElementById('open-dir');
        this.dirInput     = document.getElementById('dir-input');
        this.camReset     = document.getElementById('cam-reset');
        this.closeBtn     = document.getElementById('close-btn');
        this.progress     = document.getElementById('progress');
        this.volSlider    = document.getElementById('vol-slider');
        this.emptyModels  = this.modelList.querySelector('.empty');

        this.viewer = null;
        // Map<normalized-path, {kind:'fsa'|'legacy', handle?, file?, path}>.
        // Keyed by full relative path AND by basename so dependency lookups
        // can match either shape (FileResolver does the same on desktop).
        this.index = new Map();
        this.models = []; // [{ name, path, entry }]
        this.currentModel = null;
        this.currentInstance = null;
        this.currentTeamColor = 0;
        // Track blob URLs minted by the pathSolver so we can revoke them
        // when a fresh model is loaded (otherwise they leak until reload).
        this._objectUrls = [];
    }

    async start() {
        this.viewer = new WhiteoutViewer(this.canvas);
        await this.viewer.init();
        // Drive the progress bar from real fetch counts. Every logical
        // dep _fetchDep handles bumps the counter once on start and
        // once on finish, so the bar fills as completed/total — same
        // accounting whether the dep came from the initial spawn, the
        // background texture stream, or the runtime miss-watcher.
        this.viewer.setFetchHooks({
            start: () => this._bump(+1),
            end:   () => this._bump(-1),
        });
        // Default background matches the first chip in Background (dark
        // gray #3e3e3e on Hive). Keeps the canvas tone consistent with
        // the sidebar before the user picks a swatch.
        this.viewer.setBackground(0x3e, 0x3e, 0x3e);
        this._setActiveChip(this.bgSwatches, this.bgSwatches.firstElementChild);

        // Install a persistent path solver so the viewer's rAF loop can
        // fulfil lazily-discovered deps (corn_fx particle textures spawn
        // when an emitter first fires, often well after load() returned).
        // Same chain as the load-time solver: local dir first, CASC fallback.
        this.viewer.setPathSolver((name) => this._resolve(name));

        // Web Audio bridge — listens for the C++ WebAudioSoundEmitter's
        // EM_JS calls (`wfWebAudioPlayJS` etc.) and routes them to the
        // browser's AudioContext. Uses the same pathSolver chain for
        // sound asset fetch. Autoplay-policy gated: AudioContext is
        // created on first pointerdown/keydown.
        this.audio = new WebAudioBridge((name) => this._resolve(name));
        this.audio.install();

        this._wireTeamSwatches();
        this._wireBgSwatches();
        this._wireUi();
    }

    _wireUi() {
        this.openDirBtn.addEventListener('click', () => this._pickDirectory());
        this.dirInput.addEventListener('change',
            (e) => this._adoptFileList(e.target.files));
        this.camReset.addEventListener('click', (e) => {
            e.preventDefault();
            // Reset row = "drop back to orbital free-camera with default
            // FoV / clip" (same as desktop's ActivateCameraPreset(-1)).
            // If we have a current instance, route through it so the
            // facade knows which actor's preset slot to clear.
            if (this.currentInstance) this.currentInstance.activateCameraPreset(-1);
            else this.viewer.resetCamera();
            this._setActiveChip(this.cameraList, this.camReset);
        });
        if (this.closeBtn) {
            // Symbolic — there's no parent modal to close in the standalone
            // page. Kept for visual parity with Hive's panel.
            this.closeBtn.addEventListener('click', () => {});
        }
        // Volume slider — maps 0..100 → 0..1 and pushes to the Web Audio
        // master gain via the bridge. Default value 50 ≈ comfortable preview
        // level; updates live as the user drags.
        if (this.volSlider) {
            const apply = () => {
                const v = (Number(this.volSlider.value) || 0) / 100;
                if (this.audio) this.audio.setVolume(v);
            };
            this.volSlider.addEventListener('input', apply);
            apply(); // seed master with the slider's default value
        }
    }

    // ------------------------------------------------------------------
    // Loading progress bar — determinate fill driven by per-fetch hooks.
    // _bump(+1) is called once per logical dep when it starts, _bump(-1)
    // once when it finishes (success OR failure). We track cumulative
    // started/completed counts so the bar advances at completed/started.
    // When in-flight returns to zero the bar fades out and the counters
    // reset, so the next load starts at 0%. The bar gracefully handles
    // "new work surfaces mid-load" (corn-fx textures, miss-watcher
    // discoveries): the denominator grows, the percentage backs off
    // slightly, then catches back up as the new deps complete.
    // ------------------------------------------------------------------
    _bump(delta) {
        if (delta > 0) {
            this._started   = (this._started   || 0) + delta;
            this._inflight  = (this._inflight  || 0) + delta;
        } else {
            const sub = -delta;
            this._completed = (this._completed || 0) + sub;
            this._inflight  = Math.max(0, (this._inflight || 0) - sub);
        }
        this._updateProgress();
    }
    _updateProgress() {
        if (!this.progress) return;
        if (this._inflight > 0) {
            this.progress.classList.add('active');
            this.progress.classList.add('determinate');
            const total = this._started || 1;
            const pct = Math.max(0, Math.min(100, (this._completed / total) * 100));
            this.progress.style.setProperty('--pct', pct.toFixed(2) + '%');
        } else {
            this.progress.classList.remove('active');
            // Defer reset until after the fade-out finishes so the bar
            // doesn't visibly snap back to 0 before disappearing.
            setTimeout(() => {
                if (this._inflight === 0) {
                    this.progress.classList.remove('determinate');
                    this.progress.style.removeProperty('--pct');
                    this._started = 0;
                    this._completed = 0;
                }
            }, 220);
        }
    }
    // Back-compat shorthand for the few call sites in _selectModel that
    // bracket non-fetch work (the MDX/spawn phase) with progress.
    _addInflight(n = 1) { this._bump(+n); }
    _subInflight(n = 1) { this._bump(-n); }

    // ------------------------------------------------------------------
    // Active-chip / active-row helper. Hive's CSS keys the highlighted
    // state off the `.active` class on the chip / list item (not
    // `.selected`); we follow the same convention so the stylesheet can
    // share rules.
    // ------------------------------------------------------------------
    _setActiveChip(container, target) {
        if (!container) return;
        for (const el of container.querySelectorAll('.active'))
            el.classList.remove('active');
        if (target) target.classList.add('active');
    }

    // ------------------------------------------------------------------
    // Directory picking + indexing.
    // ------------------------------------------------------------------
    async _pickDirectory() {
        if (typeof window.showDirectoryPicker === 'function') {
            try {
                const dir = await window.showDirectoryPicker({ id: 'wf-models' });
                this.index = await this._indexFromDirHandle(dir);
            } catch (e) {
                if (e && e.name !== 'AbortError') console.error(e);
                return;
            }
        } else {
            // Firefox/Safari fallback — synthesise a click on the hidden
            // <input webkitdirectory>, which gives us a FileList where each
            // File has .webkitRelativePath set.
            this.dirInput.click();
            return; // _adoptFileList runs on the change event
        }
        this._populateModels();
    }

    _adoptFileList(files) {
        if (!files || files.length === 0) return;
        this.index = this._indexFromFileList(files);
        this._populateModels();
    }

    async _indexFromDirHandle(dirHandle) {
        const out = new Map();
        async function* walk(dir, prefix) {
            for await (const entry of dir.values()) {
                const path = prefix + entry.name;
                if (entry.kind === 'file') yield { path, handle: entry };
                else                       yield* walk(entry, path + '/');
            }
        }
        for await (const { path, handle } of walk(dirHandle, '')) {
            const norm = path.toLowerCase();
            const base = path.split('/').pop().toLowerCase();
            const rec  = { kind: 'fsa', handle, path };
            out.set(norm, rec);
            if (!out.has(base)) out.set(base, rec);
        }
        return out;
    }

    _indexFromFileList(files) {
        const out = new Map();
        for (const f of files) {
            const rel = (f.webkitRelativePath || f.name).replaceAll('\\', '/');
            // Strip the root-folder prefix the input always adds.
            const stripped = rel.includes('/') ? rel.substring(rel.indexOf('/') + 1) : rel;
            const norm = stripped.toLowerCase();
            const base = stripped.split('/').pop().toLowerCase();
            const rec  = { kind: 'legacy', file: f, path: stripped };
            out.set(norm, rec);
            if (!out.has(base)) out.set(base, rec);
        }
        return out;
    }

    _populateModels() {
        const seen = new Set();
        const list = [];
        for (const entry of this.index.values()) {
            if (seen.has(entry.path)) continue;
            const lp = entry.path.toLowerCase();
            if (!(lp.endsWith('.mdx') || lp.endsWith('.mdl'))) continue;
            seen.add(entry.path);
            const name = entry.path.split('/').pop().replace(/\.(mdx|mdl)$/i, '');
            list.push({ name, path: entry.path, entry });
        }
        list.sort((a, b) => a.name.localeCompare(b.name));
        this.models = list;

        // Wipe previously-rendered model rows (everything except the
        // pinned button / hidden input / empty placeholder, all of which
        // we recreate or hide below).
        for (const a of [...this.modelList.querySelectorAll('a')]) a.remove();
        if (this.emptyModels) this.emptyModels.remove();

        if (list.length === 0) {
            const span = document.createElement('span');
            span.className = 'empty';
            span.textContent = '(no .mdx/.mdl files in directory)';
            this.modelList.appendChild(span);
            this.emptyModels = span;
            return;
        }
        for (const m of list) {
            const a = document.createElement('a');
            a.textContent = m.name;
            a.dataset.path = m.path;
            a.addEventListener('click', () => this._selectModel(m, a));
            this.modelList.appendChild(a);
        }
    }

    // ------------------------------------------------------------------
    // Model selection / loading.
    // ------------------------------------------------------------------
    async _selectModel(m, row) {
        this._setActiveChip(this.modelList, row);

        // Tear down any previous instance/model before loading the next.
        if (this.currentInstance) {
            this.currentInstance.detach();
            this.currentInstance = null;
        }
        this.currentModel = null;
        this._revokeObjectUrls();
        this._clearAnimations();
        this._clearCameras();

        this._addInflight();
        const solver = (name) => this._resolve(name);
        try {
            const model = await this.viewer.load(m.path, solver);
            this.currentModel = model;
            this.currentInstance = model._instances[0];
            this.currentInstance.setTeamColor(this.currentTeamColor);
            // Force every animation (including ones the MDX marked
            // non-looping) to loop on the main actor — matches mdx-m3-
            // viewer's default "always loop" behaviour for previews.
            // mode 0 → `SetIgnoreNonLooping(true)` in wf_actor_set_loop_mode,
            // which tells the renderer to wrap-around instead of clamping
            // at the last frame for sequences with the nonLooping flag.
            this.currentInstance.setSequenceLoopMode(0);
            this._populateSequences();
            this._populateCameras();
            // Keep the progress bar up until the background texture
            // stream completes too. If it's already settled this
            // resolves immediately.
            if (model._texStream) {
                this._addInflight();
                model._texStream.finally(() => this._subInflight());
            }
        } catch (e) {
            console.error('[hive] load failed:', e);
        } finally {
            this._subInflight();
        }
    }

    // PathSolver — fed to viewer.load. Returns a URL for the asset:
    //   1. Local picked-directory index (full path then basename).
    //   2. CASC server (`/casc/<path>`) — set up by tools/web_viewer/
    //      casc_server/wf_casc_server.exe.
    // The C++ FetchContentProvider walks extension synonyms (.tif↔.dds↔.blp)
    // and calls this solver once per candidate, so we don't need to do
    // synonym chasing here — only one path at a time.
    async _resolve(name) {
        const norm = String(name).toLowerCase().replaceAll('\\', '/');
        const base = norm.split('/').pop();
        const entry = this.index.get(norm) || this.index.get(base);
        if (entry) {
            const file = entry.kind === 'fsa'
                ? await entry.handle.getFile()
                : entry.file;
            const url = URL.createObjectURL(file);
            this._objectUrls.push(url);
            return url;
        }
        return '/casc/' + norm;
    }

    _revokeObjectUrls() {
        for (const u of this._objectUrls) {
            try { URL.revokeObjectURL(u); } catch (_) {}
        }
        this._objectUrls.length = 0;
    }

    // ------------------------------------------------------------------
    // Sequences (Animations panel) — rebuilt on every model load.
    // ------------------------------------------------------------------
    _clearAnimations() {
        if (!this.animList) return;
        this.animList.innerHTML = '';
    }
    _populateSequences() {
        this._clearAnimations();
        if (!this.currentInstance) return;
        const seqs = this.currentInstance.getSequences();
        if (seqs.length === 0) {
            const span = document.createElement('span');
            span.className = 'empty';
            span.textContent = '(no sequences)';
            this.animList.appendChild(span);
            return;
        }
        let firstRow = null;
        seqs.forEach((name, idx) => {
            const a = document.createElement('a');
            a.textContent = name;
            a.dataset.seq = String(idx);
            a.addEventListener('click', () => this._selectSequence(idx, a));
            this.animList.appendChild(a);
            if (idx === 0) firstRow = a;
        });
        // Auto-play the first sequence so models don't render in T-pose.
        this._selectSequence(0, firstRow);
    }

    _selectSequence(idx, row) {
        this._setActiveChip(this.animList, row);
        if (!this.currentInstance) return;
        // Clear lingering splats before switching, the same way the
        // desktop viewer does (tools/basic_viewer/viewer_ui.cpp:482) —
        // skip the wipe for fade-out sequences ("decay" / "dissipate")
        // so death-trail blood pools persist through their decay cycle.
        const seqs = this.currentInstance.getSequences();
        const name = (seqs[idx] || '').toLowerCase();
        const keep = name.includes('decay') || name.includes('dissipate');
        if (!keep) this.viewer.clearSplats();
        this.currentInstance.setSequence(idx);
    }

    // ------------------------------------------------------------------
    // Cameras panel. The Reset row is static (rendered in index.html);
    // any per-model camera presets the renderer exposes get appended.
    // ------------------------------------------------------------------
    _clearCameras() {
        if (!this.cameraList) return;
        // Keep the static Reset row, drop everything after it.
        for (const a of [...this.cameraList.querySelectorAll('a')]) {
            if (a !== this.camReset) a.remove();
        }
    }
    _populateCameras() {
        if (!this.currentInstance) return;
        const presets = this.currentInstance.getCameraPresets();
        // Default-active highlight goes on the Reset row when a fresh
        // model loads (matches Hive's first-state of the panel).
        this._setActiveChip(this.cameraList, this.camReset);
        presets.forEach((name, idx) => {
            const a = document.createElement('a');
            a.textContent = name;
            a.dataset.camera = String(idx);
            a.addEventListener('click', (e) => {
                e.preventDefault();
                this.currentInstance.activateCameraPreset(idx);
                this._setActiveChip(this.cameraList, a);
            });
            this.cameraList.appendChild(a);
        });
    }

    // ------------------------------------------------------------------
    // Team-color chips. Pre-rendered in index.html with
    // `data-player-number` — we just attach click handlers that update
    // the active outline + push the colour to every live instance.
    // ------------------------------------------------------------------
    _wireTeamSwatches() {
        if (!this.teamSwatches) return;
        for (const chip of this.teamSwatches.querySelectorAll('.color-chip')) {
            const player = Number(chip.dataset.playerNumber);
            if (Number.isNaN(player)) continue;
            chip.addEventListener('click', (e) => {
                e.preventDefault();
                this.currentTeamColor = player;
                if (this.currentModel) {
                    for (const inst of this.currentModel._instances)
                        inst.setTeamColor(player);
                }
                this._setActiveChip(this.teamSwatches, chip);
            });
        }
        // Reflect the default team color (Red, index 0) as the initial
        // active chip — matches Hive's first-chip-highlighted state.
        const first = this.teamSwatches.querySelector('.color-chip[data-player-number="0"]');
        if (first) this._setActiveChip(this.teamSwatches, first);
    }

    // ------------------------------------------------------------------
    // Background chips. Same pre-rendered pattern as team colors, plus
    // the `<input type="color">` custom-color picker (Hive's
    // `color-chip--picker`). Picker uses the `change`/`input` event;
    // chips use click.
    // ------------------------------------------------------------------
    _wireBgSwatches() {
        if (!this.bgSwatches) return;
        for (const chip of this.bgSwatches.querySelectorAll('.color-chip')) {
            // The custom-color picker is wired separately below — skip it
            // here so we don't attach a click handler that would intercept
            // the colour-picker pop-up.
            if (chip.tagName === 'INPUT') continue;
            chip.addEventListener('click', (e) => {
                e.preventDefault();
                const hex = chip.dataset.bg;
                if (!hex) return;
                this._applyHexBg(hex);
                this._setActiveChip(this.bgSwatches, chip);
            });
        }
        if (this.bgPicker) {
            const apply = () => {
                this._applyHexBg(this.bgPicker.value);
                this._setActiveChip(this.bgSwatches, this.bgPicker);
            };
            this.bgPicker.addEventListener('input', apply);
            this.bgPicker.addEventListener('change', apply);
        }
    }

    _applyHexBg(hex) {
        // hex is "#rrggbb" or "#rgb" — normalise to 8-bit components.
        let h = String(hex).trim();
        if (h.startsWith('#')) h = h.slice(1);
        if (h.length === 3) h = h.split('').map(c => c + c).join('');
        if (h.length !== 6) return;
        const r = parseInt(h.slice(0, 2), 16);
        const g = parseInt(h.slice(2, 4), 16);
        const b = parseInt(h.slice(4, 6), 16);
        this.viewer.setBackground(r, g, b);
    }
}
