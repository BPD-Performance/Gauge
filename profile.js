"use strict";
// Car profiles: what is fitted, the gauge settings, and who last had them.
// A profile is keyed by its own id (not the Bluetooth id), so a gauge can move between phones and cars.
// Kept on this phone; the gauge only remembers which profile id is on it. The Bluetooth PIN is never part of a profile.

const Prof = (() => {
  const KEY = "bpdProfiles", ACT = "bpdActive";
  // Only things the gauge can't work out for itself. Sensors (oil, fuel, wideband, coolant pressure) are learned from the CAN data.
  const FEATURES = [
    { k: "keypad", label: "CAN keypad", help: "Haltech CAN keypad buttons from the gauge. Adds the keypad tab and gauge page." },
    { k: "gps", label: "GPS module (lap timer)", help: "Adds the lap timer tab and gauge page. The next step checks the module is working." }];
  const ALL = Object.fromEntries(FEATURES.map(f => [f.k, true]));
  const PG = { boost: 0, oil: 1, afr: 2, tach: 3, coolant: 4, fuel: 5, iat: 6, monitor: 7, keypad: 8, alerts: 9, lap: 10, face: 11 };

  let profiles = {}, activeId = null, lastInfo = null;
  try { profiles = JSON.parse(localStorage.getItem(KEY) || "{}") || {}; activeId = localStorage.getItem(ACT); } catch { profiles = {}; }
  if (activeId && !profiles[activeId]) activeId = null;

  const clone = o => JSON.parse(JSON.stringify(o));
  const newId = () => "p" + Math.random().toString(36).slice(2, 10);
  const persist = () => { try { localStorage.setItem(KEY, JSON.stringify(profiles)); if (activeId) localStorage.setItem(ACT, activeId); } catch {} };
  const active = () => profiles[activeId] || null;
  const when = t => t ? new Date(t).toLocaleString([], { day: "numeric", month: "short", year: "numeric", hour: "2-digit", minute: "2-digit" }) : "never";
  const esc = s => String(s).replace(/[&<>"]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
  const cleanName = s => String(s || "").replace(/[^A-Za-z0-9 _-]/g, "").trim().slice(0, 24);
  const fitted = () => (active() && active().fitted) || ALL;

  // ---------- small modal helper ----------
  function modal(build) {
    return new Promise(res => {
      const m = document.createElement("div");
      m.className = "mdl";
      m.innerHTML = `<div class="box" role="dialog" aria-modal="true"></div>`;
      document.body.appendChild(m);
      const close = v => { m.remove(); res(v); };
      build(m.firstChild, close);
    });
  }
  const choose = (title, text, opts) => modal((box, close) => {
    box.innerHTML = `<h3>${esc(title)}</h3><p class="lead">${text}</p>` + opts.map((o, i) =>
      `<button class="btn opt ${o.ghost ? "ghost" : ""}" type="button" data-i="${i}">${esc(o.label)}${o.sub ? `<small>${esc(o.sub)}</small>` : ""}</button>`).join("");
    box.querySelectorAll("[data-i]").forEach(b => b.onclick = () => close(opts[+b.dataset.i].value));
  });
  const askName = (title, def) => modal((box, close) => {
    box.innerHTML = `<h3>${esc(title)}</h3><label for="pnm">Name</label><input id="pnm" maxlength="24" value="${esc(def || "")}">
      <div class="actions"><button class="btn" id="pok" type="button">OK</button><button class="btn ghost" id="pno" type="button">Cancel</button></div>`;
    box.querySelector("#pok").onclick = () => close(cleanName(box.querySelector("#pnm").value) || null);
    box.querySelector("#pno").onclick = () => close(null);
  });

  // ---------- comparing settings ----------
  const LABELS = { pu: "Pressure unit", tu: "Temperature unit", su: "Speed unit", lam: "Mixture", fd: "Final drive", numGears: "Gears", bright: "Brightness", tachmax: "Tach max",
    w_clt: "Coolant hot", w_op: "Low oil pressure", w_bst: "Overboost", w_lean: "Lean on boost", w_ot: "Oil hot", w_shift: "Shift light", on: "Pages on", order: "Page order" };
  const norm = v => typeof v === "number" ? Math.round(v * 1000) / 1000 : Array.isArray(v) ? v.map(norm) : v;
  function diff(a, b) {
    const out = [];
    new Set([...Object.keys(a || {}), ...Object.keys(b || {})]).forEach(k => {
      if (JSON.stringify(norm(a[k])) !== JSON.stringify(norm(b[k]))) out.push(k);
    });
    return out;
  }
  const show = v => v === undefined ? "–" : Array.isArray(v) ? v.join(",") : String(norm(v));

  // ---------- profile bookkeeping ----------
  function create(name, settings, extra) {
    const p = Object.assign({ id: newId(), name: name || "My car", fitted: clone(ALL), done: false, settings: settings ? clone(settings) : null, updated: Date.now(), lastConnected: 0, pending: false }, extra || {});
    profiles[p.id] = p; activeId = p.id; persist();
    return p;
  }
  function fittedFromSettings(s) {
    const f = clone(ALL);
    if (s && s.on) { f.keypad = !!s.on[PG.keypad]; f.gps = !!s.on[PG.lap]; }
    return f;
  }
  function snapshot() {                          // S was saved to the gauge (or taken from it)
    if (!S) return;
    const p = active() || create("My car", S);
    p.settings = clone(S); p.pending = false; p.updated = Date.now(); persist();
  }
  function saveLocal() {                         // S was edited while the gauge isn't connected
    if (!S) return null;
    const p = active() || create("My car", S);
    p.settings = clone(S); p.pending = true; p.updated = Date.now(); persist();
    return p;
  }
  const pushGauge = async (p, withSettings) => {
    if (withSettings) { const r = await cmd({ cmd: "set", ...p.settings }); if (!r.ok) throw new Error(r.err || "gauge refused"); }
    const r = await cmd({ cmd: "setupset", state: p.done ? 2 : 1, pid: p.id, pname: p.name }).catch(() => ({}));   // older firmware: ignored
    if (r.ok && lastInfo) { lastInfo.setup = p.done ? 2 : 1; lastInfo.pid = p.id; lastInfo.pname = p.name; }
  };

  // ---------- on connect ----------
  async function onConnect(info, gS, adoptOnly) {
    lastInfo = info || {};
    const legacy = lastInfo.setup === undefined;
    const gid = legacy ? "dev" + (localStorage.getItem("gaugeId") || "x").replace(/[^A-Za-z0-9]/g, "").slice(0, 10) : (lastInfo.pid || "");
    const gname = lastInfo.name || "BPD Gauge";
    let p = gid ? profiles[gid] : null;

    if (adoptOnly) {                             // "Reload from gauge": take the gauge's settings as they are
      if (active()) { active().settings = clone(gS); active().pending = false; active().lastConnected = Date.now(); persist(); }
      return;
    }

    if (!p && !legacy && !gid) {
      // The gauge has no profile on it yet.
      if (lastInfo.setup === 0) {
        const c = await choose("This gauge isn't set up yet", "It is running a simple basic view for now. The setup wizard takes a minute and turns on only the pages and warnings your car has.",
          [{ label: "Set up now", sub: "Recommended", value: "wiz" }, { label: "Not now", sub: "Keep the basic view. You can run the wizard any time in Setup.", ghost: true, value: "skip" }]);
        p = create(gname, gS, { gaugeName: gname });
        if (c === "wiz") { await wizard(); }
        else await cmd({ cmd: "setupset", state: 1, pid: p.id, pname: p.name }).catch(() => {});
        p.lastConnected = Date.now(); persist();
        return;
      }
      // Set up before this firmware knew about profiles (or on another phone): keep what it has
      p = create(gname === "BPD Gauge" ? "My car" : gname, gS, { done: lastInfo.setup === 2, fitted: fittedFromSettings(gS), gaugeName: gname });
      await cmd({ cmd: "setupset", pid: p.id, pname: p.name }).catch(() => {});
      p.lastConnected = Date.now(); persist();
      return;
    }
    if (!p) {                                    // gauge names a profile this phone hasn't seen (another phone, or a transfer)
      p = create(lastInfo.pname || "My car", gS, { id: gid, done: lastInfo.setup === 2 || legacy, fitted: fittedFromSettings(gS), gaugeName: gname });
      p.lastConnected = Date.now(); persist();
      return;
    }
    activeId = p.id; p.gaugeName = gname;
    const d = p.settings ? diff(p.settings, gS) : [];
    if (!p.settings) { p.settings = clone(gS); }
    else if (d.length) {
      const first = d.slice(0, 3).map(k => LABELS[k] || k).join(", ") + (d.length > 3 ? ` and ${d.length - 3} more` : "");
      let c;
      for (;;) {
        c = await choose(p.pending ? "You changed settings on this phone" : "The gauge's settings are different",
          `${d.length} setting${d.length > 1 ? "s" : ""} differ between this phone's copy of <b>${esc(p.name)}</b> and the gauge (${esc(first)}).`,
          [{ label: "Use the gauge's settings", sub: "Update this phone to match the gauge", value: "gauge" },
           { label: "Send this phone's settings", sub: "Update the gauge to match this phone", value: "phone" },
           { label: "Review the differences", ghost: true, value: "review" },
           { label: "Decide later", ghost: true, value: "later" }]);
        if (c !== "review") break;
        await modal((box, close) => {
          box.innerHTML = `<h3>Differences</h3><table class="diff"><tr><th>Setting</th><th>This phone</th><th>Gauge</th></tr>${d.map(k =>
            `<tr><td>${esc(LABELS[k] || k)}</td><td>${esc(show(p.settings[k]))}</td><td>${esc(show(gS[k]))}</td></tr>`).join("")}</table>
            <div class="actions"><button class="btn" id="back" type="button">Back</button></div>`;
          box.querySelector("#back").onclick = () => close();
        });
      }
      if (c === "gauge") { p.settings = clone(gS); p.pending = false; }
      else if (c === "phone") { await pushGauge(p, true); S = clone(p.settings); p.pending = false; }
      else { /* later: show the gauge's values, leave the saved copy alone, ask again next time */ }
    } else p.pending = false;
    if (p.settings && !d.length) S = clone(gS);
    if (!legacy && (lastInfo.pid !== p.id || (p.done && lastInfo.setup !== 2))) await pushGauge(p, false);
    p.lastConnected = Date.now(); persist();
  }

  // ---------- setup wizard ----------
  async function wizard() {
    if (!S) { await choose("Connect first", "The wizard needs the gauge's current settings. Connect to the gauge, then run it again from Setup.", [{ label: "OK", value: 1 }]); return false; }
    const p = active() || create("My car", S);
    const w = { name: p.name, f: Object.assign(clone(ALL), p.fitted), pu: S.pu, tu: S.tu, su: S.su, numGears: S.numGears,
      fd: S.fd, g: [1, 2, 3, 4, 5, 6].map(i => S["g" + i]), rw: S.rw, ra: S.ra, rr: S.rr };
    if (!p.done) {                               // first time: start from what the gauge already shows
      w.f.keypad = !!S.on[PG.keypad]; w.f.gps = !!S.on[PG.lap];
    }
    w.skipGear = false;
    const steps = [
      { t: "Name this car", body: () => `<p class="lead">Each car gets its own profile, so the gauge can move between cars and phones.</p><label for="wn">Profile name</label><input id="wn" maxlength="24" value="${esc(w.name)}">`,
        read: b => { w.name = cleanName(b.querySelector("#wn").value) || "My car"; } },
      { t: "What's fitted?", body: () => `<p class="lead">Tick what your car has. Anything unticked is hidden and its warnings are switched off, so you never see zero readings from sensors that aren't there.</p>` +
          FEATURES.map(f => `<div class="lrow"><label class="ck2"><input type="checkbox" data-f="${f.k}" ${w.f[f.k] ? "checked" : ""}> ${f.label}</label></div><div class="lead" style="margin:0 0 8px 34px">${f.help}</div>`).join(""),
        read: b => b.querySelectorAll("[data-f]").forEach(c => w.f[c.dataset.f] = c.checked) },
      { t: "GPS module", show: () => w.f.gps, body: () => `<p class="lead">The GPS module gives the lap timer its position, and can also be the speed source and put a map on your logs. It plugs into the gauge's GPS header: module TX to the gauge's GPIO44 (RX), module RX to GPIO0 (TX), plus power and ground. It is built for u-blox M8 and M10 modules. Switching it on needs a restart of the gauge.</p>
          <div class="card" id="gpsSt">Checking…</div><div class="actions" id="gpsAct"></div>
          <p class="lead">Indoors it will say it is waiting for satellites. That is normal: go outside with a clear view of the sky, or finish now and check it later in Setup.</p>`,
        init: b => { const st = b.querySelector("#gpsSt"), act = b.querySelector("#gpsAct");
          const tick = async () => {
            if (!connected) { st.textContent = "Connect to the gauge to check the module."; act.innerHTML = ""; return; }
            try {
              const r = await cmd({ cmd: "gpsget" });
              if (!r.ok) throw new Error(r.err || "not supported");
              const g = r.gps;
              if (!g.on) { st.textContent = "The GPS module is switched off in the gauge."; act.innerHTML = `<button class="btn" id="wGpsOn" type="button">Switch on and restart gauge</button>`;
                act.querySelector("#wGpsOn").onclick = async () => { act.innerHTML = ""; st.textContent = "Restarting the gauge… it will reconnect by itself."; try { await cmd({ cmd: "gpsset", on: 1 }); await cmd({ cmd: "restart" }); } catch (e) { st.textContent = "Could not switch it on: " + e.message; } }; }
              else { act.innerHTML = "";
                st.innerHTML = g.alive === undefined ? (g.fix ? `GPS fix, ${g.sats} satellites.` : "Switched on. Waiting for a fix.")
                  : !g.alive ? "<b>No data from the module.</b> Check the wiring (TX and RX the right way round, power and ground) and that it is a u-blox M8 or M10."
                  : g.fix ? `<b>GPS fix.</b> ${g.sats} satellites. Good to go.` : `<b>Module is talking.</b> Searching for satellites (${g.sats} so far). This can take a minute or two with a clear sky.`; }
            } catch (e) { st.textContent = "Could not read the module status: " + e.message; }
          };
          tick(); b._gpsTimer = setInterval(tick, 1500); },
        leave: b => clearInterval(b._gpsTimer) },
      { t: "Units", body: () => `<div class="grid">${sel("wpu", "Pressure", PU)}${sel("wtu", "Temperature", TU)}${sel("wsu", "Speed", SU)}</div>`,
        init: b => { b.querySelector("#wpu").value = w.pu; b.querySelector("#wtu").value = w.tu; b.querySelector("#wsu").value = w.su; },
        read: b => { w.pu = +b.querySelector("#wpu").value; w.tu = +b.querySelector("#wtu").value; w.su = +b.querySelector("#wsu").value; } },
      { t: "Gearing and tyres", body: () => `<p class="lead">Used to work out the gear and corrected speed. Don't have these to hand? Skip for now: the gauge will show the gear from the ECU and the speed as the ECU reports it, and you can fill this in later in Setup.</p>
          <div class="grid">${sel("wng", "Gears", [[4, 4], [5, 5], [6, 6]])}${fld("wfd", "Final drive")}</div><div class="grid3" id="wg" style="margin:10px 0"></div>
          <div class="card"><p>Rear tyres</p><div class="grid3">${fld("wrw", "Width")}${fld("wra", "Profile")}${fld("wrr", "Rim (in)")}</div></div>`,
        init: b => { const g = () => { b.querySelector("#wg").innerHTML = Array.from({ length: +b.querySelector("#wng").value }, (_, i) => fld("wg" + i, "Gear " + (i + 1))).join("");
            for (let i = 0; i < +b.querySelector("#wng").value; i++) b.querySelector("#wg" + i).value = w.g[i] ?? ""; };
          b.querySelector("#wng").value = w.numGears; b.querySelector("#wfd").value = w.fd; b.querySelector("#wrw").value = w.rw; b.querySelector("#wra").value = w.ra; b.querySelector("#wrr").value = w.rr;
          g(); b.querySelector("#wng").onchange = () => { for (let i = 0; i < 6; i++) { const e = b.querySelector("#wg" + i); if (e && e.value !== "") w.g[i] = parseFloat(e.value); } w.numGears = +b.querySelector("#wng").value; g(); }; },
        skip: true,
        read: b => { w.skipGear = false; w.numGears = +b.querySelector("#wng").value; w.fd = parseFloat(b.querySelector("#wfd").value) || w.fd;
          for (let i = 0; i < w.numGears; i++) { const v = parseFloat(b.querySelector("#wg" + i).value); if (v > 0) w.g[i] = v; }
          w.rw = parseFloat(b.querySelector("#wrw").value) || w.rw; w.ra = parseFloat(b.querySelector("#wra").value) || w.ra; w.rr = parseFloat(b.querySelector("#wrr").value) || w.rr; } },
      { t: "Review", body: () => { const on = FEATURES.filter(f => w.f[f.k]).map(f => f.label), off = FEATURES.filter(f => !w.f[f.k]).map(f => f.label);
          return `<p class="lead">Profile <b>${esc(w.name)}</b></p><div class="card"><p>Fitted</p>${on.length ? on.map(esc).join("<br>") : "Nothing extra: basic view"}</div>
            ${off.length ? `<div class="card"><p>Not fitted (hidden)</p>${off.map(esc).join("<br>")}</div>` : ""}
            <div class="card"><p>Gearing and tyres</p>${w.skipGear ? "Skipped. Gear comes from the ECU. Fill in later in Setup." : "Entered"}</div>
            <p class="lead">Add pages like oil, fuel pressure and AFR in Setup, under Gauge pages, if your car has those sensors.</p>
            <p class="lead">${connected ? "Finish saves this to the gauge and to this phone." : "Not connected: Finish saves it on this phone and sends it the next time you connect."}</p>`; } }];

    let i = 0;
    const list = () => steps.filter(s => !s.show || s.show());
    return modal((box, close) => {
      const leave = s => { if (s.leave) s.leave(box); };
      const draw = () => {
        const s = list()[i], n = list().length;
        box.innerHTML = `<div class="steps">STEP ${i + 1} OF ${n}</div><h3>${s.t}</h3>${s.body()}
          <div class="actions">${i ? `<button class="btn ghost" id="wb" type="button">Back</button>` : `<button class="btn ghost" id="wx" type="button">Cancel</button>`}
          ${s.skip ? `<button class="btn ghost" id="wsk" type="button">Skip for now</button>` : ""}
          <button class="btn pink" id="wn2" type="button">${i === n - 1 ? "Finish" : "Next"}</button></div>`;
        if (s.init) s.init(box);
        const sk = box.querySelector("#wsk"); if (sk) sk.onclick = () => { w.skipGear = true; leave(s); i++; draw(); };
        box.querySelector("#wn2").onclick = async () => {
          if (s.read) s.read(box);
          leave(s);
          if (i < n - 1) { i++; draw(); return; }
          box.querySelector("#wn2").disabled = true; box.querySelector("#wn2").textContent = "Saving…";
          try { await finish(w); close(true); } catch (e) { box.querySelector("#wn2").disabled = false; box.querySelector("#wn2").textContent = "Finish"; alert("Could not save to the gauge: " + e.message + ". Your answers are kept on this phone."); await finishLocalOnly(w); close(true); }
        };
        const b = box.querySelector("#wb"); if (b) b.onclick = () => { if (s.read) s.read(box); leave(s); i--; draw(); };
        const x = box.querySelector("#wx"); if (x) x.onclick = () => { leave(s); close(false); };
      };
      draw();
    });
  }

  function applyWizard(w) {
    S.pu = w.pu; S.tu = w.tu; S.su = w.su; S.lam = 1;      // mixture is always lambda
    if (w.skipGear) { S.gearSrc = 1; S.spdSrc = 0; }                   // no placeholder ratios: gear from the ECU, speed as the ECU reports it
    else {
      S.numGears = w.numGears; S.fd = w.fd; S.rw = w.rw; S.ra = w.ra; S.rr = w.rr;
      w.g.forEach((v, i) => { if (v > 0 && i < w.numGears) S["g" + (i + 1)] = v; });
      if (S.gearSrc === 1) S.gearSrc = 0;                                // back to working the gear out from RPM
    }
    const on = (pg, v) => { if (S.on && S.on[pg] !== undefined) S.on[pg] = v ? 1 : 0; };
    on(PG.boost, 1); on(PG.tach, 1); on(PG.coolant, 1); on(PG.iat, 1); on(PG.monitor, 1); on(PG.alerts, 1); on(PG.face, 1);
    on(PG.keypad, w.f.keypad); on(PG.lap, w.f.gps);
    S.w_clt_on = 1; S.w_bst_on = 1; S.w_iat_on = 1; S.w_bat_on = 1; S.w_duty_on = 1; S.w_shift_on = 1;   // channels every ECU sends
    // Oil, fuel, wideband and coolant-pressure pages and warnings are left for the person to turn on in Setup if the car has those sensors.
  }
  async function finishLocalOnly(w) {
    const p = active() || create(w.name, S);
    applyWizard(w);
    p.name = w.name; p.fitted = clone(w.f); p.done = true; p.gearSkipped = !!w.skipGear; p.settings = clone(S); p.pending = true; p.updated = Date.now(); persist();
    afterChange();
  }
  async function finish(w) {
    const p = active() || create(w.name, S);
    applyWizard(w);
    p.name = w.name; p.fitted = clone(w.f); p.done = true; p.gearSkipped = !!w.skipGear; p.settings = clone(S); p.updated = Date.now();
    if (connected) { await pushGauge(p, true); p.pending = false; p.lastConnected = Date.now(); } else p.pending = true;
    persist();
    afterChange();
    if (connected) backupSd().catch(() => {});
  }
  const afterChange = () => { if (typeof rebuildNav === "function") rebuildNav(); if (typeof renderSettings === "function" && S) { renderSettings(); buildFaces(); } renderBar(); renderDevice(); };

  // ---------- customise tabs ----------
  async function customise() {
    const f = clone(fitted());
    const ok = await modal((box, close) => {
      box.innerHTML = `<h3>Customise tabs</h3><p class="lead">Show only what your car has. Gauge, Logs, Live, Setup and Device are always there. Anything you switch off can be switched back on here.</p>
        ${["keypad", "gps"].map(k => { const ft = FEATURES.find(x => x.k === k); return `<div class="lrow"><label class="ck2"><input type="checkbox" data-f="${k}" ${f[k] ? "checked" : ""}> ${k === "keypad" ? "CAN keypad tab" : "Lap timer tab"}</label></div>`; }).join("")}
        <div class="actions"><button class="btn pink" id="cok" type="button">Done</button><button class="btn ghost" id="cno" type="button">Cancel</button></div>`;
      box.querySelector("#cok").onclick = () => { box.querySelectorAll("[data-f]").forEach(c => f[c.dataset.f] = c.checked); close(true); };
      box.querySelector("#cno").onclick = () => close(false);
    });
    if (!ok) return;
    const p = active() || create("My car", S);
    p.fitted = Object.assign(clone(p.fitted || ALL), f); persist();
    afterChange();
  }

  // ---------- backup, restore, import and export ----------
  const pack = p => JSON.stringify({ bpdProfile: 1, id: p.id, name: p.name, fitted: p.fitted, done: p.done, settings: p.settings });
  function unpack(text) {
    const o = JSON.parse(text);
    if (!o || o.bpdProfile !== 1 || !o.settings || !o.id) throw new Error("not a profile file");
    return { id: String(o.id).replace(/[^A-Za-z0-9_-]/g, "").slice(0, 24) || newId(), name: cleanName(o.name) || "My car", fitted: Object.assign(clone(ALL), o.fitted), done: !!o.done, settings: o.settings };
  }
  async function addImported(o) {
    if (profiles[o.id] && !confirm(`Replace the profile "${profiles[o.id].name}" on this phone?`)) return null;
    profiles[o.id] = Object.assign({ lastConnected: 0, pending: false }, profiles[o.id] || {}, { id: o.id, name: o.name, fitted: o.fitted, done: o.done, settings: o.settings, updated: Date.now(), pending: true });
    persist(); renderDevice();
    return profiles[o.id];
  }
  function exportFile() {
    const p = active(); if (!p) return;
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([pack(p)], { type: "application/json" }));
    a.download = (p.name.replace(/ /g, "_") || "profile") + ".bpdprofile";
    document.body.appendChild(a); a.click(); a.remove(); setTimeout(() => URL.revokeObjectURL(a.href), 2000);
  }
  async function importFile() {
    const inp = document.createElement("input"); inp.type = "file"; inp.accept = ".bpdprofile,application/json";
    inp.onchange = async () => {
      try { const o = unpack(await inp.files[0].text()); const p = await addImported(o); if (p) await loadProfile(p.id); }
      catch (e) { alert("Could not read that file: " + e.message); }
    };
    inp.click();
  }
  async function backupSd() {
    const p = active(); if (!p || !p.settings) return false;
    if (!connected) throw new Error("not connected");
    const bytes = new TextEncoder().encode(pack(p)), CH = 150;
    for (let o = 0; o < bytes.length; o += CH) {
      const part = bytes.slice(o, o + CH), hex = Array.from(part, b => b.toString(16).padStart(2, "0")).join("");
      const r = await cmd({ cmd: "pput", pid: p.id, o, d: hex, last: o + CH >= bytes.length ? 1 : 0 });
      if (!r.ok) throw new Error(r.err || "SD card not available");
    }
    return true;
  }
  async function restoreSd() {
    try {
      const l = await cmd({ cmd: "pls" });
      if (!l.ok) throw new Error(l.err || "not supported by this gauge firmware");
      if (!l.sd) throw new Error("no SD card in the gauge");
      if (!l.profiles.length) { alert("No profiles are saved on the gauge's SD card yet."); return; }
      const id = await choose("Restore from SD card", "Pick a profile to copy to this phone.", [...l.profiles.map(x => ({ label: x, value: x })), { label: "Cancel", ghost: true, value: null }]);
      if (!id) return;
      const parts = []; let o = 0;
      for (;;) {
        const r = await cmd({ cmd: "pget", pid: id, o });
        if (!r.ok) throw new Error(r.err);
        if (!r.n) break;
        for (let i = 0; i < r.d.length; i += 2) parts.push(parseInt(r.d.substr(i, 2), 16));
        o += r.n; if (r.n < 400) break;
      }
      const p = await addImported(unpack(new TextDecoder().decode(new Uint8Array(parts))));
      if (p) await loadProfile(p.id);
    } catch (e) { alert("Could not restore: " + e.message); }
  }

  async function loadProfile(id) {
    const p = profiles[id]; if (!p || !p.settings) return;
    activeId = id; S = clone(p.settings);
    if (connected) {
      try { await pushGauge(p, true); p.pending = false; p.lastConnected = Date.now(); }
      catch (e) { p.pending = true; alert("Could not send it to the gauge: " + e.message); }
    } else p.pending = true;
    persist(); afterChange();
  }

  // ---------- screens ----------
  function renderBar() {
    const el = document.getElementById("profBar"); if (!el) return;
    const p = active();
    const st = lastInfo && connected ? (lastInfo.setup === 2 ? "Configured" : lastInfo.setup === 1 ? "Basic view (not set up)" : lastInfo.setup === 0 ? "Not set up" : "") : "";
    if (!p) { el.innerHTML = `<p>No gauge saved on this phone yet</p><div class="lead msg" style="margin:0">Connect to your gauge, or run the Setup wizard.</div>`; return; }
    el.innerHTML = `<p>${esc(p.name)}${st ? ` <span class="chip">${esc(st)}</span>` : ""}</p>
      <div class="lead msg" style="margin:0">${connected ? "Connected." : "Not connected. Showing the settings saved on this phone."} Last connected ${esc(when(connected ? Date.now() : p.lastConnected))}.
      ${p.pending ? "<br>You have changes that will be sent to the gauge next time you connect." : ""}${!p.done ? "<br>This profile hasn't been through the setup wizard yet." : ""}${p.gearSkipped ? "<br>Gearing and tyres not set: the gauge shows the ECU's gear and speed. Fill them in below when you have them." : ""}</div>`;
  }
  function renderDevice() {
    const el = document.getElementById("profBox"); if (!el) return;
    const ids = Object.keys(profiles).sort((a, b) => (profiles[b].lastConnected || 0) - (profiles[a].lastConnected || 0));
    el.innerHTML = `<div class="card">${ids.length ? ids.map(id => { const p = profiles[id];
        return `<div class="pr"><span class="nm">${esc(p.name)}<br><small class="lead msg" style="margin:0">Last connected ${esc(when(p.lastConnected))}</small></span>${id === activeId ? `<span class="act">ACTIVE</span>` :
          `<button class="btn ghost" type="button" data-load="${id}">Load</button>`}</div>`; }).join("") : `<div class="empty">No profiles yet. Connect to your gauge or run the Setup wizard.</div>`}
      <div class="lead" style="margin:8px 0 0">A profile holds everything for one car: what's fitted and all the gauge settings (not the Bluetooth PIN). Load a profile to put it on the connected gauge, for example after moving the gauge to another car.</div></div>
      <div class="actions"><button class="btn ghost" id="pNew" type="button">Save as new</button><button class="btn ghost" id="pRen" type="button" ${active() ? "" : "disabled"}>Rename</button>
        <button class="btn ghost red" id="pDel" type="button" ${active() ? "" : "disabled"}>Delete</button></div>
      <div class="actions"><button class="btn ghost" id="pExp" type="button" ${active() ? "" : "disabled"}>Save to phone file</button><button class="btn ghost" id="pImp" type="button">Open from file</button></div>
      <div class="actions"><button class="btn ghost" id="pSd" type="button">Back up to gauge SD</button><button class="btn ghost" id="pRes" type="button">Restore from SD</button></div>
      <p class="lead msg" id="pMsg" role="status"></p>`;
    const msg = t => { const m = document.getElementById("pMsg"); if (m) m.textContent = t; };
    el.querySelectorAll("[data-load]").forEach(b => b.onclick = () => loadProfile(b.dataset.load));
    el.querySelector("#pNew").onclick = async () => { if (!S) { msg("Connect to the gauge first, so there is something to save."); return; } const n = await askName("Save as a new profile", "New car"); if (n) { create(n, S, { done: active() ? active().done : false, fitted: clone(fitted()) }); afterChange(); } };
    el.querySelector("#pRen").onclick = async () => { const p = active(); const n = p && await askName("Rename profile", p.name); if (n) { p.name = n; persist(); if (connected) cmd({ cmd: "setupset", pid: p.id, pname: p.name }).catch(() => {}); afterChange(); } };
    el.querySelector("#pDel").onclick = () => { const p = active(); if (p && confirm(`Delete the profile "${p.name}" from this phone? The gauge keeps its settings.`)) { delete profiles[p.id]; activeId = null; persist(); afterChange(); } };
    el.querySelector("#pExp").onclick = exportFile;
    el.querySelector("#pImp").onclick = importFile;
    el.querySelector("#pSd").onclick = () => { msg("Backing up…"); backupSd().then(() => msg("Backed up to the gauge's SD card."), e => msg("Backup failed: " + e.message)); };
    el.querySelector("#pRes").onclick = restoreSd;
  }

  return { onConnect, wizard, customise, snapshot, saveLocal, active, fitted, renderBar, renderDevice, hasProfile: () => !!active(), cached: () => active() && active().settings ? clone(active().settings) : null };
})();
