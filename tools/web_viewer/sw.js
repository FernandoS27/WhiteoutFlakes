// Cache-first proxy for Hive's CASC mirror. Overrides Hive's
// `Cache-Control: no-cache` since CASC content is effectively immutable.
// Local assets (index.html, wf-core.wasm, etc.) bypass — they need the
// no-store policy serve_nocache.py emits for dev iteration.

const CACHE_NAME = 'wf-hive-v1';
const HIVE_ORIGIN = 'https://www.hiveworkshop.com';

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
    if (!req.url.startsWith(HIVE_ORIGIN)) return;
    event.respondWith((async () => {
        const cache = await caches.open(CACHE_NAME);
        // ignoreVary survives Accept-Encoding fluctuations.
        const hit = await cache.match(req, { ignoreVary: true });
        if (hit) return hit;
        const resp = await fetch(req);
        if (resp && resp.ok) {
            // clone() — the body stream is one-shot.
            cache.put(req, resp.clone()).catch(() => { /* quota etc. */ });
        }
        return resp;
    })());
});
