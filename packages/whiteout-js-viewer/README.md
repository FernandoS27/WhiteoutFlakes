# whiteout-js-viewer

A WebGPU viewer for Blizzard game models, in the browser. The heavy lifting —
parsing, skinning, materials, particles, the whole render pipeline — is the
[WhiteoutFlakes](https://github.com/FernandoS27/WhiteoutFlakes) C++ engine
compiled to WebAssembly (`wf-core.wasm`). This package is the JavaScript around
it: a viewer API shaped like mdx-m3-viewer's, plus an optional drop-in UI shell.

## What it opens

| Format | Game |
| --- | --- |
| `.mdx` / `.mdl` | Warcraft III — classic and Reforged, HD detected per model |
| `.m3` | StarCraft II / Heroes of the Storm |
| `.pkb` | Warcraft III corn effects — standalone, with no model around them |

Assets stream in as the renderer discovers them. You hand the viewer a
`pathSolver`, and it calls back for every texture, child model, sound and effect
a load turns out to need.

## Requirements

WebGPU — Chromium 113+, or a recent Firefox or Safari — **in a secure context**.
`https://` and `localhost` qualify; a plain-HTTP LAN address does not, and there
`navigator.gpu` is undefined and the viewer will not start.

## Install

```
npm install whiteout-js-viewer
```

ESM only. The entry point top-level-`await`s the wasm module, so import it from
a `<script type="module">` or a bundler that supports top-level await.
`wf-core.wasm` and `shaders/webgpu/*.bls` are fetched at run time relative to the
module's own URL, so a bundler must emit them as assets rather than inline them.

## The UI shell

`HiveApp` wires a viewer to DOM elements you provide and drives the whole panel:
model list, animation list, camera presets, team colours, background, load
progress. Everything but the canvas is optional — omit an element and that
feature simply turns off.

```js
import { HiveApp } from 'whiteout-js-viewer';

const app = new HiveApp({
  canvas:       document.getElementById('wf-canvas'),
  modelList:    document.getElementById('model-list'),
  animList:     document.getElementById('anim-list'),
  openModelBtn: document.getElementById('open-model'),   // opens files directly
  modelInput:   document.getElementById('model-input'),  // <input type=file multiple>
});
await app.start();

await app.loadFromTable({ models: [
  { name: 'Footman', url: 'units/human/footman/footman.mdx' },
  { name: 'Marine',  url: 'assets/units/terran/marine/marine.m3' },
]});
```

Paths that no local pick resolves fall through to Hiveworkshop's public CASC
mirror, which carries StarCraft II content as well as Warcraft III — which is
why both rows above load, textures and all, with nothing on disk.

Other element slots: `cameraList`, `teamSwatches`, `bgSwatches`, `bgPicker`,
`openDirBtn` + `dirInput`, `loadJsonBtn` + `jsonInput`, `camReset`, `zoomInBtn`,
`zoomOutBtn`, `progress`, `volSlider`, `lightingSel`, `debugVisSel`,
`gridToggle`, `dayNightToggle`, `todSlider`, `reforgedToggle`, `fpsReadout`.
Options: `forceHd`, `serviceWorkerUrl`, `urlRewriter`.

## The raw API

```js
import { WhiteoutViewer } from 'whiteout-js-viewer';

const viewer = new WhiteoutViewer(document.querySelector('canvas'));
await viewer.init();
viewer.setPathSolver(name => `/assets/${name}`);

const model = await viewer.load('units/human/footman/footman.mdx');
model.addInstance().setSequence('Stand - 1').setTeamColor(0);
```

`load()` resolves once the model has spawned; its textures keep arriving after
that, swapping in as they land. A `pathSolver` may return a single URL or an
array of candidates, tried in order until one answers.

**Viewer** — `init` · `load` · `loadModel` · `setPathSolver` · `setBackground` ·
`setHdMode` / `setForceHd` · `setLightingMode` · `setShowGrid` ·
`setShadowsEnabled` · `setBloomEnabled` · `setHdDebugMode` · `setTimeOfDay` ·
`setDayNightAnimate` · `setIblMode` · `resetCamera` · `zoomBy` · `zoomScale` ·
`clearSplats` · `getFps` · `toBlob` · `dispose`

**Instance** — `setLocation` · `move` · `setRotation` · `setScale` ·
`setUniformScale` · `setTransformation` · `resetTransformation` ·
`setSequence(idOrName)` · `setSequenceLoopMode` · `setAnimationTimeMs` ·
`setTeamColor` · `getSequences` · `getSequenceRarities` · `getCameraPresets` ·
`activateCameraPreset` · `show` · `hide` · `detach`

Also exported: `Model`, `Scene`, `TEAM_COLORS`, `TEAM_COLOR_NAMES`,
`HD_DEBUG_MODES`, `MODEL_EXTENSIONS` / `isModelPath`, `EFFECT_EXTENSIONS` /
`isEffectPath`, `WebAudioBridge`, and the load-table helpers.

## Camera

Mouse: left-drag orbits, any other button pans, wheel zooms, double-click
recentres. Touch: one finger orbits, two pinch to zoom and slide to pan,
double-tap recentres — the same page works on a phone.

## License

BSD-3-Clause. Game assets belong to Blizzard Entertainment and are not included
here or fetched from anywhere but the sources you point the viewer at.
