// Asset pump — drains the AssetManager needs queue and fetches each
// path through the host pathSolver. Hive's CASC does same-family
// extension expansion server-side, so we only need one candidate per
// asset; the solver may return either a single URL or a fallback chain
// (e.g. direct asset URL → /casc-contents/).

const TEX = ['.blp', '.dds', '.tga', '.png', '.tif'];
// Subset of TEX with a C++ parser. Kept as a fast-fail gate for formats
// the renderer cannot decode; if a load-table override returns bytes of
// an unsupported texture format we skip without burning CASC fallbacks.
const TEX_DECODABLE = new Set(['.blp', '.dds', '.tga', '.png', '.tif']);
const MDL = ['.mdx', '.mdl'];
const PRT = ['.pkb', '.pkfx'];
const KIND_NAMES = ['Texture', 'Model', 'Effect', 'Data'];

const FETCH_TIMEOUT_MS = 15000;

function familyFor(origExt) {
    if (TEX.includes(origExt)) return TEX;
    if (MDL.includes(origExt)) return MDL;
    if (PRT.includes(origExt)) return PRT;
    // Cornflakes can surface bare-basename particle paths; treat as PRT
    // so the `.pkb` solver-side rewrite passes the family check.
    if (origExt === '') return PRT;
    return [origExt];
}

function kindName(kind) {
    return KIND_NAMES[kind] || ('k=' + kind);
}

function extractServedExt(responseUrl, fallback) {
    try {
        const finalPath = new URL(responseUrl).pathname.toLowerCase();
        // Only the basename's dot counts. Paths like
        // /hive-proxy/repository-files/<hash.with.dots>/stream contain
        // dots that belong to directory segments, not the filename;
        // taking pathname.lastIndexOf('.') would yank one of those.
        const slash = finalPath.lastIndexOf('/');
        const basename = slash >= 0 ? finalPath.slice(slash + 1) : finalPath;
        const dot = basename.lastIndexOf('.');
        if (dot >= 0) return basename.slice(dot);
    } catch (_) { /* opaque URL */ }
    return fallback;
}

function hexHead(bytes) {
    return Array.from(bytes.slice(0, 8))
        .map(b => b.toString(16).padStart(2, '0')).join(' ');
}

// Detect the texture format from its file-header magic. Useful when the
// URL gives us no hint (e.g. Hive's /repository-files/<hash>/stream) or
// when the upload's actual format differs from the request key's
// extension (a .tif key pointing at a DDS stream, etc). Returning the
// real ext lets the C++ decoder pick the right parser instead of
// silently failing as the wrong family.
function sniffTextureExt(bytes) {
    if (!bytes || bytes.length < 4) return '';
    const b0 = bytes[0], b1 = bytes[1], b2 = bytes[2], b3 = bytes[3];
    // 'DDS '
    if (b0 === 0x44 && b1 === 0x44 && b2 === 0x53 && b3 === 0x20) return '.dds';
    // 'BLP1' / 'BLP2'
    if (b0 === 0x42 && b1 === 0x4C && b2 === 0x50 && (b3 === 0x31 || b3 === 0x32)) return '.blp';
    // PNG: 89 50 4E 47
    if (b0 === 0x89 && b1 === 0x50 && b2 === 0x4E && b3 === 0x47) return '.png';
    // TIFF: 'II*\0' (little-endian) or 'MM\0*' (big-endian)
    if (b0 === 0x49 && b1 === 0x49 && b2 === 0x2A && b3 === 0x00) return '.tif';
    if (b0 === 0x4D && b1 === 0x4D && b2 === 0x00 && b3 === 0x2A) return '.tif';
    return '';
}

// Push bytes into WASM and dispatch to wf_assets_apply.
export function applyAsset(viewer, kind, subKind, path, u8, foundExt) {
    const M = viewer._module;
    const pathPtr = viewer._cstr(path);
    const extPtr  = viewer._cstr(foundExt || '');
    const dataPtr = M._malloc(u8.byteLength);
    M.HEAPU8.set(u8, dataPtr);
    try {
        return !!M._wf_assets_apply(
            viewer._handle, kind, subKind, pathPtr, dataPtr, u8.byteLength, extPtr);
    } finally {
        M._free(dataPtr);
        M._free(pathPtr);
        M._free(extPtr);
    }
}

