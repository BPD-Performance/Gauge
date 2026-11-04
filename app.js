"use strict";
// BPD Gauge app: Bluetooth link to the gauge (Web Bluetooth, Chrome on Android / Windows)

const SVC  = "b9d00001-6a1e-4c2a-9e3d-6b5a8f0c1d01";
const RX   = "b9d00002-6a1e-4c2a-9e3d-6b5a8f0c1d01";   // commands to the gauge
const TX   = "b9d00003-6a1e-4c2a-9e3d-6b5a8f0c1d01";   // replies from the gauge
const LIVE = "b9d00004-6a1e-4c2a-9e3d-6b5a8f0c1d01";   // live data
const $ = id => document.getElementById(id);
const enc = new TextEncoder(), dec = new TextDecoder();

let dev = null, rxC = null, connected = false, userDisconnect = false, retryMs = 2000;
let S = null;                                   // gauge settings (the saved copy from this phone while not connected)
let gaugeInfo = null;
const frames = {};                              // latest CAN data by ID: {data, t}
const gaugeStatus = { kmh: null, gear: null, held: 0, sim: 0, warn: 0, dyno: 0, log: 0, t: 0 };
let diagWarns = [];
const gpsStatus = { fix: 0, sats: 0, lat: 0, lon: 0, kmh: 0, course: 0, lap: 0, cur: 0, last: 0, best: 0, delta: null, sector: 0, progress: 0, gates: 0, t: 0 };                             // active warnings from the gauge (Live tab and gauge screen)

// ---------------- Command channel ----------------
let lineBuf = "", waiters = [], chain = Promise.resolve();

function logConsole(dir, text) {
  const c = $("console");
  c.textContent = (c.textContent + `${dir} ${text}\n`).slice(-6000);
  c.scrollTop = c.scrollHeight;
}

function onTx(e) {
  lineBuf += dec.decode(e.target.value);
  let i;
  while ((i = lineBuf.indexOf("\n")) >= 0) {
    const line = lineBuf.slice(0, i).trim();
    lineBuf = lineBuf.slice(i + 1);
    if (!line) continue;
    logConsole("◀", line);
    let r; try { r = JSON.parse(line); } catch { r = { ok: false, err: "bad reply" }; }
    // Replies carry the id of the command they answer. With two apps connected (e.g. a phone and
    // the head unit) both hear every reply, so anything that isn't ours is ignored.
    let w;
    if (r && r.id !== undefined) { const i = waiters.findIndex(x => x.id === r.id); if (i < 0) continue; w = waiters.splice(i, 1)[0]; delete r.id; }
    else w = waiters.shift();                     // older gauge firmware without ids
    if (w) { clearTimeout(w.timer); w.resolve(r); }
  }
}

async function writeText(text) {
  const bytes = enc.encode(text + "\n");
  for (let i = 0; i < bytes.length; i += 180) await rxC.writeValueWithResponse(bytes.slice(i, i + 180));
}

// Send one command and wait for its reply (commands are queued one at a time)
function cmd(obj) {
  const p = chain.then(() => new Promise(async (resolve, reject) => {
    if (!connected) return reject(new Error("not connected"));
    const id = Math.floor(Math.random() * 1e9) + 1;
    const text = typeof obj === "string" ? obj : JSON.stringify({ ...obj, id });
    const timer = setTimeout(() => { waiters = waiters.filter(w => w.timer !== timer); reject(new Error("no reply")); }, 6000);
    waiters.push({ resolve, timer, id: typeof obj === "string" ? undefined : id });
    logConsole("▶", text);
    try { await writeText(text); } catch (err) { clearTimeout(timer); waiters.pop(); reject(err); }
  }));
  chain = p.catch(() => {});
  return p;
}

// ---------------- Live data ----------------
function onLive(e) {
  const v = e.target.value, n = v.byteLength, now = performance.now();
  const type = v.getUint8(0);
  if (type === 0x01) {
    for (let o = 2; o + 11 <= n; o += 11) {
      const id = v.getUint16(o, true);
      const data = new Uint8Array(v.buffer, v.byteOffset + o + 3, 8).slice();
      frames[id] = { data, t: now };
    }
  } else if (type === 0x02 && n >= 10) {
    const s10 = v.getUint16(2, true), g = v.getInt8(4);
    gaugeStatus.kmh = s10 === 0xFFFF ? null : s10 / 10;
    gaugeStatus.gear = g === 0x7F ? null : g;
    gaugeStatus.warn = n >= 13 ? v.getUint16(11, true) : 0;
    gaugeStatus.t = now;
    gaugeStatus.held = v.getUint16(8, true);
    gaugeStatus.sim = n >= 11 ? v.getUint8(10) : 0;
    gaugeStatus.dyno = n >= 14 ? v.getUint8(13) : 0;
    gaugeStatus.log = n >= 15 ? v.getUint8(14) : 0;
  } else if (type === 0x03) {
    Logs.onChunk(v);                             // log file download
  } else if (type === 0x04 && n >= 37) {   // GPS and lap timer
    const d = v.getInt32(30, true);
    Object.assign(gpsStatus, { fix: v.getUint8(2), sats: v.getUint8(3), lat: v.getInt32(4, true) / 1e7, lon: v.getInt32(8, true) / 1e7,
      kmh: v.getUint16(12, true) / 10, course: v.getUint16(14, true) / 10, lap: v.getUint16(16, true), cur: v.getUint32(18, true),
      last: v.getUint32(22, true), best: v.getUint32(26, true), delta: d === -2147483648 ? null : d, sector: v.getUint8(34),
      progress: v.getUint8(35), gates: v.getUint8(36), t: now });
  }
}

// ---------------- Connection ----------------
// Connect button: outlined = not connected, blinking dashed = connecting, solid amber = connected
function setState(text, isConnected) {
  const busy = !isConnected && text.endsWith("…");
  $("state").textContent = text;
  const b = $("connectBtn");
  b.textContent = isConnected ? "Connected" : busy ? "Connecting" : "Connect";
  b.classList.toggle("connected", isConnected);
  b.classList.toggle("connecting", busy);
}

async function openLink() {
  setState("Connecting…", false);
  const server = await dev.gatt.connect();
  const svc = await server.getPrimaryService(SVC);
  rxC = await svc.getCharacteristic(RX);
  const txC = await svc.getCharacteristic(TX);
  const liveC = await svc.getCharacteristic(LIVE);
  txC.addEventListener("characteristicvaluechanged", onTx);
  liveC.addEventListener("characteristicvaluechanged", onLive);
  await txC.startNotifications();               // first time: Android asks for the gauge's PIN here
  await liveC.startNotifications();
  connected = true;
  retryMs = 2000;
  lineBuf = ""; waiters = [];
  setState("Connected", true);
  lastWarnMask = -1;

  const info = gaugeInfo = await cmd({ cmd: "info" });
  await cmd({ cmd: "time", t: Math.floor(Date.now() / 1000), tz: -new Date().getTimezoneOffset() }).catch(() => {});   // dates the logs
  $("devInfo").innerHTML = `<p>${info.name || "BPD Gauge"}</p>Firmware ${info.fw}, protocol ${info.proto}`;
  await subscribe();
  await loadSettings();
}

