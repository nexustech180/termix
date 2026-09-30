// Service worker: makes the app work offline and lets it update itself.
// IMPORTANT: bump VERSION every time you publish changes, so installed copies update.
const VERSION = '0.9.0';
const CACHE = `temix-${VERSION}`;

// Every file the app needs to run offline. Add new files here as you create them.
const ASSETS = [
  './',
  'index.html',
  'manifest.webmanifest',
  'styles.css',
  'robot.js',
  'app.js',
  'favicon-64.png',
  'icon-192.png',
  'icon-512.png',
  'icon-maskable-512.png',
  'apple-touch-icon.png',
];

self.addEventListener('install', (event) => {
  event.waitUntil(caches.open(CACHE).then((cache) => cache.addAll(ASSETS)));
});

self.addEventListener('activate', (event) => {
  // Delete caches from older versions.
  event.waitUntil(
    caches.keys().then((keys) =>
      Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k)))
    ).then(() => self.clients.claim())
  );
});

// The page asks us to activate right away when the user taps "Update".
self.addEventListener('message', (event) => {
  if (event.data === 'skipWaiting') self.skipWaiting();
  if (event.data === 'getVersion') event.source.postMessage({ version: VERSION });
});

// Cache-first for app files, falling back to the network (and caching what we fetch).
self.addEventListener('fetch', (event) => {
  const req = event.request;
  if (req.method !== 'GET' || new URL(req.url).origin !== self.location.origin) return;

  event.respondWith(
    caches.match(req, { ignoreSearch: true }).then((cached) => {
      if (cached) return cached;
      return fetch(req).then((res) => {
        if (res.ok) {
          const copy = res.clone();
          caches.open(CACHE).then((cache) => cache.put(req, copy));
        }
        return res;
      }).catch(() => req.mode === 'navigate' ? caches.match('index.html') : undefined);
    })
  );
});
