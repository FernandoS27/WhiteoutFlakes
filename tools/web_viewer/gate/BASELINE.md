# Web asset-pipeline baseline

Numbers the gates produce against live hiveworkshop.com, recorded so a
change can be judged as a delta rather than an impression. Every row is
`node tools/web_viewer/gate/asset-gate.mjs …` with the dev server up
(`cd build-web/web && python ../../tools/web_viewer/serve_nocache.py 8080`).

Load wall-clock is the noisiest number here — it is a live third-party
service over a home connection, and it moves ±50 % run to run. **Request
counts and slot counters are stable to the unit** and are what the gates
should be judged on.

---

## W0 — post-D0, pre-W1

Recorded 2026-09-14. `render_service.cpp`'s `#ifndef __EMSCRIPTEN__` guard
is in; nothing else from `WEB_ASSET_PIPELINE_PLAN.md` has landed.

| run | flags | requests | direct HIT/MISS | casc | relative probes | acq | live/loaded |
|---|---|---|---|---|---|---|---|
| SD archmage | — | **89** | 0 / 0 | 36 | **26** | 4 | 4 / 4 |
| SD archmage (repeat) | — | **89** | 0 / 0 | 36 | **26** | 4 | 4 / 4 |
| HD archmage | `--hd` | **152** | 6 / **31** | 62 | **26** | 48 | 36 / 36 |
| 4-model SD session | 4 models | **97** | 0 / 0 | 44 | **26** | 12 | 9 / 9 |
| HD soak | `--hd --soak 60` | 152, **flat from t=20 s** | 6 / 31 | 62 | 26 | 48 | 36 / 36 |

Direct-probe outcome by extension, HD archmage — the D1 signature:

```
.mdx HIT  1     .blp MISS  3
.pkb HIT  5     .tif MISS 28
```

Slowest individual requests, every run:

```
~1540 ms  ./UI/SoundInfo/AmbientMusic.slk      <- D2: guaranteed 404
~1540 ms  ./UI/SoundInfo/Music.slk             <- D2: guaranteed 404
~2400 ms  /casc-contents/?path=…_main_diffuse.tif&context=hd
```

Aborts are **0** and `failed` is empty in every run: D0's fix holds, and
the soak shows no runaway re-fetch loop (net frozen at 152 for 40 s).

### Resolution rule (`resolve-gate.mjs`)

Paths drained from the renderer's own solver, predicted direct URL vs the
`/casc-contents/` 303 target:

| arm | unique paths | agree | cascOnly | mismatch | bothMiss | hit rate |
|---|---|---|---|---|---|---|
| HD (4 models) | 34 | 33 | 0 | 0 | 1 | **100.0 %** |
| SD (4 models) | 9 | 9 | 0 | 0 | 0 | **100.0 %** |

`bothMiss` is `units/orc/heroblademaster/1_heroblademasterspin.mdl` — a
child model neither route carries. It says nothing about the rule and is
excluded from the rate.

**A layer-prefix defect was found by this gate on its first run** and is
folded into `hive-resolve.js`: six acquired paths arrive already carrying
`_hd.w3mod/`, and joining the HD base onto them produced
`…/_hd.w3mod/_hd.w3mod/…`. `/casc-contents/` collapses the repetition
server-side, so nothing had ever noticed. `directUrl` now strips leading
layer segments the base already provides, and returns `null` — falling
through to the backstop — for a layer it does not serve (`_de.w3mod`, or
`_hd.w3mod` while in SD). Before the fix the HD arm scored 87.2 %.

---

## W1 — one resolver (`hive-resolve.js` wired into all three call sites)