// Only ask for what's on screen: every CAN ID on the Live tab, otherwise just the IDs the
// gauge faces, warnings and Keypad use. Fewer IDs leave room for faster update rates.
const FACE_IDS = [0x360, 0x361, 0x362, 0x368, 0x372, 0x3E0, 0x3E1, 0x3E4, 0x3E9];
let liveTab = false;
async function subscribe() {
  if (connected) await cmd({ cmd: "sub", ids: liveTab ? [] : FACE_IDS, hz: +$("rate").value });
}

function onDisconnected() {
  connected = false;
  rxC = null;
  waiters.forEach(w => clearTimeout(w.timer)); waiters = [];
  diagWarns = []; renderDiag(); Prof.renderBar();
  setState(userDisconnect ? "Not connected" : "Reconnecting…", false);
  if (!userDisconnect) setTimeout(tryReconnect, retryMs);
}

async function tryReconnect() {
  if (connected || userDisconnect || !dev) return;
  try { await openLink(); }
  catch { retryMs = Math.min(retryMs * 1.5, 10000); setState("Reconnecting…", false); setTimeout(tryReconnect, retryMs); }
}

async function connectClicked() {
  if (connected) { if (confirm("Disconnect from the gauge?")) { userDisconnect = true; dev.gatt.disconnect(); } return; }
  if (!navigator.bluetooth) { alert("This browser has no Bluetooth support. Use Chrome on Android or Windows."); return; }
  userDisconnect = false;
  try {
    if (!dev) {
      dev = await navigator.bluetooth.requestDevice({ filters: [{ services: [SVC] }] });
      localStorage.setItem("gaugeId", dev.id);    // remembered for connecting automatically next time
      dev.addEventListener("gattserverdisconnected", onDisconnected);
    }
    await openLink();
  } catch (err) {
    setState("Not connected", false);
    if (err.name !== "NotFoundError") alert("Could not connect: " + err.message);
  }
}

// ---------------- Units ----------------
const PU = ["kPa", "bar", "psi"], TU = ["°C", "°F"], SU = ["km/h", "mph"];
const units = () => ({ pu: S ? S.pu : 0, tu: S ? S.tu : 0, su: S ? S.su : 0 });

function fmtChannel(c, raw) {
  const v = c.f(raw), u = units();
  switch (c.kind) {
    case "p":  return u.pu === 0 ? [v.toFixed(0), "kPa"] : u.pu === 1 ? [(v / 100).toFixed(2), "bar"] : [(v / 6.894757).toFixed(1), "psi"];
    case "pa": return u.pu === 0 ? [v.toFixed(1), "kPa abs"] : u.pu === 1 ? [(v / 100).toFixed(3), "bar abs"] : [(v / 6.894757).toFixed(2), "psi abs"];
    case "t":  { const C = v - 273.15; return [(u.tu ? C * 9 / 5 + 32 : C).toFixed(0), TU[u.tu]]; }
    case "s":  return [(u.su ? v / 1.609344 : v).toFixed(0), SU[u.su]];
    case "b":  return [v ? "On" : "Off", ""];
    case "e":  return [c.map[v] !== undefined ? c.map[v] : String(v), ""];
    default: {
      const dp = c.unit === "λ" ? 3 : (c.f === d10 || c.f === gp) ? 1 : c.f === d100 ? 2 : 0;
      return [v.toFixed(dp), c.unit];
    }
  }
}

// Gauge-calculated values shown alongside the Haltech channels
const DERIVED = [
  { key: "d_boost", name: "Boost", get: () => {
      const m = frames[0x360], b = frames[0x372];
      if (!m) return null;
      const map = haltechRaw(m.data, "2:7", "3:0", false) / 10;
      const baro = b ? haltechRaw(b.data, "6:7", "7:0", false) / 10 : 101.3;
      return fmtChannel({ kind: "p", f: x => x }, map - baro);
    }, t: () => frames[0x360] && frames[0x360].t },
  { key: "d_speed", name: "Speed (gauge, tyre corrected)", get: () => gaugeStatus.kmh === null ? null : fmtChannel({ kind: "s", f: x => x }, gaugeStatus.kmh), t: () => gaugeStatus.t },
  { key: "d_gear", name: "Gear (gauge)", get: () => gaugeStatus.gear === null ? null : [gaugeStatus.gear < 0 ? "R" : gaugeStatus.gear === 0 ? "N" : String(gaugeStatus.gear), ""], t: () => gaugeStatus.t },
];

function channelValue(key) {
  const d = DERIVED.find(x => x.key === key);
  const now = performance.now();
  if (d) { const t = d.t(); if (!t || now - t > 2000) return null; return d.get(); }
  const c = H.find(x => x.key === key);
  const f = c && frames[c.id];
  if (!f || now - f.t > 2000) return null;
  return fmtChannel(c, haltechRaw(f.data, c.from, c.to, c.sg));
}
const channelName = key => (DERIVED.find(x => x.key === key) || H.find(x => x.key === key) || { name: key }).name;

// ---------------- Live tab ----------------
const findKey = (id, name) => (H.find(c => c.id === id && c.name === name) || {}).key;
let pins = JSON.parse(localStorage.getItem("pins") || "null") || [
  "d_boost", findKey(0x360, "RPM"), findKey(0x361, "Oil pressure"), findKey(0x3E0, "Coolant temperature"),
  findKey(0x368, "Wideband sensor 1"), findKey(0x372, "Target boost")].filter(Boolean);
const rowEls = {};

function savePins() { localStorage.setItem("pins", JSON.stringify(pins)); buildPinned(); syncPinButtons(); }
function togglePin(key) { pins = pins.includes(key) ? pins.filter(k => k !== key) : [...pins, key]; savePins(); }

function buildPinned() {
  $("pinned").innerHTML = pins.length
    ? `<div class="tiles">${pins.map(k => `<div class="tile" data-k="${k}"><div class="n">${channelName(k)}</div><span class="v">--</span><span class="u"></span></div>`).join("")}</div>`
    : `<div class="empty">Nothing pinned. Use Choose pinned channels, or tap ☆ next to any channel below.</div>`;
}

