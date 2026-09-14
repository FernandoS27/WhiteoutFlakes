# Web asset-pipeline gates

Headless measurement for the web viewer's asset pipeline. No npm install —
Node's global `WebSocket` and `fetch` drive Chrome over CDP directly.

Both gates need the dev server up and a build staged into it:

```sh
cmake --build build-web --target wf_web
cd build-web/web && python ../../tools/web_viewer/serve_nocache.py 8080
```

Neither gate pops a window. `--headless=new --enable-unsafe-webgpu` is
what yields a real WebGPU adapter; **do not add `--use-angle=vulkan`**,
which makes `requestAdapter()` return null and looks like a missing
browser feature.

## `asset-gate.mjs` — what a session costs

```sh
node tools/web_viewer/gate/asset-gate.mjs
node tools/web_viewer/gate/asset-gate.mjs --hd --soak 60
node tools/web_viewer/gate/asset-gate.mjs m1.mdx m2.mdx --switch-ms 400
```

Reports per-model load wall-clock, the `wf_assets_stat` counters, and
every fetch split by route (direct hit / direct miss / `casc-contents` /
local / relative probe), plus aborts, stuck assets and the slowest
requests. `--soak <s>` samples every 5 s afterwards, which is the only way
to see a runaway re-fetch loop.

Load times move ±50 % run to run against a live service. Judge request
counts and slot counters, which are stable to the unit.

## `resolve-gate.mjs` — is the mirror still shaped the way we think?

```sh
node tools/web_viewer/gate/resolve-gate.mjs --hd
```

Drains the paths the renderer actually acquires, asks `hive-resolve.js` to
predict each one's direct URL, and compares against the URL
`/casc-contents/` redirects to.

This is a canary, not a correctness test. The direct URL is a fast path;
a wrong one costs a 404 and falls through to the authoritative route, so
nothing breaks when this gate drops — only speed does, silently. Exit
status is 2 on a genuine mismatch (both routes 200, different targets),
which is the shape a layout change would take.

`bothMiss` rows are paths neither route carries; they say nothing about
the rule and are excluded from the hit rate.

Baseline numbers: [BASELINE.md](BASELINE.md).