// Outcomes. The distinction that matters is "no bytes" (worth asking
// again) vs "bytes arrived and no slot took them" (not worth asking
// again, ever): the usual cause of the latter is the slot being released
// while the fetch was in flight — a model switch — and Hive cannot fix
// that. Collapsing the two makes a durable retry queue chase assets whose
// model left the screen, forever.
export const FETCH_OK = true;
export const FETCH_UNCLAIMED = 'unclaimed';
export const FETCH_FAILED = false;

async function fetchAndApplyImpl(viewer, pathSolver, kind, subKind, relPath) {
    const fwd = relPath.replaceAll('\\', '/');
    const dot = fwd.lastIndexOf('.');
    const origExt = dot > 0 ? fwd.slice(dot).toLowerCase() : '';
    const family = familyFor(origExt);

    // AssetManager lower-cases every path it surfaces. That is harmless for
    // a name the solver looks up case-insensitively, but a standalone effect
    // was requested by the caller's own `src` — possibly a case-sensitive
    // URL — so ask under the original spelling when one was registered.
    const solverKey = (viewer._needAliases && viewer._needAliases.get(fwd)) || relPath;

    // Did any candidate deliver bytes we were willing to hand to C++?
    let sawBytes = false;

    let urls;
    try { urls = await Promise.resolve(pathSolver(solverKey)); }
    catch (_) { urls = null; }
    if (urls) {
        if (!Array.isArray(urls)) urls = [urls];
        for (const url of urls) {
            if (!url) continue;
            // 15 s timeout so a hung Hive stream doesn't peg `_inflight`.
            const ac = new AbortController();
            const timeoutId = setTimeout(() => ac.abort(), FETCH_TIMEOUT_MS);
            try {
                const r = await fetch(url, { signal: ac.signal });
                if (!r.ok) continue;
                const servedExt = extractServedExt(r.url, origExt);
                // Reject if Hive crossed our kind-family boundary
                // (pkfx/pkb ↔ mdl/mdx substitution).
                if (!family.includes(servedExt)) continue;
                const bytes = new Uint8Array(await r.arrayBuffer());
                // For textures, trust the bytes over the URL hint —
                // proxy URLs and the .tif-key/.dds-bytes mix in
                // load-tables can otherwise route bytes to the wrong
                // decoder and silently miss.
                let appliedExt = servedExt;
                if (family === TEX) {
                    const sniffed = sniffTextureExt(bytes);
                    if (sniffed) appliedExt = sniffed;
                    if (!TEX_DECODABLE.has(appliedExt)) {
                        console.warn('[wf] texture format ' + appliedExt
                            + ' has no decoder in this build; ' + relPath
                            + ' will be skipped.');
                        // Nothing about this changes on a second look.
                        return FETCH_UNCLAIMED;
                    }
                }
                if (applyAsset(viewer, kind, subKind, relPath, bytes, appliedExt))
                    return FETCH_OK;
                // Bytes came back but nothing took them — either no slot
                // is bound to this ref any more (released mid-flight by a
                // model switch) or C++ refused the decode. Log the head so
                // stale-PKB / zstd / HTML look distinguishable.
                sawBytes = true;
                console.warn('[wf] apply REJECTED (' + kindName(kind) + ', '
                    + bytes.byteLength + ' bytes, served as ' + servedExt
                    + ', head=' + hexHead(bytes) + '): ' + relPath + ' (from ' + r.url + ')');
            } catch (_) { /* try next */ }
            finally { clearTimeout(timeoutId); }
        }
    }
    if (sawBytes) return FETCH_UNCLAIMED;
    console.warn('[wf] asset MISS (' + kindName(kind)
        + ', all candidates failed): ' + relPath);
    return FETCH_FAILED;
}

export async function fetchAndApplyAsset(viewer, pathSolver, kind, subKind, relPath) {
    if (viewer._onFetchStart) viewer._onFetchStart(relPath);
    try {
        return await fetchAndApplyImpl(viewer, pathSolver, kind, subKind, relPath);
    } finally {
        if (viewer._onFetchEnd) viewer._onFetchEnd(relPath);
    }
}

