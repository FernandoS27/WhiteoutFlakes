// UI shell that imitates hiveworkshop.com's "View in 3D" panel on top of
// WhiteoutViewer. Owns directory picking, sidebar population, and the
// pathSolver chain (local index → Hive direct asset → /casc-contents/).

// Cache-buster defeats the browser's ES module map (separate from the
// HTTP cache) so wf-viewer.js edits take effect on soft reload.
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
        this.lightingSel  = document.getElementById('lighting-select');
        this.gridToggle   = document.getElementById('grid-toggle');
        this.fpsReadout   = document.getElementById('fps-readout');
        this.emptyModels  = this.modelList.querySelector('.empty');

        this.viewer = null;
        // Keyed by both full relative path and basename — dependency
        // lookups may use either shape.
        this.index = new Map();
        this.models = [];
        this.currentModel = null;
        this.currentInstance = null;
        this.currentTeamColor = 0;
        this._objectUrls = [];
    }

    async start() {
        // Register before init so first-load fetches go through the cache.
        // Don't await: blocking on claim() would just delay viewer init.
        if ('serviceWorker' in navigator) {
            navigator.serviceWorker.register('./sw.js').catch((e) => {
                console.warn('[hive] SW registration failed:', e);
            });
        }
        this.viewer = new WhiteoutViewer(this.canvas);
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
        this.openDirBtn.addEventListener('click', () => this._pickDirectory());
        this.dirInput.addEventListener('change',
            (e) => this._adoptFileList(e.target.files));
        this.camReset.addEventListener('click', (e) => {
            e.preventDefault();
            // -1 = clear preset slot, fall back to orbital free-camera.
            if (this.currentInstance) this.currentInstance.activateCameraPreset(-1);
            else this.viewer.resetCamera();
            this._setActiveChip(this.cameraList, this.camReset);
        });
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
        if (this.gridToggle) {
            const apply = () => this.viewer.setShowGrid(this.gridToggle.checked);
            this.gridToggle.addEventListener('change', apply);
            apply();
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
            this.dirInput.click();
            return;
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

    async _selectModel(m, row) {
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
            const model = await this.viewer.load(m.path, solver);
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

    // Returns one URL or a fallback chain:
    //   1. Local picked-dir blob URL on hit.
    //   2. Direct Hive asset URL (skips the /casc-contents/ 302).
    //   3. /casc-contents/?path= backstop (handles SD/locale/aliases).
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
        // Hive's model-family expansion can substitute .mdl/.mdx for a
        // .pkfx request; ask for .pkb directly to avoid that.
        let cascPath = norm;
        if (cascPath.endsWith('.pkfx')) {
            cascPath = cascPath.slice(0, -5) + '.pkb';
        }
        const chain = [];
        if (this.viewer.cascDirectAssetBase) {
            // Preserve directory slashes; encodeURIComponent eats them.
            const encoded = cascPath.split('/').map(encodeURIComponent).join('/');
            chain.push(this.viewer.cascDirectAssetBase + encoded);
        }
        chain.push(this.viewer.cascUrl(cascPath));
        return chain;
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
        // Avoid T-pose on load.
        this._selectSequence(0, firstRow);
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
        if (!this.currentInstance) return;
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
