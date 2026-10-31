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
  let map = null, layer = null, gpsDot = null, liveTimer = null;
  const AMBER = "#FFB000", PINK = "#ED008C", PINK2 = "#FF7FC8";

  function ensureMap() {
    if (map || !window.L || !$t("trkMap")) return map;
    map = L.map("trkMap", { zoomControl: true, attributionControl: true, worldCopyJump: true }).setView([-25.5, 134], 4);   // Australia until we know where you are
    L.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png", { maxZoom: 19, attribution: "&copy; OpenStreetMap contributors" }).addTo(map);
    layer = L.layerGroup().addTo(map);
    map.on("click", e => tapAt(e.latlng.lat, e.latlng.lng));
    applyTint();
    clearInterval(liveTimer);
    liveTimer = setInterval(() => { if (map && document.getElementById("trkMap")) drawGps(); else clearInterval(liveTimer); }, 1000);
    return map;
  }
  function applyTint() { const m = $t("trkMap"); if (m) m.classList.toggle("amber", localStorage.getItem("mapTint") !== "0"); }

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
    let br = cur.g.length ? cur.g[0][2] : 0;
    if (best && best.d < 40) { [lat, lon] = P.ll(best.qx, best.qy); br = best.br; }         // snap within 40 m
    if ($t("trkRev").checked) br = (br + 180) % 360;              // OSM ways may run against the race direction
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
      <div class="fbtn"><button class="btn" data-ledit="${i}" type="button">Edit</button><button class="btn ghost" data-lsend="${i}" type="button">To gauge</button><button class="btn ghost red" data-ldel="${i}" type="button">Del</button></div></div>`).join("")
      : `<div class="empty">Saved tracks appear here.</div>`;
    document.querySelectorAll("[data-ledit]").forEach(b => b.onclick = () => { cur = JSON.parse(JSON.stringify(lib[+b.dataset.ledit])); $t("trkName").value = cur.name; fitView(); renderGates(); draw(); });
    document.querySelectorAll("[data-lsend]").forEach(b => b.onclick = () => send(lib[+b.dataset.lsend]));
    document.querySelectorAll("[data-ldel]").forEach(b => b.onclick = () => { if (confirm("Delete this track from the phone?")) { lib.splice(+b.dataset.ldel, 1); saveLib(); renderLib(); } });
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

  function render(box) {
    box.innerHTML = `
      <div class="list">
        <div class="lrow"><label class="ck2"><input type="checkbox" id="gpsOn"> GPS fitted to the gauge</label></div>
        <div class="lrow"><label class="ck2"><input type="checkbox" id="gpsPulse"> Speed pulses to the ECU</label></div>
        <div class="lrow"><span>Pulses per km (match the DPI setting in NSP)</span><span><input class="num" id="gpsPpk" inputmode="numeric" value="5000"></span></div>
      </div>
      <p class="lead">Turning the GPS on uses the gauge's UART header, so the gauge restarts and its debug serial output stops. Put the lap timer page on in the page list above.</p>
      <div class="actions"><button class="btn" id="gpsSave" type="button">Save GPS settings</button></div>
      <label style="margin-top:6px">Track</label>
      <div class="grid"><div><label for="trkName">Name</label><input id="trkName" placeholder="Sandown"></div>
        <div><label for="trkWidth">Line width (m)</label><input id="trkWidth" class="num" style="width:100%" value="25" inputmode="numeric"></div></div>
      <div class="actions" style="margin:10px 0">
        <button class="btn" id="trkFind" type="button">Find circuit here</button>
        <button class="btn ghost" id="trkLoad" type="button">Load from gauge</button>
      </div>
      <div class="seg" id="trkMode"><button class="btn" data-tm="sf" type="button">Tap = start/finish</button><button class="btn ghost" data-tm="sec" type="button">Tap = sector</button></div>
      <label class="ck2" style="margin:8px 0"><input type="checkbox" id="trkRev"> Race direction is the other way</label>
      <div id="trkMap" class="trkmap"></div>
      <div class="actions" style="margin:8px 0"><button class="btn ghost" id="trkTint" type="button">Map colour</button><button class="btn ghost" id="trkMe" type="button">Go to me</button><button class="btn ghost red" id="trkTiles" type="button">Clear saved maps</button></div>
      <p class="lead" id="trkMsg" role="status">Tap the map to place each line (it snaps to the outline if there is one). You can also hold the gauge's LAP page for 2 seconds as you drive over a spot.</p>
      <div class="list" id="trkGates"></div>
      <div class="actions"><button class="btn pink" id="trkSend" type="button">Send to gauge</button><button class="btn ghost" id="trkSave" type="button">Save to library</button><button class="btn ghost red" id="trkClr" type="button">Clear gauge track</button></div>
      <label>Track library (this device)</label>
      <div class="list" id="trkLib"></div>`;
    $t("gpsSave").onclick = async () => {
      try { const r = await cmd({ cmd: "gpsset", on: $t("gpsOn").checked ? 1 : 0, pulse: $t("gpsPulse").checked ? 1 : 0, ppk: +$t("gpsPpk").value || 5000 });
        if (r.ok && confirm("Saved. Restart the gauge now so it takes effect?")) await cmd({ cmd: "restart" }).catch(() => {}); }
      catch (e) { alert(e.message); }
    };
    $t("trkFind").onclick = async () => { try { const [la, lo] = await herePos(); await findOutline(la, lo); } catch (e) { $t("trkMsg").textContent = "No position: " + e.message; } };
    $t("trkLoad").onclick = async () => { try { const r = await cmd({ cmd: "trkget" }); if (r.ok) { cur = { name: r.track.name, g: r.track.g, outline: cur.outline }; $t("trkName").value = cur.name; fitView(); renderGates(); draw(); } } catch (e) { alert(e.message); } };
    $t("trkMode").querySelectorAll("button").forEach(b => b.onclick = () => { mode = b.dataset.tm; $t("trkMode").querySelectorAll("button").forEach(o => o.classList.toggle("ghost", o !== b)); });
    $t("trkTint").onclick = () => { localStorage.setItem("mapTint", localStorage.getItem("mapTint") === "0" ? "1" : "0"); applyTint(); };
    $t("trkMe").onclick = async () => { try { const [la, lo] = await herePos(); ensureMap(); map.setView([la, lo], 16); } catch (e) { $t("trkMsg").textContent = "No position: " + e.message; } };
    $t("trkTiles").onclick = async () => { if (confirm("Delete the map tiles saved on this device? They download again next time you look at an area.")) { await caches.delete("bpd-tiles"); $t("trkMsg").textContent = "Saved maps cleared."; } };
    $t("trkSend").onclick = () => { cur.name = $t("trkName").value || "Track"; if (!cur.g.length) return alert("Place a start/finish line first."); send(cur); };
    $t("trkSave").onclick = () => { cur.name = $t("trkName").value || "Track"; const i = lib.findIndex(t => t.name === cur.name); const copy = JSON.parse(JSON.stringify(cur)); if (i >= 0) lib[i] = copy; else lib.push(copy); saveLib(); renderLib(); $t("trkMsg").textContent = "Saved on this device."; };
    $t("trkClr").onclick = async () => { if (confirm("Clear the track on the gauge?")) { await cmd({ cmd: "trkclr" }).catch(e => alert(e.message)); } };
    map = null; clearInterval(liveTimer);
    setTimeout(() => { ensureMap(); fitView(); draw(); }, 0);
    renderGates(); renderLib(); loadGps();
  }

  // Called when the GPS section is opened: Leaflet needs to re-measure a map that was hidden
  return { render, redraw: () => { if (map) { map.invalidateSize(); if (!allPts().length && map.getZoom() < 6) fitView(); } } };
})();
