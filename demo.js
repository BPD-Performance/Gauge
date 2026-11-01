"use strict";
// Demo mode: when no gauge is connected, the app can make up realistic data itself (the same
// 24-second loop as the gauge's simulator: idle, a full-throttle run through 1st-4th with boost,
// then off the throttle). Everything that reads live data works: Gauge tab, Live, Diag, logs.
const Demo = (() => {
  let timer = null, t0 = 0;
  const be = (d, o, v) => { v = Math.round(v) & 0xFFFF; d[o] = v >> 8; d[o + 1] = v & 255; };
  const RATIOS = [3.25, 1.95, 1.31, 1.0], DIFF = 4.1, TYRE = 1.95;
  let iat = 32;
  function tick() {
    const now = performance.now(), t = ((now - t0) / 1000) % 24;
    let rpm, boost, tps, gear = 1;
    if (t < 6) { rpm = 850 + 20 * Math.sin(t * 5); boost = -60; tps = 0; gear = 0; }
    else if (t < 20) {
      const p = t - 6, g = Math.min(3, Math.floor(p / 3.5)), k = (p - g * 3.5) / 3.5;
      gear = g + 1; rpm = 3200 + k * 3900; tps = 100;
      boost = rpm < 3800 ? -20 + (rpm - 3200) / 10 : Math.min(207, (rpm - 3800) * 0.25 + 40);
    } else { const k = (t - 20) / 4; rpm = 3000 - k * 2100; boost = -70; tps = 0; gear = 4; }
    const kmh = gear ? rpm / (RATIOS[gear - 1] * DIFF) * TYRE * 0.06 : 0;
    iat += ((boost > 0 ? 32 + boost * 0.15 : 32) - iat) * 0.02;
    const put = (id, fill) => { const d = new Uint8Array(8); fill(d); frames[id] = { data: d, t: now }; };
    put(0x360, d => { be(d, 0, rpm); be(d, 2, (101.3 + boost) * 10); be(d, 4, tps * 10); be(d, 6, (101.3 + 95) * 10); });
    put(0x361, d => { be(d, 0, (101.3 + 300 + boost) * 10); be(d, 2, (101.3 + 120 + rpm * 0.06) * 10); });
    put(0x362, d => { be(d, 0, (boost > 0 ? 40 + rpm / 200 : 12) * 10); be(d, 4, 150); });   // injector duty %, ignition angle
    put(0x368, d => be(d, 0, (boost > 30 ? 0.79 : 1.0) * 1000));
    put(0x370, d => be(d, 0, kmh * 10));
    put(0x372, d => { be(d, 0, 139); be(d, 4, 2070); be(d, 6, 1013); });
    put(0x3E0, d => { be(d, 0, (273.15 + 88) * 10); be(d, 2, (273.15 + iat) * 10); be(d, 6, (273.15 + 98) * 10); });
    put(0x3E1, d => be(d, 4, 720));
    put(0x3E9, d => be(d, 4, (boost > 30 ? 0.80 : 1.0) * 1000));
    Object.assign(gaugeStatus, { kmh, gear: gear || 0, t: now, sim: 1 });
  }
  function start() { if (timer) return; t0 = performance.now(); timer = setInterval(tick, 50); tick(); }
  function stop() { if (!timer) return; clearInterval(timer); timer = null; for (const k in frames) delete frames[k]; gaugeStatus.sim = 0; gaugeStatus.t = 0; }
  return { start, stop, get running() { return !!timer; } };
})();
