#pragma once
// Bench test switches for tracking down the display slits.
// Normal use: all 1 except CFG_AUTO_SIM.

#define CFG_ENABLE_BLE           1   // 0 = Bluetooth completely off
#define CFG_ENABLE_CAN_HW        1   // 0 = CAN controller never started (simulator still works)
#define CFG_ENABLE_LIVE_UPDATES  1   // 0 = screen drawn once at boot, never updated
#define CFG_AUTO_SIM             0   // 1 = start the bench simulator at power-up (handy with BLE off)
#define CFG_PARTIAL_REDRAW       1   // 1 = redraw only the parts of a gauge page that changed (faster). 0 = old way: whole screen every time

// Brand colour for the BPD wordmark on the gauge pages (RGB hex). BPD pink 0xED008C = 0xE811 in RGB565.
#define CFG_BRAND_COLOUR         0xED008C