// ── Scheduling ───────────────────────────────────────────────────────
//
// AssetManager's DrainNeeds is consumptive — `batch.swap(needs_)`. Once
// C++ surfaces a need and hands it over, C++ has forgotten it: nothing
// re-queues it, and the slot sits on its white placeholder until
// something releases and re-Acquires. **Durability is the host's job.**
// So everything drained goes into `_assetQueue` before any dedup or
// solver check can discard it; the window between the drain and the
// queue is where needs used to be lost.
//
// Concurrency is capped because the browser will otherwise open every
// fetch at once. They then share one connection, each one's timeout runs
// on wall-clock rather than on its own progress, and the slow ones abort
// — which is how a burst of speculative Acquires once cost a model its
// own textures, effects and child models. Under a cap a burst costs
// ORDER, not bytes.
// Measured, not guessed. On a 36-asset Reforged model, time-to-all-loaded
// over a live Hive (two runs each): cap 6 ≈ 1560 ms, 12 ≈ 1000 ms, 16 ≈
// 1360 ms, 24 ≈ 1250 ms, uncapped ≈ 1200 ms. Everything from 12 up is
// inside the run-to-run noise of the uncapped case, and only 6 is
// consistently slower — so 12 is the knee: it throttles a 300-request
// burst 25-fold while costing a normal model nothing measurable.
//
// `viewer.maxConcurrentFetches` overrides it; 0 means uncapped, kept so
// the cap can be measured against its own absence rather than argued
// about.
const MAX_CONCURRENT_FETCHES = 12;

function concurrencyCap(viewer) {
    const n = viewer.maxConcurrentFetches;
    if (n === 0) return Infinity;
    return (typeof n === 'number' && n > 0) ? n : MAX_CONCURRENT_FETCHES;
}

// Backoff for transient failures (Hive 5xx, network blips, a partial
// response). Doubles to a ceiling and then repeats there rather than
// giving up after a fixed count: a slot stuck behind a fault nobody can
// see from here recovers on its own once the fault clears, and an asset
// that genuinely does not exist costs one request a minute against a
// placeholder that was never going to be anything else.
const RETRY_BACKOFF_MS = 2000;
const RETRY_CEILING_MS = 60000;

function needKey(kind, subKind, path) {
    return kind + '/' + subKind + ':' + path;
}

function ensureQueues(viewer) {
    if (!viewer._inflightAssets) viewer._inflightAssets = new Map();
    if (!viewer._failedAssets)   viewer._failedAssets   = new Map();
    if (!viewer._assetQueue)     viewer._assetQueue     = [];
    if (!viewer._queuedKeys)     viewer._queuedKeys     = new Set();
}

function enqueueNeed(viewer, kind, subKind, path, generation) {
    const dedupKey = needKey(kind, subKind, path);
    // Re-surfacing cancels any standing failure record: the renderer
    // asked again, so it is wanted again, now rather than on a backoff.
    viewer._failedAssets.delete(dedupKey);

    const inflight = viewer._inflightAssets.get(dedupKey);
    if (inflight) {
        // Do NOT drop it. The running fetch may fail, and this request
        // would vanish with it — C++'s copy is already destroyed. Owed
        // another attempt only if that fetch delivers nothing.
        inflight.wantedAgain = true;
        return;
    }
    if (viewer._queuedKeys.has(dedupKey)) return;
    viewer._queuedKeys.add(dedupKey);
    viewer._assetQueue.push({ kind, subKind, path, dedupKey, generation, attempts: 0 });
}

function recordFailure(viewer, entry) {
    const info = viewer._failedAssets.get(entry.dedupKey) || entry;
    info.attempts = (info.attempts || 0) + 1;
    info.lastTryMs = performance.now();
    const backoff = Math.min(RETRY_BACKOFF_MS * Math.pow(2, info.attempts - 1),
                             RETRY_CEILING_MS);
    info.dueAt = info.lastTryMs + backoff;
    viewer._failedAssets.set(entry.dedupKey, info);
}

