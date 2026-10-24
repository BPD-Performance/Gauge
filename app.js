"use strict";
// BPD Gauge app: Bluetooth link to the gauge (Web Bluetooth, Chrome on Android / Windows)

const SVC  = "b9d00001-6a1e-4c2a-9e3d-6b5a8f0c1d01";
const RX   = "b9d00002-6a1e-4c2a-9e3d-6b5a8f0c1d01";   // commands to the gauge
const TX   = "b9d00003-6a1e-4c2a-9e3d-6b5a8f0c1d01";   // replies from the gauge
const LIVE = "b9d00004-6a1e-4c2a-9e3d-6b5a8f0c1d01";   // live data
const $ = id => document.getElementById(id);
const enc = new TextEncoder(), dec = new TextDecoder();

let dev = null, rxC = null, connected = false, userDisconnect = false, retryMs = 2000;
let S = null;                                   // gauge settings
const frames = {};                              // latest CAN data by ID: {data, t}
const gaugeStatus = { kmh: null, gear: null, wifi: 0, held: 0, sim: 0, t: 0 };

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
    const w = waiters.shift();
    if (w) { clearTimeout(w.timer); try { w.resolve(JSON.parse(line)); } catch { w.resolve({ ok: false, err: "bad reply" }); } }
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
    const text = typeof obj === "string" ? obj : JSON.stringify(obj);
    const timer = setTimeout(() => { waiters = waiters.filter(w => w.timer !== timer); reject(new Error("no reply")); }, 6000);
    waiters.push({ resolve, timer });
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
    gaugeStatus.wifi = v.getUint8(7);
    gaugeStatus.held = v.getUint16(8, true);
    gaugeStatus.sim = n >= 11 ? v.getUint8(10) : 0;
    gaugeStatus.t = now;
  }
}

// ---------------- Connection ----------------
function setState(text, isConnected) {
  $("state").textContent = text;
  $("connectBtn").textContent = isConnected ? "Disconnect" : "Connect";
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

  const info = await cmd({ cmd: "info" });
  $("devInfo").innerHTML = `<p>${info.name || "BPD Gauge"}</p>Firmware ${info.fw}, protocol ${info.proto}`;
  await subscribe();
  await loadSettings();
}

async function subscribe() {
  if (connected) await cmd({ cmd: "sub", ids: [], hz: +$("rate").value });
}

function onDisconnected() {
  connected = false;
  rxC = null;
  waiters.forEach(w => clearTimeout(w.timer)); waiters = [];
  setState(userDisconnect ? "Not connected" : "Reconnecting…", false);
  if (!userDisconnect) setTimeout(tryReconnect, retryMs);
}

async function tryReconnect() {
  if (connected || userDisconnect || !dev) return;
  try { await openLink(); }
  catch { retryMs = Math.min(retryMs * 1.5, 10000); setState("Reconnecting…", false); setTimeout(tryReconnect, retryMs); }
}

async function connectClicked() {
  if (connected) { userDisconnect = true; dev.gatt.disconnect(); return; }
  if (!navigator.bluetooth) { alert("This browser has no Bluetooth support. Use Chrome on Android or Windows."); return; }
  userDisconnect = false;
  try {
    if (!dev) {
      dev = await navigator.bluetooth.requestDevice({ filters: [{ services: [SVC] }] });
      dev.addEventListener("gattserverdisconnected", onDisconnected);
    }
    await openLink();
  } catch (err) {
    setState("Not connected", false);
    if (err.name !== "NotFoundError") alert("Could not connect: " + err.message);
  }
}

// Reconnect automatically to a gauge chosen before (where Chrome supports it)
async function autoConnect() {
  if (!navigator.bluetooth || !navigator.bluetooth.getDevices) return;
  try {
    const list = await navigator.bluetooth.getDevices();
    const d = list.find(x => (x.name || "").includes("BPD"));
    if (!d) return;
    dev = d;
    dev.addEventListener("gattserverdisconnected", onDisconnected);
    await tryReconnect();
  } catch { /* the Connect button still works */ }
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
    : `<div class="empty">Tap ☆ next to any channel below to pin it here.</div>`;
}

