# Changes (2026-11-04)

Log replay
- Replay now has its own data source and its own gauge face inside the Logs viewer (REPLAY banner, page picker).
  It never writes to the live data, so Demo mode, the gauge simulator and live data can't collide with it.
- A paused replay no longer blanks after 1-2 s. Peaks are the running peak at the cursor. Lap page shows last/best and a live delta from the log.
- Removed the "Gauge tab: live / log" switch and the replay guards in app.js (no longer needed).
- Gauge tab shows a banner when its numbers are demo data or from the gauge's simulator. Demo mode stops writing the moment a gauge connects.
- Logs recorded from the gauge simulator are tagged SIMULATED (needs the small firmware change below).

Lap timer lines
- Place a line with two taps: left edge, then right edge, as if driving along the track. Position, width and direction come from the taps.
- Drag handles to adjust: middle moves it, the ends resize/turn it, the arrow tip turns it. Heading and width can also be typed; Flip turns a line round.
- One-tap-on-outline placement is still available (Placement selector). Double-tap zoom is off on that map.
- Track names sent to the gauge have quotes/braces/brackets stripped.

Other
- Removed a duplicate autoConnect() in app.js. Service worker cache bumped to v19.
