// Asset pump — drains the AssetManager needs queue and fetches each
// path through the host pathSolver. Hive's CASC does same-family
// extension expansion server-side, so we only need one candidate per
// asset; the solver may return either a single URL or a fallback chain
// (e.g. direct asset URL → /casc-contents/).

const TEX = ['.blp', '.dds', '.tga', '.png', '.tif'];
const MDL = ['.mdx', '.mdl'];
const PRT = ['.pkb', '.pkfx'];
const KIND_NAMES = ['Texture', 'Particle', 'ChildModel'];

const FETCH_TIMEOUT_MS = 15000;

function familyFor(origExt) {
    if (TEX.includes(origExt)) return TEX;
    if (MDL.includes(origExt)) return MDL;
    if (PRT.includes(origExt)) return PRT;
    return [origExt];
}

function kindName(kind) {
    return KIND_NAMES[kind] || ('k=' + kind);
}

function extractServedExt(responseUrl, fallback) {
    try {
        const finalPath = new URL(responseUrl).pathname.toLowerCase();
        const dot = finalPath.lastIndexOf('.');
        if (dot >= 0) return finalPath.slice(dot);
    } catch (_) { /* opaque URL */ }
    return fallback;
}

function hexHead(bytes) {
    return Array.from(bytes.slice(0, 8))
        .map(b => b.toString(16).padStart(2, '0')).join(' ');
}

// Push bytes into WASM and dispatch to wf_assets_apply.
export function applyAsset(viewer, kind, path, u8, foundExt) {
    const M = viewer._module;
    const pathPtr = viewer._cstr(path);
    const extPtr  = viewer._cstr(foundExt || '');
    const dataPtr = M._malloc(u8.byteLength);
    M.HEAPU8.set(u8, dataPtr);
    try {
        return !!M._wf_assets_apply(
            viewer._handle, kind, pathPtr, dataPtr, u8.byteLength, extPtr);
    } finally {
        M._free(dataPtr);
        M._free(pathPtr);
        M._free(extPtr);
    }
}

async function fetchAndApplyImpl(viewer, pathSolver, kind, relPath) {
    const fwd = relPath.replaceAll('\\', '/');
    const dot = fwd.lastIndexOf('.');
    const origExt = dot > 0 ? fwd.slice(dot).toLowerCase() : '';
    const family = familyFor(origExt);

    let urls;
    try { urls = await Promise.resolve(pathSolver(relPath)); }
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
                if (applyAsset(viewer, kind, relPath, bytes, servedExt)) return true;
                // Bytes came back but C++ refused them. Log the head so
                // stale-PKB / zstd / HTML look distinguishable.
                console.warn('[wf] apply REJECTED (' + kindName(kind) + ', '
                    + bytes.byteLength + ' bytes, served as ' + servedExt
                    + ', head=' + hexHead(bytes) + '): ' + relPath + ' (from ' + r.url + ')');
            } catch (_) { /* try next */ }
            finally { clearTimeout(timeoutId); }
        }
    }
    console.warn('[wf] asset MISS (' + kindName(kind)
        + ', all candidates failed): ' + relPath);
    return false;
}

export async function fetchAndApplyAsset(viewer, pathSolver, kind, relPath) {
    if (viewer._onFetchStart) viewer._onFetchStart(relPath);
    try {
        return await fetchAndApplyImpl(viewer, pathSolver, kind, relPath);
    } finally {
        if (viewer._onFetchEnd) viewer._onFetchEnd(relPath);
    }
}

// Drain the needs queue. Dedup by (kind, path) for in-flight only —
// slot teardown + re-Acquire (model switch) needs a fresh fetch.
export function pumpAssetNeeds(viewer) {
    if (!viewer._handle) return;
    const M = viewer._module;
    if (!M._wf_assets_needs_count) return;
    const n = M._wf_assets_needs_count(viewer._handle);
    if (!n) return;
    if (!viewer._inflightAssets) viewer._inflightAssets = new Map();
    const CAP = 512;
    const buf = M._malloc(CAP);
    try {
        for (let i = 0; i < n; ++i) {
            const kind = M._wf_assets_needs_get_kind(viewer._handle, i);
            M._wf_assets_needs_get_path(viewer._handle, i, buf, CAP);
            const path = M.UTF8ToString(buf);
            const dedupKey = kind + ':' + path;
            if (viewer._inflightAssets.has(dedupKey)) continue;
            if (!viewer._lazySolver) continue;
            const p = fetchAndApplyAsset(viewer, viewer._lazySolver, kind, path)
                .catch(() => {})
                .finally(() => { viewer._inflightAssets.delete(dedupKey); });
            viewer._inflightAssets.set(dedupKey, p);
        }
    } finally {
        M._free(buf);
    }
}
