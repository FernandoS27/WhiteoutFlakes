// UI shell that imitates hiveworkshop.com's "View in 3D" panel on top of
// WhiteoutViewer. Owns directory picking, sidebar population, and the
// pathSolver chain (local index → Hive direct asset → /casc-contents/).

import { WhiteoutViewer, TEAM_COLORS, HD_DEBUG_MODES } from './wf-viewer.js';
import { WebAudioBridge } from './web-audio.js';
import { buildOverrideMap, tableModels, isAbsoluteUrl } from './load-table.js';

// `els` carries the DOM nodes HiveApp drives. `canvas` is required;
// every other slot is optional — if a host omits it, the corresponding
// UI feature is simply disabled (no model list, no FPS readout, etc.).
// `options` carries non-DOM behaviour toggles.
export class HiveApp {
    constructor(els = {}, options = {}) {
        if (!els.canvas) throw new Error('HiveApp: els.canvas is required');
        this.canvas       = els.canvas;
        this.modelList    = els.modelList    || null;
        this.animList     = els.animList     || null;
        this.cameraList   = els.cameraList   || null;
        this.teamSwatches = els.teamSwatches || null;
        this.bgSwatches   = els.bgSwatches   || null;
        this.bgPicker     = els.bgPicker     || null;
        this.openDirBtn   = els.openDirBtn   || null;
        this.dirInput     = els.dirInput     || null;
        // Optional `Load JSON…` affordance — triggers loadFromTable() on a
        // user-picked file matching the load-table schema (see load-table.js).
        this.loadJsonBtn  = els.loadJsonBtn  || null;
        this.jsonInput    = els.jsonInput    || null;
        this.camReset     = els.camReset     || null;
        this.closeBtn     = els.closeBtn     || null;
        this.progress     = els.progress     || null;
        this.volSlider    = els.volSlider    || null;
        this.lightingSel  = els.lightingSel  || null;
        this.debugVisSel  = els.debugVisSel  || null;
        this.gridToggle   = els.gridToggle   || null;
        this.dayNightToggle = els.dayNightToggle || null;
        this.todSlider    = els.todSlider    || null;
        this.reforgedToggle = els.reforgedToggle || null;
        this.fpsReadout   = els.fpsReadout   || null;
        this.emptyModels  = this.modelList ? this.modelList.querySelector('.empty') : null;

        // `null` disables service-worker registration entirely (e.g. for
        // hosts embedding HiveApp inside an existing SW-managed page).
        this.serviceWorkerUrl = options.serviceWorkerUrl !== undefined
            ? options.serviceWorkerUrl
            : './sw.js';

        // Last-mile URL rewriter applied to every URL that flows out of
        // _resolve and to the src passed to viewer.load. Hosts use it to
        // bounce cross-origin/auth-gated URLs through a same-origin
        // proxy (e.g. mapping beta2.hiveworkshop.com -> /hive-proxy/).
        // Returning a falsy value leaves the URL untouched.
        this.urlRewriter = typeof options.urlRewriter === 'function'
            ? options.urlRewriter
            : null;

        // HD initialisation:
        //   1. Host option `forceHd: true` wins (programmatic embeds).
        //   2. Otherwise read the page URL's `?forceHD=1` query (or `=true`).
        //      Case-insensitive so a typo-tolerant `forcehd=1` also works.
        // The flag flows into WhiteoutViewer's `hdMode` at construction,
        // which makes `cascUrl` append `&hd=1` on every Hive-CASC fetch
        // and primes the WASM renderer in HD before the first spawn.
        this.forceHd = false;
        if (options.forceHd === true) {
            this.forceHd = true;
        } else if (typeof window !== 'undefined' && window.location && window.location.search) {
            const params = new URLSearchParams(window.location.search);
            const raw = params.get('forceHD') ?? params.get('forcehd');
            if (raw === '1' || (raw && raw.toLowerCase() === 'true')) {
                this.forceHd = true;
            }
        }

        this.viewer = null;
        // Keyed by both full relative path and basename — dependency
        // lookups may use either shape.
        this.index = new Map();
        // Override map sourced from a load-table JSON; null when none is
        // active. Consulted before this.index in _resolve so an active
        // table beats a stale local directory pick.
        this._overrides = null;
        this.models = [];
        this.currentModel = null;
        this.currentInstance = null;
        this.currentTeamColor = 0;
        this._objectUrls = [];
    }

