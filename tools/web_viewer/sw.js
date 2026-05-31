// Cache-first proxy for Hive's CASC mirror, plus a transparent reroute
// of beta2.hiveworkshop.com (CORS-blocked + auth-gated) through the dev
// server's /hive-proxy/* endpoint. Local assets (index.html, wf-core.wasm,
// etc.) bypass — they need the no-store policy serve_nocache.py emits
// for dev iteration.

const CACHE_NAME = 'wf-hive-v2';
const HIVE_PROD_ORIGIN = 'https://www.hiveworkshop.com';
const HIVE_BETA_ORIGIN = 'https://beta2.hiveworkshop.com';
const HIVE_PROXY_PREFIX = '/hive-proxy/';

self.addEventListener('install', () => {
    self.skipWaiting();
});

self.addEventListener('activate', (event) => {
    event.waitUntil((async () => {
        // Drop older-versioned caches.
        const names = await caches.keys();
        await Promise.all(names.filter(n => n !== CACHE_NAME).map(n => caches.delete(n)));
        await self.clients.claim();
    })());
});

self.addEventListener('fetch', (event) => {
    const req = event.request;
    if (req.method !== 'GET') return;

    // beta2 → dev-server proxy. The proxy injects Basic auth and bypasses
    // the cross-origin CORS gate. Cache key stays the original beta URL so
    // repeated requests hit regardless of how the path got served.
    if (req.url.startsWith(HIVE_BETA_ORIGIN + '/')) {
        const rest = req.url.slice(HIVE_BETA_ORIGIN.length + 1);
        const proxied = new URL(HIVE_PROXY_PREFIX + rest, self.location.origin).toString();
        event.respondWith(serveCached(req, proxied));
        return;
    }
    // Prod Hive — direct fetch, just cache it.
    if (req.url.startsWith(HIVE_PROD_ORIGIN + '/')) {
        event.respondWith(serveCached(req, req.url));
        return;
    }
});

async function serveCached(cacheKeyReq, fetchUrl) {
    const cache = await caches.open(CACHE_NAME);
    // ignoreVary survives Accept-Encoding fluctuations.
    const hit = await cache.match(cacheKeyReq, { ignoreVary: true });
    if (hit) return hit;
    let resp;
    try {
        resp = await fetch(fetchUrl);
    } catch (e) {
        // Surface the network error to the caller — don't synthesize a
        // 502, which would mask CORS / connectivity issues.
        throw e;
    }
    if (resp && resp.ok) {
        // clone() — the body stream is one-shot.
        cache.put(cacheKeyReq, resp.clone()).catch(() => { /* quota etc. */ });
    }
    return resp;
}
