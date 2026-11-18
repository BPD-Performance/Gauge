// Keeps the app working with no internet (e.g. at the track). New versions load when online.
// Map tiles are kept in their own cache ("bpd-tiles") that survives app updates; clear it from
// the GPS section of the Setup tab.
const CACHE = "bpd-gauge-v30";
const TILES = "bpd-tiles";
const FILES = ["./", "index.html", "app.js", "haltech.js", "gauge.js", "demo.js", "logs.js", "track.js", "profile.js", "bpddot.ttf", "logo-flames.png", "manifest.webmanifest",
  "icon-192.png", "icon-512.png", "icon-maskable-512.png", "leaflet/leaflet.js", "leaflet/leaflet.css",
  "leaflet/images/layers.png", "leaflet/images/layers-2x.png", "leaflet/images/marker-icon.png", "leaflet/images/marker-icon-2x.png", "leaflet/images/marker-shadow.png"];
// Both the install and every page load bypass the browser's own 10-minute cache ("reload" / "no-cache"), so an update can never store or show a mix of old and new files.
self.addEventListener("install", e => { e.waitUntil(caches.open(CACHE).then(c => Promise.all(FILES.map(f => c.add(new Request(f, { cache: "reload" })))))); self.skipWaiting(); });
self.addEventListener("activate", e => {
  e.waitUntil(caches.keys().then(keys => Promise.all(keys.filter(k => k !== CACHE && k !== TILES).map(k => caches.delete(k)))));
  self.clients.claim();
});
self.addEventListener("fetch", e => {
  if (e.request.method !== "GET") return;
  const url = e.request.url;
  if (url.includes("tile.openstreetmap.org/") || url.includes("arcgisonline.com/ArcGIS/rest/services/")) {              // map tiles: saved copy first, download if new
    e.respondWith(caches.open(TILES).then(c => c.match(e.request).then(hit => hit || fetch(e.request).then(r => { if (r.ok) c.put(e.request, r.clone()); return r; }))));
    return;
  }
  if (url.includes("overpass-api.de")) return;               // circuit search: always live
  e.respondWith(fetch(e.request, { cache: "no-cache" }).then(r => {
    const copy = r.clone();
    if (url.startsWith("http://") || url.startsWith("https://"))   // browser extensions cannot be cached
      caches.open(CACHE).then(c => c.put(e.request, copy)).catch(() => {});
    return r;
  }).catch(() => caches.match(e.request, { ignoreSearch: true })));
});