// Pinned picker: every channel with a tick box
function buildPinPicker() {
  const q = $("pinSearch").value.trim().toLowerCase();
  const all = [...DERIVED.map(d => ({ key: d.key, name: d.name })), ...H.map(c => ({ key: c.key, name: c.name }))];
  $("pinGrid").innerHTML = all.filter(c => !q || c.name.toLowerCase().includes(q)).map(c => { const on = pins.includes(c.key);
    return `<span class="chipck${on ? " sel" : ""}" data-pk="${c.key}" style="${on ? "color:var(--amber)" : ""}"><span class="dx${on ? " on" : ""}"></span>${c.name}</span>`; }).join("");
  $("pinGrid").querySelectorAll("[data-pk]").forEach(e => e.onclick = () => { togglePin(e.dataset.pk); buildPinPicker(); });
}
$("pinEdit").onclick = () => { const show = $("pinPick").hidden; $("pinPick").hidden = !show; $("pinEdit").textContent = show ? "Hide channel picker" : "Choose pinned channels"; if (show) buildPinPicker(); };
$("pinDone").onclick = () => { $("pinPick").hidden = true; $("pinEdit").textContent = "Choose pinned channels"; };
$("pinNone").onclick = () => { if (confirm("Remove every pinned channel?")) { pins = []; savePins(); buildPinPicker(); } };
$("pinSearch").oninput = buildPinPicker;

// Which channels the Live list shows (remembered). "In use" learns as data arrives and is kept,
// so channels that only change while driving (boost, speed) stay listed once they've been seen.
const usedKeys = new Set(JSON.parse(localStorage.getItem("usedKeys") || "[]"));
let usedDirty = false, usedSaved = 0;
$("showMode").value = localStorage.getItem("showMode") || "used";
$("showMode").addEventListener("change", () => localStorage.setItem("showMode", $("showMode").value));

function buildChannelList() {
  const groups = [{ title: "Gauge", items: DERIVED.map(d => ({ key: d.key, name: d.name })) }];
  let last = -1;
  H.forEach(c => {
    if (c.id !== last) { groups.push({ id: c.id, items: [] }); last = c.id; }
    groups[groups.length - 1].items.push({ key: c.key, name: c.name });
  });
  // Name each CAN frame by what's in it, with its ID after in small text
  groups.forEach(g => { if (g.id === undefined) return;
    const names = g.items.map(i => i.name);
    g.title = names.slice(0, 3).join(", ") + (names.length > 3 ? ` and ${names.length - 3} more` : "") +
      `<span class="sm">CAN ID 0x${g.id.toString(16).toUpperCase()}</span>`; });
  $("channels").innerHTML = groups.map(g => `<div class="grp" data-grp>${g.title}</div>` + g.items.map(i =>
    `<div class="row stale" data-k="${i.key}" data-name="${i.name.toLowerCase()}">
      <button class="pin" type="button" aria-pressed="false" aria-label="Pin ${i.name}" data-pin="${i.key}">☆</button>
      <span>${i.name}</span><span class="v">--</span></div>`).join("")).join("");
  $("channels").querySelectorAll(".row").forEach(r => rowEls[r.dataset.k] = r);
  $("channels").querySelectorAll("[data-pin]").forEach(b => b.onclick = () => togglePin(b.dataset.pin));
  syncPinButtons();
}

function syncPinButtons() {
  document.querySelectorAll("[data-pin]").forEach(b => {
    const on = pins.includes(b.dataset.pin);
    b.setAttribute("aria-pressed", on); b.textContent = on ? "★" : "☆";
  });
}

function refreshLive() {
  // Pinned tiles
  document.querySelectorAll("#pinned .tile").forEach(t => {
    const r = channelValue(t.dataset.k);
    t.querySelector(".v").textContent = r ? r[0] : "--";
    t.querySelector(".u").textContent = r ? r[1] : "";
  });
  // Channel list
  const q = $("search").value.trim().toLowerCase(), mode = $("showMode").value;
  const now = performance.now();
  let hiddenUnused = 0;
  for (const k in rowEls) {
    const row = rowEls[k], r = channelValue(k);
    // A channel counts as "in use" once its raw value has been anything other than zero / off.
    // Haltech sends every channel whether it's set up or not; unused ones stay at zero.
    const c = H.find(x => x.key === k), f = c && frames[c.id];
    if (c && f && now - f.t < 2000 && !usedKeys.has(k) && haltechRaw(f.data, c.from, c.to, c.sg) !== 0) { usedKeys.add(k); usedDirty = true; }
    const unused = c && !usedKeys.has(k);
    row.querySelector(".v").textContent = r ? `${r[0]} ${r[1]}`.trim() : "--";
    row.classList.toggle("stale", !r);
    const hideForMode = mode === "used" ? (!r || unused) : mode === "data" ? !r : false;
    if (mode === "used" && r && unused) hiddenUnused++;
    row.hidden = (q && !row.dataset.name.includes(q)) || hideForMode;
  }
  if (usedDirty && now - usedSaved > 5000) { usedSaved = now; usedDirty = false; localStorage.setItem("usedKeys", JSON.stringify([...usedKeys])); }
  $("usedNote").textContent = mode === "used" ? (hiddenUnused ? `Hiding ${hiddenUnused} channels that have only ever read zero or off. Anything that changes shows up automatically.` : "") : "";
  // Status chips
  const chips = [];
  chips.push(`<span class="chip">${connected ? "Connected" : "Not connected"}</span>`);
  const sp = channelValue("d_speed"), gr = channelValue("d_gear");
  if (sp) chips.push(`<span class="chip">${sp[0]} ${sp[1]}</span>`);
  if (gr) chips.push(`<span class="chip">Gear ${gr[0]}</span>`);
  if (gaugeStatus.log & 2) chips.push(`<span class="chip alarm">REC</span>`);
  if (gaugeStatus.sim) chips.push(`<span class="chip alarm">SIMULATOR (fake data)</span>`);
  if (gaugeStatus.warn) chips.push(`<span class="chip alarm">WARNING</span>`);
  $("simBtn").textContent = gaugeStatus.sim ? "Stop simulator" : "Start simulator";
  // The Gauge tab always says when its numbers are not from the car
  const src = !connected && Demo.running ? "DEMO DATA: made up, not from the car" : connected && gaugeStatus.sim ? "SIMULATOR: the gauge is sending fake data" : "";
  $("srcBanner").hidden = !src; if (src) $("srcBanner").textContent = src;
  $("statusChips").innerHTML = chips.join("");
  refreshKeypad();
}

// ---------------- Keypad tab ----------------
let keyLabels = JSON.parse(localStorage.getItem("keyLabels") || "null") ||
  Object.fromEntries(Array.from({ length: 15 }, (_, i) => [i + 1, i === 0 ? "Boost +" : i === 1 ? "Boost −" : i === 2 ? "Rot idle" : `Button ${i + 1}`]));
let renameMode = false;

