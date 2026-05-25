// web_audio.js — JS side of the WebAudioSoundEmitter bridge.
//
// The C++ WebAudioSoundEmitter (tools/web_viewer/web_audio_emitter.cpp)
// calls three globals via EM_JS:
//
//   wfWebAudioPlayJS(path, volume, x, y, z, minDist, maxDist, cutoff)
//   wfWebAudioSetListenerJS(px, py, pz, fx, fy, fz, ux, uy, uz)
//   wfWebAudioSetVolumeJS(volume)
//
// This module's `WebAudioBridge.install()` binds those globals to its
// AudioContext + PannerNode graph. AudioContext can't auto-create on page
// load — autoplay policy requires a user gesture — so we defer ctx
// creation to the first click/keydown on the page.
//
// Asset fetch goes through the same pathSolver the host passes (HiveApp
// reuses its `_resolve` so local picked-directory hits first, CASC server
// falls back). Decoded AudioBuffers are cached per path; concurrent
// fetches for the same path coalesce.

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

        // Autoplay policy: AudioContext must be created (or resumed) under
        // a user gesture. Bind a resume hook to common interactions and
        // self-detach once the ctx is running. Capture phase + non-passive
        // so we definitely run before any handler that might preventDefault.
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

    // Build the candidate list for a SND filePath the same way
    // CubebSoundEmitter does on desktop (tools/common/cubeb_sound_emitter.cpp).
    // The SLK rows give us inconsistent shapes — some have `Sound/...`,
    // some don't; some end in `.flac`, some have no extension at all;
    // some include a trailing digit ("FootmanReady1") that's actually a
    // CASC label stem ("FootmanReady") with a numbered variant suffix.
    // Try every reasonable combination so audio resolves regardless.
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
                // Every candidate failed — pin the null so the next SND
                // event for this path doesn't re-walk all the URLs.
                this.bufferCache.set(path, null);
                return null;
            } catch (e) {
                console.warn('[wf-audio] decode/fetch failed:', path, e);
                this.bufferCache.set(path, null);
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
        // SLK Volume is a 0..127 MIDI-style scale (WC3 + StarCraft
        // convention). Web Audio's GainNode.gain is linear amplitude, so
        // a raw eventVolume of 63 becomes 63× amplification and clips
        // every speaker on the planet. Normalise to 0..1 with a
        // perceptual curve so the comfortable level sits mid-travel
        // (matches CubebSoundEmitter::kVolumeCurveExp on desktop).
        const normalized = Math.min(1, Math.max(0, eventVolume / 127));
        const perceptual = Math.pow(normalized, 1.5);
        const finalGain  = perceptual * 0.5; // headroom — leaves master room to bring up
        // Distance cutoff — engine semantics say "beyond this radius the
        // sound isn't played at all". WebAudio's PannerNode doesn't have
        // a hard cutoff, so we skip the spawn entirely if the listener is
        // outside it. Default 0 means "no cutoff" (always play).
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
