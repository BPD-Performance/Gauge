# Firmware changes (2026-11-04 21:18, speed pass)

Measured on the gauge before this drop: 9 fps, 50 to 60 ms to draw a frame on BOTH skins (BPD FC 60 ms, BPD Embers 50 ms). The cost was redrawing the whole 480 x 480 screen into PSRAM every time anything changed. NOT compiled for the ESP32 or run on the panel: please flash, then send the overlay numbers.

What changed
- Gauge pages now redraw only what changed: the ring segments that moved (with the glow and embers near them on BPD Embers) and the number or row that changed. Everything else on screen is left alone. LVGL runs in "direct mode" for this (two full-screen buffers kept in step by LVGL). Any other change (page, title, warning popup, skin, avatar page and so on) still redraws the whole screen.
- BPD Embers keeps its picture between draws and fixes up only the part of the ring that moved, instead of copying the whole 115 KB plate each time.
- Drawing files are compiled for speed (-O2) instead of size (-Os).
- Switch to go back: in BPD_Config.h set CFG_PARTIAL_REDRAW to 0 and the old whole-screen redraw is back. Use it if you see any glitch, and tell me what you saw.
- PC checks done (not hardware): over 1200 random changes on both skins, drawing only the changed rectangles gave exactly the same pixels as redrawing the whole screen; the ring update gave identical pictures in 4000 random steps. On average a partial redraw touched about 9% of the screen.

Frame-rate overlay (Setup > Screen > Show frame rate on the gauge)
- FPS = frames shown per second (a still screen reads 0).
- R = average draw time per frame in ms. W = average wait for the panel. MAX = slowest single draw.
- New third line: C = time building the picture behind the text, B = scaling it into the screen, T = text and everything else. They add up to R.
- To test: BPD Embers, a gauge page, bench simulator on. Send R, W, MAX, C, B, T. Then the same with BPD FC.

Not changed: engine data, CAN, warnings, settings, pages, the app.

Known gaps
- The panel still writes the finished frame back to PSRAM on every swap (cache write-back, on by default). It shows up in W. If W stays high I will limit it to the changed parts next.
- Shift-light flashing, warnings, the REC dot and page changes still redraw the whole screen for now.

---

# Firmware changes (2026-11-04, BPD Embers drop)

## Third pass: names and avatar
- The new skin is named BPD Embers; its avatar is Cinder. Each skin owns its avatar: BPD FC = the original dot-matrix model, BPD Embers = Cinder.
- The separate avatar style setting (face_style) is gone from the app and the gauge. Only "Gauge skin" remains. The Turbo bot is removed from the system altogether (firmware code and art, app code and turbobot.js), which also frees its flash and PSRAM.

## Added in the second pass
- Dial only where a dial is needed. In BPD Embers the segment ring, glow and embers are drawn on gauge pages only. Monitor, keypad, alerts, warning popup and the avatar page get the bezel with the ring groove filled in. The popup keeps its blinking red edge band. (+115 KB PSRAM for the dial-less plate, about 0.56 MB total.)
- FPS debug overlay. App > Setup > Screen > "Show frame rate on the gauge", or BLE `{"cmd":"fps","on":1}`. Not saved, off after a power cycle. Shows `NN FPS` and `R.. W.. MAX..` in green at the top, and prints the same once a second on serial.
  FPS = screens drawn per second (only redraws when something changes, so a still screen reads low). R = average draw time in ms, W = average wait for the panel in ms, MAX = slowest draw in the last second. 1000 / R is the most the gauge could sustain. Keep R under about 17 ms to hold the panel's 58 Hz. For the worst case run the bench simulator (`{"cmd":"sim","on":1}`) so the data changes every frame.
  Files: Gauge_UI.cpp (renderFace wrapper), LVGL_Driver.cpp/.h (flush wait), BLE_Link.cpp, Gauge_UI.h.

## Skin menu: BPD FC and BPD Embers (NOT compiled for the ESP32 or run on hardware: please build and watch the serial log)
- New setting "gauge_skin" (0 = BPD FC, the existing amber look, 1 = BPD Embers). Stored in the spare padding byte of UiPrefs, so the saved blob is still 12 bytes and every existing gauge loads as BPD FC. Applies straight away, no restart.
- The avatar is linked to the skin: BPD Embers always shows the Cinder; BPD FC uses the existing face_style (Classic or Turbo bot). face_style no longer has a knight value.
- Every page, warning and data builder is untouched. BPD Embers only changes how the same screen description is drawn, so all placements are exactly where they are in BPD FC.
- New files: Skin_Iron.cpp/.h (picture compositor, no LVGL inside), IronSkin_Art.h (generated art, about 95 KB of flash).
- Art pipeline: art/iron_gen.py turns art/src/iron_dial.jpg and iron_helm.jpg into IronSkin_Art.h. Run `python3 iron_gen.py ..` in the art folder (needs Pillow, numpy, scipy).

How it draws
- The picture behind the text is built on a 240 x 240 grid and shown at 2x: dial plate copy, ring glow, 45 ring segments, embers. On the Face page: dial, back-light, helm, visor glow, cracks, ash.
- Text, plaques and the shift band are then drawn at full resolution by the existing code.
- Redraws only happen when the screen description changes, as before. Nothing is allocated per frame. Memory: about 0.45 MB of PSRAM once at start-up. If that fails the gauge keeps BPD FC and prints a line on the serial monitor.

Text rule (BPD Embers)
- One font and one colour for all text: the 5x7 dot font in bone (ECE0C4), no outlines. Title plaque text, units, row labels and values, dial numbers and the Face readout line all match.
- Warning red is the only other colour (a value in warning, REC, warning rows). The BPD mark stays pink. In the warning popup only the value goes red; name, unit and plaque text stay bone.

Layout rule (Face page)
- Title plaque y 127-163, 16 px clear, avatar zone y 179-365, 16 px clear, readout line, 33 px clear, BPD. Glow and ash are clipped to the avatar zone. The ring stays dark on this page; the shift flash is a band on the very edge of the bezel, as before.

Known gaps
- Launch mood does not exist in the firmware (no trigger), so it is not in the Cinder set. FAST has a slightly angry visor.
- The app's live preview (gauge.js) still draws BPD FC; the setting is only the menu for now.
- Skins are built into the firmware. Loading skins from the SD card is a later step.
- Checked on a PC: Skin_Iron.cpp compiles clean and Gauge_UI.cpp compiles and renders against stand-in LVGL headers (art/host_test). Frame time on the real panel is unmeasured: turn on LVGL_DEBUG_FPS and compare the Boost page in both skins.

# Firmware changes (2026-11-04)
Only Data_Log.cpp / Data_Log.h changed. Log header byte 20 (was reserved) is now a flags byte: bit 0 = recorded while the
bench simulator was active. The `ls` reply gains "sim":0/1. Older logs read as not simulated. Needs a re-flash to take effect;
the app works without it (it just can't tag simulated logs).

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