function fireFetch(viewer, entry) {
    const { kind, subKind, path, dedupKey } = entry;
    const rec = { wantedAgain: false, ok: false, startedMs: performance.now() };
    rec.promise = fetchAndApplyAsset(viewer, viewer._lazySolver, kind, subKind, path)
        .then((result) => {
            rec.ok = result === FETCH_OK;
            // FETCH_UNCLAIMED is not a failure: bytes arrived, and asking
            // for them again would not give them somewhere to go. Clear any
            // standing record rather than starting a retry cycle that can
            // never end.
            if (result === FETCH_OK || result === FETCH_UNCLAIMED)
                viewer._failedAssets.delete(dedupKey);
            else recordFailure(viewer, entry);
        })
        .catch(() => { recordFailure(viewer, entry); })
        .finally(() => {
            viewer._inflightAssets.delete(dedupKey);
            // A success applies to every slot on the path, including one
            // Acquired while the fetch was running — only a failure leaves
            // the re-request owed.
            if (rec.wantedAgain && !rec.ok)
                enqueueNeed(viewer, kind, subKind, path, entry.generation);
            dispatchAssetQueue(viewer);
        });
    viewer._inflightAssets.set(dedupKey, rec);
}

function dispatchAssetQueue(viewer) {
    // No solver yet: entries stay queued rather than being discarded, and
    // go out on the pump that follows setPathSolver.
    if (!viewer._lazySolver) return;
    const q = viewer._assetQueue;
    const cap = concurrencyCap(viewer);
    while (q.length && viewer._inflightAssets.size < cap) {
        // Newest load first. After a model switch the model actually on
        // screen should not wait behind the queue of the one that left.
        let best = 0;
        for (let i = 1; i < q.length; ++i) {
            if (q[i].generation > q[best].generation) best = i;
        }
        const entry = q.splice(best, 1)[0];
        viewer._queuedKeys.delete(entry.dedupKey);
        const inflight = viewer._inflightAssets.get(entry.dedupKey);
        if (inflight) { inflight.wantedAgain = true; continue; }
        fireFetch(viewer, entry);
    }
}

/// Re-queue every asset that has failed, ignoring its backoff. For events
/// that invalidate a resolution failure rather than waiting one out — a
/// local directory picked, a load table attached, the art tier switched.
/// Pair with `wf_assets_retry_unloaded` on the renderer side, which
/// re-surfaces slots whose needs were drained long ago.
export function retryFailedAssets(viewer) {
    ensureQueues(viewer);
    for (const [dedupKey, info] of viewer._failedAssets) {
        if (viewer._inflightAssets.has(dedupKey)) continue;
        if (viewer._queuedKeys.has(dedupKey)) continue;
        info.dueAt = 0;
        viewer._queuedKeys.add(dedupKey);
        viewer._assetQueue.push(info);
    }
    dispatchAssetQueue(viewer);
}

/// Drain C++'s needs into the host queue, promote any retries that have
/// come due, and dispatch up to the concurrency cap. Called once at the
/// end of each load and once per animation frame.
export function pumpAssetNeeds(viewer) {
    if (!viewer._handle) return;
    const M = viewer._module;
    if (!M._wf_assets_needs_count) return;
    ensureQueues(viewer);
    const generation = viewer._loadGeneration || 0;

    const n = M._wf_assets_needs_count(viewer._handle);
    if (n) {
        const CAP = 512;
        const buf = M._malloc(CAP);
        try {
            for (let i = 0; i < n; ++i) {
                const kind = M._wf_assets_needs_get_kind(viewer._handle, i);
                // Refinement within the kind, numbered by the scene's product.
                // Always 0 for Warcraft III. Older builds have no such export,
                // so fall back rather than pumping `undefined` into WASM.
                const subKind = M._wf_assets_needs_get_subkind
                    ? M._wf_assets_needs_get_subkind(viewer._handle, i) : 0;
                M._wf_assets_needs_get_path(viewer._handle, i, buf, CAP);
                enqueueNeed(viewer, kind, subKind, M.UTF8ToString(buf), generation);
            }
        } finally {
            M._free(buf);
        }
    }

    if (viewer._failedAssets.size > 0) {
        const now = performance.now();
        for (const [dedupKey, info] of viewer._failedAssets) {
            if (now < (info.dueAt || 0)) continue;
            if (viewer._inflightAssets.has(dedupKey)) continue;
            if (viewer._queuedKeys.has(dedupKey)) continue;
            viewer._queuedKeys.add(dedupKey);
            viewer._assetQueue.push(info);
        }
    }

    dispatchAssetQueue(viewer);
}
