// Minimal headless-Chrome driver over CDP. No npm dependencies — Node's
// global WebSocket and fetch are enough, and a gate that needs an install
// step is a gate nobody runs.
//
// Flags that matter: `--headless=new --enable-unsafe-webgpu` is what gets
// a real WebGPU adapter. Do NOT add `--use-angle=vulkan` — it makes
// requestAdapter() return null on this stack, which looks exactly like a
// missing browser feature and costs an hour to diagnose.

import { spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const CHROME_CANDIDATES = [
    process.env.WF_CHROME,
    'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
    'C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe',
    '/usr/bin/google-chrome',
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
].filter(Boolean);

function findChrome() {
    for (const c of CHROME_CANDIDATES) {
        try { if (fs.existsSync(c)) return c; } catch (_) { /* keep looking */ }
    }
    throw new Error('no Chrome found; set WF_CHROME to its full path');
}

export const sleep = (ms) => new Promise(r => setTimeout(r, ms));

export async function launch({ port = 9200 + (Date.now() % 500), width = 1280, height = 800 } = {}) {
    // A unique profile per run: a previous headless Chrome that has not
    // fully exited still holds the old one, and reusing it fails EPERM.
    const profile = path.join(os.tmpdir(), 'wfgate_' + process.pid + '_' + Date.now());
    const child = spawn(findChrome(), [
        '--headless=new',
        '--remote-debugging-port=' + port,
        '--user-data-dir=' + profile,
        '--enable-unsafe-webgpu',
        '--disable-gpu-sandbox',
        '--no-first-run',
        '--no-default-browser-check',
        '--window-size=' + width + ',' + height,
        'about:blank',
    ], { stdio: 'ignore' });

    let wsUrl = null;
    for (let i = 0; i < 80 && !wsUrl; ++i) {
        try {
            const r = await fetch('http://127.0.0.1:' + port + '/json/version');
            if (r.ok) wsUrl = (await r.json()).webSocketDebuggerUrl;
        } catch (_) { /* not up yet */ }
        if (!wsUrl) await sleep(250);
    }
    if (!wsUrl) { try { child.kill(); } catch (_) {} throw new Error('Chrome did not expose a debugger on port ' + port); }

    const ws = new WebSocket(wsUrl);
    await new Promise((res, rej) => {
        ws.addEventListener('open', res, { once: true });
        ws.addEventListener('error', rej, { once: true });
    });

    let nextId = 0;
    const pending = new Map();
    const console_ = [];
    ws.addEventListener('message', (e) => {
        const m = JSON.parse(e.data);
        if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); return; }
        if (m.method === 'Runtime.consoleAPICalled') {
            console_.push(m.params.type + ': ' +
                (m.params.args || []).map(a => a.value ?? a.description ?? '').join(' '));
        }
    });
    const send = (method, params = {}, sessionId) => new Promise((res) => {
        const id = ++nextId;
        pending.set(id, res);
        ws.send(JSON.stringify({ id, method, params, sessionId }));
    });

    const ct = await send('Target.createTarget', { url: 'about:blank' });
    const at = await send('Target.attachToTarget', { targetId: ct.result.targetId, flatten: true });
    const sessionId = at.result.sessionId;
    const S = (method, params) => send(method, params, sessionId);

    await S('Page.enable');
    await S('Runtime.enable');
    await S('Emulation.setDeviceMetricsOverride',
        { width, height, deviceScaleFactor: 1, mobile: false });

    return {
        console: console_,
        send: S,
        async close() { try { child.kill(); } catch (_) {} try { ws.close(); } catch (_) {} },

        /// Install a script that runs before any page script on every
        /// subsequent navigation.
        onNewDocument(source) { return S('Page.addScriptToEvaluateOnNewDocument', { source }); },

        async navigate(url) { return S('Page.navigate', { url }); },

        /// Evaluate in the page. Returns the value, or a string beginning
        /// `__ERR` so a caller that forgets to check still prints something
        /// diagnosable rather than silently seeing undefined.
        async eval(expression, awaitPromise = false) {
            const r = await S('Runtime.evaluate', { expression, returnByValue: true, awaitPromise });
            const res = r.result || {};
            if (res.exceptionDetails) {
                return '__ERR ' + JSON.stringify(res.exceptionDetails).slice(0, 400);
            }
            return res.result ? res.result.value : null;
        },

        /// Poll `expression` until it is true. Returns false on timeout
        /// rather than throwing, so a gate can report the failure with its
        /// own context attached.
        async waitFor(expression, { timeoutMs = 80000, everyMs = 500 } = {}) {
            const deadline = Date.now() + timeoutMs;
            while (Date.now() < deadline) {
                if (await this.eval(expression) === true) return true;
                await sleep(everyMs);
            }
            return false;
        },
    };
}

// Records every fetch the page makes: URL, timing, status, and the error
// name when it rejects (AbortError is the one that matters — it is what a
// saturated connection pool looks like from inside the page).
export const FETCH_RECORDER = [
    'window.__net = [];',
    'const __of = window.fetch;',
    'window.fetch = function (input) {',
    '  const u = typeof input === "string" ? input : (input && input.url) || String(input);',
    '  const rec = { url: u, t0: performance.now(), t1: -1, status: -1, err: null };',
    '  window.__net.push(rec);',
    '  return __of.apply(this, arguments).then(',
    '    function (r) { rec.t1 = performance.now(); rec.status = r.status; return r; },',
    '    function (e) { rec.t1 = performance.now(); rec.err = String((e && e.name) || e); throw e; });',
    '};',
].join('\n');

/// Boot the viewer page and wait for the WASM handle to exist.
export async function bootViewer(page, { url = 'http://localhost:8080/index.html', recordFetch = true } = {}) {
    if (recordFetch) await page.onNewDocument(FETCH_RECORDER);
    await page.navigate(url);
    const ok = await page.waitFor('!!(window.__hive && window.__hive.viewer && window.__hive.viewer._handle)');
    if (!ok) throw new Error('viewer did not boot at ' + url + ' (is serve_nocache.py running?)');
    return true;
}

/// The AssetManager counters, by the indices wf_assets_stat exposes.
export const STAT_EXPR = `(() => {
  const v = window.__hive.viewer, M = v._module, h = v._handle;
  const s = i => M._wf_assets_stat(h, i);
  return { live: s(0), loaded: s(1), pending: s(2), acq: s(3), rel: s(4), app: s(5), miss: s(6) };
})()`;
