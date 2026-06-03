// Cache-first front for Hive (production www). /repository-files/
// (user-uploaded models + textures) send no CORS headers, so those route
// through the dev server's same-origin /hive-proxy/; /casc-contents/ and
// /assets/ send CORS and are fetched direct. Local assets bypass — they
// need the no-store policy serve_nocache.py emits for dev iteration.

const CACHE_NAME = 'wf-hive-v4';
const HIVE_PROD_ORIGIN = 'https://www.hiveworkshop.com';
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

    // Prod Hive. /repository-files/ send no CORS — route through the
    // same-origin /hive-proxy/ (dev server forwards to www). Everything
    // else on www (assets, casc-contents) sends CORS — fetch direct.
    // Cache key stays the original request either way.
    if (req.url.startsWith(HIVE_PROD_ORIGIN + '/')) {
        const rest = req.url.slice(HIVE_PROD_ORIGIN.length + 1);
        const target = rest.startsWith('repository-files/')
            ? new URL(HIVE_PROXY_PREFIX + rest, self.location.origin).toString()
            : req.url;
        event.respondWith(serveCached(req, target));
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