    async start() {
        // Register before init so first-load fetches go through the cache.
        // Don't await: blocking on claim() would just delay viewer init.
        if (this.serviceWorkerUrl && 'serviceWorker' in navigator) {
            // updateViaCache:'none' makes the browser bypass the HTTP cache
            // when fetching sw.js — without it a stale-but-still-valid
            // entry can keep an old worker installed across reloads.
            navigator.serviceWorker
                .register(this.serviceWorkerUrl, { updateViaCache: 'none' })
                .then(reg => reg.update().catch(() => {}))
                .catch((e) => {
                    console.warn('[hive] SW registration failed:', e);
                });
        }
        this.viewer = new WhiteoutViewer(this.canvas, { forceHd: this.forceHd });
        await this.viewer.init();
        this.viewer.setFetchHooks({
            start: () => this._bump(+1),
            end:   () => this._bump(-1),
        });
        // Matches the first chip in the Background list.
        this.viewer.setBackground(0x3e, 0x3e, 0x3e);
        this._setActiveChip(this.bgSwatches, this.bgSwatches.firstElementChild);

        // Persistent solver — covers deps the renderer surfaces lazily
        // (corn-fx textures, child models) after load() returned.
        this.viewer.setPathSolver((name) => this._resolve(name));

        // Routes wfWebAudioPlay* EM_JS calls to the browser's AudioContext.
        // Gated on first user gesture by browser autoplay policy.
        this.audio = new WebAudioBridge((name) => this._resolve(name));
        this.audio.install();

        this._wireTeamSwatches();
        this._wireBgSwatches();
        this._wireUi();
    }

    _wireUi() {
        if (this.openDirBtn) {
            this.openDirBtn.addEventListener('click', () => this._pickDirectory());
        }
        if (this.dirInput) {
            this.dirInput.addEventListener('change',
                (e) => this._adoptFileList(e.target.files));
        }
        if (this.loadJsonBtn) {
            this.loadJsonBtn.addEventListener('click', () => {
                if (this.jsonInput) this.jsonInput.click();
            });
        }
        if (this.jsonInput) {
            this.jsonInput.addEventListener('change', (e) => {
                const f = e.target.files && e.target.files[0];
                if (f) this._adoptJsonFile(f);
                // Reset so re-picking the same file fires `change` again.
                e.target.value = '';
            });
        }
        if (this.camReset) {
            this.camReset.addEventListener('click', (e) => {
                e.preventDefault();
                // -1 = clear preset slot, fall back to orbital free-camera.
                if (this.currentInstance) this.currentInstance.activateCameraPreset(-1);
                else this.viewer.resetCamera();
                this._setActiveChip(this.cameraList, this.camReset);
            });
        }
        if (this.closeBtn) {
            // Symbolic — no parent modal in the standalone page.
            this.closeBtn.addEventListener('click', () => {});
        }
        if (this.volSlider) {
            const apply = () => {
                const v = (Number(this.volSlider.value) || 0) / 100;
                if (this.audio) this.audio.setVolume(v);
            };
            this.volSlider.addEventListener('input', apply);
            apply();
        }
        if (this.lightingSel) {
            const apply = () =>
                this.viewer.setLightingMode(Number(this.lightingSel.value) | 0);
            this.lightingSel.addEventListener('change', apply);
            apply();
        }
        if (this.debugVisSel) {
            // Populate from the HD_DEBUG_MODES list so the labels stay
            // in sync with basic_viewer without hard-coding here.
            this.debugVisSel.innerHTML = '';
            for (const m of HD_DEBUG_MODES) {
                const opt = document.createElement('option');
                opt.value = String(m.value);
                opt.textContent = m.label;
                this.debugVisSel.appendChild(opt);
            }
            const apply = () =>
                this.viewer.setHdDebugMode(Number(this.debugVisSel.value) | 0);
            this.debugVisSel.addEventListener('change', apply);
            apply();
        }
        if (this.gridToggle) {
            const apply = () => this.viewer.setShowGrid(this.gridToggle.checked);
            this.gridToggle.addEventListener('change', apply);
            apply();
        }
        // "Reforged Graphics" — force the HD pipeline regardless of the model's
        // detected SD/HD preference, then reload the current model so its deps
        // re-fetch under the new (HD vs auto) asset overlay.
        if (this.reforgedToggle) {
            this.reforgedToggle.checked = this.forceHd;
            this.reforgedToggle.addEventListener('change', () => {
                this.forceHd = this.reforgedToggle.checked;
                this.viewer.setForceHd(this.forceHd);
                if (this._currentModelM)
                    this._selectModel(this._currentModelM, this._currentModelRow);
            });
        }
        // Day-night cycle: the checkbox runs the 60 s animation; the slider
        // sets time-of-day manually (and seeds the animation's start point).
        if (this.todSlider) {
            this.todSlider.addEventListener('input', () => {
                this.viewer.setTimeOfDay(Number(this.todSlider.value));
            });
            this.viewer.setTimeOfDay(Number(this.todSlider.value));
        }
        if (this.dayNightToggle) {
            const apply = () => {
                const on = this.dayNightToggle.checked;
                this.viewer.setDayNightAnimate(on);
                // Swap the HD IBL probe set to match: day/night sky while
                // animating, portrait studio light otherwise (IblMode 1 vs 0).
                this.viewer.setIblMode(on ? 1 : 0);
            };
            this.dayNightToggle.addEventListener('change', apply);
            apply();
            // While animating, mirror the engine's advancing TOD onto the
            // slider so the handle tracks the cycle.
            if (this.todSlider) {
                setInterval(() => {
                    if (this.dayNightToggle.checked && this.viewer)
                        this.todSlider.value = String(this.viewer.getTimeOfDay());
                }, 200);
            }
        }
        if (this.fpsReadout) {
            // 2 Hz — the dt EMA is already smooth.
            setInterval(() => {
                if (!this.viewer) return;
                const fps = this.viewer.getFps();
                this.fpsReadout.textContent = fps > 0 ? fps.toFixed(0) : '—';
            }, 500);
        }
    }