function buildKeypad() {
  $("keypad").innerHTML = Array.from({ length: 15 }, (_, i) =>
    `<button class="key" type="button" data-b="${i + 1}">${keyLabels[i + 1]}<small>Button ${i + 1}</small></button>`).join("") +
    `<button class="btn ghost" type="button" id="renameBtn" style="grid-column:1/-1">${renameMode ? "Done renaming" : "Rename buttons"}</button>`;
  $("renameBtn").onclick = () => { renameMode = !renameMode; buildKeypad(); };
  document.querySelectorAll(".key").forEach(k => {
    const b = +k.dataset.b;
    let timer = null, long = false;
    k.onpointerdown = () => {
      if (renameMode) return;
      long = false;
      timer = setTimeout(async () => {          // held 1 s: latch on / off
        long = true;
        const on = !((gaugeStatus.held >> (b - 1)) & 1);
        try { await cmd({ cmd: "hold", btn: b, on: on ? 1 : 0 }); } catch (e) { alert(e.message); }
      }, 1000);
    };
    const cancel = () => { clearTimeout(timer); timer = null; };
    k.onpointerleave = cancel;
    k.onpointerup = async () => {
      if (renameMode) {
        const n = prompt(`Name for button ${b}`, keyLabels[b]);
        if (n) { keyLabels[b] = n; localStorage.setItem("keyLabels", JSON.stringify(keyLabels)); buildKeypad(); }
        return;
      }
      if (!timer) return;
      cancel();
      if (long) return;
      if ((gaugeStatus.held >> (b - 1)) & 1) { await cmd({ cmd: "hold", btn: b, on: 0 }).catch(e => alert(e.message)); return; }
      try { await cmd({ cmd: "key", btn: b }); } catch (e) { alert(e.message); }
    };
  });
}

function refreshKeypad() {
  document.querySelectorAll(".key").forEach(k => k.classList.toggle("held", !!((gaugeStatus.held >> (+k.dataset.b - 1)) & 1)));
  const tp = channelValue(findKey(0x3E4, "Rotary trim pot 1")), tb = channelValue(findKey(0x372, "Target boost"));
  $("trimChips").innerHTML = `<span class="chip">Trim position ${tp ? tp[0] : "--"}</span><span class="chip">Target boost ${tb ? tb.join(" ") : "--"}</span>`;
}

// ---------------- Settings tab ----------------
const PAGES = ["Boost", "Oil", "AFR", "RPM and speed", "Coolant", "Fuel pressure", "Intake air temp", "Monitor", "CAN keypad", "Alerts", "Lap timer", "Face"];
const pTo = (k, u) => u == 0 ? k : u == 1 ? k / 100 : k / 6.894757, pFrom = (v, u) => u == 0 ? v : u == 1 ? v * 100 : v * 6.894757;
const tTo = (c, u) => u == 1 ? c * 9 / 5 + 32 : c, tFrom = (v, u) => u == 1 ? (v - 32) * 5 / 9 : v;
const WARN = [
  { key: "w_op", title: "Low oil pressure", f: [["w_op", "Below", "p"], ["w_op_rpm", "Only above (rpm)", "r"]] },
  { key: "w_clt", title: "Coolant hot", f: [["w_clt", "Above", "t"]] },
  { key: "w_lean", title: "Lean on boost", f: [["w_lean", "Leaner than (lambda)", "l"], ["w_lean_bst", "Only above boost", "p"]] },
  { key: "w_bst", title: "Overboost", f: [["w_bst", "Above", "p"]] },
  { key: "w_ot", title: "Oil hot", f: [["w_ot", "Above", "t"]] },
  { key: "w_shift", title: "Shift light and redline", f: [["w_shift", "At (rpm)", "r"]] },
  { key: "w_cp", title: "Coolant pressure high", f: [["w_cp", "Above", "p"]] },
  { key: "w_fd", title: "Fuel pressure low (fuel minus boost)", f: [["w_fd", "Below", "p"]] },
  { key: "w_iat", title: "Intake air hot", f: [["w_iat", "Above", "t"]] },
  { key: "w_bat", title: "Battery low (engine running)", f: [["w_bat", "Below (V)", "v"]] },
  { key: "w_duty", title: "Injector duty high", f: [["w_duty", "Above (%)", "r"]] }];
const uLbl = k => k == "p" ? PU[S.pu] : k == "t" ? TU[S.tu] : "";
const shown = (v, k) => k == "p" ? pTo(v, S.pu).toFixed(S.pu == 1 ? 2 : S.pu == 2 ? 1 : 0) : k == "t" ? tTo(v, S.tu).toFixed(0) : k == "l" ? (+v).toFixed(2) : k == "v" ? (+v).toFixed(1) : Math.round(v);
const metric = (v, k) => { v = parseFloat(v); return k == "p" ? pFrom(v, S.pu) : k == "t" ? tFrom(v, S.tu) : v; };
const fld = (id, label, attrs = "") => `<div><label for="${id}">${label}</label><input id="${id}" inputmode="decimal" ${attrs}></div>`;
const sel = (id, label, opts) => `<div><label for="${id}">${label}</label><select id="${id}">${opts.map((o, i) => `<option value="${Array.isArray(o) ? o[0] : i}">${Array.isArray(o) ? o[1] : o}</option>`).join("")}</select></div>`;

async function loadSettings(adopt) {
  const r = await cmd({ cmd: "get" });
  if (r.ok) {
    S = r.settings;
    await Prof.onConnect(gaugeInfo, S, !!adopt);        // compares with this phone's saved copy; asks only when they differ
    rebuildNav(); renderSettings(); buildFaces(); Prof.renderBar(); Prof.renderDevice();
  }
  await Logs.refresh().catch(() => {});
  if ($("logSettings")) Logs.renderSettings($("logSettings"));
  if ($("gpsBox")) Track.render($("gpsBox"));
}

// Turn each heading into a drop-down section; everything up to the next heading (or an
// always-visible ".keep" block such as the save buttons) folds away. Open/closed is remembered.
function collapseSections(box) {
  const open = JSON.parse(localStorage.getItem("secOpen") || "{}");
  [...box.querySelectorAll(":scope > h2")].forEach(h => {
    const d = document.createElement("details"), sm = document.createElement("summary"), body = document.createElement("div");
    const key = h.textContent;
    d.className = "sec"; sm.innerHTML = `<span>${h.textContent}</span><span class="pm" aria-hidden="true"></span>`; body.className = "secbody";
    d.open = !!open[key];
    let n = h.nextElementSibling;
    while (n && n.tagName !== "H2" && !n.classList.contains("keep")) { const next = n.nextElementSibling; body.appendChild(n); n = next; }
    h.replaceWith(d); d.append(sm, body);
    d.addEventListener("toggle", () => { open[key] = d.open; localStorage.setItem("secOpen", JSON.stringify(open)); if (d.open) Track.redraw(); });
  });
}

const SHIFT_COLOURS = [["Amber", "#FFB000"], ["Red", "#FF3B1F"], ["BPD pink", "#ED008C"], ["Blue", "#2EC4FF"], ["Green", "#7CFF4F"], ["White", "#FFFFFF"]];