| run | requests | direct HIT/MISS | casc | rel. probes | hive calls | hive **round trips** |
|---|---|---|---|---|---|---|
| SD archmage | 63 (was 89) | **31 / 0** | 5 (was 36) | **0** (was 26) | 36 | **41** (was 72) |
| SD blademaster | 67 | 35 / 0 | 5 | 0 | 40 | 45 |
| HD archmage | 95 (was 152) | **63 / 0** | 5 (was 62) | **0** | 68 (was 99) | **73** (was 161) |
| 4-model SD session | 71 (was 97) | 39 / 0 | 5 | 0 | 44 | 49 (was 88) |
| HD soak 60 s | 95, flat | 63 / 0 | 5 | 0 | 68 | 73 |

Round trips are the honest number: a `/casc-contents/` hit is a 303 *and*
the GET it points at, two hops behind one `fetch()`. `asset-gate.mjs`
reports both since W1.

- **HD direct misses 31 → 0.** Every `.tif`/`.blp` reference now asks for
  the `.dds` the mirror actually stores.
- **HD round trips 161 → 73, −55 %.** SD 72 → 41, −43 %.
- **Relative probes 26 → 0**, with `provider entries` unchanged at 57 —
  every engine asset still arrives, just through the route that works.
- The 5 remaining `casc-contents` calls are exactly the backstop-only set:
  3 IBL probes + 2 DNC layers.
- Slots, applies and misses unchanged; aborts 0; `failed` empty.

Rule agreement after the change, on a larger sample: HD 68/69 (100 % of
resolvable), SD 12/12 (100 %).

**Looked at it** (`--shot`): HD Reforged archmage renders with full PBR —
horse, armour, fur, staff crystal, the blue aura and the red team splat.
SD blademaster renders skin, armour, team-coloured cloth, sword ribbon.
Both lit, so the DNC rig still resolves through its backstop-only path.

---

## W2 — scheduler (durable queue, concurrency cap, priority)

Cap sweep, HD archmage, **time to all-loaded** (two runs each):

| cap | 4 | 6 | 12 | 16 | 24 | uncapped |
|---|---|---|---|---|---|---|
| ms | 1643 | 1820 / 1299 | **896 / 1101** | 1534 / 1183 | 970 / 1524 | 1411 / 983 |

Everything from 12 up is inside the run-to-run noise of uncapped; only 6
is consistently slower. **Default is 12.**

**W2.3 — flood tolerance**, the gate that matters most here. D0's guard
disabled in a scratch build, HD archmage:

| | pre-W1 baseline (recorded before this work) | now, cap 12 | now, uncapped |
|---|---|---|---|
| acquires | 339 | 353 | 353 |
| slots loaded | **39 / 149** | **146 / 156** | 146 / 156 |
| aborts | **67** | **0** | **0** |
| the model's own 36 | starved | all loaded, fully textured | all loaded |

The 10 unloaded slots are catalogue junk that exists in neither route
(`init`, `_`, `bugger.mdx`, six `spawnmodels/*dissipate*`), not the
model's.

**The cap is not what removed the aborts — W1 is.** Capped and uncapped
score identically under the flood, because a direct-URL fetch is one hop
to a static file where the old path was a 303 plus a GET, and it was the
303 hop that timed out under saturation. The cap is retained as bounded
insurance (a 300-request burst throttled 25-fold at no measurable cost),
not as the fix. Recorded because it is the opposite of what the design
predicted.

Rapid model switching (4 models, 400 ms apart): `live 9 / loaded 9`,
aborts 0, `failed` empty, 57 round trips.

## W3 — durable retry + `RetryUnloaded`

| gate | measured |
|---|---|
| W3.1 | one texture blocked 45 s → 5 attempts, then **recovered 18.3 s after unblock, no reload**. Old behaviour: gave up permanently at 3. |
| W3.2 | blocked **120 s → 6 attempts** (2/4/8/16/32/60 s), i.e. converging to 1 per minute |
| W3.3 | `retryUnloadedAssets()` re-queued exactly **1** slot; recovered in 518 ms instead of waiting out the backoff |
| W3.4 | desktop `renderer_api.cpp` compiles; C ABI links and exports `whiteout_flakes_FlakesAssetsView_RetryUnloaded`; `scripts/build-rust.ps1` green (fmt + clippy + 3 tests) after two emitter fixes — see below |

