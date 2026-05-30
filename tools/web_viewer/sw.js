// Service Worker — persistently caches GET responses from Hive's CASC
// mirror so repeat loads of the same model skip the network entirely.
//
// Why this exists: Hive returns `Cache-Control: private, no-cache,
// max-age=0` on /casc-contents/ responses, which forces the HTTP cache
// to revalidate on every reload — best case a 304, still a round-trip.
// CASC content is effectively immutable (a given path resolves to one
// file forever, give or take patch days), so we override that with a
// cache-first strategy stored in CacheStorage. Hits resolve in ~0 ms.
//
// Scope: only intercepts hiveworkshop.com requests. Local /index.html,
// /wf-viewer.js, /wf-core.wasm etc. are untouched and follow the
// no-cache headers serve_nocache.py emits.

const CACHE_NAME = 'wf-hive-v1';
const HIVE_ORIGIN = 'https://www.hiveworkshop.com';

self.addEventListener('install', () => {
    // Take over from any previous SW version immediately — we don't
    // need the old caches around once a new build ships.
    self.skipWaiting();
});

self.addEventListener('activate', (event) => {
    event.waitUntil((async () => {
        // Drop caches from older versions to bound storage.
        const names = await caches.keys();
        await Promise.all(names.filter(n => n !== CACHE_NAME).map(n => caches.delete(n)));
        await self.clients.claim();
    })());
});

self.addEventListener('fetch', (event) => {
    const req = event.request;
    if (req.method !== 'GET') return;
    if (!req.url.startsWith(HIVE_ORIGIN)) return;
    event.respondWith((async () => {
        const cache = await caches.open(CACHE_NAME);
        // Ignore the Vary header: Hive may send `Vary: Accept-Encoding`
        // and our match would otherwise miss when the browser's
        // Accept-Encoding line varies slightly between requests.
        const hit = await cache.match(req, { ignoreVary: true });
        if (hit) return hit;
        // Cache miss — go to network. Issue a fresh request without
        // `cache: 'no-store'` so we get whatever the H2 connection
        // hands back (the 302 chain is followed automatically by the
        // browser). Successful responses (including the 200 final
        // body after a redirect) get tucked into CacheStorage so the
        // next load is a hit.
        const resp = await fetch(req);
        if (resp && resp.ok) {
            // resp.clone() because the response body is a one-shot
            // stream — we hand the original back to the page and
            // store the clone.
            cache.put(req, resp.clone()).catch(() => { /* quota etc. */ });
        }
        return resp;
    })());
});
