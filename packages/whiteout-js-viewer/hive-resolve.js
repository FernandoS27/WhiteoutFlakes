// Hive CASC mirror URL policy — the one place that knows how a
// provider-relative path becomes a URL. The asset pump, the startup
// prefetch and the MDX load all resolve through here, so there is exactly
// one definition of the policy rather than three that drift.
//
// Two routes reach the same bytes:
//
//   /casc-contents/?path=<p>&context=sd|hd — authoritative. A 303 whose
//     Location points into /assets/wc3/. It carries `private, no-cache,
//     max-age=0`, so the hop is uncacheable by the server's own
//     instruction and every asset on every cold load pays it. It handles
//     what a client cannot compute: basename relocation (an SD MDX's TEXS
//     chunk stores bare basenames, so the renderer prefixes a guessed
//     directory and is often wrong — Textures/Clouds8x8.blp really lives
//     at ReplaceableTextures/Weather/), locale layers and aliases.
//
//   /assets/wc3/war3.w3mod[/_hd.w3mod]/<p> — the static file. Computable
//     from the request path, so one round trip instead of two. Case-
//     SENSITIVE and lowercase-only.
//
// Measured against the live mirror: the 303's target is the request path
// lower-cased, with any texture extension forced to `.dds` (the mirror
// stores every WC3 texture that way in BOTH trees, whatever the model
// names it — `.blp` in SD, `.tif` in Reforged HD) and `.mdl` forced to
// `.mdx`. 101/102 HD and 24/25 SD renderer-acquired paths agreed.
//
// So the direct URL is a fast path, never a replacement. It goes first,
// /casc-contents/ stays behind it, and a wrong guess costs one honest 404
// — the mirror never serves a placeholder with 200 — before the route
// that always works.

const ORIGIN = 'https://www.hiveworkshop.com';
const CASC_CONTENTS = ORIGIN + '/casc-contents/?path=';
const SD_BASE = ORIGIN + '/assets/wc3/war3.w3mod/';
const HD_BASE = SD_BASE + '_hd.w3mod/';

// Extensions the mirror stores as `.dds` regardless of what names them.
const TEX_EXTS = new Set(['.blp', '.dds', '.tga', '.png', '.tif']);

function extOf(p) {
    const slash = p.lastIndexOf('/');
    const base = slash >= 0 ? p.slice(slash + 1) : p;
    const dot = base.lastIndexOf('.');
    return dot >= 0 ? base.slice(dot) : '';
}

function swapExt(p, ext) {
    const dot = p.lastIndexOf('.');
    return (dot >= 0 ? p.slice(0, dot) : p) + ext;
}

// encodeURIComponent eats the separators; encode each segment instead.
function encodePath(p) {
    return p.split('/').map(encodeURIComponent).join('/');
}

// Lower-case + forward slashes. Lower-casing is not tidiness: the direct
// tree is case-sensitive, so `Units/Human/...` 404s where the identical
// lower-case path is a 200. AssetManager::Normalize already folds every
// path it surfaces, so the pump arrives pre-folded; callers with
// hand-written path lists (the startup prefetch) do not.
export function normalizePath(p) {
    return String(p).split('\\').join('/').replace(/^\.\//, '').toLowerCase();
}

// The name to ASK for, on either route. Two rewrites that are about what
// the mirror stores rather than which route we take, so both arms share
// them:
//   - `.pkfx` -> `.pkb`, because Hive's model-family expansion will
//     happily substitute an `.mdl`/`.mdx` for a `.pkfx` request;
//   - a basename with no extension -> `.pkb`, because Cornflakes
//     occasionally references particles by bare name and CASC will not
//     resolve that without a literal path.
export function requestName(p) {
    const norm = normalizePath(p);
    if (norm.endsWith('.pkfx')) return swapExt(norm, '.pkb');
    return extOf(norm) === '' ? norm + '.pkb' : norm;
}

// The name the static mirror stores it under — `requestName` plus the two
// transforms that only the direct arm needs. The /casc-contents/ arm must
// NOT get these: it does the expansion itself, and asking it for a name
// the model does not use loses the alias information it resolves on.
export function mirrorName(p) {
    const name = requestName(p);
    const ext = extOf(name);
    if (TEX_EXTS.has(ext)) return swapExt(name, '.dds');
    if (ext === '.mdl') return swapExt(name, '.mdx');
    return name;
}

export function directBase(hd) {
    return hd ? HD_BASE : SD_BASE;
}

// A mod-pinned key (`war3.w3mod:Environment/...`) addresses a mount, not a
// file in one tree, so it has no computable direct URL — DncService asks
// for those and only /casc-contents/ resolves them.
function hasMountPrefix(norm) {
    const colon = norm.indexOf(':');
    if (colon < 0) return false;
    const slash = norm.indexOf('/');
    return slash < 0 || colon < slash;
}

// Mod-layer directory segments. Some acquired paths arrive already
// carrying the layer they came from — a Reforged corn effect names
// `_hd.w3mod/textures/fx/...` — and the direct base already encodes a
// layer, so a naive join produces `_hd.w3mod/_hd.w3mod/...` and 404s.
// /casc-contents/ collapses the repetition server-side, which is why
// nothing noticed.
const LAYER_SEGMENTS = new Set(['war3.w3mod', '_hd.w3mod', '_de.w3mod']);

function splitLayers(name) {
    const parts = name.split('/');
    let i = 0;
    while (i < parts.length - 1 && LAYER_SEGMENTS.has(parts[i])) ++i;
    return { layers: parts.slice(0, i), rest: parts.slice(i).join('/') };
}

/// Direct static URL for `p`, or null when the path has no computable one
/// — a mod-pinned key, or a path naming a layer this base does not serve.
/// Returning null is never a failure: the caller falls through to
/// /casc-contents/, which resolves everything.
export function directUrl(p, hd) {
    const name = mirrorName(p);
    if (hasMountPrefix(name)) return null;
    const { layers, rest } = splitLayers(name);
    for (const layer of layers) {
        // The base is always rooted at war3.w3mod, and the HD base adds
        // _hd.w3mod. A path repeating either of those is addressing the
        // same bytes; a path naming any other layer is asking for content
        // this base does not carry, so it has no direct URL.
        if (layer === 'war3.w3mod') continue;
        if (layer === '_hd.w3mod' && hd) continue;
        return null;
    }
    return directBase(hd) + encodePath(rest);
}

/// The authoritative /casc-contents/ URL. `withContext` false omits the
/// mod-stack selector entirely, for mode-agnostic engine content (the IBL
/// probes) that exists in one layer only.
export function cascContentsUrl(p, { hd = false, withContext = true } = {}) {
    const url = CASC_CONTENTS + encodePath(requestName(p));
    if (!withContext) return url;
    return url + (hd ? '&context=hd' : '&context=sd');
}

/// The ordered candidate list for one provider-relative path: computable
/// fast path first, authoritative backstop second. Pure — no fetching, no
/// viewer state beyond the mode flag.
///
/// `direct: false` drops the fast path, for content the caller knows is
/// only reachable through the backstop.
export function hiveCandidates(p, { hd = false, withContext = true, direct = true } = {}) {
    const out = [];
    if (direct) {
        const d = directUrl(p, hd);
        if (d) out.push(d);
    }
    out.push(cascContentsUrl(p, { hd, withContext }));
    return out;
}