function buildChannelList() {
  const groups = [{ title: "Gauge", items: DERIVED.map(d => ({ key: d.key, name: d.name })) }];
  let last = -1;
  H.forEach(c => {
    if (c.id !== last) { groups.push({ title: "0x" + c.id.toString(16).toUpperCase(), items: [] }); last = c.id; }
    groups[groups.length - 1].items.push({ key: c.key, name: c.name });
  });
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
  const q = $("search").value.trim().toLowerCase(), showAll = $("showAll").checked;
  for (const k in rowEls) {
    const row = rowEls[k], r = channelValue(k);
    row.querySelector(".v").textContent = r ? `${r[0]} ${r[1]}`.trim() : "--";
    row.classList.toggle("stale", !r);
    row.hidden = (q && !row.dataset.name.includes(q)) || (!showAll && !r);
  }
  // Status chips
  const chips = [];
  chips.push(`<span class="chip">${connected ? "Connected" : "Not connected"}</span>`);
  const sp = channelValue("d_speed"), gr = channelValue("d_gear");
  if (sp) chips.push(`<span class="chip">${sp[0]} ${sp[1]}</span>`);
  if (gr) chips.push(`<span class="chip">Gear ${gr[0]}</span>`);
  if (gaugeStatus.wifi) chips.push(`<span class="chip">Gauge WiFi on</span>`);
  if (gaugeStatus.sim) chips.push(`<span class="chip alarm">SIMULATOR (fake data)</span>`);
  $("simBtn").textContent = gaugeStatus.sim ? "Stop simulator" : "Start simulator";
  $("statusChips").innerHTML = chips.join("");
  refreshKeypad();
}

// ---------------- Keypad tab ----------------
let keyLabels = JSON.parse(localStorage.getItem("keyLabels") || "null") ||
  Object.fromEntries(Array.from({ length: 15 }, (_, i) => [i + 1, i === 0 ? "Boost +" : i === 1 ? "Boost −" : `Button ${i + 1}`]));
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
const PAGES = ["Boost", "Oil pressure", "AFR", "RPM and speed", "Coolant"];
const pTo = (k, u) => u == 0 ? k : u == 1 ? k / 100 : k / 6.894757, pFrom = (v, u) => u == 0 ? v : u == 1 ? v * 100 : v * 6.894757;
const tTo = (c, u) => u == 1 ? c * 9 / 5 + 32 : c, tFrom = (v, u) => u == 1 ? (v - 32) * 5 / 9 : v;
const WARN = [
  { key: "w_op", title: "Low oil pressure", f: [["w_op", "Below", "p"], ["w_op_rpm", "Only above (rpm)", "r"]] },
  { key: "w_clt", title: "Coolant hot", f: [["w_clt", "Above", "t"]] },
  { key: "w_lean", title: "Lean on boost", f: [["w_lean", "Leaner than (lambda)", "l"], ["w_lean_bst", "Only above boost", "p"]] },
  { key: "w_bst", title: "Overboost", f: [["w_bst", "Above", "p"]] },
  { key: "w_ot", title: "Oil hot", f: [["w_ot", "Above", "t"]] },
  { key: "w_shift", title: "Shift light", f: [["w_shift", "At (rpm)", "r"]] }];
const uLbl = k => k == "p" ? PU[S.pu] : k == "t" ? TU[S.tu] : "";
const shown = (v, k) => k == "p" ? pTo(v, S.pu).toFixed(S.pu == 1 ? 2 : S.pu == 2 ? 1 : 0) : k == "t" ? tTo(v, S.tu).toFixed(0) : k == "l" ? (+v).toFixed(2) : Math.round(v);
const metric = (v, k) => { v = parseFloat(v); return k == "p" ? pFrom(v, S.pu) : k == "t" ? tFrom(v, S.tu) : v; };
const fld = (id, label, attrs = "") => `<div><label for="${id}">${label}</label><input id="${id}" inputmode="decimal" ${attrs}></div>`;
const sel = (id, label, opts) => `<div><label for="${id}">${label}</label><select id="${id}">${opts.map((o, i) => `<option value="${Array.isArray(o) ? o[0] : i}">${Array.isArray(o) ? o[1] : o}</option>`).join("")}</select></div>`;

async function loadSettings() {
  const r = await cmd({ cmd: "get" });
  if (r.ok) { S = r.settings; renderSettings(); }
}