function renderSettings() {
  const tyre = (p, title) => `<div class="card"><p>${title}</p><div class="grid3">${fld(p + "w", "Width")}${fld(p + "a", "Profile")}${fld(p + "r", "Rim (in)")}</div></div>`;
  $("settingsBody").innerHTML = `
  <h2>Gearing and tyres</h2>
  <div class="grid" style="margin-bottom:10px">${sel("numGears", "Gears", [[4, 4], [5, 5], [6, 6]])}${fld("fd", "Final drive")}</div>
  <div class="grid3" id="gearBox" style="margin-bottom:10px"></div>
  ${tyre("r", "Rear tyres (fitted now)")}${tyre("f", "Front tyres")}${tyre("c", "Tyres the ECU speed is calibrated for")}
  ${sel("spdSrc", "ECU speed comes from", ["Use ECU speed as is", "Gearbox or rear wheels", "Front wheels", "Gauge GPS"])}
  <div style="height:10px"></div>${sel("gearSrc", "Gear display", ["Work out from RPM", "Use ECU gear", "Off"])}
  <h2>Units</h2>
  <div class="grid">${sel("pu", "Pressure", PU)}${sel("tu", "Temperature", TU)}${sel("su", "Speed", SU)}${sel("lam", "Mixture", ["AFR", "Lambda"])}
   ${sel("flex", "AFR stoich", [[1, "Follow ethanol content"], [0, "Fixed value"]])}${fld("stoich", "Fixed stoich")}</div>
  <h2>Gauge pages</h2><div class="list" id="pagesBox"></div>
  <h2>Warnings</h2><div id="warnBox"></div>
  <h2>Screen</h2>
  <div class="grid">${fld("bright", "Brightness (%)")}${sel("tachmax", "Tach dial max", [[7000, 7000], [8000, 8000], [9000, 9000], [10000, 10000]])}
   ${sel("acycle", "Warning swap time", [[1000, "1 s"], [1500, "1.5 s"], [2000, "2 s"], [3000, "3 s"], [4000, "4 s"], [5000, "5 s"]])}
   ${sel("face_style", "Face page style", [[0, "Classic (dot matrix)"], [1, "Turbo bot (16-bit)"]])}</div>
  <div class="list" style="margin-top:12px">
   <div class="lrow"><label class="ck2"><input type="checkbox" id="face_popup"> Show warning popups over the Face page</label></div>
   <div class="lrow"><label class="ck2"><input type="checkbox" id="shift_show"> Flash the shift light ring</label></div>
   <div class="lrow"><span>Shift light colour</span><span style="display:flex;gap:8px;align-items:center">
     <select id="shift_pre" style="width:auto;min-height:36px">${SHIFT_COLOURS.map(c => `<option value="${c[1]}">${c[0]}</option>`).join("")}<option value="custom">Custom</option></select>
     <input type="color" id="shift_rgb" style="width:46px;min-height:36px;padding:2px"></span></div>
  </div>
  <div class="actions keep"><button class="btn pink" id="saveBtn" type="button">Save to gauge</button><button class="btn ghost" id="reloadBtn" type="button">Reload from gauge</button></div>
`;
  // Face popup and shift light (older gauge firmware doesn't send these, so default them)
  $("face_popup").checked = S.face_popup === undefined ? true : !!S.face_popup;
  $("shift_show").checked = S.shift_show === undefined ? true : !!S.shift_show;
  $("face_style").value = S.face_style === undefined ? 0 : S.face_style;     // older firmware: Classic only
  $("face_style").disabled = S.face_style === undefined;
  const hex = "#" + ((S.shift_rgb === undefined ? 0xFFB000 : S.shift_rgb) >>> 0).toString(16).padStart(6, "0").toUpperCase();
  $("shift_rgb").value = hex;
  $("shift_pre").value = SHIFT_COLOURS.some(c => c[1] === hex) ? hex : "custom";
  $("shift_pre").onchange = () => { if ($("shift_pre").value !== "custom") $("shift_rgb").value = $("shift_pre").value; };
  $("shift_rgb").oninput = () => { const v = $("shift_rgb").value.toUpperCase(); $("shift_pre").value = SHIFT_COLOURS.some(c => c[1] === v) ? v : "custom"; };
  collapseSections($("settingsBody"));

  ["fd", "rw", "ra", "rr", "fw", "fa", "fr", "cw", "ca", "cr", "spdSrc", "gearSrc", "pu", "tu", "su", "lam", "flex", "stoich", "bright", "tachmax", "numGears", "acycle"]
    .forEach(k => { if (S[k] !== undefined) $(k).value = S[k]; });
  const gears = () => { $("gearBox").innerHTML = Array.from({ length: +$("numGears").value }, (_, i) => fld("g" + (i + 1), "Gear " + (i + 1))).join("");
    for (let i = 1; i <= +$("numGears").value; i++) $("g" + i).value = S["g" + i]; };
  gears();
  $("numGears").onchange = () => { readForm(); gears(); };
  ["pu", "tu"].forEach(k => $(k).onchange = () => { readWarns(); S[k] = +$(k).value; renderWarns(); });
  renderPages(); renderWarns();
  $("saveBtn").onclick = saveSettings;
  $("reloadBtn").disabled = !connected;
  $("reloadBtn").onclick = () => loadSettings(true).catch(e => $("msg").textContent = e.message);
}

function renderPages() {
  if (S.order.length < PAGES.length) {               // older gauge firmware: only its own pages
    S.order = S.order.filter(p => p < PAGES.length);
  }
  $("pagesBox").innerHTML = S.order.map((p, i) => `<div class="pg"><input type="checkbox" aria-label="Show ${PAGES[p]}" data-on="${p}" ${S.on[p] ? "checked" : ""} style="width:24px;height:24px;min-height:0">
    <span>${PAGES[p]}</span><button class="mv" type="button" data-up="${i}" ${i == 0 ? "disabled" : ""} aria-label="Move ${PAGES[p]} earlier">↑</button>
    <button class="mv" type="button" data-dn="${i}" ${i == S.order.length - 1 ? "disabled" : ""} aria-label="Move ${PAGES[p]} later">↓</button></div>`).join("");
  $("pagesBox").querySelectorAll("[data-on]").forEach(c => c.onchange = () => S.on[c.dataset.on] = c.checked ? 1 : 0);
  $("pagesBox").querySelectorAll("[data-up]").forEach(b => b.onclick = () => { const i = +b.dataset.up; [S.order[i - 1], S.order[i]] = [S.order[i], S.order[i - 1]]; renderPages(); });
  $("pagesBox").querySelectorAll("[data-dn]").forEach(b => b.onclick = () => { const i = +b.dataset.dn; [S.order[i + 1], S.order[i]] = [S.order[i], S.order[i + 1]]; renderPages(); });
}

