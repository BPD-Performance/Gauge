#pragma once
// BPD Embers skin: builds the 240 x 240 art-grid picture behind the gauge text.
// No LVGL in here, so it also compiles on a PC for testing. Colours are plain RGB565
// (red in the top bits); Gauge_UI.cpp scales it 2x into the screen and swaps bytes if LVGL needs it.
#include <stdint.h>

#define IRON_GRID  240
#define IRON_SEGS  45

enum { IRON_RING_GAUGE = 0,      // pages with a dial: segments from the page, glow, hot tip, embers
       IRON_RING_NONE };         // every other page (monitor, keypad, alerts, avatar, popup): no ring at all

bool Iron_Init(void);                                       // call once; false = no memory, keep BPD FC
bool Iron_Ready(void);
void Iron_ComposeGauge(const uint8_t *seg, int ringMode);   // seg[45]: 0 off, 1 lit, 2 warning (may be NULL for NONE)
int  Iron_GaugeDirty(const uint8_t *oldSeg, const uint8_t *newSeg, int *x1, int *y1, int *x2, int *y2);   // 0 no change, 1 = rect (screen px, inclusive), 2 = all
void Iron_ComposeKnight(int mood, int phase, int dx, int dy);   // avatar page; mood as MOOD_* in Gauge_UI.cpp
void Iron_MoodPlate(int mood, uint8_t fill[3], uint8_t edge[3], uint8_t hl[3]);   // title plate colours for a mood
const uint16_t *Iron_Frame(void);                           // the finished 240 x 240 picture
