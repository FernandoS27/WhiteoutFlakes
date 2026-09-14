// Render-mode gate. Checks that a model's slots are stamped with the mode
// its BYTES belong to, not the mode that happened to be live when the
// spawn ran.
//
// Two arms, because the probe behaves differently by source:
//
//   CASC  — the MDX is fetched under a mode-parameterised URL, so Hive
//           returns the SD variant and the probe can only ever agree. The
//           respawn path must NOT fire: acquire/release counts stay as
//           they were.
//   local — a Reforged MDX served from somewhere that is not the mirror.
//           The probe says HD while the spawn ran in SD, so the reconcile
//           fires exactly once and every dep is fetched under HD.
//
// Usage (dev server up; a Reforged .mdx staged under web/models/):
//   node tools/web_viewer/gate/mode-gate.mjs
//   node tools/web_viewer/gate/mode-gate.mjs --local http://localhost:8080/models/hd_archmage.mdx

import { launch, bootViewer, sleep, STAT_EXPR } from './cdp.mjs';

const VALUED = new Set(['local', 'casc', 'url', 'settle']);
const opts = {};
for (let i = 0, a = process.argv.slice(2); i < a.length; ++i) {
    if (!a[i].startsWith('--')) continue;
    const name = a[i].slice(2);
    if (VALUED.has(name)) opts[name] = a[++i];
    else opts[name] = true;
}
const CASC_MODEL = opts.casc || 'units/human/heroarchmage/heroarchmage.mdx';
const LOCAL_MODEL = opts.local || '';
const URL_ = opts.url || 'http://localhost:8080/index.html';
const SETTLE = Number(opts.settle ?? 20000);

// Every Hive URL this run asked for, so we can assert which overlay the
// deps actually came from — the point of the whole exercise.
const OVERLAY = `(() => {
  let hd = 0, sd = 0, other = 0;
  for (const r of window.__net) {
    const u = r.url;
    if (u.indexOf('hiveworkshop') < 0) continue;
    if (u.indexOf('_hd.w3mod') >= 0 || u.indexOf('context=hd') >= 0) hd++;
    else if (u.indexOf('/assets/wc3/') >= 0 || u.indexOf('context=sd') >= 0) sd++;
    else other++;
  }
  return { hd, sd, other };
})()`;

const page = await launch();
let status = 0;
try {
    await bootViewer(page, { url: URL_ });
    const respawns = [];
    page.console.length = 0;

    console.log('── CASC arm: ' + CASC_MODEL);
    await page.eval('window.__hive._selectModel({name:"gate",path:'
        + JSON.stringify(CASC_MODEL) + '},null).then(()=>"ok")', true);
    await sleep(SETTLE);
    const cascStat = await page.eval(STAT_EXPR);
    const cascHd = await page.eval('!!window.__hive.viewer.hdMode');
    const cascRespawn = page.console.filter(l => l.indexOf('respawning') >= 0).length;
    console.log('  hdMode=' + cascHd + '  respawns=' + cascRespawn
        + '  ' + JSON.stringify(cascStat));
    if (cascRespawn !== 0) {
        console.log('  FAIL: the reconcile fired on a CASC load; it cannot have '
            + 'anything to reconcile, since the MDX came from the mode it is probing.');
        status = 2;
    } else {
        console.log('  PASS: no respawn, acquire/release untouched');
    }

    if (LOCAL_MODEL) {
        console.log('\n── local arm: ' + LOCAL_MODEL);
        page.console.length = 0;
        const before = await page.eval(STAT_EXPR);
        await page.eval('window.__hive._selectModel({name:"gate",url:'
            + JSON.stringify(LOCAL_MODEL) + '},null).then(()=>"ok")', true);
        await sleep(SETTLE);
        const after = await page.eval(STAT_EXPR);
        const hd = await page.eval('!!window.__hive.viewer.hdMode');
        const respawn = page.console.filter(l => l.indexOf('respawning') >= 0).length;
        const overlay = await page.eval(OVERLAY);
        console.log('  hdMode=' + hd + '  respawns=' + respawn
            + '  ' + JSON.stringify(after));
        console.log('  acquires this load: ' + (after.acq - before.acq)
            + '  releases: ' + (after.rel - before.rel));
        console.log('  hive URLs by overlay: ' + JSON.stringify(overlay));
        respawns.push(respawn);

        if (!hd) {
            console.log('  FAIL: a Reforged MDX did not put the viewer in HD');
            status = 2;
        } else if (respawn !== 1) {
            console.log('  FAIL: expected exactly one respawn, saw ' + respawn);
            status = 2;
        } else if (after.loaded < after.live) {
            console.log('  FAIL: ' + (after.live - after.loaded) + ' slot(s) never loaded');
            status = 2;
        } else {
            console.log('  PASS: probed HD, respawned once, ' + after.loaded + '/'
                + after.live + ' slots loaded under the HD overlay');
        }
    } else {
        console.log('\n(no --local model given; the arm where the probe can '
            + 'actually fire was not exercised)');
    }
} finally {
    await page.close();
}
process.exit(status);