function renderWarns() {
  $("warnBox").innerHTML = WARN.map(w => `<div class="card"><div class="chk" style="margin-bottom:8px"><input type="checkbox" id="${w.key}_on" ${S[w.key + "_on"] ? "checked" : ""}>
    <label for="${w.key}_on" style="margin:0;color:var(--ink);font-size:1rem">${w.title}</label></div>
    <div class="grid">${w.f.map(([k, l, t]) => `<div><label for="${k}">${l} ${uLbl(t)}</label><input id="${k}" inputmode="decimal" value="${shown(S[k], t)}"></div>`).join("")}</div></div>`).join("");
}
function readWarns() { WARN.forEach(w => { S[w.key + "_on"] = $(w.key + "_on").checked ? 1 : 0; w.f.forEach(([k, , t]) => { if ($(k).value !== "") S[k] = metric($(k).value, t); }); }); }

function readForm() {
  ["fd", "rw", "ra", "rr", "fw", "fa", "fr", "cw", "ca", "cr", "stoich", "bright"].forEach(k => S[k] = parseFloat($(k).value));
  ["numGears", "spdSrc", "gearSrc", "lam", "flex", "tachmax", "acycle"].forEach(k => S[k] = +$(k).value);
  for (let i = 1; i <= 6; i++) if ($("g" + i)) S["g" + i] = parseFloat($("g" + i).value);
  S.face_popup = $("face_popup").checked ? 1 : 0;
  S.shift_show = $("shift_show").checked ? 1 : 0;
  if (S.face_style !== undefined) S.face_style = +$("face_style").value;
  S.shift_rgb = parseInt($("shift_rgb").value.slice(1), 16);
  readWarns();
}

async function saveSettings() {
  readForm();
  S.pu = +$("pu").value; S.tu = +$("tu").value; S.su = +$("su").value;
  if (!connected) {                              // away from the gauge: keep it on this phone, ask to send next time
    Prof.saveLocal(); Prof.renderBar();
    $("msg").textContent = "Saved on this phone. It will ask to send these to the gauge next time you connect.";
    return;
  }
  $("msg").textContent = "Saving…";
  try {
    const r = await cmd({ cmd: "set", ...S });
    if (r.ok) Prof.snapshot();
    $("msg").textContent = r.ok ? "Saved. The gauge has updated." : "Not saved: " + r.err;
    Prof.renderBar();
  } catch (e) { $("msg").textContent = "Not saved: " + e.message; }
}

// ---------------- Device tab ----------------
$("simBtn").onclick = async () => {
  try { const r = await cmd({ cmd: "sim", on: gaugeStatus.sim ? 0 : 1 }); if (!r.ok) alert(r.err); } catch (e) { alert(e.message); }
};
$("cmdSend").onclick = () => cmd($("cmdIn").value.trim()).catch(e => logConsole("!", e.message));
$("forgetBtn").onclick = () => { if (connected) { userDisconnect = true; dev.gatt.disconnect(); } };
$("defaultsBtn").onclick = async () => {
  if (!confirm("Reset every gauge setting to its defaults?")) return;
  try { const r = await cmd({ cmd: "defaults" }); if (r.ok) setTimeout(() => loadSettings(true), 1500); else alert(r.err); } catch (e) { alert(e.message); }
};

// ---------------- Gauge screen tab ----------------
// A grid of gauge faces sized to the screen. Each device keeps its own layout, so a phone
// and a laptop can show different numbers of gauges. The grid works out the column count
// that makes the faces as big as possible, and stops adding once faces would get smaller
// than MIN_FACE (still readable).
const FACE_GAP = 12, MAX_FACE = 900;
const SIZE_MIN = { small: 160, medium: 240, large: 330 };      // smallest face for each size setting (screen px)
const SIMPLE_BELOW = 270;                                        // faces smaller than this drop the fine detail
const minFace = () => SIZE_MIN[localStorage.getItem("faceSize") || "small"] || 160;
const faceOpts = size => ({ clear: localStorage.getItem("faceStyle") === "clear", simple: size < SIMPLE_BELOW });
let gaugeTab = true, editMode = false, faceSizePx = 300;
let tiles = JSON.parse(localStorage.getItem("gaugeTiles") || "null") || [0];
const tileEls = [];

const allowedPages = () => {
  if (!S || !S.order) return PAGES.map((_, i) => i);
  const list = S.order.filter(p => p < PAGES.length && S.on[p]);
  return list.length ? list : [0];
};
const saveTiles = () => localStorage.setItem("gaugeTiles", JSON.stringify(tiles));

// The Gauge tab is locked to the screen: it fills the space between the header and the tab bar
// and never scrolls. The faces are centred in it, so the space above and below is equal.
function sizeGaugeTab() {
  const tab = $("tab-gauge");
  if (document.fullscreenElement === tab) { tab.style.height = ""; return; }
  const top = tab.getBoundingClientRect().top, nav = document.querySelector("nav").getBoundingClientRect().height;
  tab.style.height = Math.max(200, window.innerHeight - top - nav - 8) + "px";
}
function gridSpace() {
  sizeGaugeTab();
  const box = $("faces");
  return { W: box.clientWidth || window.innerWidth - 32, H: box.clientHeight || window.innerHeight - 200 };
}
function bestFit(n, sp) {
  let best = 0, cols = 1;
  for (let c = 1; c <= n; c++) {
    const r = Math.ceil(n / c);
    const size = Math.min((sp.W - (c - 1) * FACE_GAP) / c, (sp.H - (r - 1) * FACE_GAP) / r);
    if (size > best) { best = size; cols = c; }
  }
  return { size: Math.floor(Math.min(best, MAX_FACE)), cols };
}
function capacity(sp) {
  let n = 1;
  while (n < 64 && bestFit(n + 1, sp).size >= minFace()) n++;
  return n;
}

function layoutFaces() {
  const sp = gridSpace();
  const fit = bestFit(tiles.length, sp);           // always fits the screen (shrinks rather than scrolls)
  const cap = capacity(sp);
  const size = Math.max(60, fit.size);
  faceSizePx = size;
  $("faces").style.gridTemplateColumns = `repeat(${fit.cols}, ${size}px)`;
  tileEls.forEach(t => t.wrap.style.width = size + "px");
  $("gCap").textContent = `${tiles.length} of ${cap} fit`;
  $("gAdd").disabled = tiles.length >= cap;
}

