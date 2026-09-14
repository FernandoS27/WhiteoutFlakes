// Web asset-pipeline gate. Boots the viewer headless against a live Hive,
// loads the named models, and reports what it cost: per-model wall-clock,
// the AssetManager counters, and every fetch split by route.
//
// Usage (from anywhere; the dev server must already be up):
//   node tools/web_viewer/gate/asset-gate.mjs [model…] [flags]
//
//   --hd              force Reforged Graphics before loading
//   --soak <seconds>  after the loads, sample every 5 s (catches runaway
//                     re-fetch loops, which a one-shot run cannot see)
//   --switch-ms <ms>  start the next load this long after the previous,
//                     without waiting for it — the model-switch stress
//   --settle <ms>     quiet time before the final sample (default 20000)
//   --url <url>       page to drive (default http://localhost:8080/index.html)
//   --json <path>     also write the report as JSON
//
// Exit status is 0 unless the page failed to boot; the gate reports
// numbers and lets the caller judge them against BASELINE.md.

import fs from 'node:fs';
import { launch, bootViewer, sleep, STAT_EXPR } from './cdp.mjs';

// Flags that take a value, so their argument is not mistaken for a model.
const VALUED = new Set(['soak', 'switch-ms', 'settle', 'url', 'json', 'shot', 'cap']);
const opts = {};
const positional = [];
for (let i = 0, a = process.argv.slice(2); i < a.length; ++i) {
    if (!a[i].startsWith('--')) { positional.push(a[i]); continue; }
    const name = a[i].slice(2);
    if (VALUED.has(name)) opts[name] = a[++i];
    else opts[name] = true;
}

const models = positional.length ? positional : ['units/human/heroarchmage/heroarchmage.mdx'];
const HD = !!opts.hd;
const SOAK_S = Number(opts.soak || 0);
const SWITCH_MS = Number(opts['switch-ms'] || 0);
const SETTLE_MS = Number(opts.settle ?? 20000);
const URL_ = opts.url || 'http://localhost:8080/index.html';
const JSON_OUT = opts.json || '';
// Counters cannot see a texture that resolved to the wrong bytes, or a
// scene the DNC rig stopped lighting. Those gates are "look at it", and
// this is how you look at it without popping a window.
const SHOT = opts.shot || '';

// Split every recorded fetch by the route it took. `direct` is the
// computable static URL, `casc` the /casc-contents/ 303 backstop; a direct
// MISS is a round trip that bought nothing, which is the number W1 exists
// to drive down.
const CLASSIFY = `(() => {
  const out = { direct_hit: 0, direct_miss: 0, direct_err: 0,
                casc_ok: 0, casc_miss: 0, casc_err: 0,
                local: 0, relative_probe: 0, other: 0,
                aborts: 0, total: 0 };
  const byExt = {};
  const slow = [];
  for (const r of window.__net) {
    out.total++;
    if (r.err === 'AbortError') out.aborts++;
    const u = r.url;
    let bucket;
    if (u.indexOf('/assets/wc3/') >= 0) {
      bucket = r.err ? 'direct_err' : (r.status >= 400 ? 'direct_miss' : 'direct_hit');
      const base = u.split('?')[0].split('/').pop().toLowerCase();
      const dot = base.lastIndexOf('.');
      const ext = dot >= 0 ? base.slice(dot) : '(none)';
      const key = ext + ' ' + (bucket === 'direct_hit' ? 'HIT' : 'MISS');
      byExt[key] = (byExt[key] || 0) + 1;
    } else if (u.indexOf('casc-contents') >= 0) {
      bucket = r.err ? 'casc_err' : (r.status >= 400 ? 'casc_miss' : 'casc_ok');
    } else if (u.indexOf('localhost') >= 0 || u.indexOf('127.0.0.1') >= 0) {
      bucket = 'local';
    } else if (u.charAt(0) === '.' || u.charAt(0) === '/') {
      bucket = 'relative_probe';
    } else {
      bucket = 'other';
    }
    out[bucket]++;
    if (r.t1 > 0 && r.t1 - r.t0 > 1000) slow.push(Math.round(r.t1 - r.t0) + 'ms ' + u.slice(0, 140));
  }
  // fetch() calls understate what the network did: a /casc-contents/ hit
  // is a 303 AND the GET it points at — two round trips behind one call.
  // That second hop is what the direct arm exists to remove, so count it.
  out.hiveCalls = out.direct_hit + out.direct_miss + out.direct_err
                + out.casc_ok + out.casc_miss + out.casc_err;
  out.hiveRoundTrips = out.direct_hit + out.direct_miss + out.direct_err
                     + out.casc_ok * 2 + out.casc_miss + out.casc_err;
  return { routes: out, byExt: byExt, slowest: slow.sort((a,b)=>parseInt(b)-parseInt(a)).slice(0, 8) };
})()`;

const FAILED = `(() => {
  const v = window.__hive.viewer, out = [];
  if (v._failedAssets) for (const e of v._failedAssets) out.push(e[0] + ' x' + e[1].attempts);
  return { failed: out, inflight: v._inflightAssets ? v._inflightAssets.size : 0, hd: !!v.hdMode };
})()`;

