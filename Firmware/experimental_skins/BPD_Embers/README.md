# BPD Embers skin (experimental, not built)

This folder holds the BPD Embers skin, removed from the main gauge firmware so the basic build only has BPD FC.
Nothing in this folder is compiled. The Arduino IDE only builds files in the sketch folder (`Firmware/BPD_Gauge/`).

Files:
- `Skin_Iron.cpp`, `Skin_Iron.h`: the Embers picture builder.
- `IronSkin_Art.h`: the Embers art (generated; the generator script and source art are not in this repo).
- `Gauge_UI_with_embers.cpp`: the main `Gauge_UI.cpp` as it was before Embers was removed, with Embers wired in and working.

To bring Embers back:
1. Copy `Skin_Iron.cpp`, `Skin_Iron.h` and `IronSkin_Art.h` back into `Firmware/BPD_Gauge/`.
2. Use `git checkout 5dbf383 -- Firmware/BPD_Gauge` to restore the full working Embers version, or copy `Gauge_UI_with_embers.cpp` over `Gauge_UI.cpp` by hand.
3. Revert the three forced `SKIN_BPDFC` lines in `Settings.cpp`, which force the BPD FC skin.
