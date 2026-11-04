"use strict";
// Gauge screen: draws the same amber Commander-style pages as the gauge itself, from the
// live CAN data the gauge streams to the app. This is a direct port of Gauge_UI.cpp
// (same 480 x 480 layout, same 5x7 dot font, same ring, same 5 px dot grid).

const GaugeView = (() => {
  // ---------------- Font (same as DotFont.h; '*' = degree sign) ----------------
  const FONT = {
    "0":[".###.","#...#","#..##","#.#.#","##..#","#...#",".###."], "1":["..#..",".##..","..#..","..#..","..#..","..#..",".###."],
    "2":[".###.","#...#","....#","...#.","..#..",".#...","#####"], "3":["#####","...#.","..#..","...#.","....#","#...#",".###."],
    "4":["...#.","..##.",".#.#.","#..#.","#####","...#.","...#."], "5":["#####","#....","####.","....#","....#","#...#",".###."],
    "6":["..##.",".#...","#....","####.","#...#","#...#",".###."], "7":["#####","....#","...#.","..#..",".#...",".#...",".#..."],
    "8":[".###.","#...#","#...#",".###.","#...#","#...#",".###."], "9":[".###.","#...#","#...#",".####","....#","...#.",".##.."],
    "A":[".###.","#...#","#...#","#####","#...#","#...#","#...#"], "B":["####.","#...#","#...#","####.","#...#","#...#","####."],
    "C":[".###.","#...#","#....","#....","#....","#...#",".###."], "D":["###..","#..#.","#...#","#...#","#...#","#..#.","###.."],
    "E":["#####","#....","#....","####.","#....","#....","#####"], "F":["#####","#....","#....","####.","#....","#....","#...."],
    "G":[".###.","#...#","#....","#.###","#...#","#...#",".####"], "H":["#...#","#...#","#...#","#####","#...#","#...#","#...#"],
    "I":[".###.","..#..","..#..","..#..","..#..","..#..",".###."], "J":["..###","...#.","...#.","...#.","...#.","#..#.",".##.."],
    "K":["#...#","#..#.","#.#..","##...","#.#..","#..#.","#...#"], "L":["#....","#....","#....","#....","#....","#....","#####"],
    "M":["#...#","##.##","#.#.#","#.#.#","#...#","#...#","#...#"], "N":["#...#","#...#","##..#","#.#.#","#..##","#...#","#...#"],
    "O":[".###.","#...#","#...#","#...#","#...#","#...#",".###."], "P":["####.","#...#","#...#","####.","#....","#....","#...."],
    "Q":[".###.","#...#","#...#","#...#","#.#.#","#..#.",".##.#"], "R":["####.","#...#","#...#","####.","#.#..","#..#.","#...#"],
    "S":[".####","#....","#....",".###.","....#","....#","####."], "T":["#####","..#..","..#..","..#..","..#..","..#..","..#.."],
    "U":["#...#","#...#","#...#","#...#","#...#","#...#",".###."], "V":["#...#","#...#","#...#","#...#","#...#",".#.#.","..#.."],
    "W":["#...#","#...#","#...#","#.#.#","#.#.#","#.#.#",".#.#."], "X":["#...#","#...#",".#.#.","..#..",".#.#.","#...#","#...#"],
    "Y":["#...#","#...#",".#.#.","..#..","..#..","..#..","..#.."], "Z":["#####","....#","...#.","..#..",".#...","#....","#####"],
    "-":["....","....","....","####","....","....","...."], "+":[".....","..#..","..#..","#####","..#..","..#..","....."],
    ".":["..","..","..","..","..","##","##"], ":":["..","##","##","..","##","##",".."],
    "%":["##..#","##..#","...#.","..#..",".#...","#..##","#..##"], "/":["....#","....#","...#.","..#..",".#...","#....","#...."],
    "*":[".##.","#..#","#..#",".##.","....","....","...."], "!":["#","#","#","#","#",".","#"], "?":[".###.","#...#","....#","...#.","..#..",".....","..#.."],
    " ":["...","...","...","...","...","...","..."],
  };
  const glyph = ch => FONT[ch.toUpperCase()] || FONT["?"];

  // ---------------- Colours / geometry (as Gauge_UI.cpp) ----------------
  const hex = h => (0xFF000000 | ((h & 0xFF) << 16) | (h & 0xFF00) | ((h >> 16) & 0xFF)) >>> 0;   // canvas is RGBA little endian
  const C_BG = hex(0x060300), C_DIM = hex(0x1F1400), C_LIT = hex(0xFFB000), C_WARN = hex(0xFF3B1F), C_OFF = hex(0x6B4800), C_BRAND = hex(0xED008C), C_BLACK = hex(0x000000);
  const CX = 240, CY = 240, LR = 158, NY = 236, UNIT_Y = NY + 70, ROW1_Y = 352, ROW2_Y = 392, BPD_Y = 446, TH = 36, TITLE_W = 150;
  const NSEG = 45, R_OUT = 216, R_IN = 186, R_OUT_BIG = 222, R_IN_BIG = 182;
  const CELL_LABEL = 2.25, CELL_BIG = 12, CELL_MED = 3, CELL_SMALL = 2.5;
  const rad = d => d * Math.PI / 180;

  const img = new ImageData(480, 480);
  const px = new Uint32Array(img.data.buffer);

  const textW = (s, cell) => { let c = 0; for (const ch of s) c += glyph(ch)[0].length + 1; return Math.max(0, c - 1) * cell; };
  const gRefBot = CY + LR * Math.sin(rad(255)) + 7 * CELL_LABEL / 2;
  const gNumTop = NY - 3.5 * CELL_BIG;
  const gTitleY = gRefBot + (gNumTop - gRefBot - TH) / 2;
  const gLeftLbl = CX + LR * Math.cos(rad(135)) + textW("-10", CELL_LABEL) / 2;
  const gLeftEdge = gLeftLbl + 22, gRightEdge = 480 - gLeftEdge;

  // ---------------- Pixel drawing ----------------
  function fillRect(x1, y1, x2, y2, c) {
    x1 = Math.max(0, Math.round(x1)); y1 = Math.max(0, Math.round(y1));
    x2 = Math.min(479, Math.round(x2)); y2 = Math.min(479, Math.round(y2));
    for (let y = y1; y <= y2; y++) px.fill(c, y * 480 + x1, y * 480 + x2 + 1);
  }
  function outlineRect(x, y, w, h, t, c, dashed) {
    if (!dashed) {
      fillRect(x, y, x + w - 1, y + t - 1, c); fillRect(x, y + h - t, x + w - 1, y + h - 1, c);
      fillRect(x, y, x + t - 1, y + h - 1, c); fillRect(x + w - t, y, x + w - 1, y + h - 1, c); return;
    }
    for (let i = 0; i < w; i += 18) { const e = Math.min(i + 9, w - 1); fillRect(x + i, y, x + e, y + t - 1, c); fillRect(x + i, y + h - t, x + e, y + h - 1, c); }
    for (let i = 0; i < h; i += 18) { const e = Math.min(i + 9, h - 1); fillRect(x, y + i, x + t - 1, y + e, c); fillRect(x + w - t, y + i, x + w - 1, y + e, c); }
  }
  function fillAnnulus(rIn, rOut, c) {
    for (let y = 0; y < 480; y++) {
      const dy = y + 0.5 - CY;
      if (Math.abs(dy) >= rOut) continue;
      const xo = Math.floor(Math.sqrt(rOut * rOut - dy * dy));
      const xi = Math.abs(dy) < rIn ? Math.floor(Math.sqrt(rIn * rIn - dy * dy)) : 0;
      fillRect(CX - xo, y, CX - xi - 1, y, c); fillRect(CX + xi, y, CX + xo - 1, y, c);
    }
  }
  function drawText(s, x, cy, cell, c, align) {
    s = String(s);
    const w = textW(s, cell);
    let x0 = align === 1 ? x - w / 2 : align === 2 ? x - w : x;
    const y0 = cy - 3.5 * cell;
    for (const ch of s) {
      const g = glyph(ch), gw = g[0].length;
      for (let r = 0; r < 7; r++) {
        const yA = Math.round(y0 + r * cell), yB = Math.round(y0 + (r + 1) * cell) - 1;
        let col = 0;
        while (col < gw) {
          if (g[r][col] !== "#") { col++; continue; }
          const st = col;
          while (col < gw && g[r][col] === "#") col++;
          fillRect(Math.round(x0 + st * cell), yA, Math.round(x0 + col * cell) - 1, yB, c);
        }
      }
      x0 += (gw + 1) * cell;
    }
  }

  // Ring lookup table (built once)
  const segTab = new Uint8Array(480 * 480);
  (function buildSegTable() {
    for (let y = 0; y < 480; y++) {
      const dy = y + 0.5 - CY;
      for (let x = 0; x < 480; x++) {
        const dx = x + 0.5 - CX, r = Math.sqrt(dx * dx + dy * dy);
        if (r < R_IN_BIG || r >= R_OUT_BIG) continue;
        let a = Math.atan2(dy, dx) * 180 / Math.PI; if (a < 0) a += 360;
        let rel = a - 135; if (rel < 0) rel += 360;
        if (rel >= 270) continue;
        const i = Math.floor(rel / 6), w = rel - i * 6;
        if (w < 1 || w >= 5) continue;
        const extra = !(r >= R_IN && r < R_OUT);
        segTab[y * 480 + x] = (i + 1) | (extra ? 0x80 : 0);
      }
    }
  })();

  function drawRing(seg) {
    for (let i = 0; i < 480 * 480; i++) {
      const code = segTab[i];
      if (!code) continue;
      const st = seg[(code & 0x3F) - 1];
      if (!st || ((code & 0x80) && st !== 2)) continue;
      px[i] = st === 2 ? C_WARN : C_LIT;
    }
  }
  function drawGrid() {
    for (let y = 0; y < 480; y++) {
      if (y % 5 === 0) { px.fill(C_BG, y * 480, y * 480 + 480); continue; }
      for (let x = 0; x < 480; x += 5) px[y * 480 + x] = C_BG;
    }
  }
  function drawTitle(t, warn) {
    const w = Math.max(TITLE_W, Math.floor(textW(t, CELL_MED) + 24)), ty = Math.round(gTitleY);
    fillRect(CX - Math.floor(w / 2), ty, CX - Math.floor(w / 2) + w - 1, ty + TH - 1, warn ? C_WARN : C_LIT);
    drawText(t, CX, ty + TH / 2, CELL_MED, C_BG, 1);
  }

  // ---------------- Data sources ----------------
  // A face draws from one data source. LIVE is what the gauge (or Demo mode) is sending right now.
  // The log viewer makes its own source with makeSource(true), so replaying a log never touches the
  // live data and nothing live (gauge, simulator, demo) can overwrite it. A replay source is always
  // treated as fresh, so a paused replay keeps showing its values instead of going blank.
  // Each source keeps its own peaks and face-mood timers.
  const newMem = () => ({ boostPk: null, iatPk: null, runSince: 0, idleSince: 0 });
  function makeSource(replay) {
    return { replay: !!replay, frames: {}, status: { kmh: null, gear: null, held: 0, sim: 0, warn: 0, log: 0, t: 0 },
      gps: { fix: 0, sats: 0, lat: 0, lon: 0, kmh: 0, course: 0, lap: 0, cur: 0, last: 0, best: 0, delta: null, sector: 0, progress: 0, gates: 0, t: 0 },
      warns: [], mem: newMem() };
  }
  const liveMem = newMem();
  const LIVE = { replay: false, get frames() { return frames; }, get status() { return gaugeStatus; }, get gps() { return gpsStatus; },
    get warns() { return diagWarns; }, mem: liveMem };
  let cur = LIVE;                                  // source being drawn (render() is synchronous)

  // ---------------- Data ----------------
  const be16 = (d, o) => (d[o] << 8) | d[o + 1];
  function readData() {
    const now = performance.now(), R = cur.replay, gs = cur.status;
    const fr = id => { const f = cur.frames[id]; return f && (R || now - f.t < 1000) ? f.data : null; };
    const a = fr(0x360), b = fr(0x361), c = fr(0x362), l = fr(0x368), t = fr(0x372), e = fr(0x3E0), e1 = fr(0x3E1), tl = fr(0x3E9);
    const baro = t ? be16(t, 6) / 10 : 101.3;
    const d = {
      rpm: a ? be16(a, 0) : null, tps: a ? be16(a, 4) / 10 : null, boost: a ? be16(a, 2) / 10 - baro : null, cltP: a ? be16(a, 6) / 10 - 101.3 : null,
      fuelP: b ? be16(b, 0) / 10 - 101.3 : null, oilP: b ? be16(b, 2) / 10 - 101.3 : null,
      duty: c ? be16(c, 0) / 10 : null, lam: l ? be16(l, 0) / 1000 : null,
      batt: t ? be16(t, 0) / 10 : null, tgtBoost: t ? be16(t, 4) / 10 : null,
      clt: e ? be16(e, 0) / 10 - 273.15 : null, iat: e ? be16(e, 2) / 10 - 273.15 : null, oilT: e ? be16(e, 6) / 10 - 273.15 : null,
      eth: e1 ? be16(e1, 4) / 10 : null, tgtLam: tl ? be16(tl, 4) / 1000 : null,
      kmh: gs.t && (R || now - gs.t < 2000) ? gs.kmh : null,
      gear: gs.t && (R || now - gs.t < 2000) ? gs.gear : null,
    };
    if (d.cltP !== null && d.cltP < -50) d.cltP = null;
    if (d.fuelP !== null && d.fuelP < -50) d.fuelP = null;
    if (d.lam !== null && (d.lam < 0.5 || d.lam > 2)) d.lam = null;
    if (d.tgtLam !== null && (d.tgtLam < 0.5 || d.tgtLam > 2)) d.tgtLam = null;
    return d;
  }

  // Settings (falls back to the gauge defaults until settings are loaded)
  const DEF = { pu: 2, tu: 0, su: 0, lam: 1, flex: 1, stoich: 14.7, tachmax: 8000,
    w_clt_on: 1, w_clt: 105, w_ot_on: 1, w_ot: 130, w_op_on: 1, w_op: 100, w_op_rpm: 2000, w_bst_on: 1, w_bst: 220,
    w_lean_on: 1, w_lean: 0.85, w_lean_bst: 50, w_shift_on: 1, w_shift: 7000, w_cp_on: 1, w_cp: 140, w_fd_on: 1, w_fd: 250,
    w_iat_on: 1, w_iat: 60, w_bat_on: 1, w_bat: 12, w_duty_on: 1, w_duty: 85 };
  const st = k => (S && S[k] !== undefined ? S[k] : DEF[k]);
  const pConv = k => st("pu") == 0 ? k : st("pu") == 1 ? k / 100 : k / 6.894757;
  const pUnit = () => ["KPA", "BAR", "PSI"][st("pu")];
  const fmtP = (k, barDp) => st("pu") == 1 ? (k / 100).toFixed(barDp) : String(Math.round(pConv(k)));
  const tConv = c => Math.round(st("tu") == 1 ? c * 9 / 5 + 32 : c);
  const tUnit = () => st("tu") == 1 ? "*F" : "*C";
  const g = (v, f) => v === null ? "--" : String(f(v));
  const fmtL = v => { let s = (Math.round(v * 100) / 100).toString(); return s; };

  function scale(min, step, dp) {
    return [0, 1, 2, 3, 4].map(k => { const v = min + k * step; return dp < 0 ? fmtL(v) : v.toFixed(dp); });
  }
  function ring(mn, mx, v, mk, zt, zth) {
    const span = mx - mn, seg = new Array(NSEG).fill(0);
    const lit = v === null ? 0 : Math.max(0, Math.min(NSEG, Math.round((v - mn) / span * NSEG)));
    let mi = mk === null || mk === undefined ? -1 : Math.round((mk - mn) / span * NSEG) - 1;
    if (mi >= NSEG) mi = NSEG - 1;
    for (let i = 0; i < NSEG; i++) {
      if (!(i < lit || i === mi)) continue;
      const mid = mn + (i + 0.5) * span / NSEG;
      const z = (zt === 1 && mid >= zth) || (zt === 2 && v !== null && v < zth && mid < zth);
      seg[i] = z ? 2 : 1;
    }
    return seg;
  }

  function buildPage(type, d) {
    const s = { mode: "gauge", title: "", lbl: ["", "", "", "", ""], seg: new Array(NSEG).fill(0), centre: "--", cw: false, unit: "", rows: [["", "", false], ["", "", false]] };
    const on = k => st(k + "_on");
    const run = d.rpm !== null && d.rpm > 400;
    const boostPk = cur.mem.boostPk, iatPk = cur.mem.iatPk, gs = cur.status, gp = cur.gps;
    switch (type) {
      case 0: {          // boost
        const pu = st("pu"), mn = pu == 0 ? -100 : pu == 1 ? -1 : -10, step = pu == 0 ? 100 : pu == 1 ? 1 : 10;
        s.title = "BOOST"; s.lbl = scale(mn, step, -1);
        s.seg = ring(mn, mn + 4.5 * step, d.boost === null ? null : pConv(d.boost), boostPk === null ? null : pConv(boostPk), on("w_bst") ? 1 : 0, pConv(st("w_bst")));
        s.centre = g(d.boost, v => fmtP(v, 2)); s.unit = pUnit();
        s.rows = [["TGT", g(d.tgtBoost, v => fmtP(v, 2)), false], ["PK", g(boostPk, v => fmtP(v, 2)), boostPk !== null && on("w_bst") && boostPk > st("w_bst")]];
        break;
      }
      case 1: {          // oil
        const pu = st("pu"), step = pu == 0 ? 200 : pu == 1 ? 2 : 30;
        s.title = "OIL"; s.lbl = scale(0, step, -1);
        s.seg = ring(0, 4.5 * step, d.oilP === null ? null : pConv(d.oilP), null, on("w_op") ? 2 : 0, pConv(st("w_op")));
        const tw = d.oilT !== null && on("w_ot") && d.oilT > st("w_ot");
        s.centre = g(d.oilT, tConv); s.cw = tw; s.unit = tUnit();
        s.rows = [["PRES", g(d.oilP, v => fmtP(v, 1)), d.oilP !== null && run && on("w_op") && d.oilP < st("w_op")], ["TEMP", s.centre, tw]];
        break;
      }
      case 2: {          // AFR
        const lamMode = st("lam") == 1;
        const sto = st("flex") == 1 && d.eth !== null ? 14.7 - (14.7 - 9) * Math.min(Math.max(d.eth, 0), 100) / 100 : st("stoich");
        s.title = "AFR"; s.lbl = lamMode ? scale(0.6, 0.2, 1) : [0, 1, 2, 3, 4].map(k => ((0.6 + k * 0.2) * sto).toFixed(1));
        const leanZone = on("w_lean") && d.boost !== null && d.boost > st("w_lean_bst");
        s.seg = ring(0.6, 1.5, d.lam, d.tgtLam, leanZone ? 1 : 0, st("w_lean"));
        s.centre = g(d.lam, v => lamMode ? v.toFixed(2) : (v * sto).toFixed(1));
        s.cw = d.lam !== null && leanZone && d.lam > st("w_lean"); s.unit = lamMode ? "LAMBDA" : "AFR";
        s.rows = [["TGT", g(d.tgtLam, v => lamMode ? v.toFixed(2) : (v * sto).toFixed(1)), false], ["ETH", g(d.eth, v => Math.round(v) + "%"), false]];
        break;
      }
      case 3: {          // RPM
        const stepK = Math.max(1, Math.ceil(st("tachmax") / 4500));
        s.title = "RPM"; s.lbl = scale(0, stepK, -1);
        s.seg = ring(0, 4.5 * stepK, d.rpm === null ? null : d.rpm / 1000, null, on("w_shift") ? 1 : 0, st("w_shift") / 1000);
        s.centre = g(d.kmh, v => String(Math.round(st("su") == 1 ? v / 1.609344 : v))); s.unit = st("su") == 1 ? "MPH" : "KM/H";
        const gr = d.gear === null ? "-" : d.gear < 0 ? "R" : d.gear === 0 ? "-" : String(d.gear);
        s.rows = [["RPM", g(d.rpm, v => String(Math.round(v / 10) * 10)), d.rpm !== null && on("w_shift") && d.rpm >= st("w_shift")], ["GEAR", gr, false]];
        break;
      }
      case 4: {          // coolant
        const pu = st("pu"), step = pu == 0 ? 50 : pu == 1 ? 0.5 : 10;
        s.title = "COOLANT"; s.lbl = scale(0, step, -1);
        s.seg = ring(0, 4.5 * step, d.cltP === null ? null : pConv(d.cltP), null, on("w_cp") ? 1 : 0, pConv(st("w_cp")));
        const tw = d.clt !== null && on("w_clt") && d.clt > st("w_clt");
        s.centre = g(d.clt, tConv); s.cw = tw; s.unit = tUnit();
        s.rows = [["PRES", g(d.cltP, v => fmtP(v, 2)), d.cltP !== null && on("w_cp") && d.cltP >= st("w_cp")], ["TEMP", s.centre, tw]];
        break;
      }
      case 5: {          // fuel
        const pu = st("pu"), step = pu == 0 ? 100 : pu == 1 ? 1 : 20;
        const diff = d.fuelP !== null && d.boost !== null ? d.fuelP - d.boost : null;
        s.title = "FUEL"; s.lbl = scale(0, step, -1);
        s.seg = ring(0, 4.5 * step, diff === null ? null : pConv(diff), null, on("w_fd") ? 2 : 0, pConv(st("w_fd")));
        s.centre = g(diff, v => fmtP(v, 2)); s.cw = diff !== null && run && on("w_fd") && diff < st("w_fd"); s.unit = "DIFF " + pUnit();
        s.rows = [["FUEL", g(d.fuelP, v => fmtP(v, 2)), false], ["DUTY", g(d.duty, v => Math.round(v) + "%"), d.duty !== null && on("w_duty") && d.duty >= st("w_duty")]];
        break;
      }
      case 6: {          // IAT
        const f = st("tu") == 1, mn = f ? 20 : 0, step = f ? 40 : 20;
        s.title = "IAT"; s.lbl = scale(mn, step, -1);
        s.seg = ring(mn, mn + 4.5 * step, d.iat === null ? null : tConv(d.iat), iatPk === null ? null : tConv(iatPk), on("w_iat") ? 1 : 0, tConv(st("w_iat")));
        s.centre = g(d.iat, tConv); s.cw = d.iat !== null && on("w_iat") && d.iat >= st("w_iat"); s.unit = tUnit();
        s.rows = [["PK", g(iatPk, tConv), iatPk !== null && on("w_iat") && iatPk >= st("w_iat")], ["BOOST", g(d.boost, v => fmtP(v, 2)), false]];
        break;
      }
      case 7: {          // monitor
        s.mode = "monitor";
        const fd = d.fuelP !== null && d.boost !== null ? d.fuelP - d.boost : null;
        s.mon = [
          ["WATER", g(d.clt, tConv), tUnit(), d.clt !== null && on("w_clt") && d.clt > st("w_clt")],
          ["OIL T", g(d.oilT, tConv), tUnit(), d.oilT !== null && on("w_ot") && d.oilT > st("w_ot")],
          ["OIL P", g(d.oilP, v => fmtP(v, 1)), pUnit(), d.oilP !== null && run && on("w_op") && d.rpm > st("w_op_rpm") && d.oilP < st("w_op")],
          ["FUEL", g(fd, v => fmtP(v, 2)), pUnit(), fd !== null && run && on("w_fd") && fd < st("w_fd")],
          ["BATT", g(d.batt, v => v.toFixed(1)), "V", d.batt !== null && run && on("w_bat") && d.batt < st("w_bat")],
          ["IAT", g(d.iat, tConv), tUnit(), d.iat !== null && on("w_iat") && d.iat >= st("w_iat")]];
        break;
      }
      case 8: {          // keypad
        s.mode = "keypad";
        s.tgt = d.tgtBoost === null ? "TGT --" : `TGT ${fmtP(d.tgtBoost, 2)} ${pUnit()}`;
        s.idle = !!((gs.held >> 2) & 1);
        s.log = !(gs.log & 1) ? 0 : (gs.log & 2) ? 2 : 1;
        break;
      }
      case 10: {         // lap timer (gauge GPS)
        s.title = "LAP";
        const fix = gp.fix && (cur.replay || performance.now() - gp.t < 2000);
        s.seg = ring(0, 255, fix && gp.lap > 0 && gp.progress > 0 ? gp.progress : null, null, 0, 0);
        const lt = ms => !ms ? "--" : `${Math.floor(ms / 60000)}:${String(Math.floor(ms / 1000) % 60).padStart(2, "0")}.${Math.floor(ms / 100) % 10}`;
        s.big = 10; s.rowCell = 2.5;
        s.centre = fix && gp.lap > 0 ? lt(gp.cur) : "--";
        if (!fix) s.unit = "NO GPS";
        else if (!gp.gates) s.unit = "HOLD = S/F";
        else if (!gp.lap) s.unit = "CROSS S/F";
        else if (gp.delta !== null) { const d = gp.delta; s.unit = (d < 0 ? "-" : "+") + (Math.abs(d) / 1000).toFixed(2); s.cw = d > 0; }
        else s.unit = "LAP " + gp.lap;
        s.rows = [["LAST", lt(gp.last), false], ["BEST", lt(gp.best), false]];
        break;
      }
      case 11: {         // face: reacts to the car (same rules as the gauge)
        s.mode = "face";
        const now = performance.now(), run = d.rpm !== null && d.rpm >= 100;
        const red = st("w_shift_on") ? st("w_shift") : st("tachmax") * 0.85;
        const m = cur.mem;
        if (run) { if (!m.runSince) m.runSince = now; } else m.runSince = 0;
        const idling = run && d.rpm < 1500 && (d.kmh === null || d.kmh < 3);
        if (idling) { if (!m.idleSince) m.idleSince = now; } else m.idleSince = 0;
        s.mood = gs.warn ? 6 : !run ? 0 : now - m.runSince < 4000 ? 7 : d.rpm >= red ? 4 : d.boost !== null && d.boost >= 34.5 ? 3
          : d.rpm > 2500 && d.tps !== null && d.tps < 3 && d.boost !== null && d.boost < 0 ? 10 : d.kmh !== null && d.kmh >= 120 ? 9
          : d.gear !== null && d.gear < 0 ? 11 : d.clt !== null && d.clt >= 95 ? 8 : d.clt !== null && d.clt < 60 ? 5
          : d.rpm > 1500 ? 2 : m.idleSince && now - m.idleSince > 60000 ? 12 : 1;
        s.line = `${d.rpm === null ? "--" : d.rpm} RPM  ${g(d.boost, v => fmtP(v, 2))} ${pUnit()}`;
        break;
      }
      case 9: {          // alerts
        s.mode = "alerts";
        s.lines = (cur.warns || []).slice(0, 3).map(w => `${w.name}  ${w.val} ${w.unit}`.replace("°", "*"));
        break;
      }
    }
    return s;
  }

  // Face page art (same as the gauge)
  const FE = {
    open: [".#####.", "#.....#", "#.###.#", "#.###.#", "#.....#", ".#####."], wide: [".#####.", "#.....#", "#..#..#", "#.###.#", "#..#..#", "#.....#", ".#####."],
    blink: [".......", ".......", ".......", "#######", ".......", "......."], happy: [".......", "..###..", ".#...#.", "#.....#", ".......", "......."],
    sleep: [".......", ".......", "#.....#", ".#####.", ".......", "......."], x: ["#.....#", ".#...#.", "..#.#..", "...#...", "..#.#..", ".#...#.", "#.....#"],
    left: ["##.....", "..##...", "....##.", "..##...", "##.....", "......."], right: [".....##", "...##..", ".##....", "...##..", ".....##", "......."],
    cold: [".......", ".#####.", "#.###.#", ".#####.", ".......", "......."],
    lookl: [".#####.", "#.....#", "###...#", "###...#", "#.....#", ".#####."], lookr: [".#####.", "#.....#", "#...###", "#...###", "#.....#", ".#####."],
    lidl: [".......", "#######", "###...#", "###...#", ".#####.", "......."], lidr: [".......", "#######", "#...###", "#...###", ".#####.", "......."],
    tired: [".......", "#######", "#.###.#", ".#####.", ".......", "......."], shade: ["#######", "#######", "#######", ".#####.", ".......", "......."] };
  const FM = {
    flat: ["...#########..."], smile: ["#.............#", ".#...........#.", "..###########.."], grin: ["###############", "#.............#", ".#...........#.", "..###########.."],
    teeth: ["###############", "#.#.#.#.#.#.#.#", "###############"], o: ["..#####..", ".#.....#.", "#.......#", ".#.....#.", "..#####.."],
    snore: ["....###....", "...#...#...", "....###...."], wobble: ["..##..##..##..#", ".#..##..##..##."],
    smallo: [".###.", "#...#", ".###."], smirk: ["............##", "..#########..."], wavy: [".##...##...##..", "#..###..###..##"] };
  const DROP = ["..#..", ".###.", "#####", ".###."];
  const MOODS = [["ZZZ", "sleep", "sleep", "snore"], ["IDLE", "open", "open", "flat"], ["CRUISE", "happy", "happy", "smile"], ["BOOST!", "wide", "wide", "grin"],
    ["REDLINE", "left", "right", "teeth"], ["COLD", "cold", "cold", "wobble"], ["WARNING", "x", "x", "o"],
    ["HELLO!", "happy", "blink", "smile"], ["HOT", "tired", "tired", "wavy"], ["FAST", "shade", "shade", "smirk"],
    ["POP POP", "happy", "happy", "o"], ["REVERSE", "lookr", "lookr", "flat"], ["BORED", "lidr", "lidr", "flat"]];
  let nextBlink = 0, blinkEnd = 0;
  function blitArt(art, cx, cy, cell, c) {
    const h = art.length, w = art[0].length, x0 = cx - w * cell / 2, y0 = cy - h * cell / 2;
    for (let r = 0; r < h; r++) for (let k = 0; k < w; k++) if (art[r][k] === "#") fillRect(x0 + k * cell, y0 + r * cell, x0 + (k + 1) * cell - 1, y0 + (r + 1) * cell - 1, c);
  }
  function faceMotion(s, now) {                    // bob, shake, shiver, blink, drifting Zs
    s.dy = Math.round(Math.sin(now / 600) * 3);
    const shake = s.mood === 6 ? 3 : s.mood === 4 ? 2 : s.mood === 3 ? 1 : 0;
    s.dx = shake ? Math.floor(Math.random() * (shake * 4 + 1)) - shake * 2 : 0;
    if (s.mood === 5) s.dx = Math.round(Math.sin(now / 40) * 3);
    if (s.mood === 1) { if (now > nextBlink) { blinkEnd = now + 140; nextBlink = now + 2500 + Math.random() * 3000; } s.blink = now < blinkEnd ? 1 : 0; }
    if (s.mood === 0 || s.mood === 8) s.z = Math.floor(now / 25) % 40;
    else if (s.mood === 10) s.z = Math.floor(now / 160) % 2;
    else if (s.mood === 11) s.z = Math.floor(now / 500) % 2;
    else if (s.mood === 12) s.z = Math.floor(now / 700) % 32;
  }


  // ---------------- Face page, Turbo bot style (16-bit) ----------------
  // Same as the gauge: a 160 x 160 picture (pre-drawn art from turbobot.js plus title plate,
  // readout, Zs, REC and BPD drawn on the grid with a dark outline) shown at 3x.
  const TB = (() => {
    if (typeof TURBOBOT === "undefined") return null;
    const T = TURBOBOT, G = T.grid;
    const dec = b => Uint8Array.from(atob(b), c => c.charCodeAt(0));
    const frames = {};
    for (const k in T.frames) frames[k] = dec(T.frames[k]);
    const inside = new Uint8Array(G * G);
    for (let y = 0; y < G; y++) for (let x = 0; x < G; x++) inside[y * G + x] = (x - 79.5) ** 2 + (y - 79.5) ** 2 <= 6400 ? 1 : 0;
    return { T, G, pal: T.pal.map(hex), bg: dec(T.bg), frames, inside, comp: new Uint32Array(G * G) };
  })();
  function tbPut(x, y, c) { if (x >= 0 && x < TB.G && y >= 0 && y < TB.G) TB.comp[y * TB.G + x] = c; }
  function tbRle(d, x0, y0, w) {
    let i = 0;
    for (let k = 0; k + 1 < d.length; k += 2) {
      const n = d[k], v = d[k + 1];
      if (!v) { i += n; continue; }
      const c = TB.pal[v];
      for (let j = 0; j < n; j++, i++) tbPut(x0 + i % w, y0 + Math.floor(i / w), c);
    }
  }
  const tbTextW = (s, sc) => { let c = 0; for (const ch of String(s)) c += glyph(ch)[0].length + 1; return Math.max(0, c - 1) * sc; };
  function tbText(s, x, y, c, sc, align) {
    s = String(s);
    const w = tbTextW(s, sc), x0 = align === 1 ? x - Math.floor(w / 2) : align === 2 ? x - w : x, o = hex(0x060A10);
    for (let pass = 0; pass < 2; pass++) {
      let cx = x0;
      for (const ch of s) {
        const g = glyph(ch);
        for (let r = 0; r < 7; r++) for (let k = 0; k < g[r].length; k++) {
          if (g[r][k] !== "#") continue;
          const a0 = cx + k * sc, b0 = y + r * sc;
          if (pass === 0) { for (let a = -1; a <= sc; a++) for (let b = -1; b <= sc; b++) tbPut(a0 + a, b0 + b, o); }
          else { for (let a = 0; a < sc; a++) for (let b = 0; b < sc; b++) tbPut(a0 + a, b0 + b, c); }
        }
        cx += (g[0].length + 1) * sc;
      }
    }
  }
  function tbFrame(s) {
    const alt = (s.z || 0) & 1;
    switch (s.mood) {
      case 0: return "sleep";   case 1: return s.blink ? "idle_blink" : "idle"; case 2: return "cruise";
      case 3: return "boost";   case 4: return "redline"; case 5: return "cold";  case 6: return "warn";
      case 7: return "hello";   case 8: return "hot";     case 9: return "fast";
      case 10: return alt ? "pop_b" : "pop_a";
      case 11: return alt ? "rev_l" : "rev_r";
      case 12: return s.z % 16 === 0 ? "bored_yawn" : alt ? "bored_l" : "bored_r";
      default: return "idle";
    }
  }
  function tbDraw(s) {
    const T = TB.T, G = TB.G, gdx = Math.round((s.dx || 0) / 3), gdy = Math.round((s.dy || 0) / 3);
    tbRle(TB.bg, 0, 0, G);
    tbRle(TB.frames[tbFrame(s)], T.fx + gdx, T.fy + gdy, T.fw);
    if (s.mood === 8) {                                   // sweat drop sliding down
      const y0 = 46 + Math.floor((s.z || 0) / 3) + gdy;
      for (let y = 0; y < T.drop.h; y++) for (let x = 0; x < T.drop.w; x++) { const v = T.drop.px[y * T.drop.w + x]; if (v) tbPut(114 + gdx + x, y0 + y, TB.pal[v]); }
    }
    if (s.mood === 0) {                                   // Zs drifting up
      const zy = Math.floor((s.z || 0) / 4), ice = hex(0xDCF4FF);
      tbText("Z", 122, 38 - zy + gdy, ice, 2, 0); tbText("Z", 134, 26 - zy + gdy, ice, 1, 0);
    }
    const name = MOODS[s.mood][0], pl = T.plates[s.mood].map(hex), ol = hex(0x081216);
    const w = Math.max(56, tbTextW(name, 1) + 14), x0 = 80 - Math.floor(w / 2);
    for (let y = 12; y <= 25; y++) for (let x = x0; x < x0 + w; x++)
      tbPut(x, y, y === 12 || y === 25 || x === x0 || x === x0 + w - 1 ? ol : y < 16 ? pl[0] : y > 22 ? pl[2] : pl[1]);
    tbText(name, 80, 15, hex(0xF0F4F6), 1, 1);
    tbText(s.line, 80, 128, hex(0xFFCE28), 1, 1);
    tbText("BPD", 80, 143, C_BRAND, 1, 1);
    if (s.rec) {
      tbText("REC", 64, 143, C_WARN, 1, 2);
      const dx = 64 - tbTextW("REC", 1) - 6;
      for (let y = 145; y <= 147; y++) for (let x = dx; x <= dx + 2; x++) tbPut(x, y, C_WARN);
    }
    const comp = TB.comp, inside = TB.inside;
    for (let i = 0; i < G * G; i++) if (!inside[i]) comp[i] = C_BLACK;
    for (let y = 0; y < 480; y++) {                       // 3x into the 480 x 480 buffer
      const row = Math.floor(y / 3) * G, o = y * 480;
      for (let x = 0; x < 480; x++) px[o + x] = comp[row + Math.floor(x / 3)];
    }
  }

  // Keypad buttons (same 2 x 2 grid as the gauge)
  const KB = [[94, 210], [246, 210], [94, 312], [246, 312]], KB_W = 140, KB_H = 90;
  let flashBtn = 0, flashUntil = 0;

  // Draw one page into ctx (a 480 x 480 canvas). Several canvases share the pixel buffer.
  // tile remembers what was last drawn on that canvas, so nothing is redrawn until it changes.
  // opts.clear: no dot grid, black background (easier to read small or in sunlight)
  // opts.simple: small face, so drop the dial numbers and bottom rows and keep the big number
  // opts.src: where the data comes from (default: live). A replay source has its peaks set by the log viewer.
  function render(type, ctx, tile, opts) {
    opts = opts || {};
    cur = opts.src || LIVE;
    const d = readData(), mem = cur.mem;
    if (!cur.replay) {
      if (d.boost !== null && (mem.boostPk === null || d.boost > mem.boostPk)) mem.boostPk = d.boost;
      if (d.iat !== null && (mem.iatPk === null || d.iat > mem.iatPk)) mem.iatPk = d.iat;
    }
    const s = buildPage(type, d);
    const now = performance.now();
    s.pressed = now < flashUntil ? flashBtn : 0;
    s.rec = (cur.status.log & 2) && Math.floor(now / 600) % 2 === 0 ? 1 : 0;
    s.shift = st("w_shift_on") && (S && S.shift_show !== undefined ? S.shift_show : 1) && d.rpm !== null && d.rpm >= st("w_shift") && Math.floor(now / 80) % 2 === 0 ? 1 : 0;
    if (s.mode === "face") {
      faceMotion(s, now);
      s.turbo = TB && st("face_style") === 1 ? 1 : 0;       // Face page style saved on the gauge
      if (s.turbo) { s.dx = Math.round(s.dx / 3) * 3; s.dy = Math.round(s.dy / 3) * 3; }   // whole art pixels
    }
    const sig = type + (opts.clear ? "c" : "d") + (opts.simple ? "s" : "f") + JSON.stringify(s);
    if (tile && tile.sig === sig) return;            // nothing changed on this face
    if (tile) tile.sig = sig;
    px.fill(opts.clear ? C_BLACK : C_DIM);
    if (s.mode === "face" && s.turbo) {
      tbDraw(s);
      if (s.shift) fillAnnulus(226, 240, hex(S && S.shift_rgb !== undefined ? S.shift_rgb : 0xFFB000));
      ctx.putImageData(img, 0, 0);
      return;
    }
    if (s.mode === "gauge") {
      drawRing(s.seg);
      if (!opts.simple) s.lbl.forEach((l, k) => { const a = rad(135 + k * 60); drawText(l, CX + LR * Math.cos(a), CY + LR * Math.sin(a), CELL_LABEL, C_LIT, 1); });
      drawTitle(s.title, false);
      drawText(s.centre, CX, NY, s.big || CELL_BIG, s.cw ? C_WARN : C_LIT, 1);
      drawText(s.unit, CX, UNIT_Y, CELL_MED, C_LIT, 1);
      const rc = s.rowCell || CELL_MED;
      if (!opts.simple) s.rows.forEach((r, i) => { const y = i ? ROW2_Y : ROW1_Y; drawText(r[0], gLeftEdge, y, rc, C_LIT, 0); drawText(r[1], gRightEdge, y, rc, r[2] ? C_WARN : C_LIT, 2); });
    } else if (s.mode === "monitor") {
      drawTitle("MONITOR", false);
      s.mon.forEach((r, i) => { const y = gTitleY + TH + 34 + i * 40, c = r[3] ? C_WARN : C_LIT;
        drawText(r[0], 100, y, CELL_MED, C_LIT, 0); drawText(r[1], 322, y, CELL_MED, c, 2); drawText(r[2], 334, y + 3, CELL_LABEL, c, 0); });
    } else if (s.mode === "keypad") {
      drawTitle("KEYPAD", false);
      drawText(s.tgt, CX, gTitleY + TH + 26, CELL_MED, C_LIT, 1);
      const pressed = s.pressed;
      KB.forEach(([x, y], b) => {
        const spare = b === 3 && s.log === 0, on = (b === 2 && s.idle) || (b === 3 && s.log === 2) || pressed === b + 1, col = spare ? C_OFF : C_LIT;
        if (on) fillRect(x, y, x + KB_W - 1, y + KB_H - 1, col); else outlineRect(x, y, KB_W, KB_H, 4, col, spare);
        const tc = on ? C_BG : col, cx = x + KB_W / 2;
        if (b < 2) { drawText("BOOST", cx, y + 28, CELL_SMALL, tc, 1); drawText(b ? "+" : "-", cx, y + 62, 4.5, tc, 1); }
        else if (b === 2) { drawText("ROT IDLE", cx, y + 28, CELL_SMALL, tc, 1); drawText(s.idle ? "ON" : "OFF", cx, y + 62, CELL_MED, tc, 1); }
        else { drawText("LOG", cx, y + 28, CELL_SMALL, tc, 1); drawText(s.log === 0 ? "NO SD" : s.log === 2 ? "REC" : "OFF", cx, y + 62, CELL_MED, tc, 1); }
      });
    } else if (s.mode === "face") {
      let [name, el, er, mo] = MOODS[s.mood];
      const col = s.mood === 6 ? C_WARN : C_LIT, alt = (s.z || 0) & 1;
      if (s.mood === 10) mo = alt ? "smallo" : "o";
      if (s.mood === 11) el = er = alt ? "lookl" : "lookr";
      if (s.mood === 12) { el = er = alt ? "lidl" : "lidr"; mo = s.z % 16 === 0 ? "o" : "flat"; }
      drawTitle(name, s.mood === 6);
      const top = gTitleY + TH, bottom = 392 - 3.5 * CELL_MED, gap = (bottom - top - 84 - 24 - 60) / 2;   // equal space above and below the face
      const eyeY = Math.round(top + gap + 42) + s.dy, mouthY = Math.round(top + gap + 84 + 24 + 30) + s.dy;
      blitArt(FE[s.blink ? "blink" : el], 168 + s.dx, eyeY, 12, col);
      blitArt(FE[s.blink ? "blink" : er], 312 + s.dx, eyeY, 12, col);
      if (s.mood === 9) fillRect(168 + 42 + s.dx, eyeY - 30, 312 - 42 + s.dx, eyeY - 19, col);   // glasses bridge
      blitArt(FM[mo], 240 + s.dx, mouthY, 12, col);
      if (s.mood === 0) { drawText("Z", 362, eyeY - 30 - s.z, 4.5, C_LIT, 1); drawText("Z", 396, eyeY - 60 - s.z, 3, C_LIT, 1); }
      if (s.mood === 8) blitArt(DROP, 392, eyeY - 20 + s.z, 5, C_LIT);
      drawText(s.line, CX, 392, CELL_MED, C_LIT, 1);
    } else if (s.mode === "alerts") {
      drawTitle("ALERTS", false);
      let y = gTitleY + TH + 32;
      if (!s.lines.length) { drawText("NO ACTIVE WARNINGS", CX, y, 2.75, C_LIT, 1); y += 36; }
      s.lines.forEach(l => { drawText(l, CX, y, 2.75, C_WARN, 1); y += 36; });
    }
    drawText("BPD", CX, BPD_Y, CELL_LABEL, C_BRAND, 1);
    if (s.rec) {
      const rx = CX - 48, dx = rx - textW("REC", CELL_LABEL) - 14;
      drawText("REC", rx, BPD_Y, CELL_LABEL, C_WARN, 2);
      fillRect(dx, BPD_Y - 5, dx + 9, BPD_Y + 4, C_WARN);
    }
    if (s.shift) fillAnnulus(226, 240, hex(S && S.shift_rgb !== undefined ? S.shift_rgb : 0xFFB000));
    if (!opts.clear) drawGrid();
    ctx.putImageData(img, 0, 0);
  }

  // Tap on the keypad page sends the same keypad B buttons as the gauge (1 boost +, 2 boost -, 3 rotational idle)
  function tap(type, x, y) {
    if (type !== 8) return;                       // (only live faces take taps; the replay face has no input)
    for (let b = 0; b < 4; b++) {
      const [bx, by] = KB[b];
      if (x < bx || x >= bx + KB_W || y < by || y >= by + KB_H) continue;
      flashBtn = b + 1; flashUntil = performance.now() + 150;
      if (b === 0) cmd({ cmd: "key", btn: 2 }).catch(e => alert(e.message));
      if (b === 1) cmd({ cmd: "key", btn: 1 }).catch(e => alert(e.message));
      if (b === 2) cmd({ cmd: "hold", btn: 3, on: ((gaugeStatus.held >> 2) & 1) ? 0 : 1 }).catch(e => alert(e.message));
      if (b === 3 && (gaugeStatus.log & 1)) cmd({ cmd: "log", on: (gaugeStatus.log & 2) ? 0 : 1 }).catch(e => alert(e.message));
    }
  }

  function resetPeak(type) { if (type === 0) liveMem.boostPk = null; if (type === 6) liveMem.iatPk = null; }

  return { render, tap, resetPeak, makeSource };
})();
