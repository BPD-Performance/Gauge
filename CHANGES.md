# App changes (2026-11-04, BPD Embers drop)

## Third pass: names and avatar
- The new skin is named BPD Embers; its avatar is Cinder. Each skin owns its avatar: BPD FC = the original dot-matrix model, BPD Embers = Cinder.
- The separate avatar style setting (face_style) is gone from the app and the gauge. Only "Gauge skin" remains. The Turbo bot is removed from the system altogether (firmware code and art, app code and turbobot.js), which also frees its flash and PSRAM.

## Second pass
- Avatar follows the skin: the Face page style select is now "Avatar style (BPD FC)" and is greyed out with a note when BPD Embers is chosen.
- Setup > Screen: new "Show frame rate on the gauge" checkbox (debug, not saved, switches off at power-off). Needs the matching firmware.
- Setup > Screen: new "Gauge skin" menu (BPD FC / BPD Embers). Greyed out on older firmware.
- The live gauge preview in the app still draws BPD FC for every skin. Service worker cache v24.

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

## Step 1: setup, profiles, one Live page (second drop, 2026-11-04)

App
- Setup wizard (Setup tab, re-runnable any time): name the car, what's fitted (keypad, GPS, oil, fuel, wideband, coolant pressure), units, gearing and tyres, review. Turns on only the pages and warnings the car has.
- Tab bar follows what's fitted. A "+" appears when something is hidden; Customise tabs (Setup) does the same. Before any profile exists every tab shows, so nothing vanishes on upgrade.
- Car profiles (Device tab > Car profile): one per car, keyed by its own id (not the Bluetooth id), so the gauge can move between cars and phones. Load, save as new, rename, delete, save to a phone file (.bpdprofile), open from a file, back up to / restore from the gauge's SD card. The Bluetooth PIN is never in a profile.
- The phone remembers each profile and "last connected". Setup opens offline from the saved copy; offline edits are kept and offered next time you connect.
- On connect it compares the phone's copy with the gauge. Same = silent. Different = "use the gauge's / send this phone's / review / decide later". New gauge = offer the wizard (or Not now = Basic view).
- Live and Diag are one page: warnings (dismiss + history) at the top, then pinned tiles and channels.
- DTC removed everywhere (Diag cards, Alerts face).
- "Open the app on" is now "Start-up tab (this device)" with a plain explanation.
- Tab bar can no longer cut tabs off on narrow screens (flexible widths).
- Service worker cache v20.

Firmware (not compiled or run on hardware here: please build and check)
- New Setup_State.cpp/.h: setup state 0/1/2 and profile id/name in their own flash area. A gauge that already had saved settings counts as configured, so upgrading changes nothing.
- A brand-new gauge starts in the Basic view (Boost, RPM/speed, Coolant, IAT, Alerts; only the coolant-hot warning on). Same screens, nothing extra.
- info now reports setup, pid, pname. New commands: setupset, pls, pput, pget, pdel. Profile files live in /profiles on the SD card (written to .tmp then swapped in, previous copy kept as .bak). The gauge runs from flash and never needs them.
- Each gauge advertises a unique name ("BPD Gauge A1B2", from its Bluetooth address).
- DTC placeholder removed from the Alerts page. Firmware version 1.2-setup.

## Patch 2 (2026-11-04, later)

App
- Setup sections are in A-Z order. Help ("?") added to Setup (wizard, customise, every section, save/reload), Live, Logs and Device.
- Dot font: the letter "I" no longer has the extra gap after it (digits stay fixed width). Cache v21.
- Wizard: the sensor tick-boxes are gone (the gauge learns them from the CAN data). It now asks only about CAN keypad and GPS.
- Wizard: new GPS module step (only if GPS is ticked): explains wiring, shows live status (off / no data / talking, searching / fix) and can switch the module on and restart the gauge.
- Wizard: gearing and tyres has "Skip for now". Skipped = gear from the ECU, speed as the ECU reports it, no placeholder ratios. Setup says gearing is not set.
- The phone quietly takes page/warning changes the gauge made by itself when it detected a sensor, instead of asking "settings differ".

Firmware (not compiled for the ESP32 or run on hardware: please build and watch the serial log)
- Sensor auto-detect (Setup_State.cpp, Setup_Detect): oil pressure, oil temp, fuel pressure, wideband, coolant pressure and the GPS module. First time one shows a real reading for 3 s with the engine running, its page and warning are switched on once. The simulator never counts. Gauges that already had settings start with everything "seen", so nothing changes on upgrade. Setup command {"cmd":"setupset","forget":1} starts learning again.
- GPS: the gauge now reports whether the module is sending valid NMEA (alive) even without a fix. In gpsget and as byte 37 of the 0x04 live packet. A talking module turns the lap page on.
- info reports "seen" (bit mask of what has been detected). Firmware 1.2-setup.
- Syntax-checked against stub headers only.

## Patch 3 (2026-11-04, later): auto-detect removed
- Sensor auto-detect is gone from the firmware and the app (decision: what is displayed is up to the person; a page for a missing sensor just shows zero). Ignore the auto-detect lines in Patch 2.
- Oil, fuel pressure, AFR and coolant pressure pages and warnings are turned on by hand in Setup (Gauge pages, Warnings). The wizard no longer touches them.
- The GPS module "talking" status is kept: it is only used by the wizard's GPS step. It no longer turns any page on.
- info no longer reports "seen"; the setupset "forget" option is removed.

## Patch 4 (2026-11-04, later)
- GPS warning banner: if GPS is ticked as fitted and the gauge is connected, a red strip under the header says why there is no lap data: switched off in the gauge / no data from the module (check wiring) / searching for satellites (n). It disappears on a fix. Shows on every tab. (Ticking GPS in the wizard still turns the Lap timer page on.)
- Zips are now one file with the firmware and the app in clearly named folders (see the zip layout in the chat). Service worker cache v22.

## Patch 5 (2026-11-04, later): lambda only
- Mixture is always shown as lambda. The Mixture, AFR stoich and Fixed stoich settings are gone from Setup and from the wizard's Units step; saving always sends lambda.
- Firmware forces lambda when it loads settings (afrAsLambda = 1), so a gauge that was on AFR switches to lambda after this update. The old stoich fields stay in the saved data but are unused.
- Service worker cache v23.
