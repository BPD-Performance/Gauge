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

  // ---------------- Map drawing ----------------
  function allPts() {
    const pts = [];
    (cur.outline || []).forEach(w => w.forEach(p => pts.push(p)));
    cur.g.forEach(g => pts.push([g[0], g[1]]));
    if (gpsStatus.fix) pts.push([gpsStatus.lat, gpsStatus.lon]);
    return pts;
  }
  function fitView() {
    const pts = allPts();
    if (!pts.length) { view = null; return; }
    const lat0 = pts.reduce((a, p) => a + p[0], 0) / pts.length, lon0 = pts.reduce((a, p) => a + p[1], 0) / pts.length;
    const P = proj(lat0, lon0);
    let mx = 50;
    pts.forEach(p => { const [x, y] = P.xy(p[0], p[1]); mx = Math.max(mx, Math.abs(x), Math.abs(y)); });
    view = { P, scale: mx * 1.1 };
  }
  function draw() {
    const c = $t("trkCv"), dpr = window.devicePixelRatio || 1, W = c.clientWidth * dpr, H = c.clientHeight * dpr;
    if (c.width !== W || c.height !== H) { c.width = W; c.height = H; }
    const x = c.getContext("2d");
    x.fillStyle = "#060300"; x.fillRect(0, 0, W, H);
    if (!view) { x.fillStyle = "#C98A00"; x.font = `${13 * dpr}px "BPD Dot", monospace`; x.textAlign = "center"; x.fillText("FIND THE CIRCUIT OR LOAD A TRACK", W / 2, H / 2); return; }
    const s = Math.min(W, H) / 2 / view.scale, toPx = (la, lo) => { const [mx, my] = view.P.xy(la, lo); return [W / 2 + mx * s, H / 2 - my * s]; };
    x.strokeStyle = "#FFB000"; x.lineWidth = 3 * dpr; x.lineJoin = "round";
    (cur.outline || []).forEach(w => { x.beginPath(); w.forEach((p, i) => { const [a, b] = toPx(p[0], p[1]); i ? x.lineTo(a, b) : x.moveTo(a, b); }); x.stroke(); });
    cur.g.forEach((g, i) => {
      const [cx, cy] = toPx(g[0], g[1]), br = g[2] * Math.PI / 180, hw = g[3] / 2 * s;
      const px = Math.cos(br), py = Math.sin(br);                  // across the track
      x.strokeStyle = i ? "#FF7FC8" : "#ED008C"; x.lineWidth = 4 * dpr;
      x.beginPath(); x.moveTo(cx - px * hw, cy - py * hw); x.lineTo(cx + px * hw, cy + py * hw); x.stroke();
      x.beginPath(); x.moveTo(cx, cy); x.lineTo(cx + Math.sin(br) * 14 * dpr, cy - Math.cos(br) * 14 * dpr); x.stroke();   // direction of travel
      x.fillStyle = x.strokeStyle; x.font = `${12 * dpr}px "BPD Dot", monospace`; x.textAlign = "left";
      x.fillText(i ? "S" + i : "S/F", cx + 8 * dpr, cy - 8 * dpr);
    });
    if (gpsStatus.fix) { const [a, b] = toPx(gpsStatus.lat, gpsStatus.lon); x.fillStyle = "#2EC4FF"; x.beginPath(); x.arc(a, b, 5 * dpr, 0, 7); x.fill(); }
  }

  // Tap: place a line across the outline at the nearest point, facing along it
  function tapAt(ev) {
    if (!view) return;
    const c = $t("trkCv"), r = c.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
    const s = Math.min(c.width, c.height) / 2 / view.scale;
    const mx = ((ev.clientX - r.left) * dpr - c.width / 2) / s, my = -((ev.clientY - r.top) * dpr - c.height / 2) / s;
    let best = null;
    (cur.outline || []).forEach(w => {
      for (let i = 1; i < w.length; i++) {
        const [ax, ay] = view.P.xy(w[i - 1][0], w[i - 1][1]), [bx, by] = view.P.xy(w[i][0], w[i][1]);
        const dx = bx - ax, dy = by - ay, L2 = dx * dx + dy * dy || 1;
        const t = Math.max(0, Math.min(1, ((mx - ax) * dx + (my - ay) * dy) / L2));
        const qx = ax + dx * t, qy = ay + dy * t, d = Math.hypot(mx - qx, my - qy);
        if (!best || d < best.d) best = { d, qx, qy, br: (Math.atan2(dx, dy) * 180 / Math.PI + 360) % 360 };
      }
    });
    let lat, lon, br = 0;
    if (best && best.d < view.scale * 0.15) { [lat, lon] = view.P.ll(best.qx, best.qy); br = best.br; }
    else { [lat, lon] = view.P.ll(mx, my); br = cur.g.length ? cur.g[0][2] : 0; }
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
      <canvas id="trkCv" style="display:block;width:100%;height:320px;border:2px solid var(--line);touch-action:manipulation"></canvas>
      <p class="lead" id="trkMsg" role="status">Tap the outline to place each line. You can also hold the gauge's LAP page for 2 seconds as you drive over a spot.</p>
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
    $t("trkCv").onclick = tapAt;
    $t("trkSend").onclick = () => { cur.name = $t("trkName").value || "Track"; if (!cur.g.length) return alert("Place a start/finish line first."); send(cur); };
    $t("trkSave").onclick = () => { cur.name = $t("trkName").value || "Track"; const i = lib.findIndex(t => t.name === cur.name); const copy = JSON.parse(JSON.stringify(cur)); if (i >= 0) lib[i] = copy; else lib.push(copy); saveLib(); renderLib(); $t("trkMsg").textContent = "Saved on this device."; };
    $t("trkClr").onclick = async () => { if (confirm("Clear the track on the gauge?")) { await cmd({ cmd: "trkclr" }).catch(e => alert(e.message)); } };
    fitView(); renderGates(); renderLib(); draw(); loadGps();
  }

  return { render, redraw: () => { if ($t("trkCv")) draw(); } };
})();
