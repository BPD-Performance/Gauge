"use strict";
// GPS settings and the track editor: circuit outline from OpenStreetMap, start/finish and
// sector lines placed by tapping the map, a track library kept on this device, and
// send/load to the gauge. Lines can also be set on the gauge by holding the LAP page.

const Track = (() => {
  const $t = id => document.getElementById(id);
  const R = 6371000;
  let lib = JSON.parse(localStorage.getItem("trackLib") || "[]");
  let cur = { name: "", g: [] };            // track being edited: g = [[lat, lon, bearing, width], ...]
  let outline = [];                         // OSM ways: [[[lat, lon], ...], ...]
  let mode = "sf", gpsCfg = null, view = null;
  const saveLib = () => localStorage.setItem("trackLib", JSON.stringify(lib));

  // ---------------- Geometry ----------------
  function proj(lat0, lon0) {
    const k = Math.cos(lat0 * Math.PI / 180);
    return {
      xy: (la, lo) => [(lo - lon0) * Math.PI / 180 * R * k, (la - lat0) * Math.PI / 180 * R],
      ll: (x, y) => [lat0 + y / R * 180 / Math.PI, lon0 + x / (R * k) * 180 / Math.PI],
    };
  }

  // ---------------- OpenStreetMap outline (Overpass) ----------------
  async function findOutline(lat, lon) {
    $t("trkMsg").textContent = "Looking for a circuit near you…";
    const q = `[out:json][timeout:20];(way(around:2500,${lat},${lon})["highway"="raceway"];way(around:2500,${lat},${lon})["sport"="motor"]["leisure"="track"];);out geom;`;
    try {
      const r = await fetch("https://overpass-api.de/api/interpreter", { method: "POST", body: "data=" + encodeURIComponent(q) });
      const j = await r.json();
      outline = j.elements.filter(e => e.geometry).map(e => e.geometry.map(p => [p.lat, p.lon]));
      $t("trkMsg").textContent = outline.length ? `Found ${outline.length} track section${outline.length > 1 ? "s" : ""} on OpenStreetMap.` : "No circuit found within 2.5 km on OpenStreetMap.";
      if (outline.length) cur.outline = outline;
      fitView(); draw();
    } catch (e) { $t("trkMsg").textContent = "Couldn't reach OpenStreetMap (needs internet): " + e.message; }
  }

  function herePos() {
    if (gpsStatus.fix) return Promise.resolve([gpsStatus.lat, gpsStatus.lon]);
    return new Promise((res, rej) => navigator.geolocation ? navigator.geolocation.getCurrentPosition(p => res([p.coords.latitude, p.coords.longitude]), e => rej(new Error(e.message)), { enableHighAccuracy: true, timeout: 15000 }) : rej(new Error("no location on this device")));
  }

  // ---------------- Map (Leaflet + OpenStreetMap) ----------------
  // OpenStreetMap tiles under everything; the outline, lines and GPS dot are drawn on top.
  // Tiles you've looked at are kept by the service worker, so the map works offline at the track.
  let map = null, layer = null, gpsDot = null, liveTimer = null, street = null, sat = null, labels = null;
  const AMBER = "#FFB000", PINK = "#ED008C", PINK2 = "#FF7FC8";

  function ensureMap() {
    if (map || !window.L || !$t("trkMap")) return map;
    map = L.map("trkMap", { zoomControl: true, attributionControl: true, worldCopyJump: true }).setView([-25.5, 134], 4);   // Australia until we know where you are
    street = L.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png", { maxZoom: 20, maxNativeZoom: 19, attribution: "&copy; OpenStreetMap contributors" });
    sat = L.tileLayer("https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}",
      { maxZoom: 20, maxNativeZoom: 19, attribution: "Imagery &copy; Esri, Maxar, Earthstar Geographics" });
    labels = L.layerGroup([
      L.tileLayer("https://server.arcgisonline.com/ArcGIS/rest/services/Reference/World_Transportation/MapServer/tile/{z}/{y}/{x}", { maxZoom: 20, maxNativeZoom: 19 }),
      L.tileLayer("https://server.arcgisonline.com/ArcGIS/rest/services/Reference/World_Boundaries_and_Places/MapServer/tile/{z}/{y}/{x}", { maxZoom: 20, maxNativeZoom: 19 })]);
    layer = L.layerGroup().addTo(map);
    map.on("click", e => tapAt(e.latlng.lat, e.latlng.lng));
    applyTint();
    clearInterval(liveTimer);
    liveTimer = setInterval(() => { if (map && document.getElementById("trkMap")) drawGps(); else clearInterval(liveTimer); }, 1000);
    return map;
  }
  // Map view: hybrid (satellite + road and place names), satellite only, or street map.
  // The amber tint only applies to the street map.
  function applyTint() { const m = $t("trkMap"); if (m) m.classList.toggle("amber", (localStorage.getItem("mapLayer") || "hybrid") === "street" && localStorage.getItem("mapTint") !== "0"); }
  function setLayer(which) {
    if (!["hybrid", "sat", "street"].includes(which)) which = "hybrid";
    localStorage.setItem("mapLayer", which);
    if (map) {
      [street, sat, labels].forEach(l => { if (l && map.hasLayer(l)) map.removeLayer(l); });
      if (which === "street") street.addTo(map); else { sat.addTo(map); if (which === "hybrid") labels.addTo(map); }
    }
    if ($t("trkView")) $t("trkView").value = which;
    if ($t("trkTintBox")) $t("trkTintBox").hidden = which !== "street";
    applyTint();
  }

  function allPts() {
    const pts = [];
    (cur.outline || []).forEach(w => w.forEach(p => pts.push(p)));
    cur.g.forEach(g => pts.push([g[0], g[1]]));
    return pts;
  }
  function fitView() {
    if (!ensureMap()) return;
    const pts = allPts();
    if (pts.length) map.fitBounds(L.latLngBounds(pts), { padding: [20, 20], maxZoom: 17 });
    else if (gpsStatus.fix) map.setView([gpsStatus.lat, gpsStatus.lon], 16);
  }

  function gateEnds(g) {                      // the two ends of a line, and the tip of its direction arrow
    const P = proj(g[0], g[1]), b = g[2] * Math.PI / 180, hw = g[3] / 2;
    const ax = Math.cos(b) * hw, ay = -Math.sin(b) * hw;           // across the direction of travel
    return { a: P.ll(-ax, -ay), b: P.ll(ax, ay), tip: P.ll(Math.sin(b) * 12, Math.cos(b) * 12) };
  }
  function draw() {
    if (!ensureMap()) return;
    layer.clearLayers();
    (cur.outline || []).forEach(w => L.polyline(w, { color: AMBER, weight: 4, opacity: 0.9, interactive: false }).addTo(layer));
    cur.g.forEach((g, i) => {
      const e = gateEnds(g), col = i ? PINK2 : PINK;
      L.polyline([e.a, e.b], { color: col, weight: 5, interactive: false }).addTo(layer);
      L.polyline([[g[0], g[1]], e.tip], { color: col, weight: 3, interactive: false }).addTo(layer);
      L.marker([g[0], g[1]], { interactive: false, icon: L.divIcon({ className: "gateLbl", html: `<span style="color:${col}">${i ? "S" + i : "S/F"}</span>`, iconSize: [40, 16], iconAnchor: [-8, 20] }) }).addTo(layer);
    });
    gpsDot = null;
    drawGps();
  }
  function drawGps() {
    if (!map) return;
    const live = gpsStatus.fix && performance.now() - gpsStatus.t < 3000;
    if (!live) { if (gpsDot) { gpsDot.remove(); gpsDot = null; } return; }
    if (!gpsDot) gpsDot = L.circleMarker([gpsStatus.lat, gpsStatus.lon], { radius: 6, color: "#2EC4FF", fillColor: "#2EC4FF", fillOpacity: 1, interactive: false }).addTo(map);
    else gpsDot.setLatLng([gpsStatus.lat, gpsStatus.lon]);
  }

  // Tap: place a line across the outline at the nearest point, facing along it.
  // Away from the outline, the line goes where you tapped, facing the same way as start/finish.
  function tapAt(lat, lon) {
    const P = proj(lat, lon);
    let best = null;
    (cur.outline || []).forEach(w => {
      for (let i = 1; i < w.length; i++) {
        const [ax, ay] = P.xy(w[i - 1][0], w[i - 1][1]), [bx, by] = P.xy(w[i][0], w[i][1]);
        const dx = bx - ax, dy = by - ay, L2 = dx * dx + dy * dy || 1;
        const t = Math.max(0, Math.min(1, (-ax * dx - ay * dy) / L2));
        const qx = ax + dx * t, qy = ay + dy * t, d = Math.hypot(qx, qy);
        if (!best || d < best.d) best = { d, qx, qy, br: (Math.atan2(dx, dy) * 180 / Math.PI + 360) % 360 };
      }
    });
    let br = cur.g.length ? cur.g[0][2] : 0;                      // away from the outline: same way as start/finish
    if (best && best.d < 40) {                                     // snap within 40 m, facing along the outline
      [lat, lon] = P.ll(best.qx, best.qy); br = best.br;
      if ($t("trkRev").checked) br = (br + 180) % 360;            // OSM ways may run against the race direction
    }
    const gate = [+lat.toFixed(7), +lon.toFixed(7), +br.toFixed(1), +$t("trkWidth").value || 25];
    if (mode === "sf") { if (cur.g.length) cur.g[0] = gate; else cur.g.push(gate); }
    else { if (!cur.g.length) { $t("trkMsg").textContent = "Place the start/finish first."; return; } if (cur.g.length < 9) cur.g.push(gate); }
    renderGates(); draw();
  }

  function renderGates() {
    $t("trkGates").innerHTML = cur.g.length ? cur.g.map((g, i) => `<div class="lrow"><span>${i ? "Sector " + i : "Start/finish"}</span><span class="sm">${g[0].toFixed(5)}, ${g[1].toFixed(5)}  ${Math.round(g[2])}°</span><button class="btn ghost red" data-gdel="${i}" type="button" style="min-height:30px">X</button></div>`).join("")
      : `<div class="empty">No lines yet.</div>`;
    document.querySelectorAll("[data-gdel]").forEach(b => b.onclick = () => { const i = +b.dataset.gdel; if (i === 0) cur.g = []; else cur.g.splice(i, 1); renderGates(); draw(); });
  }
  function renderLib() {
    $t("trkLib").innerHTML = lib.length ? lib.map((t, i) => `<div class="frow"><div><div>${t.name || "Unnamed"}</div><div class="sm">${t.g.length ? t.g.length - 1 + " sectors" : "no lines"}</div></div>
      <div class="fbtn"><button class="btn" data-ledit="${i}" type="button">Edit</button><button class="btn pink" data-lsend="${i}" type="button">To gauge</button><button class="btn ghost" data-lexp="${i}" type="button">Export</button><button class="btn ghost red" data-ldel="${i}" type="button">Del</button></div></div>`).join("")
      : `<div class="empty">Saved tracks appear here.</div>`;
    document.querySelectorAll("[data-ledit]").forEach(b => b.onclick = () => { openTrack(+b.dataset.ledit); $t("trkName").scrollIntoView({ behavior: "smooth", block: "center" }); });
    document.querySelectorAll("[data-lexp]").forEach(b => b.onclick = () => { const t = lib[+b.dataset.lexp]; download(safe(t.name) + ".bpdtrack.json", JSON.stringify(t)); });
    document.querySelectorAll("[data-lsend]").forEach(b => b.onclick = () => send(lib[+b.dataset.lsend]));
    document.querySelectorAll("[data-ldel]").forEach(b => b.onclick = () => { if (confirm("Delete this track from the phone?")) { const i = +b.dataset.ldel; lib.splice(i, 1); if (editIdx === i) editIdx = -1; else if (editIdx > i) editIdx--; saveLib(); renderLib(); } });
  }

  async function send(t) {
    try { const r = await cmd({ cmd: "trkset", name: (t.name || "Track").slice(0, 23), g: t.g }); $t("trkMsg").textContent = r.ok ? "Sent to the gauge." : "Not sent: " + r.err; }
    catch (e) { $t("trkMsg").textContent = "Not sent: " + e.message; }
  }

  // ---------------- GPS settings ----------------
  async function loadGps() {
    if (!connected) return;
    try { const r = await cmd({ cmd: "gpsget" }); if (r.ok) gpsCfg = r.gps; } catch { gpsCfg = null; }
    if (gpsCfg) { $t("gpsOn").checked = !!gpsCfg.on; $t("gpsPulse").checked = !!gpsCfg.pulse; $t("gpsPpk").value = gpsCfg.ppk; }
  }

  // ---------------- Import / export ----------------
  // Own format: {"name":..., "g":[[lat, lon, bearing, width], ...], "outline":[[[lat, lon], ...]]}
  // GIS files: a 2-point line = a timing line, drawn from the LEFT edge to the RIGHT edge of the
  // track as you face the direction of travel (the first one is start/finish); longer lines,
  // polygons and GPS tracks become the outline. CSV: name,lat1,lon1,lat2,lon2 per timing line, or
  // name,lat,lon rows for outline points.
  function gateFromEnds(a, b) {
    const P = proj((a[0] + b[0]) / 2, (a[1] + b[1]) / 2);
    const [ax, ay] = P.xy(a[0], a[1]), [bx, by] = P.xy(b[0], b[1]);
    const across = (Math.atan2(bx - ax, by - ay) * 180 / Math.PI + 360) % 360;   // left -> right
    const width = Math.max(5, Math.hypot(bx - ax, by - ay));
    return [+((a[0] + b[0]) / 2).toFixed(7), +((a[1] + b[1]) / 2).toFixed(7), +((across + 270) % 360).toFixed(1), +width.toFixed(1)];
  }
  function fromLines(name, lines, outline) {                       // lines: [{name, pts:[[lat,lon],...]}]
    const gates = [], out = outline || [];
    const isSF = n => /start|finish|s\s*\/\s*f|\bsf\b/i.test(n || "");
    lines.forEach(l => { if (l.pts.length === 2) gates.push(l); else if (l.pts.length > 2) out.push(l.pts); });
    gates.sort((p, q) => (isSF(q.name) ? 1 : 0) - (isSF(p.name) ? 1 : 0));   // start/finish first, the rest in file order
    return { name: name || "Imported track", g: gates.slice(0, 9).map(l => gateFromEnds(l.pts[0], l.pts[1])), outline: out };
  }
  const xmlText = (el, tag) => { const n = el.getElementsByTagName(tag)[0]; return n ? n.textContent.trim() : ""; };
  function parseKml(txt) {
    const doc = new DOMParser().parseFromString(txt, "application/xml"), lines = [];
    [...doc.getElementsByTagName("Placemark")].forEach(pm => {
      const nm = xmlText(pm, "name");
      [...pm.getElementsByTagName("coordinates")].forEach(c => {
        const pts = c.textContent.trim().split(/\s+/).map(t => t.split(",").map(Number)).filter(p => p.length >= 2 && !isNaN(p[0])).map(p => [p[1], p[0]]);
        if (pts.length >= 2) lines.push({ name: nm, pts });
      });
    });
    return fromLines(xmlText(doc, "name"), lines);
  }
  function parseGpx(txt) {
    const doc = new DOMParser().parseFromString(txt, "application/xml"), lines = [];
    const pts = els => els.map(p => [+p.getAttribute("lat"), +p.getAttribute("lon")]);
    [...doc.getElementsByTagName("trkseg")].forEach(s => lines.push({ name: "", pts: pts([...s.getElementsByTagName("trkpt")]) }));
    [...doc.getElementsByTagName("rte")].forEach(r => lines.push({ name: xmlText(r, "name"), pts: pts([...r.getElementsByTagName("rtept")]) }));
    return fromLines(xmlText(doc, "name"), lines);
  }
  function parseGeoJson(j) {
    const lines = [], feats = j.type === "FeatureCollection" ? j.features : [j.type === "Feature" ? j : { geometry: j, properties: {} }];
    feats.forEach(f => {
      const g = f.geometry || {}, nm = (f.properties || {}).name || "";
      const add = cs => lines.push({ name: nm, pts: cs.map(c => [c[1], c[0]]) });
      if (g.type === "LineString") add(g.coordinates);
      else if (g.type === "MultiLineString") g.coordinates.forEach(add);
      else if (g.type === "Polygon") add(g.coordinates[0]);
      else if (g.type === "MultiPolygon") g.coordinates.forEach(p => add(p[0]));
    });
    return fromLines(j.name, lines);
  }
  function parseCsv(txt) {
    const rows = txt.split(/\r?\n/).map(r => r.split(",").map(s => s.trim())).filter(r => r.length >= 3);
    const lines = [], outline = [];
    rows.forEach(r => {
      const n = r.slice(1).map(Number);
      if (n.length >= 4 && n.slice(0, 4).every(v => !isNaN(v))) lines.push({ name: r[0], pts: [[n[0], n[1]], [n[2], n[3]]] });
      else if (n.length >= 2 && !isNaN(n[0]) && !isNaN(n[1])) outline.push([n[0], n[1]]);
    });
    return fromLines("", lines, outline.length > 2 ? [outline] : []);
  }
  async function unzipFirst(buf, want) {               // minimal zip reader (KMZ): first file matching want
    const v = new DataView(buf);
    let e = buf.byteLength - 22;
    while (e >= 0 && v.getUint32(e, true) !== 0x06054b50) e--;
    if (e < 0) throw new Error("not a zip file");
    let p = v.getUint32(e + 16, true);
    const count = v.getUint16(e + 10, true);
    for (let i = 0; i < count; i++) {
      const method = v.getUint16(p + 10, true), csize = v.getUint32(p + 20, true), nlen = v.getUint16(p + 28, true),
        xlen = v.getUint16(p + 30, true), clen = v.getUint16(p + 32, true), off = v.getUint32(p + 42, true);
      const name = new TextDecoder().decode(new Uint8Array(buf, p + 46, nlen));
      p += 46 + nlen + xlen + clen;
      if (!want.test(name)) continue;
      const start = off + 30 + v.getUint16(off + 26, true) + v.getUint16(off + 28, true);
      const data = new Uint8Array(buf, start, csize);
      if (method === 0) return new TextDecoder().decode(data);
      const ds = new Blob([data]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
      return await new Response(ds).text();
    }
    throw new Error("no track found inside the file");
  }
  async function importFile(file) {
    const ext = (file.name.split(".").pop() || "").toLowerCase();
    let tracks;
    if (ext === "kmz") tracks = [parseKml(await unzipFirst(await file.arrayBuffer(), /\.kml$/i))];
    else {
      const txt = await file.text();
      if (ext === "kml") tracks = [parseKml(txt)];
      else if (ext === "gpx") tracks = [parseGpx(txt)];
      else if (ext === "csv" || ext === "txt") tracks = [parseCsv(txt)];
      else {
        const j = JSON.parse(txt);
        if (Array.isArray(j)) tracks = j;                                   // our own export (library)
        else if (j.g) tracks = [j];                                         // our own single track
        else tracks = [parseGeoJson(j)];                                    // GeoJSON
      }
    }
    tracks = tracks.filter(t => t && (t.g || []).length || (t.outline || []).length);
    if (!tracks.length) throw new Error("no timing lines or outline found in that file");
    tracks.forEach(t => {
      if (!t.name || t.name === "Imported track") t.name = file.name.replace(/\.[^.]+$/, "");
      const i = lib.findIndex(x => x.name === t.name);
      if (i >= 0) lib[i] = t; else lib.push(t);
    });
    saveLib(); renderLib();
    openTrack(lib.findIndex(x => x.name === tracks[0].name));
    $t("trkMsg").textContent = `Imported ${tracks.length} track${tracks.length > 1 ? "s" : ""}: ${tracks.map(t => t.name).join(", ")}. Check each line's arrow points the way you drive.`;
  }
  function download(name, text) {
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([text], { type: "application/json" }));
    a.download = name; a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 5000);
  }
  const safe = n => (n || "track").replace(/[^\w\- ]+/g, "").trim().replace(/\s+/g, "_");

  // ---------------- Editing a library track ----------------
  let editIdx = -1;                            // library entry being edited (-1 = new)
  function openTrack(i) {
    if (i < 0) return;
    editIdx = i;
    cur = JSON.parse(JSON.stringify(lib[i]));
    $t("trkName").value = cur.name || "";
    $t("trkRev").checked = false;
    $t("trkEditing").textContent = `Editing "${cur.name}" from the library. Change the name here and press Save to library to rename it.`;
    fitView(); renderGates(); draw();
    $t("trkName").focus();
  }

  // ---------------- GPS settings + track editor ----------------
  function render(box) {
    box.innerHTML = `
      <details class="sec"><summary><span>GPS module</span><span class="pm" aria-hidden="true"></span></summary><div class="secbody">
        <div class="list">
          <div class="lrow"><label class="ck2"><input type="checkbox" id="gpsOn"> GPS fitted to the gauge</label></div>
          <div class="lrow"><label class="ck2"><input type="checkbox" id="gpsPulse"> Speed pulses to the ECU</label></div>
          <div class="lrow"><span>Pulses per km (match the DPI setting in NSP)</span><span><input class="num" id="gpsPpk" inputmode="numeric" value="5000"></span></div>
        </div>
        <p class="lead">Turning the GPS on uses the gauge's UART header, so the gauge restarts and its debug serial output stops. Turn the gauge's LAP page on in Setup, Gauge pages.</p>
        <div class="actions"><button class="btn pink" id="gpsSave" type="button">Save GPS settings to gauge</button></div>
      </div></details>
      <label style="margin-top:12px">Track</label>
      <p class="lead" id="trkEditing">New track. Give it a name, find the circuit or tap the map, then save it to the library.</p>
      <div class="grid"><div><label for="trkName">Name</label><input id="trkName" placeholder="Sandown"></div>
        <div><label for="trkWidth">Line width (m)</label><input id="trkWidth" class="num" style="width:100%" value="25" inputmode="numeric"></div></div>
      <div class="actions" style="margin:10px 0">
        <button class="btn" id="trkFind" type="button">Find circuit here</button>
        <button class="btn ghost" id="trkLoad" type="button">Load from gauge</button>
        <button class="btn ghost" id="trkNew" type="button">New track</button>
      </div>
      <div class="seg" id="trkMode"><button class="btn" data-tm="sf" type="button">Tap = start/finish</button><button class="btn ghost" data-tm="sec" type="button">Tap = sector</button></div>
      <label class="ck2" style="margin:8px 0"><input type="checkbox" id="trkRev"> Race direction is the other way (flips every line)</label>
      <div class="grid" style="margin-bottom:8px;grid-template-columns:repeat(auto-fit,minmax(200px,1fr))"><div><label for="trkView">Map view</label>
        <select id="trkView"><option value="hybrid">Hybrid (satellite + names)</option><option value="sat">Satellite</option><option value="street">Street map</option></select></div>
        <div id="trkTintBox"><label for="trkTint">Street map colour</label><select id="trkTint"><option value="1">Amber</option><option value="0">Normal</option></select></div></div>
      <div id="trkMap" class="trkmap"></div>
      <div class="actions" style="margin:8px 0"><button class="btn ghost" id="trkMe" type="button">Go to me</button><button class="btn ghost red" id="trkTiles" type="button">Clear saved maps</button></div>
      <p class="lead" id="trkMsg" role="status">Tap the map to place each line (it snaps to the outline if there is one). You can also hold the gauge's LAP page for 2 seconds as you drive over a spot.</p>
      <div class="list" id="trkGates"></div>
      <div class="actions"><button class="btn pink" id="trkSend" type="button">Send track to gauge</button><button class="btn ghost" id="trkSave" type="button">Save to library</button><button class="btn ghost red" id="trkClr" type="button">Clear gauge track</button></div>
      <label>Track library (this device)</label>
      <div class="actions" style="margin:6px 0 10px"><button class="btn ghost" id="trkImport" type="button">Import track file</button><button class="btn ghost" id="trkExportAll" type="button">Export all</button>
        <input type="file" id="trkFile" accept=".json,.bpdtrack,.kml,.kmz,.gpx,.geojson,.csv,.txt" hidden></div>
      <p class="lead">Imports BPD track files, Google Earth KML/KMZ, GPX, GeoJSON and CSV. Draw each timing line from the left edge to the right edge of the track as you face the way you drive; name the start/finish "Start" or "S/F". CSV rows: name,lat1,lon1,lat2,lon2.</p>
      <div class="list" id="trkLib"></div>`;
    $t("gpsSave").onclick = async () => {
      try { const r = await cmd({ cmd: "gpsset", on: $t("gpsOn").checked ? 1 : 0, pulse: $t("gpsPulse").checked ? 1 : 0, ppk: +$t("gpsPpk").value || 5000 });
        if (r.ok && confirm("Saved. Restart the gauge now so it takes effect?")) await cmd({ cmd: "restart" }).catch(() => {}); }
      catch (e) { alert(e.message); }
    };
    $t("trkFind").onclick = async () => { try { const [la, lo] = await herePos(); await findOutline(la, lo); } catch (e) { $t("trkMsg").textContent = "No position: " + e.message; } };
    $t("trkLoad").onclick = async () => { try { const r = await cmd({ cmd: "trkget" }); if (r.ok) { editIdx = lib.findIndex(t => t.name === r.track.name); cur = { name: r.track.name, g: r.track.g, outline: cur.outline }; $t("trkName").value = cur.name; $t("trkRev").checked = false; fitView(); renderGates(); draw(); } } catch (e) { alert(e.message); } };
    $t("trkNew").onclick = () => { editIdx = -1; cur = { name: "", g: [] }; $t("trkName").value = ""; $t("trkRev").checked = false; $t("trkEditing").textContent = "New track. Give it a name, find the circuit or tap the map, then save it to the library."; renderGates(); draw(); };
    $t("trkMode").querySelectorAll("button").forEach(b => b.onclick = () => { mode = b.dataset.tm; $t("trkMode").querySelectorAll("button").forEach(o => o.classList.toggle("ghost", o !== b)); });
    $t("trkRev").onchange = () => { cur.g.forEach(g => g[2] = +((g[2] + 180) % 360).toFixed(1)); renderGates(); draw(); };   // flip every line now
    $t("trkView").value = localStorage.getItem("mapLayer") || "hybrid";
    $t("trkView").onchange = () => setLayer($t("trkView").value);
    $t("trkTint").value = localStorage.getItem("mapTint") === "0" ? "0" : "1";
    $t("trkTint").onchange = () => { localStorage.setItem("mapTint", $t("trkTint").value); applyTint(); };
    $t("trkMe").onclick = async () => { try { const [la, lo] = await herePos(); ensureMap(); map.setView([la, lo], 16); } catch (e) { $t("trkMsg").textContent = "No position: " + e.message; } };
    $t("trkTiles").onclick = async () => { if (confirm("Delete the map tiles saved on this device? They download again next time you look at an area.")) { await caches.delete("bpd-tiles"); $t("trkMsg").textContent = "Saved maps cleared."; } };
    $t("trkSend").onclick = () => { cur.name = $t("trkName").value || "Track"; if (!cur.g.length) return alert("Place a start/finish line first."); send(cur); };
    $t("trkSave").onclick = () => {
      cur.name = $t("trkName").value.trim() || "Track";
      const copy = JSON.parse(JSON.stringify(cur));
      if (editIdx >= 0 && lib[editIdx]) lib[editIdx] = copy;            // editing: update (and rename) this entry
      else { const i = lib.findIndex(t => t.name === cur.name); if (i >= 0) lib[i] = copy; else lib.push(copy); editIdx = lib.indexOf(copy); }
      saveLib(); renderLib(); $t("trkMsg").textContent = `Saved "${cur.name}" on this device.`;
      $t("trkEditing").textContent = `Editing "${cur.name}" from the library. Change the name here and press Save to library to rename it.`;
    };
    $t("trkClr").onclick = async () => { if (confirm("Clear the track on the gauge?")) { await cmd({ cmd: "trkclr" }).catch(e => alert(e.message)); } };
    $t("trkImport").onclick = () => $t("trkFile").click();
    $t("trkFile").onchange = async e => { const f = e.target.files[0]; e.target.value = ""; if (!f) return; try { await importFile(f); } catch (err) { $t("trkMsg").textContent = "Couldn't import: " + err.message; } };
    $t("trkExportAll").onclick = () => { if (!lib.length) return alert("The library is empty."); download("BPD_tracks.json", JSON.stringify(lib)); };
    map = null; clearInterval(liveTimer);
    setTimeout(() => { ensureMap(); setLayer(localStorage.getItem("mapLayer") || "hybrid"); fitView(); draw(); }, 0);
    renderGates(); renderLib(); loadGps();
  }


  // Called when the GPS section is opened: Leaflet needs to re-measure a map that was hidden
  return { render, redraw: () => { if (map) { map.invalidateSize(); if (!allPts().length && map.getZoom() < 6) fitView(); } } };
})();
