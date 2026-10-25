// DermEdge service worker: makes the installed app open and work offline.
//
//   App shell (pages, scripts, fonts, icons, the small model JSONs): network first, so a new
//     deploy shows up on the next visit; the cached copy when offline.
//   ONNX Runtime from the CDN (onnxruntime-web@1.20.1, a pinned version): cache first.
//   The ONNX model files (/models/*.onnx, ~135 MB): left to engine.mjs, which already keeps them
//     in its own Cache API store; caching them here too would store them twice.
//   /api/*: never reaches the network; local-api.js answers it inside the page.
// Bump VERSION when the list of shell files changes.
const VERSION = 'v1';
const SHELL = `dermedge-shell-${VERSION}`, CDN = `dermedge-cdn-${VERSION}`;
const SHELL_FILES = [
  '/', '/index.html', '/manifest.webmanifest',
  '/js/engine.mjs', '/js/local-api.js', '/js/band.js', '/js/wearable-core.js',
  '/static/chart.umd.min.js', '/static/fonts/fonts.css', '/static/fonts/Figtree-variable.woff2',
  '/static/fonts/YoungSerif-400.woff2', '/icons/icon-192.png', '/icons/icon-512.png',
];

self.addEventListener('install', e => {
  e.waitUntil(caches.open(SHELL).then(c => c.addAll(SHELL_FILES)).then(() => self.skipWaiting()));
});

self.addEventListener('activate', e => {
  e.waitUntil((async () => {
    for (const k of await caches.keys()) if (k.startsWith('dermedge-') && ![SHELL, CDN, 'dermedge-models'].includes(k)) await caches.delete(k);
    await self.clients.claim();
  })());
});

async function networkFirst(req, cacheName) {
  const cache = await caches.open(cacheName);
  try {
    const res = await fetch(req);
    if (res.ok) cache.put(req, res.clone());
    return res;
  } catch (err) {
    const hit = await cache.match(req, { ignoreSearch: req.mode === 'navigate' });
    if (hit) return hit;
    if (req.mode === 'navigate') { const page = await cache.match('/'); if (page) return page; }
    throw err;
  }
}

async function cacheFirst(req, cacheName) {
  const cache = await caches.open(cacheName), hit = await cache.match(req);
  if (hit) return hit;
  const res = await fetch(req);
  if (res.ok || res.type === 'opaque') cache.put(req, res.clone());
  return res;
}

self.addEventListener('fetch', e => {
  const req = e.request;
  if (req.method !== 'GET') return;
  const url = new URL(req.url);
  if (url.origin === location.origin) {
    if (url.pathname.startsWith('/api/') || url.pathname.endsWith('.onnx')) return;
    e.respondWith(networkFirst(req, SHELL));
  } else if (url.hostname === 'cdn.jsdelivr.net' && url.pathname.includes('/onnxruntime-web@')) {
    e.respondWith(cacheFirst(req, CDN));
  }
});
