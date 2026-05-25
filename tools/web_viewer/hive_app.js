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

// Top-level dynamic import with a per-load cache-buster — defeats the
// browser's ES module map (keyed by URL) so wf-viewer.js edits actually
// take effect on a soft reload. The HTTP-layer `Cache-Control: no-store`
// stops the network layer from caching, but the module map is a separate
// in-page cache that survives identical URLs across this same document.
const { WhiteoutViewer, TEAM_COLORS } =
    await import('./wf-viewer.js?t=' + Date.now());
const { WebAudioBridge } =
    await import('./web_audio.js?t=' + Date.now());

// Background-swatch palette (matches Hive's tile set). Each entry is the
// sRGB tuple fed straight to wf_set_background.
const BG_COLORS = [
    [128, 128, 128], // gray
    [  0,   0,   0], // black
    [255, 255, 255], // white
    [ 64, 160, 200], // teal
    [ 40, 160,  60], // green
    [180,  40, 140], // magenta
    [ 80,  80,  80], // dark gray
];

export class HiveApp {
    constructor() {
        this.canvas       = document.getElementById('wf-canvas');
        this.modelList    = document.getElementById('model-list');
        this.animList     = document.getElementById('anim-list');
        this.teamSwatches = document.getElementById('team-swatches');
        this.bgSwatches   = document.getElementById('bg-swatches');
        this.openDirBtn   = document.getElementById('open-dir');
        this.dirInput     = document.getElementById('dir-input');
        this.camReset     = document.getElementById('cam-reset');
        this.closeBtn     = document.getElementById('close-btn');

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
        // Default to a near-black "Hive" background so the canvas matches
        // the dark UI before the user picks a swatch.
        this.viewer.setBackground(10, 10, 10);

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

        this._buildTeamSwatches();
        this._buildBgSwatches();
        this._wireUi();
    }

    _wireUi() {
        this.openDirBtn.addEventListener('click', () => this._pickDirectory());
        this.dirInput.addEventListener('change',
            (e) => this._adoptFileList(e.target.files));
        this.camReset.addEventListener('click', () => this.viewer.resetCamera());
        if (this.closeBtn) {
            // Symbolic — there's no parent modal to close in the standalone
            // page. Kept for visual parity with Hive's panel.
            this.closeBtn.addEventListener('click', () => {});
        }
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

        this.modelList.innerHTML = '';
        if (list.length === 0) {
            const li = document.createElement('li');
            li.className = 'empty';
            li.textContent = '(no .mdx/.mdl files in directory)';
            this.modelList.appendChild(li);
            return;
        }
        for (const m of list) {
            const li = document.createElement('li');
            li.textContent = m.name;
            li.addEventListener('click', () => this._selectModel(m, li));
            this.modelList.appendChild(li);
        }
    }

    // ------------------------------------------------------------------
    // Model selection / loading.
    // ------------------------------------------------------------------
    async _selectModel(m, li) {
        for (const x of this.modelList.querySelectorAll('.selected'))
            x.classList.remove('selected');
        li.classList.add('selected');

        // Tear down any previous instance/model before loading the next.
        if (this.currentInstance) {
            this.currentInstance.detach();
            this.currentInstance = null;
        }
        this.currentModel = null;
        this._revokeObjectUrls();
        this.animList.innerHTML = '';

        const solver = (name) => this._resolve(name);
        try {
            const model = await this.viewer.load(m.path, solver);
            this.currentModel = model;
            this.currentInstance = model._instances[0];
            this.currentInstance.setTeamColor(this.currentTeamColor);
            this._populateSequences();
        } catch (e) {
            console.error('[hive] load failed:', e);
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
    // Sequences (Animations panel).
    // ------------------------------------------------------------------
    _populateSequences() {
        this.animList.innerHTML = '';
        if (!this.currentInstance) return;
        const seqs = this.currentInstance.getSequences();
        if (seqs.length === 0) {
            const li = document.createElement('li');
            li.className = 'empty';
            li.textContent = '(no sequences)';
            this.animList.appendChild(li);
            return;
        }
        seqs.forEach((name, idx) => {
            const li = document.createElement('li');
            li.textContent = name;
            li.addEventListener('click', () => this._selectSequence(idx, li));
            this.animList.appendChild(li);
        });
        // Auto-play the first sequence so models don't render in T-pose.
        this._selectSequence(0, this.animList.firstChild);
    }

    _selectSequence(idx, li) {
        for (const x of this.animList.querySelectorAll('.selected'))
            x.classList.remove('selected');
        if (li) li.classList.add('selected');
        if (this.currentInstance) this.currentInstance.setSequence(idx);
    }

    // ------------------------------------------------------------------
    // Static swatches.
    // ------------------------------------------------------------------
    _buildTeamSwatches() {
        this.teamSwatches.innerHTML = '';
        for (let i = 0; i < TEAM_COLORS.length; ++i) {
            const [r, g, b] = TEAM_COLORS[i];
            const sw = document.createElement('div');
            sw.className = 'swatch';
            sw.style.background = `rgb(${r},${g},${b})`;
            sw.title = `Team ${i}`;
            sw.addEventListener('click', () => {
                this.currentTeamColor = i;
                // Apply to every live instance of the current model.
                if (this.currentModel) {
                    for (const inst of this.currentModel._instances) inst.setTeamColor(i);
                }
            });
            this.teamSwatches.appendChild(sw);
        }
    }

    _buildBgSwatches() {
        this.bgSwatches.innerHTML = '';
        for (const [r, g, b] of BG_COLORS) {
            const sw = document.createElement('div');
            sw.className = 'swatch';
            sw.style.background = `rgb(${r},${g},${b})`;
            sw.addEventListener('click', () => this.viewer.setBackground(r, g, b));
            this.bgSwatches.appendChild(sw);
        }
    }
}
