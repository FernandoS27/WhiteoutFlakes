// Resolution-rule gate. Drains the paths the renderer actually acquires
// for a set of models, then — for each — compares the URL hive-resolve.js
// predicts against the URL /casc-contents/ redirects to.
//
// This is the canary for "Hive changed its mirror layout". Nothing breaks
// when it drops: the direct URL is a fast path and a wrong one costs a
// 404 before the authoritative route. What drops is speed, silently, and
// this is the only thing that would notice.
//
// Usage (dev server must be up):
//   node tools/web_viewer/gate/resolve-gate.mjs [model…] [--hd] [--json <path>]

import fs from 'node:fs';
import { launch, bootViewer, sleep } from './cdp.mjs';
import { directUrl, cascContentsUrl } from '../../../packages/whiteout-js-viewer/hive-resolve.js';

const VALUED = new Set(['json', 'url', 'settle']);
const opts = {};
const positional = [];
for (let i = 0, a = process.argv.slice(2); i < a.length; ++i) {
    if (!a[i].startsWith('--')) { positional.push(a[i]); continue; }
    const name = a[i].slice(2);
    if (VALUED.has(name)) opts[name] = a[++i];
    else opts[name] = true;
}
const models = positional.length ? positional : [
    'units/human/heroarchmage/heroarchmage.mdx',
    'units/human/footman/footman.mdx',
    'units/orc/heroblademaster/heroblademaster.mdx',
    'units/undead/abomination/abomination.mdx',
];
const HD = !!opts.hd;
const URL_ = opts.url || 'http://localhost:8080/index.html';
const SETTLE_MS = Number(opts.settle ?? 12000);

// Wrap the solver rather than reading the network log: the solver sees the
// path the renderer asked for, which is the input the rule is defined on.
// The network log only has URLs, which is the rule's output.
const TAP = `(() => {
  const v = window.__hive.viewer;
  if (v.__solverTapped) return true;
  window.__solverPaths = [];
  const orig = v._lazySolver;
  v._lazySolver = function (name) { window.__solverPaths.push(String(name)); return orig(name); };
  v.__solverTapped = true;
  return true;
})()`;

async function outcome(url) {
    if (!url) return { status: -1, final: '' };
    try {
        const r = await fetch(url, { redirect: 'follow' });
        await r.arrayBuffer();
        return { status: r.status, final: r.url };
    } catch (e) {
        return { status: -1, final: String((e && e.name) || e) };
    }
}

// Bounded fan-out. This gate issues two requests per path against a live
// third-party service; do not let it become a load test.
async function pool(items, width, fn) {
    const out = new Array(items.length);
    let next = 0;
    await Promise.all(Array.from({ length: width }, async () => {
        for (;;) {
            const i = next++;
            if (i >= items.length) return;
            out[i] = await fn(items[i]);
        }
    }));
    return out;
}

const page = await launch();
let paths = [];
try {
    await bootViewer(page, { url: URL_, recordFetch: false });
    if (HD) await page.eval('window.__hive.viewer.setForceHd(true)');
    await page.eval(TAP);
    for (const m of models) {
        await page.eval('window.__hive._selectModel({name:"gate",path:'
            + JSON.stringify(m) + '},null).then(()=>"ok")', true);
        // The tap is installed on _lazySolver, which the pump re-reads each
        // call, but a model switch can replace the solver — re-arm.
        await page.eval(TAP);
    }
    await sleep(SETTLE_MS);
    paths = await page.eval('window.__solverPaths') || [];
} finally {
    await page.close();
}

const uniq = [...new Set(paths.map(p => String(p)))]
    .filter(p => !/^https?:|^\//.test(p)); // absolute URLs bypass the rule
console.log('acquired paths: ' + paths.length + ' (' + uniq.length + ' unique, mode '
    + (HD ? 'hd' : 'sd') + ')');
if (!uniq.length) {
    console.log('nothing acquired — did the models load?');
    process.exit(1);
}

const rows = await pool(uniq, 4, async (p) => {
    const pred = directUrl(p, HD);
    const casc = cascContentsUrl(p, { hd: HD });
    const [a, b] = await Promise.all([outcome(pred), outcome(casc)]);
    return { p, pred, a, b };
});

let agree = 0, bothMiss = 0, cascOnly = 0, directOnly = 0, mismatch = 0;
const notes = [];
for (const { p, a, b } of rows) {
    if (a.status === 200 && b.status === 200 && a.final === b.final) { ++agree; continue; }
    if (a.status !== 200 && b.status !== 200) { ++bothMiss; notes.push('bothMiss   ' + p); continue; }
    if (a.status !== 200 && b.status === 200) {
        ++cascOnly;
        notes.push('cascOnly   ' + p + '  -> ' + decodeURIComponent(b.final).replace(/^.*\/assets\/wc3\//, ''));
        continue;
    }
    if (a.status === 200 && b.status !== 200) { ++directOnly; notes.push('directOnly ' + p); continue; }
    ++mismatch;
    notes.push('MISMATCH   ' + p + '\n   predicted ' + decodeURIComponent(a.final)
        + '\n   casc      ' + decodeURIComponent(b.final));
}

// Paths absent from both routes say nothing about the rule — they are
// references the mirror simply does not carry. Rate is over the rest.
const resolvable = rows.length - bothMiss;
const rate = resolvable ? (agree / resolvable) : 0;
console.log('\nagree ' + agree + '  cascOnly ' + cascOnly + '  directOnly ' + directOnly
    + '  mismatch ' + mismatch + '  bothMiss ' + bothMiss + '  (n=' + rows.length + ')');
console.log('direct-arm hit rate over resolvable paths: ' + (rate * 100).toFixed(1) + '%');
if (notes.length) console.log('\n' + notes.join('\n'));

if (opts.json) {
    fs.writeFileSync(opts.json, JSON.stringify(
        { hd: HD, models, n: rows.length, agree, cascOnly, directOnly, mismatch, bothMiss, rate,
          rows: rows.map(r => ({ path: r.p, predicted: r.pred, directStatus: r.a.status,
                                 cascStatus: r.b.status, cascFinal: r.b.final })) }, null, 2));
    console.log('wrote ' + opts.json);
}
process.exit(mismatch > 0 ? 2 : 0);
