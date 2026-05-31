// Load-table support: a JSON manifest pointing at a set of model URLs
// plus per-asset texture/dependency overrides.
//
// Shape:
//   {
//     "models":    [ { "name": "<display>", "url": "<mdx/mdl url>" }, ... ],
//     "overrides": { "<lowercase relative path>": "<url>", ... }
//   }
//
// Override keys can be full relative paths
// ("units/human/footman/.../diffuse.tif") OR basenames ("diffuse.tif").
// Matches mirror what HiveApp's local-directory index already does.

// Lowercase + forward-slash an override-map key so the lookup is
// invariant under case and path separators.
function normKey(k) {
    return String(k).toLowerCase().replaceAll('\\', '/');
}

// Recognises URLs the renderer must pass through to the network rather
// than re-resolving against the override / CASC fallback chain.
const ABSOLUTE_URL_RX = /^(https?:|blob:|data:|file:)/i;

export function isAbsoluteUrl(s) {
    return typeof s === 'string' && ABSOLUTE_URL_RX.test(s);
}

// Build the override Map<lowercase-path, url> that the path solver
// consults. Returns null for a missing / empty override block so the
// caller can short-circuit (`if (!overrides) skip override layer`).
export function buildOverrideMap(table) {
    const src = table && table.overrides;
    if (!src || typeof src !== 'object') return null;
    const out = new Map();
    for (const [k, v] of Object.entries(src)) {
        if (typeof v !== 'string') continue;
        out.set(normKey(k), v);
    }
    return out.size > 0 ? out : null;
}

// Returns a pathSolver suitable for WhiteoutViewer.setPathSolver /
// WhiteoutViewer.load. The solver:
//   1. passes absolute URLs through unchanged (so viewer.load(url,…)
//      hits the network with the URL the table specified);
//   2. on an override hit, returns [override_url, …baseSolver_result]
//      — the asset pump tries each URL in order, so a dead override
//      falls through to the host's normal resolution (e.g. CASC);
//   3. with no override, defers entirely to `baseSolver(name)`.
//
// `baseSolver` is what the host would have used without a load-table —
// e.g. HiveApp's local-index/CASC chain. Wrapping rather than replacing
// it means a partial overrides block still benefits from the host's
// normal resolution for unspecified assets, and a 404 on an override
// doesn't fail the asset outright.
export function makeLoadTableSolver(table, baseSolver = null) {
    const overrides = buildOverrideMap(table);
    return async (name) => {
        if (isAbsoluteUrl(name)) return name;
        let overrideUrl = null;
        if (overrides && typeof name === 'string') {
            const norm = normKey(name);
            overrideUrl = overrides.get(norm)
                || overrides.get(norm.split('/').pop())
                || null;
        }
        const baseResult = baseSolver
            ? await Promise.resolve(baseSolver(name))
            : null;
        if (!overrideUrl) return baseResult;
        if (baseResult == null) return overrideUrl;
        const baseArr = Array.isArray(baseResult) ? baseResult : [baseResult];
        return [overrideUrl, ...baseArr];
    };
}

// Pull the model list out of a table, normalized to { name, url }.
// Skips entries missing a url and falls back to the URL basename when
// name is absent.
export function tableModels(table) {
    const src = table && table.models;
    if (!Array.isArray(src)) return [];
    const out = [];
    for (const m of src) {
        if (!m || typeof m.url !== 'string') continue;
        const url = m.url;
        const name = (typeof m.name === 'string' && m.name.trim())
            ? m.name.trim()
            : url.split(/[\\/]/).pop() || url;
        out.push({ name, url });
    }
    return out;
}
