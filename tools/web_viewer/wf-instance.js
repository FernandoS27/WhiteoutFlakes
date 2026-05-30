// Per-actor / per-model / per-scene host-side classes. mdx-m3-viewer
// shape so host code keeps working unchanged.

import { composeTRS, identityMat } from './wf-math.js';

// WC3 player team-color palette (0-23). Hex lifted from Hive's
// `ratory_wc3model_preview` so the swatch grid matches their viewer.
export const TEAM_COLORS = [
    [0xff, 0x04, 0x02], [0x00, 0x42, 0xff], [0x1b, 0xe6, 0xba], [0x54, 0x00, 0x81],
    [0xff, 0xfc, 0x00], [0xff, 0x8a, 0x0d], [0x20, 0xc0, 0x00], [0xe4, 0x5b, 0xb0],
    [0x94, 0x96, 0x97], [0x7e, 0xbf, 0xf1], [0x10, 0x62, 0x47], [0x4f, 0x2a, 0x05],
    [0x9c, 0x00, 0x00], [0x00, 0x00, 0xc3], [0x00, 0xeb, 0xff], [0xbd, 0x00, 0xff],
    [0xec, 0xcd, 0x86], [0xf7, 0xa4, 0x8b], [0xc0, 0xff, 0x80], [0xdc, 0xb9, 0xec],
    [0x4f, 0x4f, 0x55], [0xec, 0xf0, 0xff], [0x00, 0x78, 0x1e], [0xa4, 0x6f, 0x33],
];
export const TEAM_COLOR_NAMES = [
    'Red',     'Blue',       'Teal',      'Purple',
    'Yellow',  'Orange',     'Green',     'Pink',
    'Gray',    'Light blue', 'Dark green','Brown',
    'Maroon',  'Navy',       'Turquoise', 'Violet',
    'Wheat',   'Peach',      'Mint',      'Lavender',
    'Coal',    'Snow',       'Emerald',   'Peanut',
];

// Scratch matrix shared across instances — TRS push is synchronous and
// single-threaded so the scratch can't race.
const _mat = new Float32Array(16);

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

    // hide() pushes a zero-scale matrix so the actor stays alive but
    // contributes nothing; detach() destroys it.
    _pushTransform() {
        if (!this._handle) return;
        if (this._visible) {
            composeTRS(_mat, this._location, this._rotation, this._scale);
        } else {
            identityMat(_mat);
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

    // `-1` drops back to orbital free-camera.
    activateCameraPreset(idx) {
        if (this._handle) this._M._wf_camera_activate_preset(this._vh, this._handle, idx | 0);
        return this;
    }
}

// Loaded MDX asset. The renderer caches the template after the first
// successful SpawnUnit; addInstance() cheaply re-spawns from it.
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
        const keyPtr = this._viewer._cstr(this._mdxKey);
        let handle = 0;
        try {
            handle = M._wf_spawn_unit(this._viewer._handle, keyPtr);
        } finally {
            M._free(keyPtr);
        }
        if (!handle) throw new Error('addInstance: wf_spawn_unit returned 0');
        const inst = new Instance(this._viewer, this, handle);
        this._instances.push(inst);
        return inst;
    }

    whenLoaded() { return this._loadPromise; }
}

// Housekeeping bucket for API parity — renderer has only one real scene.
export class Scene {
    constructor(viewer) {
        this._viewer = viewer;
        this._instances = [];
    }

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
        // Cheaper than wf_clear_all when the scene owns only a subset.
        for (const inst of this._instances) {
            if (inst._handle) this._viewer._module._wf_actor_destroy(this._viewer._handle, inst._handle);
            inst._handle = 0;
        }
        this._instances.length = 0;
    }
}