const page = await launch();
const report = { models, hd: HD, url: URL_, loads: [], samples: [] };
try {
    await bootViewer(page, { url: URL_ });
    // Startup traffic is its own measurement: everything before the first
    // load is prefetch, and D2 lives entirely in that window.
    report.startup = await page.eval(CLASSIFY);
    report.startupProviderCount = await page.eval(
        '(() => { const v = window.__hive.viewer; return v._module._wf_provider_count(v._handle); })()');

    if (opts.cap !== undefined) {
        await page.eval('window.__hive.viewer.maxConcurrentFetches = ' + Number(opts.cap));
        report.cap = Number(opts.cap);
    }
    if (HD) await page.eval('window.__hive.viewer.setForceHd(true)');

    for (const m of models) {
        const t0 = Date.now();
        const p = page.eval('window.__hive._selectModel({name:"gate",path:'
            + JSON.stringify(m) + '},null).then(()=>"ok")', true);
        if (SWITCH_MS) { await sleep(SWITCH_MS); } else { await p; }
        const ms = Date.now() - t0;
        const stat = await page.eval(STAT_EXPR);
        report.loads.push({ model: m, ms, stat });
        console.log(m.split('/').pop().padEnd(26) + ' load=' + String(ms).padStart(6) + 'ms  '
            + JSON.stringify(stat));
    }

    // Spawn wall-clock says nothing about when the model is actually
    // textured — the pump runs after it. This is the number a concurrency
    // cap trades against, so measure it directly rather than inferring it
    // from a fixed settle.
    const QUIET = `(() => {
      const v = window.__hive.viewer, M = v._module, h = v._handle;
      const live = M._wf_assets_stat(h, 0), loaded = M._wf_assets_stat(h, 1);
      const inflight = v._inflightAssets ? v._inflightAssets.size : 0;
      const queued = v._assetQueue ? v._assetQueue.length : 0;
      return (inflight === 0 && queued === 0 && loaded >= live);
    })()`;
    const tq = Date.now();
    const quiet = await page.waitFor(QUIET, { timeoutMs: SETTLE_MS, everyMs: 100 });
    report.timeToQuietMs = Date.now() - tq;
    report.reachedQuiet = quiet;
    console.log('time to all-loaded: ' + report.timeToQuietMs + ' ms'
        + (quiet ? '' : '  (NOT reached within ' + SETTLE_MS + ' ms)'));

    console.log('settling ' + (SETTLE_MS / 1000) + ' s…');
    await sleep(SETTLE_MS);
    report.final = await page.eval(STAT_EXPR);
    report.failed = await page.eval(FAILED);
    report.traffic = await page.eval(CLASSIFY);

    if (SOAK_S > 0) {
        console.log('\nt    live loaded  acq  rel  app miss   net failed');
        for (let t = 5; t <= SOAK_S; t += 5) {
            await sleep(5000);
            const s = await page.eval(STAT_EXPR);
            const n = await page.eval('window.__net.length');
            const f = await page.eval('window.__hive.viewer._failedAssets ? window.__hive.viewer._failedAssets.size : 0');
            report.samples.push({ t, ...s, net: n, failed: f });
            console.log(String(t).padStart(3) + ' ' + String(s.live).padStart(5)
                + String(s.loaded).padStart(7) + String(s.acq).padStart(5)
                + String(s.rel).padStart(5) + String(s.app).padStart(5)
                + String(s.miss).padStart(5) + String(n).padStart(6) + String(f).padStart(7));
        }
    }

    const t = report.traffic.routes;
    const startupRelative = report.startup.routes.relative_probe;
    console.log('\n── final ──');
    console.log('  slots        live ' + report.final.live + '  loaded ' + report.final.loaded
        + '  acq ' + report.final.acq + '  rel ' + report.final.rel
        + '  app ' + report.final.app + '  miss ' + report.final.miss);
    console.log('  direct       HIT ' + t.direct_hit + '  MISS ' + t.direct_miss + '  ERR ' + t.direct_err);
    console.log('  casc         ok ' + t.casc_ok + '  404 ' + t.casc_miss + '  ERR ' + t.casc_err);
    console.log('  hive         calls ' + t.hiveCalls + '  round trips ' + t.hiveRoundTrips);
    console.log('  local ' + t.local + '   relative-probe ' + t.relative_probe
        + '   other ' + t.other + '   aborts ' + t.aborts + '   total ' + t.total);
    console.log('  startup relative-probes ' + startupRelative
        + '   provider entries ' + report.startupProviderCount);
    console.log('  failed       ' + (report.failed.failed.length
        ? report.failed.failed.join(', ') : '(none)'));
    if (Object.keys(report.traffic.byExt).length) {
        console.log('  direct by ext ' + JSON.stringify(report.traffic.byExt));
    }
    if (report.traffic.slowest.length) {
        console.log('  slowest:\n    ' + report.traffic.slowest.join('\n    '));
    }
    const warn = page.console.filter(l => /asset MISS|REJECT|load failed|prefetch FAIL/.test(l));
    if (warn.length) console.log('  warnings (' + warn.length + '):\n    ' + warn.slice(0, 12).join('\n    '));

    if (SHOT) {
        const r = await page.send('Page.captureScreenshot', { format: 'png' });
        if (r.result && r.result.data) {
            fs.writeFileSync(SHOT, Buffer.from(r.result.data, 'base64'));
            console.log('  wrote ' + SHOT);
        } else {
            console.log('  screenshot FAILED: ' + JSON.stringify(r).slice(0, 200));
        }
    }

    if (JSON_OUT) {
        report.consoleWarnings = warn;
        fs.writeFileSync(JSON_OUT, JSON.stringify(report, null, 2));
        console.log('  wrote ' + JSON_OUT);
    }
} finally {
    await page.close();
}
process.exit(0);