    // Progress bar — _bump(+1) on dep queued, _bump(-1) on resolved.
    // New deps surfacing mid-load grow the denominator.
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
            // Defer reset until the fade-out finishes, otherwise the
            // bar snaps to 0 before disappearing.
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
    _addInflight(n = 1) { this._bump(+n); }
    _subInflight(n = 1) { this._bump(-n); }

    // Hive's CSS uses `.active`, not `.selected`.
    _setActiveChip(container, target) {
        if (!container) return;
        for (const el of container.querySelectorAll('.active'))
            el.classList.remove('active');
        if (target) target.classList.add('active');
    }

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
            // Firefox/Safari fallback — webkitdirectory FileList.
            if (this.dirInput) this.dirInput.click();
            return;
        }
        this._populateModels();
    }

    _adoptFileList(files) {
        if (!files || files.length === 0) return;
        this.index = this._indexFromFileList(files);
        this._populateModels();
    }

    async _adoptJsonFile(file) {
        let table;
        try {
            const text = await file.text();
            table = JSON.parse(text);
        } catch (e) {
            console.error('[hive] load-table parse failed:', e);
            return;
        }
        await this.loadFromTable(table);
    }

    // Programmatic entry point: install a load-table JSON and repopulate
    // the model list. Overrides take precedence over the local index.
    async loadFromTable(table) {
        if (!table || typeof table !== 'object') {
            console.warn('[hive] loadFromTable: not an object');
            return;
        }
        this._overrides = buildOverrideMap(table);
        this.models = tableModels(table).map(m => ({
            name: m.name, url: m.url
        }));
        this._renderModelList(this.models, '(load-table has no models)');
    }

    // Shared between _populateModels (local directory) and loadFromTable
    // (JSON). Each entry must have `name`; either `path` (local) or `url`
    // (load-table) — _selectModel picks the right field.
    _renderModelList(list, emptyMsg) {
        if (!this.modelList) return;
        for (const a of [...this.modelList.querySelectorAll('a')]) a.remove();
        if (this.emptyModels) { this.emptyModels.remove(); this.emptyModels = null; }
        if (list.length === 0) {
            const span = document.createElement('span');
            span.className = 'empty';
            span.textContent = emptyMsg;
            this.modelList.appendChild(span);
            this.emptyModels = span;
            return;
        }
        for (const m of list) {
            const a = document.createElement('a');
            a.textContent = m.name;
            if (m.path) a.dataset.path = m.path;
            if (m.url)  a.dataset.url  = m.url;
            a.addEventListener('click', () => this._selectModel(m, a));
            this.modelList.appendChild(a);
        }
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
            // Strip the root-folder prefix the input adds.
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
        // A new local-directory pick supersedes any active load-table.
        this._overrides = null;
        this._renderModelList(list, '(no .mdx/.mdl files in directory)');
    }

    async _selectModel(m, row) {
        // Remember the selection so the Reforged-Graphics toggle can reload it
        // under the new render mode / asset overlay.
        this._currentModelM = m;
        this._currentModelRow = row;
        this._setActiveChip(this.modelList, row);

        if (this.currentInstance) {
            this.currentInstance.detach();
            this.currentInstance = null;
        }
        this.currentModel = null;
        this._revokeObjectUrls();
        this._clearAnimations();
        this._clearCameras();
        if (this.viewer) this.viewer.clearSplats();

        this._addInflight();
        const solver = (name) => this._resolve(name);
        try {
            // Load-table rows carry a `url`; local-directory rows carry a
            // relative `path` the solver translates. URLs flow through the
            // host's rewriter (e.g. beta2 -> /hive-proxy/) before the load.
            const src = this._rewrite(m.url || m.path);
            const model = await this.viewer.load(src, solver);
            this.currentModel = model;
            this.currentInstance = model._instances[0];
            this.currentInstance.setTeamColor(this.currentTeamColor);
            // mode 0 = SetIgnoreNonLooping(true) — preview ergonomics.
            this.currentInstance.setSequenceLoopMode(0);
            this._populateSequences();
            this._populateCameras();
        } catch (e) {
            console.error('[hive] load failed:', e);
        } finally {
            this._subInflight();
        }
    }

    // Returns one URL or a fallback chain consumed by the asset pump in
    // order (first 2xx response wins). Priority:
    //   0. Absolute-URL pass-through (e.g. viewer.load(url) re-entered).
    //   1. Load-table override (full path or basename hit) — if it 404s
    //      or errors we fall through to the rest of the chain rather
    //      than failing the asset outright.
    //   2. Local picked-dir blob URL on hit.
    //   3. Direct Hive asset URL (skips the /casc-contents/ 302).
    //   4. /casc-contents/?path= backstop (handles SD/locale/aliases).
    _rewrite(url) {
        if (!this.urlRewriter || typeof url !== 'string') return url;
        const out = this.urlRewriter(url);
        return (typeof out === 'string' && out) ? out : url;
    }

    async _resolve(name) {
        // Pass-through: full URLs, and root-relative URLs (which a host
        // rewriter may have produced, e.g. /hive-proxy/...). Both are
        // resolvable by fetch() without help from the CASC chain.
        if (isAbsoluteUrl(name)) return this._rewrite(name);
        if (typeof name === 'string' && name.startsWith('/')) {
            return this._rewrite(name);
        }
        const norm = String(name).toLowerCase().replaceAll('\\', '/');
        const base = norm.split('/').pop();

        const chain = [];
        if (this._overrides) {
            const hit = this._overrides.get(norm) || this._overrides.get(base);
            if (hit) chain.push(hit);
        }
        const entry = this.index.get(norm) || this.index.get(base);
        if (entry) {
            const file = entry.kind === 'fsa'
                ? await entry.handle.getFile()
                : entry.file;
            const url = URL.createObjectURL(file);
            this._objectUrls.push(url);
            chain.push(url);
        }
        // Hive's model-family expansion can substitute .mdl/.mdx for a
        // .pkfx request; ask for .pkb directly to avoid that.
        let cascPath = norm;
        if (cascPath.endsWith('.pkfx')) {
            cascPath = cascPath.slice(0, -5) + '.pkb';
        }
        // Cornflakes occasionally references particles by bare name with
        // no extension; Hive's CASC won't resolve that without a literal
        // path, so default to .pkb (the canonical particle format).
        const lastSlash = cascPath.lastIndexOf('/');
        const baseName = lastSlash >= 0 ? cascPath.slice(lastSlash + 1) : cascPath;
        if (!baseName.includes('.')) cascPath += '.pkb';
        if (this.viewer.cascDirectAssetBase) {
            // Preserve directory slashes; encodeURIComponent eats them.
            const encoded = cascPath.split('/').map(encodeURIComponent).join('/');
            chain.push(this.viewer.cascDirectAssetBase + encoded);
        }
        chain.push(this.viewer.cascUrl(cascPath));
        // Object URLs (createObjectURL output) don't need rewriting and
        // would be left alone by the host's rewriter anyway, but cycling
        // every entry keeps the contract uniform.
        return chain.map(u => this._rewrite(u));
    }

    _revokeObjectUrls() {
        for (const u of this._objectUrls) {
            try { URL.revokeObjectURL(u); } catch (_) {}
        }
        this._objectUrls.length = 0;
    }

    _clearAnimations() {
        if (!this.animList) return;
        this.animList.innerHTML = '';
    }
    _populateSequences() {
        this._clearAnimations();
        if (!this.currentInstance) return;
        const seqs = this.currentInstance.getSequences();
        // Even without an animList we still want to auto-pick the default
        // sequence so the model animates; we just skip the DOM population.
        if (!this.animList) {
            const rarities = this.currentInstance.getSequenceRarities();
            const preferredIdx = this._pickDefaultSequence(seqs, rarities);
            if (seqs.length > 0) this._selectSequence(preferredIdx, null);
            return;
        }
        if (seqs.length === 0) {
            const span = document.createElement('span');
            span.className = 'empty';
            span.textContent = '(no sequences)';
            this.animList.appendChild(span);
            return;
        }
        const rarities = this.currentInstance.getSequenceRarities();
        const preferredIdx = this._pickDefaultSequence(seqs, rarities);
        let preferredRow = null;
        seqs.forEach((name, idx) => {
            const a = document.createElement('a');
            a.textContent = name;
            a.dataset.seq = String(idx);
            a.addEventListener('click', () => this._selectSequence(idx, a));
            this.animList.appendChild(a);
            if (idx === preferredIdx) preferredRow = a;
        });
        this._selectSequence(preferredIdx, preferredRow);
    }

    // Pick the default idle: any Stand-family sequence with the lowest
    // rarity (MDX SEQS weight; lower = more common). Falls back to the
    // first sequence if the model has no Stand. Matches the in-game pick.
    _pickDefaultSequence(seqs, rarities) {
        let bestIdx = -1;
        let bestRarity = Infinity;
        for (let i = 0; i < seqs.length; ++i) {
            const lower = seqs[i].toLowerCase().trim();
            // "stand", "stand 1", "stand ready 1", "stand work" — anything
            // whose first token is "stand" qualifies as an idle variant.
            if (lower !== 'stand' && !lower.startsWith('stand ')) continue;
            const r = rarities[i] ?? 0;
            if (r < bestRarity) {
                bestRarity = r;
                bestIdx = i;
            }
        }
        return bestIdx >= 0 ? bestIdx : 0;
    }

    _selectSequence(idx, row) {
        this._setActiveChip(this.animList, row);
        if (!this.currentInstance) return;
        // Keep death-trail splats through decay/dissipate cycles.
        const seqs = this.currentInstance.getSequences();
        const name = (seqs[idx] || '').toLowerCase();
        const keep = name.includes('decay') || name.includes('dissipate');
        if (!keep) this.viewer.clearSplats();
        this.currentInstance.setSequence(idx);
    }

    _clearCameras() {
        if (!this.cameraList) return;
        // Keep the static Reset row.
        for (const a of [...this.cameraList.querySelectorAll('a')]) {
            if (a !== this.camReset) a.remove();
        }
    }
    _populateCameras() {
        if (!this.currentInstance || !this.cameraList) return;
        const presets = this.currentInstance.getCameraPresets();
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
        const first = this.teamSwatches.querySelector('.color-chip[data-player-number="0"]');
        if (first) this._setActiveChip(this.teamSwatches, first);
    }

    _wireBgSwatches() {
        if (!this.bgSwatches) return;
        for (const chip of this.bgSwatches.querySelectorAll('.color-chip')) {
            // The <input type=color> picker is wired separately.
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
