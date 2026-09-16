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
| `.m2` | World of Warcraft — its `.skin` siblings must resolve through the same `pathSolver` |
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

`serviceWorkerUrl` defaults to `./sw.js`, resolved against your page — a
worker that caches mirror downloads across visits. The package does not ship
one, so either serve your own there or pass `serviceWorkerUrl: null`; if
neither, registration fails with a console warning and everything else works.

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
`setShadowsEnabled` · `setBloomEnabled` · `setDebugView` · `setTimeOfDay` ·
`setDayNightAnimate` · `setIblMode` · `resetCamera` · `zoomBy` · `zoomScale` ·
`clearSplats` · `retryUnloadedAssets` · `getFps` · `toBlob` · `dispose`

**Instance** — `setLocation` · `move` · `setRotation` · `setScale` ·
`setUniformScale` · `setTransformation` · `resetTransformation` ·
`setSequence(idOrName)` · `setSequenceLoopMode` · `setAnimationTimeMs` ·
`setTeamColor` · `getSequences` · `getSequenceRarities` · `getCameraPresets` ·
`activateCameraPreset` · `show` · `hide` · `detach`

Also exported: `Model`, `Scene`, `TEAM_COLORS`, `TEAM_COLOR_NAMES`,
`DEBUG_VIEWS` / `debugViewsFor` (with `Instance.debugFamily()`), `MODEL_EXTENSIONS` / `isModelPath`, `EFFECT_EXTENSIONS` /
`isEffectPath`, `WebAudioBridge`, the load-table helpers, and the Hive URL
helpers below.

## Asset loading

Every asset a load discovers is queued and fetched at most
`viewer.maxConcurrentFetches` at a time (default 12; `0` removes the cap). An
asset for which no candidate returns bytes is retried on its own, backing off
to once a minute rather than giving up. When something changes what a path
can resolve to, such as a directory the user has just picked, call
`viewer.retryUnloadedAssets()` to re-request everything still missing at once.
`HiveApp` does this for you on a directory pick and on `loadFromTable`.

**Hiveworkshop's mirror.** A path nothing local resolves is tried in two ways,
in order: the mirror's static file URL, which is one round trip, then its
`/casc-contents/` endpoint, which redirects and also finds files the path
names in the wrong directory. Both are assignable on the viewer if you need
to route them elsewhere, and everything that fetches — the asset pump, the
startup prefetch and `HiveApp` — goes through them:

```js
const hive = viewer.cascUrl;
viewer.cascUrl = (path, withContext) =>
  hive(path, withContext).replace('https://www.hiveworkshop.com', '/my-proxy');
viewer.cascDirectUrl = () => null;   // skip the static-file fast path
```

Wrap the original rather than rebuilding the URL: it carries the SD/HD
`context` that keeps a classic model from being handed Reforged textures.

The same rules are exported for a `pathSolver` of your own:
`hiveCandidates(path, { hd })` returns the ordered URL list, and `directUrl`,
`cascContentsUrl`, `mirrorName`, `requestName` and `normalizePath` are its
parts.

## Camera

Mouse: left-drag orbits, any other button pans, wheel zooms, double-click
recentres. Touch: one finger orbits, two pinch to zoom and slide to pan,
double-tap recentres — the same page works on a phone.

## License

BSD-3-Clause. Game assets belong to Blizzard Entertainment and are not included
here or fetched from anywhere but the sources you point the viewer at.
