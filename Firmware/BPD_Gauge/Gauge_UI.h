#pragma once
#include <Arduino.h>
#include <lvgl.h>

void Gauge_UI_Init(void);
void Gauge_UI_Rebuild(void);     // after settings change (main loop only)
void Gauge_SetFps(bool on);      // debug: show frame rate and draw time on the gauge (not saved)
bool Gauge_GetFps(void);

// Warnings, for the Bluetooth app (safe to call from other tasks)
uint16_t Gauge_WarnMask(void);           // bit per active warning
void     Gauge_WarnJson(String &out);    // [{"id":0,"name":"OIL P","val":"0.6","unit":"BAR","ack":0},...]
void     Gauge_AckWarnings(void);        // same as tapping the warning popup
