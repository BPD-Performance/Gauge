"use strict";
// Data logs: file manager for the gauge's TF card, download over Bluetooth (kept on this
// device so they open offline), the log viewer with playback, replay onto the Gauge tab,
// CSV export, and the logging settings. File format: see Data_Log.h in the firmware.

const Logs = (() => {
  // ---------------- Channels (same ids and scaling as the firmware) ----------------
  const CH = [
    { n: "RPM", s: 1, k: "n", u: "RPM", c: "#FFB000" },        { n: "BOOST", s: 10, k: "p", c: "#ED008C" },
    { n: "TGT BOOST", s: 10, k: "p", c: "#FF7FC8" },            { n: "AFR", s: 1000, k: "l", c: "#2EC4FF" },
    { n: "TGT AFR", s: 1000, k: "l", c: "#9BE7FF" },            { n: "ETHANOL", s: 10, k: "n", u: "%", c: "#D4FF3A" },
    { n: "OIL P", s: 10, k: "p", c: "#7CFF4F" },                { n: "OIL T", s: 10, k: "t", c: "#2EF0C0" },
    { n: "WATER", s: 10, k: "t", c: "#E8E8E8" },                { n: "COOLANT P", s: 10, k: "p", c: "#A0A0A0" },
    { n: "FUEL P", s: 10, k: "p", c: "#B98CFF" },               { n: "INJ DUTY", s: 10, k: "n", u: "%", c: "#FFD86B" },
    { n: "IAT", s: 10, k: "t", c: "#FF7A1F" },                  { n: "BATT", s: 100, k: "n", u: "V", c: "#C9A0FF" },
    { n: "THROTTLE", s: 10, k: "n", u: "%", c: "#00E5FF" },     { n: "SPEED", s: 10, k: "s", c: "#FFB38A" },
    { n: "GEAR", s: 1, k: "g", u: "", c: "#A6FF8F" },           { n: "IGN ANGLE", s: 10, k: "n", u: "°", c: "#F2A0FF" },
    { n: "GPS", s: 10, k: "s", c: "#4FFFE0" },                  // bit 18: GPS block (position, GPS speed, lap)
  ];
  const NUM16 = 18, GPS_BIT = 18;
  const HDR = 32;
  const unitsOf = () => ({ pu: S ? S.pu : 2, tu: S ? S.tu : 0, su: S ? S.su : 0, lam: S ? S.lam : 1, sto: S ? S.stoich : 14.7 });

  // Real value -> shown value and unit (follows the gauge's unit settings)
  function show(ch, v) {
    if (v === null || isNaN(v)) return ["--", ""];
    const u = unitsOf(), c = CH[ch];
    switch (c.k) {
      case "p": return u.pu == 0 ? [v.toFixed(0), "kPa"] : u.pu == 1 ? [(v / 100).toFixed(2), "bar"] : [(v / 6.894757).toFixed(1), "psi"];
      case "t": return [(u.tu == 1 ? v * 9 / 5 + 32 : v).toFixed(0), u.tu == 1 ? "°F" : "°C"];
      case "s": return [(u.su == 1 ? v / 1.609344 : v).toFixed(0), u.su == 1 ? "mph" : "km/h"];
      case "l": return u.lam == 1 ? [v.toFixed(2), "λ"] : [(v * u.sto).toFixed(1), "AFR"];
      case "g": return [v < 0 ? "R" : v === 0 ? "-" : String(v), ""];
      default:  return [c.s >= 100 ? v.toFixed(1) : c.s >= 10 ? v.toFixed(1) : v.toFixed(0), c.u || ""];
    }
  }

  // ---------------- Local storage of downloaded logs (IndexedDB) ----------------
  let dbp = null;
  function db() {
    if (!dbp) dbp = new Promise((res, rej) => {
      const r = indexedDB.open("bpdlogs", 1);
      r.onupgradeneeded = () => r.result.createObjectStore("files", { keyPath: "n" });
      r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error);
    });
    return dbp;
  }
  const tx = async (mode, fn) => { const d = await db(); return new Promise((res, rej) => { const t = d.transaction("files", mode); const r = fn(t.objectStore("files")); t.oncomplete = () => res(r && r.result); t.onerror = () => rej(t.error); }); };
  const cacheGet = n => tx("readonly", s => s.get(n));
  const cachePut = rec => tx("readwrite", s => s.put(rec));
  const cacheDel = n => tx("readwrite", s => s.delete(n));
  const cacheAll = () => tx("readonly", s => s.getAll());

  // ---------------- Parsing ----------------
  function parse(buf, name) {
    const v = new DataView(buf);
    if (buf.byteLength < HDR || String.fromCharCode(...new Uint8Array(buf, 0, 4)) !== "BPDL") throw new Error("not a BPD log");
    const rate = v.getUint8(5), t0 = v.getUint32(8, true), mask = v.getUint32(12, true), preMs = v.getUint32(16, true);
    const hasGps = v.getUint8(4) >= 2 && !!(mask & (1 << GPS_BIT));
    const ids = []; for (let i = 0; i < NUM16; i++) if (mask & (1 << i)) ids.push(i);
    const rs = 4 + ids.length * 2 + (hasGps ? 12 : 0) + 2, rows = Math.floor((buf.byteLength - HDR) / rs);
    const t = new Float32Array(rows), w = new Uint16Array(rows), data = {};
    ids.forEach(id => data[id] = new Float32Array(rows));
    const lat = hasGps ? new Float64Array(rows) : null, lon = hasGps ? new Float64Array(rows) : null, lap = hasGps ? new Uint16Array(rows) : null;
    if (hasGps) data[GPS_BIT] = new Float32Array(rows);
    for (let r = 0, o = HDR; r < rows; r++, o += rs) {
      t[r] = v.getUint32(o, true) / 1000;
      ids.forEach((id, j) => { const raw = v.getInt16(o + 4 + j * 2, true); data[id][r] = raw === -32768 ? NaN : raw / CH[id].s; });
      let g = o + 4 + ids.length * 2;
      if (hasGps) {
        const la = v.getInt32(g, true), lo = v.getInt32(g + 4, true), sp = v.getUint16(g + 8, true);
        lat[r] = la === -2147483648 ? NaN : la / 1e7; lon[r] = lo === -2147483648 ? NaN : lo / 1e7;
        data[GPS_BIT][r] = sp === 0xFFFF ? NaN : sp / 10; lap[r] = v.getUint16(g + 10, true);
        g += 12;
      }
      w[r] = v.getUint16(g, true);
    }
    if (hasGps) ids.push(GPS_BIT);
    const laps = [];                               // completed laps from the lap counter
    if (hasGps) for (let r = 1, s0 = -1; r < rows; r++) if (lap[r] !== lap[r - 1]) { if (s0 >= 0 && lap[r - 1] > 0) laps.push({ n: lap[r - 1], i0: s0, i1: r, ms: Math.round((t[r] - t[s0]) * 1000) }); s0 = r; }
    const marks = [];
    for (let r = 1; r < rows; r++) if (w[r] & ~w[r - 1]) marks.push(r);       // a new warning tripped
    return { name, rate, t0, preMs, ids, rows, t, w, data, marks, dur: rows ? t[rows - 1] : 0, hasGps, lat, lon, lap, laps };
  }

  // ---------------- Download over Bluetooth ----------------
  // Bluetooth can drop packets when it's busy, so every byte is tracked and any gaps are
  // asked for again, a piece at a time, instead of failing the whole download.
  let dl = null;                                   // {buf, got: 1 per byte received}
  function onChunk(v) {                            // LIVE packet type 0x03
    if (!dl) return;
    const off = v.getUint32(2, true), n = v.byteLength - 6;
    if (off + n > dl.buf.length) return;
    dl.buf.set(new Uint8Array(v.buffer, v.byteOffset + 6, n), off);
    dl.got.fill(1, off, off + n);
  }
  const gaps = (o, len) => { const out = []; let s = -1;
    for (let i = o; i < o + len; i++) { if (!dl.got[i]) { if (s < 0) s = i; } else if (s >= 0) { out.push([s, i - s]); s = -1; } }
    if (s >= 0) out.push([s, o + len - s]); return out; };
  async function fetchRange(name, o, len) {        // one request; returns bytes the gauge says it sent
    const r = await cmd({ cmd: "rd", n: name, o, l: len });
    if (!r.ok) throw new Error(r.err || "read failed");
    await new Promise(res => setTimeout(res, 60));                 // let the last packets land
    return r.l;
  }
  async function download(f, progress) {
    const CHUNK = 4096;
    dl = { buf: new Uint8Array(f.size), got: new Uint8Array(f.size) };
    try {
      for (let off = 0; off < f.size; off += CHUNK) {
        const want = Math.min(CHUNK, f.size - off);
        let sent = want;
        try { sent = await fetchRange(f.n, off, want); } catch (e) { if (!/no reply/.test(e.message)) throw e; }
        if (sent === 0) break;                                       // end of file
        const span = Math.min(want, sent || want);
        for (let tries = 0; tries < 6; tries++) {                    // fill any holes
          const missing = gaps(off, span);
          if (!missing.length) break;
          for (const [o, n] of missing) { try { await fetchRange(f.n, o, n); } catch (e) { if (!/no reply/.test(e.message)) throw e; } }
        }
        if (gaps(off, span).length) throw new Error("Bluetooth kept dropping data. Move closer to the gauge and try again");
        progress(Math.min(1, (off + want) / f.size));
      }
      return dl.buf.buffer;
    } finally { dl = null; }
  }

  // ---------------- File manager ----------------
  let files = [], status = null, view = null;
  const $l = id => document.getElementById(id);
  const fmtSize = b => b > 1073741824 ? (b / 1073741824).toFixed(1) + " GB" : b > 1048576 ? (b / 1048576).toFixed(1) + " MB" : Math.max(1, Math.round(b / 1024)) + " KB";
  const rowSizeOf = mask => { let n = 0; for (let i = 0; i < NUM16; i++) if (mask & (1 << i)) n++; return 4 + n * 2 + ((mask & (1 << GPS_BIT)) ? 12 : 0) + 2; };
  const fmtDur = s => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, "0")}`;
  const fmtDate = (t0, n) => t0 ? new Date(t0 * 1000).toLocaleString([], { day: "numeric", month: "short", hour: "numeric", minute: "2-digit" }) : n;

  async function refresh() {
    if (!connected) { renderFiles(); return; }
    try {
      const [l, st] = [await cmd({ cmd: "ls" }), await cmd({ cmd: "logget" })];
      files = l.ok ? l.files.sort((a, b) => (b.t0 - a.t0) || b.n.localeCompare(a.n)) : [];   // newest first
      status = st.ok ? st.log : null;
    } catch (e) { $l("logMsg").textContent = e.message; }
    renderFiles();
  }

  async function renderFiles() {
    const st = status && status.st;
    $l("logStore").innerHTML = !connected ? `<div class="empty">Connect to the gauge to see the logs on its card.</div>`
      : !st || !st.sd ? `<div class="empty">No SD card in the gauge.</div>`
      : `<div class="bar"><div style="width:${Math.max(1, st.used / st.total * 100)}%"></div></div><div class="sm">${fmtSize(st.used)} used of ${fmtSize(st.total)}</div>`;
    $l("recBtn").textContent = st && st.rec ? "Stop log" : "Start log";
    $l("recBtn").className = "btn " + (st && st.rec ? "warn" : "pink");
    $l("recState").textContent = st && st.rec ? `Recording ${st.file}, ${fmtDur(st.secs)}` : st && st.sd ? "Not recording" : "";
    const cached = await cacheAll().catch(() => []);
    const cachedNames = new Set(cached.map(c => c.n));
    $l("fileList").innerHTML = connected && files.length ? files.map((f, i) => {
      const secs = (f.size - HDR) / rowSizeOf(f.ch) / (f.rate || 20);
      return `<div class="frow" data-i="${i}"><div><div>${fmtDate(f.t0, f.n)}</div><div class="sm">${f.n}</div><div class="sm">${fmtDur(secs)}  ${fmtSize(f.size)}${cachedNames.has(f.n) ? "  ON PHONE" : ""}${f.open ? "  RECORDING" : ""}</div></div>
        <div class="fbtn"><button class="btn" data-open="${i}" type="button">Open</button><button class="btn ghost" data-csv="${i}" type="button">CSV</button>${f.open ? "" : `<button class="btn ghost red" data-del="${i}" type="button">Del</button>`}</div></div>`; }).join("")
      : `<div class="empty">${connected ? "No logs on the card." : "Not connected."}</div>`;
    const local = cached.filter(c => !files.find(f => f.n === c.n));
    $l("localList").innerHTML = local.length ? local.map(c => `<div class="frow"><div><div>${fmtDate(c.t0, c.n)}</div><div class="sm">${fmtSize(c.size)}</div></div>
      <div class="fbtn"><button class="btn" data-lopen="${c.n}" type="button">Open</button><button class="btn ghost" data-lcsv="${c.n}" type="button">CSV</button><button class="btn ghost red" data-ldel="${c.n}" type="button">Del</button></div></div>`).join("")
      : `<div class="empty">Logs you open are kept here too, so they work without the gauge.</div>`;

    document.querySelectorAll("[data-open]").forEach(b => b.onclick = () => openFile(files[+b.dataset.open], false));
    document.querySelectorAll("[data-csv]").forEach(b => b.onclick = () => openFile(files[+b.dataset.csv], true));
    document.querySelectorAll("[data-del]").forEach(b => b.onclick = () => confirmRow(b.closest(".frow"), `Delete ${files[+b.dataset.del].n}?`, async () => {
      const r = await cmd({ cmd: "rm", n: files[+b.dataset.del].n }); if (!r.ok) alert(r.err); refresh(); }));
    document.querySelectorAll("[data-lopen]").forEach(b => b.onclick = async () => showLog(await cacheGet(b.dataset.lopen)));
    document.querySelectorAll("[data-lcsv]").forEach(b => b.onclick = async () => exportCsv(await cacheGet(b.dataset.lcsv)));
    document.querySelectorAll("[data-ldel]").forEach(b => b.onclick = () => confirmRow(b.closest(".frow"), "Remove from this phone?", async () => { await cacheDel(b.dataset.ldel); renderFiles(); }));
  }

  function confirmRow(row, text, yes) {
    row.innerHTML = `<div style="color:var(--warn)">${text}</div><div class="fbtn"><button class="btn ghost" type="button" data-no>Cancel</button><button class="btn warn" type="button" data-yes>Delete</button></div>`;
    row.querySelector("[data-no]").onclick = renderFiles;
    row.querySelector("[data-yes]").onclick = () => yes().catch(e => alert(e.message));
  }

  async function getFile(f) {
    const c = await cacheGet(f.n).catch(() => null);
    if (c && c.size === f.size && !f.open) return c;
    $l("logMsg").textContent = "Downloading…";
    const buf = await download(f, p => $l("logMsg").textContent = `Downloading ${Math.round(p * 100)}%`);
    const rec = { n: f.n, size: f.size, t0: f.t0, buf };
    if (!f.open) await cachePut(rec).catch(() => {});
    $l("logMsg").textContent = "";
    return rec;
  }
  async function openFile(f, csv) {
    try { const rec = await getFile(f); csv ? exportCsv(rec) : showLog(rec); renderFiles(); }
    catch (e) { $l("logMsg").textContent = "Couldn't download: " + e.message; }
  }

  function exportCsv(rec) {
    const L = parse(rec.buf, rec.n);
    const head = ["time_s", ...L.ids.map(id => { const u = show(id, 1)[1]; return (id === GPS_BIT ? "GPS SPEED" : CH[id].n) + (u ? ` (${u})` : ""); }), ...(L.hasGps ? ["latitude", "longitude", "lap"] : []), "warnings"];
    const lines = [head.join(",")];
    for (let r = 0; r < L.rows; r++) lines.push([L.t[r].toFixed(2), ...L.ids.map(id => { const v = L.data[id][r]; return isNaN(v) ? "" : show(id, v)[0]; }),
      ...(L.hasGps ? [isNaN(L.lat[r]) ? "" : L.lat[r].toFixed(7), isNaN(L.lon[r]) ? "" : L.lon[r].toFixed(7), L.lap[r]] : []), L.w[r]].join(","));
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([lines.join("\n")], { type: "text/csv" }));
    a.download = rec.n.replace(".BPL", ".csv");
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 5000);
  }

  // ---------------- Viewer ----------------
  let L = null, cur = 0, v0 = 0, v1 = 1, lanes = false, playing = false, speed = 2, lastT = 0, sel = {}, replayOn = false;
  const cv = () => $l("logCv");

  function showLog(rec) {
    try { L = parse(rec.buf, rec.n); } catch (e) { alert(e.message); return; }
    sel = {}; L.ids.slice(0, 3).forEach(id => sel[id] = 1);
    cur = 0; v0 = 0; v1 = Math.max(1, L.dur);
    $l("viewer").hidden = false;
    $l("logTitle").textContent = fmtDate(L.t0, L.name);
    $l("logMapBox").hidden = !L.hasGps;
    renderLaps();
    buildChips(); draw();
    $l("viewer").scrollIntoView({ behavior: "smooth" });
  }

  function buildChips() {
    $l("logChips").innerHTML = L.ids.map(id => `<span class="chipck${sel[id] ? " sel" : ""}" data-ch="${id}" style="${sel[id] ? "color:" + CH[id].c : ""}"><span class="dx${sel[id] ? " on" : ""}"></span>${CH[id].n}</span>`).join("");
    document.querySelectorAll("[data-ch]").forEach(e => e.onclick = () => { const id = +e.dataset.ch; sel[id] = !sel[id]; buildChips(); draw(); });
  }

  const idxAt = t => { let lo = 0, hi = L.rows - 1; while (lo < hi) { const m = (lo + hi) >> 1; if (L.t[m] < t) lo = m + 1; else hi = m; } return lo; };

  function draw() {
    if (!L) return;
    const c = cv(), dpr = window.devicePixelRatio || 1, W = c.clientWidth * dpr, H = c.clientHeight * dpr;
    if (c.width !== W || c.height !== H) { c.width = W; c.height = H; }
    const x = c.getContext("2d"), PB = 26 * dpr, gh = H - PB;
    x.fillStyle = "#060300"; x.fillRect(0, 0, W, H);
    const span = v1 - v0, tx = t => (t - v0) / span * W;
    // time grid
    const steps = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600], step = steps.find(s => span / s <= 8) || 1200;
    x.strokeStyle = "#2A1C00"; x.lineWidth = dpr; x.fillStyle = "#C98A00"; x.font = `${11 * dpr}px "BPD Dot", monospace`; x.textAlign = "center";
    for (let s = Math.ceil(v0 / step) * step; s <= v1; s += step) { const px = tx(s); x.beginPath(); x.moveTo(px, 0); x.lineTo(px, gh); x.stroke(); x.fillText(fmtDur(s), px, H - 8 * dpr); }
    // traces
    const ks = L.ids.filter(id => sel[id]);
    const i0 = Math.max(0, idxAt(v0) - 1), i1 = Math.min(L.rows - 1, idxAt(v1) + 1), stride = Math.max(1, Math.floor((i1 - i0) / (W / dpr)));
    ks.forEach((id, j) => {
      const arr = L.data[id];
      let lo = Infinity, hi = -Infinity;
      for (let i = 0; i < L.rows; i++) { const v = arr[i]; if (!isNaN(v)) { if (v < lo) lo = v; if (v > hi) hi = v; } }
      if (lo === Infinity) return;
      if (hi - lo < 1e-6) { hi += 1; lo -= 1; }
      const pad = (hi - lo) * 0.08; lo -= pad; hi += pad;
      let y0 = 0, yh = gh;
      if (lanes) { yh = gh / ks.length; y0 = j * yh; x.strokeStyle = "#3A2A00"; x.beginPath(); x.moveTo(0, y0 + yh); x.lineTo(W, y0 + yh); x.stroke(); }
      x.strokeStyle = CH[id].c; x.lineWidth = 2 * dpr; x.beginPath();
      let pen = false;
      for (let i = i0; i <= i1; i += stride) {
        const v = arr[i]; if (isNaN(v)) { pen = false; continue; }
        const px = tx(L.t[i]), py = y0 + yh - 3 * dpr - (v - lo) / (hi - lo) * (yh - 6 * dpr);
        pen ? x.lineTo(px, py) : x.moveTo(px, py); pen = true;
      }
      x.stroke();
      if (lanes) {                                  // lane name on a dark tab so the trace can't hide it
        const tw = x.measureText(CH[id].n).width;
        x.fillStyle = "#060300"; x.fillRect(2 * dpr, y0 + 2 * dpr, tw + 8 * dpr, 16 * dpr);
        x.fillStyle = CH[id].c; x.textAlign = "left"; x.fillText(CH[id].n, 6 * dpr, y0 + 14 * dpr);
      }
    });
    // warning marks
    x.fillStyle = "#FF3B1F";
    L.marks.forEach(i => { const px = tx(L.t[i]); if (px < 0 || px > W) return; x.beginPath(); x.moveTo(px - 7 * dpr, 0); x.lineTo(px + 7 * dpr, 0); x.lineTo(px, 11 * dpr); x.fill(); });
    // cursor
    const ci = idxAt(cur), cx = tx(cur);
    x.strokeStyle = "#FFFFFF"; x.lineWidth = 1.5 * dpr; x.beginPath(); x.moveTo(cx, 0); x.lineTo(cx, gh); x.stroke();
    $l("logTime").textContent = `${fmtDur(cur)}.${Math.floor((cur % 1) * 10)}`;
    $l("logRead").innerHTML = ks.map(id => { const [val, u] = show(id, L.data[id][ci]);
      return `<div style="border:2px solid var(--line);padding:2px 6px;color:${CH[id].c}">${id === GPS_BIT ? "GPS SPEED" : CH[id].n}<br><span style="font-size:16px">${val}</span> ${u}</div>`; }).join("");
    if (replayOn) feedReplay(ci);
    drawMap(ci);
  }

  // ---------------- GPS track map and laps ----------------
  let mapView = null;
  function drawMap(ci) {
    if (!L || !L.hasGps) return;
    const c = $l("logMap"), dpr = window.devicePixelRatio || 1, W = c.clientWidth * dpr, H = c.clientHeight * dpr;
    if (!W) return;
    if (c.width !== W || c.height !== H) { c.width = W; c.height = H; mapView = null; }
    if (!mapView) {
      let a = Infinity, b = -Infinity, cmin = Infinity, d = -Infinity;
      for (let i = 0; i < L.rows; i++) if (!isNaN(L.lat[i])) { a = Math.min(a, L.lat[i]); b = Math.max(b, L.lat[i]); cmin = Math.min(cmin, L.lon[i]); d = Math.max(d, L.lon[i]); }
      if (a === Infinity) return;
      const lat0 = (a + b) / 2, lon0 = (cmin + d) / 2, k = Math.cos(lat0 * Math.PI / 180);
      const sx = (d - cmin) * k || 1e-5, sy = (b - a) || 1e-5, s = Math.min(W, H) * 0.9 / Math.max(sx, sy);
      mapView = { px: (la, lo) => [W / 2 + (lo - lon0) * k * s, H / 2 - (la - lat0) * s] };
    }
    const x = c.getContext("2d");
    x.fillStyle = "#060300"; x.fillRect(0, 0, W, H);
    x.strokeStyle = "#6B4800"; x.lineWidth = 2 * dpr; x.beginPath();
    let pen = false;
    for (let i = 0; i < L.rows; i += 2) { if (isNaN(L.lat[i])) { pen = false; continue; } const [a, b] = mapView.px(L.lat[i], L.lon[i]); pen ? x.lineTo(a, b) : x.moveTo(a, b); pen = true; }
    x.stroke();
    const lapNow = L.laps.find(l => ci >= l.i0 && ci < l.i1);             // highlight the lap the cursor is in
    if (lapNow) { x.strokeStyle = "#FFB000"; x.lineWidth = 3 * dpr; x.beginPath(); pen = false;
      for (let i = lapNow.i0; i < lapNow.i1; i++) { if (isNaN(L.lat[i])) continue; const [a, b] = mapView.px(L.lat[i], L.lon[i]); pen ? x.lineTo(a, b) : x.moveTo(a, b); pen = true; } x.stroke(); }
    if (!isNaN(L.lat[ci])) { const [a, b] = mapView.px(L.lat[ci], L.lon[ci]); x.fillStyle = "#ED008C"; x.beginPath(); x.arc(a, b, 6 * dpr, 0, 7); x.fill(); }
  }
  function renderLaps() {
    if (!L || !L.hasGps) { $l("logLaps").innerHTML = ""; return; }
    const best = L.laps.length ? Math.min(...L.laps.map(l => l.ms)) : 0;
    const lt = ms => `${Math.floor(ms / 60000)}:${String(Math.floor(ms / 1000) % 60).padStart(2, "0")}.${String(Math.floor(ms % 1000 / 10)).padStart(2, "0")}`;
    $l("logLaps").innerHTML = L.laps.length ? L.laps.map((l, i) => `<button class="btn ${l.ms === best ? "pink" : "ghost"}" data-lap="${i}" type="button" style="min-height:34px;font-size:12px;margin:0 6px 6px 0">LAP ${l.n}  ${lt(l.ms)}</button>`).join("")
      : `<div class="sm">No complete laps in this log (set a start/finish line on the gauge or in Setup).</div>`;
    document.querySelectorAll("[data-lap]").forEach(b => b.onclick = () => { const l = L.laps[+b.dataset.lap]; v0 = L.t[l.i0]; v1 = L.t[l.i1 - 1]; cur = v0; draw(); });
  }

  // Cursor drag, wheel zoom, pinch zoom
  function setupCanvas() {
    const c = cv(), pts = new Map();
    let pinch = null;
    const tAt = clientX => { const r = c.getBoundingClientRect(); return v0 + (clientX - r.left) / r.width * (v1 - v0); };
    const clampView = () => { const span = Math.min(Math.max(v1 - v0, 2), Math.max(2, L.dur)); if (v0 < 0) v0 = 0; v1 = v0 + span; if (v1 > Math.max(span, L.dur)) { v1 = Math.max(span, L.dur); v0 = v1 - span; } };
    const zoomAt = (t, f) => { v0 = t - (t - v0) * f; v1 = t + (v1 - t) * f; clampView(); draw(); };
    c.onpointerdown = e => { if (!L) return; c.setPointerCapture(e.pointerId); pts.set(e.pointerId, e.clientX);
      if (pts.size === 2) { const [a, b] = [...pts.values()]; pinch = { d: Math.abs(a - b), t: tAt((a + b) / 2), v0, v1 }; }
      else { cur = Math.max(0, Math.min(L.dur, tAt(e.clientX))); draw(); } };
    c.onpointermove = e => { if (!pts.has(e.pointerId) || !L) return; pts.set(e.pointerId, e.clientX);
      if (pinch && pts.size === 2) { const [a, b] = [...pts.values()], f = pinch.d / Math.max(10, Math.abs(a - b));
        v0 = pinch.t - (pinch.t - pinch.v0) * f; v1 = pinch.t + (pinch.v1 - pinch.t) * f; clampView(); draw(); }
      else if (!pinch) { cur = Math.max(0, Math.min(L.dur, tAt(e.clientX))); draw(); } };
    c.onpointerup = c.onpointercancel = e => { pts.delete(e.pointerId); if (pts.size < 2) pinch = null; };
    c.onwheel = e => { if (!L) return; e.preventDefault(); zoomAt(tAt(e.clientX), e.deltaY > 0 ? 1.25 : 0.8); };
    $l("zIn").onclick = () => L && zoomAt(cur, 0.5);
    $l("zOut").onclick = () => L && zoomAt(cur, 2);
    $l("zFit").onclick = () => { if (L) { v0 = 0; v1 = Math.max(1, L.dur); draw(); } };
    $l("logMap").onclick = e => {                          // tap the map: jump the cursor to the nearest point
      if (!L || !L.hasGps || !mapView) return;
      const c = $l("logMap"), r = c.getBoundingClientRect(), dpr = window.devicePixelRatio || 1, mx = (e.clientX - r.left) * dpr, my = (e.clientY - r.top) * dpr;
      let bi = -1, bd = Infinity;
      for (let i = 0; i < L.rows; i++) { if (isNaN(L.lat[i])) continue; const [a, b] = mapView.px(L.lat[i], L.lon[i]), d = (a - mx) ** 2 + (b - my) ** 2; if (d < bd) { bd = d; bi = i; } }
      if (bi >= 0) { cur = L.t[bi]; draw(); }
    };
  }

  function tick(ts) {
    if (!playing || !L) return;
    cur += (ts - lastT) / 1000 * speed; lastT = ts;
    if (cur >= L.dur) { cur = L.dur; setPlay(false); }
    const span = v1 - v0;
    if (cur > v1 - span * 0.1) { v0 = Math.min(cur - span * 0.1, Math.max(0, L.dur - span)); v1 = v0 + span; }   // follow the cursor
    draw();
    if (playing) requestAnimationFrame(tick);
  }
  function setPlay(on) {
    playing = on; $l("logPlay").textContent = on ? "Pause" : "Play";
    if (on) { if (cur >= L.dur) cur = 0; lastT = performance.now(); requestAnimationFrame(tick); }
  }

  // ---------------- Replay onto the Gauge tab ----------------
  // Builds Haltech frames from the log row so the gauge faces show the log instead of live data.
  function feedReplay(i) {
    const g = id => (L.data[id] ? L.data[id][i] : NaN), ok = v => !isNaN(v), now = performance.now();
    const be = (d, o, v) => { v = Math.round(v) & 0xFFFF; d[o] = v >> 8; d[o + 1] = v & 255; };
    const put = (id, fill) => { const d = new Uint8Array(8); if (fill(d)) frames[id] = { data: d, t: now }; else delete frames[id]; };
    put(0x360, d => { if (!ok(g(0))) return false; be(d, 0, g(0)); be(d, 2, ((ok(g(1)) ? g(1) : 0) + 101.3) * 10); be(d, 4, (ok(g(14)) ? g(14) : 0) * 10); be(d, 6, ok(g(9)) ? (g(9) + 101.3) * 10 : 0); return true; });
    put(0x361, d => { if (!ok(g(6)) && !ok(g(10))) return false; be(d, 0, ok(g(10)) ? (g(10) + 101.3) * 10 : 0); be(d, 2, ok(g(6)) ? (g(6) + 101.3) * 10 : 0); return true; });
    put(0x362, d => { if (!ok(g(11))) return false; be(d, 0, g(11) * 10); be(d, 4, ok(g(17)) ? g(17) * 10 : 0); return true; });
    put(0x368, d => { if (!ok(g(3))) return false; be(d, 0, g(3) * 1000); return true; });
    put(0x372, d => { if (!ok(g(13)) && !ok(g(2))) return false; be(d, 0, ok(g(13)) ? g(13) * 10 : 0); be(d, 4, ok(g(2)) ? g(2) * 10 : 0); be(d, 6, 1013); return true; });
    put(0x3E0, d => { if (!ok(g(8)) && !ok(g(7)) && !ok(g(12))) return false; be(d, 0, ok(g(8)) ? (g(8) + 273.15) * 10 : 0); be(d, 2, ok(g(12)) ? (g(12) + 273.15) * 10 : 0); be(d, 6, ok(g(7)) ? (g(7) + 273.15) * 10 : 0); return true; });
    put(0x3E1, d => { if (!ok(g(5))) return false; be(d, 4, g(5) * 10); return true; });
    put(0x3E9, d => { if (!ok(g(4))) return false; be(d, 4, g(4) * 1000); return true; });
    if (L.hasGps) Object.assign(gpsStatus, { fix: !isNaN(L.lat[i]) ? 1 : 0, lat: L.lat[i], lon: L.lon[i], lap: L.lap[i], t: now, gates: 1,
      cur: (() => { const l = L.laps.find(q => i >= q.i0 && i < q.i1); return l ? Math.round((L.t[i] - L.t[l.i0]) * 1000) : 0; })(),
      last: (() => { const p = L.laps.filter(q => q.i1 <= i); return p.length ? p[p.length - 1].ms : 0; })(),
      best: (() => { const p = L.laps.filter(q => q.i1 <= i); return p.length ? Math.min(...p.map(q => q.ms)) : 0; })(), delta: null, progress: 0 });
    gaugeStatus.kmh = ok(g(15)) ? g(15) : null; gaugeStatus.gear = ok(g(16)) ? g(16) : null; gaugeStatus.t = now;
    gaugeStatus.warn = L.w[i];
  }
  function setReplay(on) {
    replayOn = on;
    $l("logReplay").textContent = on ? "Gauges: log" : "Gauges: live";
    $l("logReplay").classList.toggle("pink", on);
    if (on && L) feedReplay(idxAt(cur)); else for (const k in frames) delete frames[k];
  }

  // ---------------- Logging settings (Setup tab) ----------------
  const fld = (id, label, unit) => `<div class="lrow"><label class="ck2"><input type="checkbox" id="${id}_on"> ${label}</label><span><input class="num" id="${id}" inputmode="decimal"> ${unit}</span></div>`;
  let cfg = null;
  function renderSettings(box) {
    if (!status) { box.innerHTML = `<div class="empty">Connect to load the logging settings.</div>`; return; }
    cfg = { ...status.cfg };
    const pu = unitsOf().pu, bstShown = pu == 0 ? cfg.st_bst : pu == 1 ? cfg.st_bst / 100 : cfg.st_bst / 6.894757;
    box.innerHTML = `
      <label>Trigger mode</label>
      <div class="seg" id="lgMode"><button class="btn ghost" data-m="0" type="button">Manual</button><button class="btn ghost" data-m="1" type="button">Auto</button><button class="btn ghost" data-m="2" type="button">Both</button></div>
      <label style="margin-top:10px">Auto start, any of</label>
      <div class="list">${fld("st_rpm", "RPM above", "rpm")}${fld("st_spd", "Speed above", "km/h")}${fld("st_tps", "Throttle above", "%")}${fld("st_bst", "Boost above", ["kPa", "bar", "psi"][pu])}</div>
      <label style="margin-top:10px">Auto stop</label>
      <div class="list">${fld("sp_eng", "Engine off for", "s")}${fld("sp_still", "Stopped for", "s")}
        <div class="lrow"><span>Pre-trigger buffer</span><span><input class="num" id="lg_pre" inputmode="numeric"> s</span></div></div>
      <label style="margin-top:10px">Files</label>
      <div class="list"><div class="lrow"><span>Discard logs shorter than</span><span><input class="num" id="lg_min" inputmode="numeric"> s</span></div>
        <div class="lrow"><span>New file every (0 = never)</span><span><input class="num" id="lg_split" inputmode="numeric"> min</span></div>
        <div class="lrow"><span>Card full</span><span><select id="lg_full" style="width:auto;min-height:36px"><option value="0">Stop logging</option><option value="1">Delete oldest</option></select></span></div></div>
      <label style="margin-top:10px">Channels and rate</label>
      <div class="seg" id="lgRate"><button class="btn ghost" data-r="10" type="button">10/s</button><button class="btn ghost" data-r="20" type="button">20/s</button><button class="btn ghost" data-r="50" type="button">50/s</button></div>
      <div id="lgCh" style="margin-top:8px"></div>
      <div id="lgEst" class="lead" style="margin-top:6px"></div>
      <p id="lgMsg" class="lead" role="status">Changes are sent to the gauge as you make them.</p>
      <div class="actions"><button class="btn pink" id="lgSave" type="button">Save logging settings to gauge</button></div>`;
    ["st_rpm", "st_spd", "st_tps", "st_bst", "sp_eng", "sp_still"].forEach(k => { $l(k + "_on").checked = !!cfg[k + "_on"]; $l(k).value = k === "st_bst" ? +bstShown.toFixed(pu == 1 ? 2 : 0) : cfg[k]; });
    $l("lg_pre").value = cfg.pre; $l("lg_min").value = cfg.min; $l("lg_split").value = cfg.split; $l("lg_full").value = cfg.full;
    const seg = (boxId, attr, key) => { const sync = () => $l(boxId).querySelectorAll("button").forEach(b => b.classList.toggle("ghost", +b.dataset[attr] !== cfg[key]));
      $l(boxId).querySelectorAll("button").forEach(b => b.onclick = () => { cfg[key] = +b.dataset[attr]; sync(); est(); autoSave(); }); sync(); };
    seg("lgMode", "m", "mode"); seg("lgRate", "r", "rate");
    const chips = () => { $l("lgCh").innerHTML = CH.map((c, i) => { const on = cfg.ch & (1 << i); return `<span class="chipck${on ? " sel" : ""}" data-lc="${i}" style="${on ? "color:var(--amber)" : ""}"><span class="dx${on ? " on" : ""}"></span>${c.n}</span>`; }).join("");
      document.querySelectorAll("[data-lc]").forEach(e => e.onclick = () => { cfg.ch ^= 1 << +e.dataset.lc; if (!cfg.ch) cfg.ch = 1; chips(); est(); autoSave(); }); };
    const est = () => { const perMin = rowSizeOf(cfg.ch) * cfg.rate * 60, s20 = perMin * 20;
      $l("lgEst").innerHTML = `About <span style="color:var(--pink)">${fmtSize(perMin)}</span> per minute. A 20 minute session is ${fmtSize(s20)}, roughly ${Math.max(1, Math.round(s20 / 30000))} s to download.`; };
    // Send changes automatically, shortly after the last change (so typing a number sends once)
    let saveTimer = null;
    const autoSave = () => { clearTimeout(saveTimer); $l("lgMsg").textContent = "Sending…"; saveTimer = setTimeout(() => $l("lgSave").onclick(), 700); };
    box.querySelectorAll("input, select").forEach(el => { el.addEventListener("change", autoSave); if (el.type !== "checkbox") el.addEventListener("input", autoSave); });
    chips(); est();
    $l("lgSave").onclick = async () => {
      const out = { cmd: "logset", mode: cfg.mode, rate: cfg.rate, ch: cfg.ch, pre: +$l("lg_pre").value, min: +$l("lg_min").value, split: +$l("lg_split").value, full: +$l("lg_full").value };
      ["st_rpm", "st_spd", "st_tps", "st_bst", "sp_eng", "sp_still"].forEach(k => { out[k + "_on"] = $l(k + "_on").checked ? 1 : 0; let v = parseFloat($l(k).value) || 0;
        if (k === "st_bst") v = pu == 0 ? v : pu == 1 ? v * 100 : v * 6.894757; out[k] = v; });
      try { const r = await cmd(out); $l("lgMsg").textContent = r.ok ? `Saved to the gauge at ${new Date().toLocaleTimeString([], { hour: "numeric", minute: "2-digit", second: "2-digit" })}.` : "Not saved: " + r.err; await refresh(); }
      catch (e) { $l("lgMsg").textContent = "Not saved: " + e.message; }
    };
  }

  // ---------------- Wiring ----------------
  function init() {
    setupCanvas();
    $l("logRefresh").onclick = refresh;
    $l("recBtn").onclick = async () => { try { const r = await cmd({ cmd: "log", on: (gaugeStatus.log & 2) ? 0 : 1 }); if (!r.ok) alert(r.err); setTimeout(refresh, 600); } catch (e) { alert(e.message); } };
    $l("delAll").onclick = () => { if (!files.length) return; confirmRow($l("fileList").firstElementChild, "Delete every log on the card?", async () => { await cmd({ cmd: "rmall" }); refresh(); }); };
    $l("logPlay").onclick = () => L && setPlay(!playing);
    document.querySelectorAll("[data-sp]").forEach(b => b.onclick = () => { speed = +b.dataset.sp; document.querySelectorAll("[data-sp]").forEach(o => o.classList.toggle("ghost", o !== b)); });
    $l("logMode").onclick = () => { lanes = !lanes; $l("logMode").textContent = lanes ? "Lanes" : "Overlay"; draw(); };
    $l("logReplay").onclick = () => setReplay(!replayOn);
    $l("logClose").onclick = () => { setPlay(false); setReplay(false); L = null; $l("viewer").hidden = true; };
    window.addEventListener("resize", draw);
    renderFiles();
  }

  return { init, refresh, onChunk, renderSettings, get replaying() { return replayOn; }, get status() { return status; } };
})();
