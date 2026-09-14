// Retry gate. Blocks one asset's URLs for a while, then unblocks, and
// checks the slot fills in on its own — no model reload.
//
// Before the durable-retry change the pump gave up permanently after 3
// attempts (~51 s worst case with the fetch timeout), so the only cure
// for a transient Hive fault was the user noticing and reloading.
//
// Also exercises `wf_assets_retry_unloaded`: the renderer-side re-drive
// for slots whose need was drained and lost long before the fault
// cleared.
//
// Usage:
//   node tools/web_viewer/gate/retry-gate.mjs [--hd] [--block <substr>]
//        [--block-ms <ms>] [--model <path>] [--explicit-retry]

import { launch, bootViewer, sleep, STAT_EXPR } from './cdp.mjs';

const VALUED = new Set(['block', 'block-ms', 'model', 'url', 'wait-ms']);
const opts = {};
for (let i = 0, a = process.argv.slice(2); i < a.length; ++i) {
    if (!a[i].startsWith('--')) continue;
    const name = a[i].slice(2);
    if (VALUED.has(name)) opts[name] = a[++i];
    else opts[name] = true;
}
const HD = !!opts.hd;
const MODEL = opts.model || 'units/human/heroarchmage/heroarchmage.mdx';
// Default target is one HD texture of the archmage; in SD, its body skin.
const BLOCK = opts.block || (HD ? 'human_archmage_main_diffuse' : 'heroarchmage.dds');
const BLOCK_MS = Number(opts['block-ms'] || 45000);
const WAIT_MS = Number(opts['wait-ms'] || 90000);
const URL_ = opts.url || 'http://localhost:8080/index.html';

// Fail every request whose URL contains the marker, until the page is told
// to stop. Patching window.fetch rather than using CDP's Fetch domain keeps
// the block inside the page, where the pump's own error handling sees a
// normal network failure — which is the thing under test.
const INSTALL_BLOCK = (marker) => `(() => {
  window.__blocked = 0;
  window.__blockOn = true;
  const of = window.fetch;
  window.fetch = function (input) {
    const u = typeof input === 'string' ? input : (input && input.url) || String(input);
    if (window.__blockOn && u.indexOf(${JSON.stringify(marker)}) >= 0) {
      window.__blocked++;
      return Promise.reject(new TypeError('gate: blocked'));
    }
    return of.apply(this, arguments);
  };
  return true;
})()`;

const PROBE = `(() => {
  const v = window.__hive.viewer, M = v._module, h = v._handle;
  const failed = [];
  if (v._failedAssets) for (const e of v._failedAssets) failed.push(e[0] + ' x' + e[1].attempts);
  return { live: M._wf_assets_stat(h, 0), loaded: M._wf_assets_stat(h, 1),
           blocked: window.__blocked, failed: failed,
           queued: v._assetQueue ? v._assetQueue.length : 0,
           inflight: v._inflightAssets ? v._inflightAssets.size : 0 };
})()`;

const page = await launch();
let status = 0;
try {
    await page.onNewDocument(INSTALL_BLOCK(BLOCK));
    await bootViewer(page, { url: URL_, recordFetch: false });
    if (HD) await page.eval('window.__hive.viewer.setForceHd(true)');

    console.log('blocking any URL containing "' + BLOCK + '" for ' + BLOCK_MS + ' ms');
    await page.eval('window.__hive._selectModel({name:"gate",path:'
        + JSON.stringify(MODEL) + '},null).then(()=>"ok")', true);

    await sleep(BLOCK_MS);
    const during = await page.eval(PROBE);
    console.log('while blocked: ' + JSON.stringify(during));
    if (during.blocked === 0) {
        console.log('FAIL: nothing was blocked — the marker matched no URL, so this '
            + 'gate proved nothing. Pass --block with a substring the run actually fetches.');
        status = 1;
    }
    const missingDuring = during.live - during.loaded;

    console.log('unblocking; waiting up to ' + WAIT_MS + ' ms for self-recovery');
    await page.eval('window.__blockOn = false');
    if (opts['explicit-retry']) {
        const n = await page.eval('window.__hive.viewer.retryUnloadedAssets()');
        console.log('explicit retryUnloadedAssets() re-queued ' + n + ' slot(s)');
    }

    const t0 = Date.now();
    const ok = await page.waitFor(`(() => {
      const v = window.__hive.viewer, M = v._module, h = v._handle;
      return M._wf_assets_stat(h, 1) >= M._wf_assets_stat(h, 0);
    })()`, { timeoutMs: WAIT_MS, everyMs: 500 });
    const after = await page.eval(PROBE);
    console.log('recovered in ' + (Date.now() - t0) + ' ms: ' + JSON.stringify(after));
    console.log('stats ' + JSON.stringify(await page.eval(STAT_EXPR)));

    if (missingDuring <= 0) {
        console.log('WARN: nothing was missing while blocked; the block did not '
            + 'starve a slot, so recovery was trivially true.');
        status = status || 1;
    }
    console.log(ok && missingDuring > 0
        ? 'PASS: ' + missingDuring + ' slot(s) starved by the fault filled in with no reload'
        : 'FAIL: slots still unloaded after ' + WAIT_MS + ' ms');
    if (!ok) status = 2;
} finally {
    await page.close();
}
process.exit(status);