function buildFaces() {
  const allowed = allowedPages();
  tiles = tiles.map(t => allowed.includes(t) ? t : allowed[0]);
  $("faces").innerHTML = "";
  tileEls.length = 0;
  tiles.forEach((type, i) => {
    const wrap = document.createElement("div");
    wrap.className = "ft";
    const cv = document.createElement("canvas");
    cv.className = "face"; cv.width = 480; cv.height = 480;
    cv.setAttribute("aria-label", "Gauge: " + PAGES[type]);
    wrap.appendChild(cv);
    if (editMode) {
      const ed = document.createElement("div");
      ed.className = "ed";
      ed.innerHTML = `<select aria-label="Page for gauge ${i + 1}">${allowed.map(p => `<option value="${p}" ${p === type ? "selected" : ""}>${PAGES[p]}</option>`).join("")}</select>
        <button class="btn ghost" type="button" aria-label="Remove gauge ${i + 1}">X</button>`;
      ed.querySelector("select").onchange = e => { tiles[i] = +e.target.value; saveTiles(); };
      ed.querySelector("button").onclick = () => { if (tiles.length > 1) { tiles.splice(i, 1); saveTiles(); buildFaces(); } };
      wrap.appendChild(ed);
    }
    $("faces").appendChild(wrap);
    const el = { wrap, cv, ctx: cv.getContext("2d") };
    tileEls.push(el);
    attachFaceInput(cv, i);
  });
  layoutFaces();
}

function attachFaceInput(cv, i) {                 // swipe = change page, tap = keypad buttons, hold 2 s = reset peak
  let x0 = 0, y0 = 0, holdTimer = null, held = false;
  const pt = e => { const r = cv.getBoundingClientRect(); return [(e.clientX - r.left) * 480 / r.width, (e.clientY - r.top) * 480 / r.height]; };
  cv.onpointerdown = e => {
    [x0, y0] = pt(e); held = false;
    clearTimeout(holdTimer);
    holdTimer = setTimeout(() => { held = true; GaugeView.resetPeak(tiles[i]); }, 2000);
  };
  cv.onpointerup = e => {
    clearTimeout(holdTimer);
    if (held) return;
    const [x, y] = pt(e), dx = x - x0, dy = y - y0;
    if (Math.abs(dx) > 60 && Math.abs(dx) > Math.abs(dy)) {
      const allowed = allowedPages(), k = allowed.indexOf(tiles[i]);
      tiles[i] = allowed[((k + (dx < 0 ? 1 : -1)) % allowed.length + allowed.length) % allowed.length];
      saveTiles();
      if (editMode) buildFaces();
      return;
    }
    if (Math.abs(dx) < 15 && Math.abs(dy) < 15) GaugeView.tap(tiles[i], x, y);
  };
  cv.onpointerleave = cv.onpointercancel = () => clearTimeout(holdTimer);
}

$("gAdd").onclick = () => {
  const allowed = allowedPages(), used = new Set(tiles);
  tiles.push(allowed.find(p => !used.has(p)) ?? allowed[0]);   // next page not already shown
  saveTiles(); buildFaces();
};
$("gEdit").onclick = () => { editMode = !editMode; $("gEdit").textContent = editMode ? "Done" : "Edit"; buildFaces(); };
$("gFull").onclick = () => {
  if (document.fullscreenElement) document.exitFullscreen();
  else if ($("tab-gauge").requestFullscreen) $("tab-gauge").requestFullscreen().catch(() => {});
};
document.addEventListener("fullscreenchange", () => {
  $("gFull").textContent = document.fullscreenElement ? "Exit full screen" : "Full screen";
  setTimeout(layoutFaces, 50);
});
window.addEventListener("resize", () => layoutFaces());

let lastFace = 0;
function faceLoop(t) {
  const period = tileEls.length > 4 ? 66 : 45;     // lighter refresh with lots of gauges
  if (gaugeTab && !document.hidden && t - lastFace > period) {
    lastFace = t;
    const o = faceOpts(faceSizePx);
    tileEls.forEach((el, i) => GaugeView.render(tiles[i], el.ctx, el, o));
  }
  requestAnimationFrame(faceLoop);
}

// ---------------- Warnings (top of the Live tab) ----------------
const LOW_WARN = ["OIL P", "FUEL", "BATT"];      // for these, the worst value is the lowest
let warnHist = JSON.parse(localStorage.getItem("warnHist") || "{}");

function recordHistory(list) {
  const now = Date.now();
  list.forEach(w => {
    const v = parseFloat(w.val), h = warnHist[w.name];
    if (!h) { warnHist[w.name] = { unit: w.unit, worst: w.val, first: now, last: now, count: 1, active: true }; return; }
    if (!h.active) { h.count++; h.active = true; }
    h.last = now; h.unit = w.unit;
    const worse = LOW_WARN.includes(w.name) ? v < parseFloat(h.worst) : v > parseFloat(h.worst);
    if (!isNaN(v) && worse) h.worst = w.val;
  });
  for (const k in warnHist) if (!list.find(w => w.name === k)) warnHist[k].active = false;
  localStorage.setItem("warnHist", JSON.stringify(warnHist));
}

function renderDiag() {
  $("warnList").innerHTML = !connected ? `<div class="empty">Connect to the gauge to see warnings.</div>`
    : diagWarns.length ? diagWarns.map(w => `<div class="wl"><span>${w.name}${w.ack ? " (dismissed)" : ""}</span><span class="v">${w.val} ${w.unit}</span></div>`).join("")
    : `<div class="empty">No active warnings.</div>`;
  const keys = Object.keys(warnHist).sort((a, b) => warnHist[b].last - warnHist[a].last);
  const when = t => new Date(t).toLocaleString([], { day: "numeric", month: "short", hour: "2-digit", minute: "2-digit" });
  $("histList").innerHTML = keys.length ? keys.map(k => { const h = warnHist[k];
    return `<div class="wl"><span>${k}</span><span class="v">${h.worst} ${h.unit}</span><small>${h.count} time${h.count > 1 ? "s" : ""}, last ${when(h.last)}</small></div>`; }).join("")
    : `<div class="empty">No warnings recorded.</div>`;
}

let warnPollBusy = false, lastWarnMask = -1;
async function pollWarns() {
  if (!connected || warnPollBusy) return;
  if (gaugeStatus.warn === 0 && lastWarnMask === 0) return;   // nothing active and nothing changed
  warnPollBusy = true;
  try {
    const r = await cmd({ cmd: "warns" });
    if (r.ok && Array.isArray(r.warns)) {
      diagWarns = r.warns.map(w => ({ ...w, unit: String(w.unit).replace("*", "°") }));
      recordHistory(diagWarns);
      lastWarnMask = gaugeStatus.warn;
      renderDiag();
    }
  } catch { /* older firmware or busy: try again next time */ }
  warnPollBusy = false;
}
$("ackBtn").onclick = () => cmd({ cmd: "ack" }).then(() => setTimeout(pollWarns, 300)).catch(e => alert(e.message));
$("histClear").onclick = () => { if (confirm("Clear the warning history on this device?")) { warnHist = {}; localStorage.removeItem("warnHist"); renderDiag(); } };