function renderSettings() {
  const tyre = (p, title) => `<div class="card"><p>${title}</p><div class="grid3">${fld(p + "w", "Width")}${fld(p + "a", "Profile")}${fld(p + "r", "Rim (in)")}</div></div>`;
  $("settingsBody").innerHTML = `
  <h2>Gearing and tyres</h2>
  <div class="grid" style="margin-bottom:10px">${sel("numGears", "Gears", [[4, 4], [5, 5], [6, 6]])}${fld("fd", "Final drive")}</div>
  <div class="grid3" id="gearBox" style="margin-bottom:10px"></div>
  ${tyre("r", "Rear tyres (fitted now)")}${tyre("f", "Front tyres")}${tyre("c", "Tyres the ECU speed is calibrated for")}
  ${sel("spdSrc", "ECU speed comes from", ["Use ECU speed as is", "Gearbox or rear wheels", "Front wheels"])}
  <div style="height:10px"></div>${sel("gearSrc", "Gear display", ["Work out from RPM", "Use ECU gear", "Off"])}
  <h2>Units</h2>
  <div class="grid">${sel("pu", "Pressure", PU)}${sel("tu", "Temperature", TU)}${sel("su", "Speed", SU)}${sel("lam", "Mixture", ["AFR", "Lambda"])}
   ${sel("flex", "AFR stoich", [[1, "Follow ethanol content"], [0, "Fixed value"]])}${fld("stoich", "Fixed stoich")}</div>
  <h2>Gauge pages</h2><div class="list" id="pagesBox"></div>
  <h2>Warnings</h2><div id="warnBox"></div>
  <h2>Screen</h2>
  <div class="grid">${fld("bright", "Brightness (%)")}${sel("tachmax", "Tach dial max", [[7000, 7000], [8000, 8000], [9000, 9000], [10000, 10000]])}</div>
  <div class="actions"><button class="btn" id="saveBtn" type="button">Save to gauge</button><button class="btn ghost" id="reloadBtn" type="button">Reload from gauge</button></div>`;

  ["fd", "rw", "ra", "rr", "fw", "fa", "fr", "cw", "ca", "cr", "spdSrc", "gearSrc", "pu", "tu", "su", "lam", "flex", "stoich", "bright", "tachmax", "numGears"]
    .forEach(k => $(k).value = S[k]);
  const gears = () => { $("gearBox").innerHTML = Array.from({ length: +$("numGears").value }, (_, i) => fld("g" + (i + 1), "Gear " + (i + 1))).join("");
    for (let i = 1; i <= +$("numGears").value; i++) $("g" + i).value = S["g" + i]; };
  gears();
  $("numGears").onchange = () => { readForm(); gears(); };
  ["pu", "tu"].forEach(k => $(k).onchange = () => { readWarns(); S[k] = +$(k).value; renderWarns(); });
  renderPages(); renderWarns();
  $("saveBtn").onclick = saveSettings;
  $("reloadBtn").onclick = () => loadSettings().catch(e => $("msg").textContent = e.message);
}

function renderPages() {
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
  ["numGears", "spdSrc", "gearSrc", "lam", "flex", "tachmax"].forEach(k => S[k] = +$(k).value);
  for (let i = 1; i <= 6; i++) if ($("g" + i)) S["g" + i] = parseFloat($("g" + i).value);
  readWarns();
}

async function saveSettings() {
  readForm();
  S.pu = +$("pu").value; S.tu = +$("tu").value; S.su = +$("su").value;
  $("msg").textContent = "Saving…";
  try {
    const r = await cmd({ cmd: "set", ...S });
    $("msg").textContent = r.ok ? "Saved. The gauge has updated." : "Not saved: " + r.err;
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
  try { const r = await cmd({ cmd: "defaults" }); if (r.ok) setTimeout(() => loadSettings(), 1500); else alert(r.err); } catch (e) { alert(e.message); }
};

// ---------------- Tabs and start-up ----------------
document.querySelectorAll("nav button").forEach(b => b.onclick = () => {
  document.querySelectorAll("nav button").forEach(x => x.setAttribute("aria-selected", x === b));
  document.querySelectorAll("main section").forEach(s => s.hidden = s.id !== "tab-" + b.dataset.tab);
});
$("connectBtn").onclick = connectClicked;
$("rate").onchange = () => subscribe().catch(() => {});

buildPinned();
buildChannelList();
buildKeypad();
setInterval(refreshLive, 100);                  // screen updates 10 times a second
autoConnect();
if ("serviceWorker" in navigator) navigator.serviceWorker.register("sw.js").catch(() => {});
