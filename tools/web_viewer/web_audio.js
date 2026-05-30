// JS half of WebAudioSoundEmitter. Binds three EM_JS globals
// (wfWebAudioPlayJS / wfWebAudioSetListenerJS / wfWebAudioSetVolumeJS)
// to an AudioContext + PannerNode graph. AudioContext init is deferred
// to the first user gesture (autoplay policy). Decoded AudioBuffers are
// cached per path; concurrent fetches for the same path coalesce.

export class WebAudioBridge {
    constructor(pathSolver) {
        this.pathSolver = pathSolver;
        this.ctx = null;
        this.masterGain = null;
        this.volume = 1.0;
        this.bufferCache = new Map(); // path → AudioBuffer (or null on permanent fail)
        this.inFlight = new Map();    // path → Promise<AudioBuffer|null>
    }

    setPathSolver(solver) {
        this.pathSolver = solver;
    }

    install() {
        globalThis.wfWebAudioPlayJS = (path, vol, x, y, z, minDist, maxDist, cutoff) =>
            this._play(path, vol, x, y, z, minDist, maxDist, cutoff);
        globalThis.wfWebAudioSetVolumeJS = (v) => this.setVolume(v);
        globalThis.wfWebAudioSetListenerJS = (px, py, pz, fx, fy, fz, ux, uy, uz) =>
            this.setListener(px, py, pz, fx, fy, fz, ux, uy, uz);

        // Capture-phase so we beat preventDefault handlers.
        const resume = () => {
            this._ensureCtx();
            if (this.ctx && this.ctx.state === 'running') {
                document.removeEventListener('pointerdown', resume, true);
                document.removeEventListener('keydown', resume, true);
            }
        };
        document.addEventListener('pointerdown', resume, true);
        document.addEventListener('keydown', resume, true);
    }

    _ensureCtx() {
        if (!this.ctx) {
            const C = window.AudioContext || window.webkitAudioContext;
            if (!C) return null;
            this.ctx = new C();
            this.masterGain = this.ctx.createGain();
            this.masterGain.gain.value = this.volume;
            this.masterGain.connect(this.ctx.destination);
        }
        if (this.ctx.state === 'suspended') this.ctx.resume().catch(() => {});
        return this.ctx;
    }

    // Mirrors desktop CubebSoundEmitter's candidate walk — SLK shapes
    // vary (Sound/ prefix optional, extension optional, trailing variant
    // digit may or may not be CASC-real).
    _candidates(rawPath) {
        const path = rawPath.replaceAll('\\', '/');
        const slash = path.lastIndexOf('/');
        const dot   = path.lastIndexOf('.');
        const dir   = slash >= 0 ? path.slice(0, slash + 1) : '';
        const file  = slash >= 0 ? path.slice(slash + 1)    : path;
        const hasExt = dot > slash;
        const stem   = hasExt ? file.slice(0, file.lastIndexOf('.')) : file;

        const stems = [stem];
        let n = stem.length;
        while (n > 0 && stem[n - 1] >= '0' && stem[n - 1] <= '9') --n;
        if (n > 0 && n < stem.length) stems.push(stem.slice(0, n));

        const base = [];
        const pushBase = (s) => { if (!base.includes(s)) base.push(s); };
        if (hasExt) pushBase(path);
        for (const s of stems) {
            pushBase(dir + s + '.wav');
            pushBase(dir + s + '.flac');
            pushBase(dir + s + '.mp3');
        }

        const hasSoundPrefix = (s) => /^sound[\/\\]/i.test(s);
        const out = [];
        const push = (s) => { if (!out.includes(s)) out.push(s); };
        for (const b of base) {
            push(b);
            if (!hasSoundPrefix(b)) push('Sound/' + b);
        }
        return out;
    }

    async _fetchBuffer(path) {
        if (this.bufferCache.has(path)) return this.bufferCache.get(path);
        if (this.inFlight.has(path)) return this.inFlight.get(path);
        const ctx = this._ensureCtx();
        if (!ctx) return null;
        const p = (async () => {
            try {
                if (!this.pathSolver) return null;
                for (const cand of this._candidates(path)) {
                    const url = await Promise.resolve(this.pathSolver(cand));
                    if (!url) continue;
                    try {
                        const r = await fetch(url, { cache: 'no-store' });
                        if (!r.ok) continue;
                        const arr = await r.arrayBuffer();
                        const buf = await ctx.decodeAudioData(arr);
                        this.bufferCache.set(path, buf);
                        return buf;
                    } catch (_) { /* try next candidate */ }
                }
                // Don't cache null — model-switch may revoke blob URLs
                // mid-flight and the same sound will resolve next play.
                return null;
            } catch (e) {
                console.warn('[wf-audio] decode/fetch failed:', path, e);
                return null;
            } finally {
                this.inFlight.delete(path);
            }
        })();
        this.inFlight.set(path, p);
        return p;
    }

    async _play(path, eventVolume, x, y, z, minDist, maxDist, cutoff) {
        const ctx = this._ensureCtx();
        if (!ctx || !this.masterGain) return;
        // SLK volume is 0..127; convert with the same perceptual curve
        // CubebSoundEmitter uses, with headroom for the master gain.
        const normalized = Math.min(1, Math.max(0, eventVolume / 127));
        const perceptual = Math.pow(normalized, 1.5);
        const finalGain  = perceptual * 0.5;
        // Engine-side hard cutoff — PannerNode has no equivalent.
        if (cutoff > 0) {
            const L = ctx.listener;
            const lx = (L.positionX ? L.positionX.value : 0);
            const ly = (L.positionY ? L.positionY.value : 0);
            const lz = (L.positionZ ? L.positionZ.value : 0);
            const dx = x - lx, dy = y - ly, dz = z - lz;
            if (dx*dx + dy*dy + dz*dz > cutoff * cutoff) return;
        }
        const buf = await this._fetchBuffer(path);
        if (!buf) return;
        const src = ctx.createBufferSource();
        src.buffer = buf;
        const panner = ctx.createPanner();
        panner.panningModel = 'HRTF';
        panner.distanceModel = 'inverse';
        panner.refDistance   = Math.max(1, minDist);
        panner.maxDistance   = Math.max(panner.refDistance + 1, maxDist || 10000);
        panner.rolloffFactor = 1.0;
        if (panner.positionX) {
            panner.positionX.value = x;
            panner.positionY.value = y;
            panner.positionZ.value = z;
        } else {
            // Older Safari API
            panner.setPosition(x, y, z);
        }
        const eventGain = ctx.createGain();
        eventGain.gain.value = finalGain;
        src.connect(panner);
        panner.connect(eventGain);
        eventGain.connect(this.masterGain);
        src.start(0);
    }

    setVolume(v) {
        this.volume = Math.max(0, Math.min(1, v));
        if (this.masterGain) this.masterGain.gain.value = this.volume;
    }

    setListener(px, py, pz, fx, fy, fz, ux, uy, uz) {
        const ctx = this.ctx;
        if (!ctx) return;
        const L = ctx.listener;
        if (L.positionX) {
            L.positionX.value = px;
            L.positionY.value = py;
            L.positionZ.value = pz;
            L.forwardX.value  = fx;
            L.forwardY.value  = fy;
            L.forwardZ.value  = fz;
            L.upX.value       = ux;
            L.upY.value       = uy;
            L.upZ.value       = uz;
        } else {
            L.setPosition(px, py, pz);
            L.setOrientation(fx, fy, fz, ux, uy, uz);
        }
    }
}