## W4 — mode before bytes

| arm | result |
|---|---|
| CASC | `hdMode=false`, **0 respawns**, acquire/release untouched — the probe cannot fire here and does not |
| local Reforged `.mdx` | `hdMode=true`, **exactly 1 respawn**, **37/37 slots loaded**, 13 releases, deps fetched from the HD overlay |

---

## Gate summary

| gate | target | measured | |
|---|---|---|---|
| W1.1 | HD direct MISS ≤ 2 (from 31) | **0**; round trips 161 → 73 (−55 %) | ✅ |
| W1.2 | SD direct hit rate ≥ 70 %; total < 89 | **100 %**; 63 | ✅ |
| W1.3 | relative probes 0; provider entries 57 | **0**; **57** | ✅ |
| W1.4 | rule agreement unchanged | 100 % both arms | ✅ |
| W1.5/6 | looked at it; DNC lights the scene | SD + HD render correctly | ✅ |
| W2.1 | cap knee chosen from a sweep | 12 | ✅ |
| W2.2 | no worse than W1 by >10 % | within noise | ✅ |
| W2.3 | flood: aborts 0, model's own slots load | **0 aborts**, 146/156 | ✅ |
| W2.4 | soak flat | 95 requests, flat 40 s+, 0 failed | ✅ |
| W2.5 | rapid switch leaves nothing stuck | 9/9, 0 failed | ✅ |
| W3.1 | recovers with no reload | 18.3 s | ✅ |
| W3.2 | ≤ 1 request/min at steady state | 6 attempts / 120 s | ✅ |
| W3.3 | explicit re-drive re-queues | 1 | ✅ |
| W3.4 | Rust crate builds | fmt + clippy + tests green | ✅ |
| W4.1 | CASC: acquire/release unchanged | 0 respawns | ✅ |
| W4.2 | local Reforged: one respawn, HD deps | 1 respawn, 37/37 | ✅ |

### W3.4 — two emitter fixes were needed

`scripts/build-rust.ps1` failed at `cargo fmt`, and failed the same way at
HEAD, for two gaps in `externals/WhiteoutLib/tools/codegen/emit_rust.py`
that `RetryUnloaded` merely happened to reach:

1. **Rust keywords were not escaped in parameter names.**
   `ActorView::Play(..., bool loop, ...)` emitted `loop: i32`, which does
   not parse. The emitter already escapes method names (`_method_name`)
   and field names (`_field_name`) with a trailing `_`; parameters had no
   equivalent. Added `_param_name` and routed all 13 parameter-spelling
   sites through it — the extern declaration, the wrapper signature and
   the call-through have to agree or the three drift apart.

   The knock-on: the emitter pipes its output through `rustfmt` and falls
   back to unformatted text when rustfmt cannot parse it. So `flakes.rs`
   had been shipping **unformatted** ever since `Play` landed, which is
   exactly why `cargo fmt --check` failed. Fixing the keyword let rustfmt
   run for the first time in a while, hence the ~1,680-line reformat in
   the regenerated file. It is whitespace; the only semantic changes are
   `loop_` and the new method.

2. **`std::size_t` was not a known return type.** `_rust_prim` strips a
   `whiteout::` prefix but the table had only the bare `size_t`, so the
   qualified spelling fell through and the method was skipped with
   `(return std::size_t)`. Added `'std::size_t': 'u64'` — **u64, not
   usize**, because that is what the C emitter already spells it as
   wherever it crosses today (every `std::size_t` field on a value object
   is a `uint64_t` getter), and the two sides have to agree on width.

`AssetsView::RetryUnloaded` is now bound as `retry_unloaded(&mut self) ->
u64`. The emitter's skip list went from 17 entries to 16; the other 16 are
untouched, and the C ABI output is byte-identical to before these fixes.