// ---------------- Tabs and start-up ----------------
// The tab bar only shows what the car has (see Prof.fitted); a "+" button appears when something is hidden.
const TABS = [{ id: "gauge", label: "Gauge" }, { id: "keypad", label: "CAN<br>keypad", opt: "keypad" }, { id: "lap", label: "Lap<br>timer", opt: "gps" },
  { id: "logs", label: "Logs" }, { id: "live", label: "Live" }, { id: "settings", label: "Setup" }, { id: "device", label: "Device" }];
let curTab = "gauge";
function selectTab(id) {
  curTab = id;
  document.querySelectorAll("nav button[data-tab]").forEach(x => x.setAttribute("aria-selected", x.dataset.tab === id));
  document.querySelectorAll("main section").forEach(s => s.hidden = s.id !== "tab-" + id);
  gaugeTab = id === "gauge";
  document.body.classList.toggle("gaugeMode", gaugeTab);
  const wasLive = liveTab;
  liveTab = id === "live";
  if (liveTab !== wasLive) subscribe().catch(() => {});
  if (id === "logs") Logs.refresh();
  if (id === "lap") Track.redraw();
  if (id === "settings") Prof.renderBar();
  if (id === "device") Prof.renderDevice();
  if (gaugeTab) setTimeout(layoutFaces, 0);
  if (id === "live") renderDiag();
}
function rebuildNav() {
  const f = Prof.fitted();
  const vis = TABS.filter(t => !t.opt || f[t.opt]);
  const nav = $("nav");
  nav.innerHTML = vis.map(t => `<button role="tab" aria-selected="${t.id === curTab}" data-tab="${t.id}">${t.label}</button>`).join("") +
    (vis.length < TABS.length ? `<button class="plus" type="button" id="navPlus" aria-label="Show hidden tabs">+</button>` : "");
  nav.querySelectorAll("[data-tab]").forEach(b => b.onclick = () => selectTab(b.dataset.tab));
  if ($("navPlus")) $("navPlus").onclick = () => Prof.customise();
  if (!vis.some(t => t.id === curTab)) selectTab("gauge");
  sizeGaugeTab();
}
$("connectBtn").onclick = connectClicked;
$("rate").onchange = () => subscribe().catch(() => {});

$("wizBtn").onclick = () => Prof.wizard();
$("custBtn").onclick = () => Prof.customise();
{ const c = Prof.cached(); if (c) { S = c; try { renderSettings(); } catch {} } }       // Setup works without the gauge, from the copy saved on this phone
rebuildNav(); Prof.renderBar(); Prof.renderDevice();
buildPinned();
buildChannelList();
buildKeypad();
setInterval(refreshLive, 100);                  // screen updates 10 times a second
setInterval(() => { if (gaugeStatus.warn !== lastWarnMask || gaugeStatus.warn) pollWarns(); }, 1000);
renderDiag();
buildFaces();
Logs.init();

// Per-device Gauge tab size and style
$("faceSize").value = localStorage.getItem("faceSize") || "small";
$("faceStyle").value = localStorage.getItem("faceStyle") || "dot";
$("faceSize").onchange = () => { localStorage.setItem("faceSize", $("faceSize").value); layoutFaces(); };
$("faceStyle").onchange = () => { localStorage.setItem("faceStyle", $("faceStyle").value); tileEls.forEach(t => t.sig = ""); };
document.body.classList.add("gaugeMode");
setTimeout(layoutFaces, 50);

// "?" help: every page with explanations gets a ? button at the top that shows them
function addHelpButtons() {
  document.querySelectorAll("main > section").forEach(sec => {
    if (sec.querySelector(":scope > .helpBtn") || !sec.querySelector(".lead:not(.msg)")) return;
    const b = document.createElement("button");
    b.className = "helpBtn"; b.type = "button"; b.textContent = "?"; b.setAttribute("aria-label", "Help for this page");
    b.onclick = () => { sec.classList.toggle("showhelp"); b.setAttribute("aria-expanded", sec.classList.contains("showhelp")); if (sec.id === "tab-gauge") setTimeout(layoutFaces, 0); };
    sec.prepend(b);
  });
}
addHelpButtons();
setInterval(addHelpButtons, 2000);                // sections filled in later (settings, logs) get theirs too

// CAN keypad full screen: big buttons filling the screen, for use in the car
$("kpFull").onclick = () => {
  if (document.fullscreenElement) document.exitFullscreen();
  else if ($("tab-keypad").requestFullscreen) $("tab-keypad").requestFullscreen().catch(() => {});
};
document.addEventListener("fullscreenchange", () => { $("kpFull").textContent = document.fullscreenElement === $("tab-keypad") ? "Exit full screen" : "Full screen"; });

// Demo mode: made-up data whenever no gauge is connected
$("demoOn").checked = localStorage.getItem("demoOn") === "1";
const demoCheck = () => { if ($("demoOn").checked && !connected) Demo.start(); else Demo.stop(); };
$("demoOn").onchange = () => { localStorage.setItem("demoOn", $("demoOn").checked ? "1" : "0"); demoCheck(); };
setInterval(demoCheck, 1000);
demoCheck();

// Auto-connect: reconnect to the gauge this browser has already paired with
$("autoConn").checked = localStorage.getItem("autoConn") !== "0";
$("autoConn").onchange = () => localStorage.setItem("autoConn", $("autoConn").checked ? "1" : "0");
async function autoConnect() {
  if (!$("autoConn").checked || connected || !navigator.bluetooth || !navigator.bluetooth.getDevices) return;
  try {
    const list = await navigator.bluetooth.getDevices();
    const saved = localStorage.getItem("gaugeId");
    const d = list.find(x => x.id === saved) || list.find(x => /BPD/i.test(x.name || ""));
    if (!d) return;
    dev = d;
    dev.addEventListener("gattserverdisconnected", onDisconnected);
    setState("Looking for gauge…", false);
    try { await openLink(); return; } catch { /* not in range yet: wait for it to advertise */ }
    if (dev.watchAdvertisements) {
      dev.addEventListener("advertisementreceived", async () => {
        if (connected) return;
        try { await dev.unwatchAdvertisements?.(); } catch {}
        try { await openLink(); } catch { setState("Not connected", false); }
      }, { once: true });
      await dev.watchAdvertisements();
    } else setState("Not connected", false);
  } catch { setState("Not connected", false); }
}
autoConnect();
$("startTab").value = localStorage.getItem("startTab") || "gauge";
$("startTab").onchange = () => localStorage.setItem("startTab", $("startTab").value);
{ const t = document.querySelector(`nav button[data-tab="${$("startTab").value}"]`); if (t && $("startTab").value !== "gauge") setTimeout(() => selectTab($("startTab").value), 0); }
Track.render($("gpsBox"));                       // lap timer tab works without the gauge (map, library, import)
setInterval(() => { if (connected && !document.getElementById("tab-logs").hidden) Logs.refresh(); }, 5000);
requestAnimationFrame(faceLoop);
if ("serviceWorker" in navigator) navigator.serviceWorker.register("sw.js").catch(() => {});
