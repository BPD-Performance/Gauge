// Amber Commander-style gauge pages (dot-matrix look based on the Power FC hand controller).
//
// Every gauge page is drawn by one full-screen object ("face") that writes pixels straight
// into LVGL's frame buffer: dim background, segmented ring, 5x7 dot-matrix text, then a
// 5 px grid over everything so it all looks like individual LCD dots. The update timer
// builds a small description of what should be on screen (FaceState) and only asks LVGL
// to redraw when that description changes.
//
// The WiFi/Bluetooth SETUP page still uses normal LVGL widgets.
// Swipe left/right to change page. Warnings pop up full screen on any gauge page,
// cycle through every active warning, and a tap dismisses them.

#pragma GCC optimize ("O2")
#include <Arduino.h>
#include <limits.h>
#include <math.h>
#include <esp_heap_caps.h>
#include "Gauge_UI.h"
#include "Haltech_CAN.h"
#include "Settings.h"
#include "Gauge_Calc.h"
#include "Data_Log.h"
#include "GPS_Lap.h"
#include "BLE_Link.h"
#include "BPD_Config.h"
#include "DotFont.h"
#include "Skin_Iron.h"

#if !LV_FONT_MONTSERRAT_16 || !LV_FONT_MONTSERRAT_24
#error "Enable LV_FONT_MONTSERRAT_16 and LV_FONT_MONTSERRAT_24 in lvgl/src/lv_conf.h (set them to 1)"
#endif
#if LV_COLOR_DEPTH != 16
#error "The amber skin expects LV_COLOR_DEPTH 16"
#endif

#define UPDATE_MS    33
#define MAX_TILES    (NUM_GAUGE_PAGES + 1)
#define TILE_SETUP   100

// ---------------- Colours ----------------
#define HEX_BG     0x060300      // gaps between the dots
#define HEX_DIM    0x1F1400      // unlit dots
#define HEX_LIT    0xFFB000      // amber
#define HEX_WARN   0xFF3B1F      // warning red
#define HEX_OFF    0x6B4800      // dimmed amber (spare keypad slot)
#ifndef CFG_BRAND_COLOUR
#define CFG_BRAND_COLOUR 0xED008C   // BPD pink
#endif

static lv_color_t C_BG, C_DIM, C_LIT, C_WARN, C_OFF, C_BRAND;

// BPD Embers text rule: one text colour (bone) and one warning colour, nothing else.
#define IRON_BONE  0xECE0C4
#define IRON_WARN  0xFF4628
static bool gIron = false;           // set at the start of every redraw: this redraw uses the BPD Embers look
static void setColours(bool iron)
{
  gIron = iron;
  if (iron) { C_BG = lv_color_hex(0x0A080A); C_LIT = lv_color_hex(IRON_BONE); C_WARN = lv_color_hex(IRON_WARN); C_OFF = lv_color_hex(0x8C8068); }
  else      { C_BG = lv_color_hex(HEX_BG);   C_LIT = lv_color_hex(HEX_LIT);    C_WARN = lv_color_hex(HEX_WARN);  C_OFF = lv_color_hex(HEX_OFF); }
}

// ---------------- Template geometry (480 x 480) ----------------
#define CX        240
#define CY        240
#define LR        158.0f     // radius of the dial numbers
#define NY        236        // centre of the big number
#define UNIT_Y    (NY + 70)
#define ROW1_Y    352
#define ROW2_Y    392
#define BPD_Y     446
#define TH        36         // title box height
#define TITLE_W   150
#define EDGE_GAP  22.0f      // clearance between the -10 dial number and the bottom rows
#define NSEG      45         // ring segments over 270 degrees
#define R_OUT     216        // normal segment: 186..216
#define R_IN      186
#define R_OUT_BIG 222        // warning segment: 182..222
#define R_IN_BIG  182

// Text sizes (pixels per font dot)
#define CELL_LABEL  2.25f
#define CELL_BIG    12.0f
#define CELL_MED    3.0f
#define CELL_SMALL  2.5f

static float gTitleY, gLeftEdge, gRightEdge, gLeftLbl, gRefBot, gNumTop;

// ---------------- Screen description ----------------
enum { M_GAUGE, M_MONITOR, M_KEYPAD, M_ALERTS, M_POPUP, M_FACE };
enum { SEG_OFF = 0, SEG_LIT = 1, SEG_WARN = 2 };

struct FaceState {
  uint8_t mode;
  char    title[12]; uint8_t titleWarn;
  uint8_t seg[NSEG];
  char    lbl[5][8];
  char    centre[12]; uint8_t centreWarn; float centreCell, rowCell;
  char    unit[10];
  char    rowL[2][8], rowV[2][10]; uint8_t rowWarn[2];
  char    mL[6][8], mV[6][8], mU[6][6]; uint8_t mW[6];          // monitor
  char    kTgt[24]; uint8_t idleOn, pressed, logState;          // keypad (logState 0 no card, 1 idle, 2 recording)
  uint8_t aN; char aLine[3][26];  // alerts
  char    pName[10], pVal[12], pUnit[8]; uint8_t pN, pIdx, blink; // popup
  uint8_t shift, rec;
  uint8_t fMood, fBlink; int8_t fDx, fDy; uint8_t fZ; char fLine[24];   // face page
  uint8_t fStyle;                                                        // face page look (FACE_CLASSIC / FACE_KNIGHT)
  uint8_t skin;                                                          // gauge skin (SKIN_BPDFC / SKIN_IRON), so a change redraws
  uint8_t ring;                                                          // 1 = this page has a dial (set by setRing)
  uint8_t dbg;                                                           // debug overlay on: changes every second so the numbers refresh
};

static FaceState fsCur, fsNew;

// ---------------- Objects / navigation ----------------
static lv_obj_t *face, *setupCont;
static int tileType[MAX_TILES];
static int numTiles = 0, activeTile = 0;
static lv_obj_t *bleInfo, *sdInfo;
static int sdLast = -1;
static int bleLast = -1;
static uint32_t lastSwipeMs = 0;
static lv_point_t pressPt = {0, 0};
static int pressedBtn = 0;           // keypad button held down (1-4), 0 = none
static int flashBtn = 0;             // keypad button to show pressed for a moment after a tap
static uint32_t flashUntil = 0;
static bool forceRedraw = true;
static char toast[20] = "";              // short message after marking a lap line
static uint32_t toastUntil = 0;

// Peaks (reset with a 2 s hold on the page)
static float boostPeakKpa = -1000, iatPeakC = -1000;

// Warning popup
static uint16_t warnAck = 0;         // warnings already dismissed (cleared once they go away)
static uint16_t lastMask = 0;
static int      popIdx = 0;
static uint32_t popSwitchMs = 0;

static bool wasSwipe(void) { return lastSwipeMs != 0 && (millis() - lastSwipeMs) < 300; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------- Units ----------------
static float pConv(float kpa) { return cfg.pressUnit == PU_KPA ? kpa : cfg.pressUnit == PU_BAR ? kpa / 100.0f : kpa / 6.894757f; }
static const char *pUnit(void) { return cfg.pressUnit == PU_KPA ? "KPA" : cfg.pressUnit == PU_BAR ? "BAR" : "PSI"; }
static void fmtP(char *b, size_t n, float kpa, int barDp)
{
  if (cfg.pressUnit == PU_BAR) snprintf(b, n, "%.*f", barDp, kpa / 100.0f);
  else snprintf(b, n, "%ld", lroundf(pConv(kpa)));
}
static long tConv(float c) { return lroundf(cfg.tempUnit == TU_F ? c * 9.0f / 5.0f + 32.0f : c); }
static const char *tUnit(void) { return cfg.tempUnit == TU_F ? "*F" : "*C"; }
static long sConv(float k) { return lroundf(cfg.speedUnit == SU_MPH ? k / 1.609344f : k); }
static const char *sUnit(void) { return cfg.speedUnit == SU_MPH ? "MPH" : "KM/H"; }

// =====================================================================================
//  Pixel renderer
// =====================================================================================
struct Canvas { lv_color_t *buf; lv_area_t ba; lv_area_t clip; int stride; };
static Canvas cv;
static uint8_t *segTab = NULL;            // 480x480: ring segment for each pixel (bit 7 = only part of a warning segment)
static int16_t rowOut[480], rowIn[480];   // half-widths of the ring on each row
static int16_t segBox[NSEG][4];           // x1,y1,x2,y2 of every ring segment (including its warning-size part), for partial redraws

static inline lv_color_t *px(int x, int y) { return cv.buf + (y - cv.ba.y1) * cv.stride + (x - cv.ba.x1); }

static void fillRect(int x1, int y1, int x2, int y2, lv_color_t c)
{
  if (x1 < cv.clip.x1) x1 = cv.clip.x1;
  if (y1 < cv.clip.y1) y1 = cv.clip.y1;
  if (x2 > cv.clip.x2) x2 = cv.clip.x2;
  if (y2 > cv.clip.y2) y2 = cv.clip.y2;
  if (x1 > x2 || y1 > y2) return;
  for (int y = y1; y <= y2; y++) lv_color_fill(px(x1, y), c, x2 - x1 + 1);
}

static void outlineRect(int x, int y, int w, int h, int t, lv_color_t c, bool dashed)
{
  if (!dashed) {
    fillRect(x, y, x + w - 1, y + t - 1, c);
    fillRect(x, y + h - t, x + w - 1, y + h - 1, c);
    fillRect(x, y, x + t - 1, y + h - 1, c);
    fillRect(x + w - t, y, x + w - 1, y + h - 1, c);
    return;
  }
  for (int i = 0; i < w; i += 18) {
    int e = min(i + 9, w - 1);
    fillRect(x + i, y, x + e, y + t - 1, c);
    fillRect(x + i, y + h - t, x + e, y + h - 1, c);
  }
  for (int i = 0; i < h; i += 18) {
    int e = min(i + 9, h - 1);
    fillRect(x, y + i, x + t - 1, y + e, c);
    fillRect(x + w - t, y + i, x + w - 1, y + e, c);
  }
}

static void fillAnnulus(float rIn, float rOut, lv_color_t c)
{
  for (int y = cv.clip.y1; y <= cv.clip.y2; y++) {
    float dy = y + 0.5f - CY;
    if (fabsf(dy) >= rOut) continue;
    int xo = (int)sqrtf(rOut * rOut - dy * dy);
    int xi = fabsf(dy) < rIn ? (int)sqrtf(rIn * rIn - dy * dy) : 0;
    fillRect(CX - xo, y, CX - xi - 1, y, c);
    fillRect(CX + xi, y, CX + xo - 1, y, c);
  }
}

// ---- Text ----
static float textW(const char *s, float cell)
{
  int cells = 0;
  for (; *s; s++) cells += (int)strlen(dotFind(*s)->r[0]) + 1;
  if (cells) cells--;
  return cells * cell;
}

enum { AL_LEFT, AL_CENTRE, AL_RIGHT };

static void drawText(const char *s, float x, float cy, float cell, lv_color_t c, int align)
{
  float w = textW(s, cell);
  float x0 = align == AL_CENTRE ? x - w / 2 : align == AL_RIGHT ? x - w : x;
  float y0 = cy - 3.5f * cell;
  for (; *s; s++) {
    const DotGlyph *g = dotFind(*s);
    int gw = (int)strlen(g->r[0]);
    for (int r = 0; r < 7; r++) {
      int yA = lroundf(y0 + r * cell), yB = lroundf(y0 + (r + 1) * cell) - 1;
      int col = 0;
      while (col < gw) {
        if (g->r[r][col] != '#') { col++; continue; }
        int start = col;
        while (col < gw && g->r[r][col] == '#') col++;
        fillRect(lroundf(x0 + start * cell), yA, lroundf(x0 + col * cell) - 1, yB, c);
      }
    }
    x0 += (gw + 1) * cell;
  }
}

// ---- Ring lookup table (built once) ----
static void buildSegTable(void)
{
  if (segTab) return;
  segTab = (uint8_t *)heap_caps_malloc(480 * 480, MALLOC_CAP_SPIRAM);
  if (!segTab) { Serial.println("UI: no PSRAM for the ring table"); return; }
  memset(segTab, 0, 480 * 480);
  for (int i = 0; i < NSEG; i++) { segBox[i][0] = segBox[i][1] = 9999; segBox[i][2] = segBox[i][3] = -1; }
  for (int y = 0; y < 480; y++) {
    float dy = y + 0.5f - CY;
    rowOut[y] = fabsf(dy) < R_OUT_BIG ? (int16_t)ceilf(sqrtf(R_OUT_BIG * R_OUT_BIG - dy * dy)) : -1;
    rowIn[y]  = fabsf(dy) < R_IN_BIG  ? (int16_t)floorf(sqrtf(R_IN_BIG * R_IN_BIG - dy * dy)) - 1 : 0;
    if (rowIn[y] < 0) rowIn[y] = 0;
    if (rowOut[y] < 0) continue;
    for (int x = CX - rowOut[y]; x < CX + rowOut[y]; x++) {
      if (x < 0 || x >= 480) continue;
      float dx = x + 0.5f - CX;
      float r = sqrtf(dx * dx + dy * dy);
      if (r < R_IN_BIG || r >= R_OUT_BIG) continue;
      float a = atan2f(dy, dx) * 180.0f / PI;          // clockwise from 3 o'clock (screen y points down)
      if (a < 0) a += 360.0f;
      float rel = a - 135.0f;
      if (rel < 0) rel += 360.0f;
      if (rel >= 270.0f) continue;
      int i = (int)(rel / 6.0f);
      float w = rel - i * 6.0f;
      if (w < 1.0f || w >= 5.0f) continue;              // 2 degree gap between segments
      bool extra = !(r >= R_IN && r < R_OUT);
      segTab[y * 480 + x] = (uint8_t)((i + 1) | (extra ? 0x80 : 0));
      if (x < segBox[i][0]) segBox[i][0] = x; if (y < segBox[i][1]) segBox[i][1] = y;
      if (x > segBox[i][2]) segBox[i][2] = x; if (y > segBox[i][3]) segBox[i][3] = y;
    }
  }
}

static void drawRing(const uint8_t *seg)
{
  if (!segTab) return;
  for (int y = cv.clip.y1; y <= cv.clip.y2; y++) {
    if (y < 0 || y >= 480 || rowOut[y] < 0) continue;
    const uint8_t *t = segTab + y * 480;
    for (int side = 0; side < 2; side++) {
      int xa = side ? CX + rowIn[y] : CX - rowOut[y];
      int xb = side ? CX + rowOut[y] : CX - rowIn[y];
      if (xa < cv.clip.x1) xa = cv.clip.x1;
      if (xb > cv.clip.x2 + 1) xb = cv.clip.x2 + 1;
      lv_color_t *p = px(xa, y);
      for (int x = xa; x < xb; x++, p++) {
        uint8_t code = t[x];
        if (!code) continue;
        uint8_t st = seg[(code & 0x3F) - 1];
        if (st == SEG_OFF) continue;
        if ((code & 0x80) && st != SEG_WARN) continue;
        *p = st == SEG_WARN ? C_WARN : C_LIT;
      }
    }
  }
}

// Gaps between the dots: every 5th row and column
static void drawGrid(void)
{
  for (int y = cv.clip.y1; y <= cv.clip.y2; y++) {
    if (y % 5 == 0) { lv_color_fill(px(cv.clip.x1, y), C_BG, cv.clip.x2 - cv.clip.x1 + 1); continue; }
    int x = cv.clip.x1 + (5 - cv.clip.x1 % 5) % 5;
    lv_color_t *p = px(x, y);
    for (; x <= cv.clip.x2; x += 5, p += 5) *p = C_BG;
  }
}

// BPD Embers title plaque: dark iron plate with a light top edge and a stud at each end
static void ironPlaque(const char *t, lv_color_t fill, lv_color_t edge, lv_color_t hl)
{
  int w = max(154, (int)textW(t, CELL_MED) + 60);
  int x0 = CX - w / 2, y0 = lroundf(gTitleY);
  fillRect(x0, y0, x0 + w - 1, y0 + TH - 1, edge);
  fillRect(x0 + 3, y0 + 3, x0 + w - 4, y0 + TH - 4, fill);
  fillRect(x0 + 3, y0 + 3, x0 + w - 4, y0 + 4, hl);
  fillRect(x0 + 5, y0 + TH / 2 - 2, x0 + 9, y0 + TH / 2 + 2, hl);
  fillRect(x0 + w - 10, y0 + TH / 2 - 2, x0 + w - 6, y0 + TH / 2 + 2, hl);
  drawText(t, CX, y0 + TH / 2.0f, CELL_MED, C_LIT, AL_CENTRE);
}

static void drawTitle(const char *t, bool warn)
{
  if (gIron) {
    if (warn) ironPlaque(t, lv_color_make(120, 16, 10), lv_color_make(10, 4, 4), lv_color_make(255, 120, 80));
    else      ironPlaque(t, lv_color_make(34, 30, 36),  lv_color_make(10, 8, 10), lv_color_make(120, 100, 80));
    return;
  }
  float tw = textW(t, CELL_MED);
  int w = max(TITLE_W, (int)(tw + 24));
  int ty = lroundf(gTitleY);
  fillRect(CX - w / 2, ty, CX - w / 2 + w - 1, ty + TH - 1, warn ? C_WARN : C_LIT);
  drawText(t, CX, ty + TH / 2.0f, CELL_MED, C_BG, AL_CENTRE);
}

// ---- Page layouts ----
static void renderGauge(const FaceState &s)
{
  if (!gIron) drawRing(s.seg);       // BPD Embers draws its ring in the picture behind
  for (int k = 0; k < 5; k++) {
    float a = (135.0f + k * 60.0f) * PI / 180.0f;
    if (s.lbl[k][0]) drawText(s.lbl[k], CX + LR * cosf(a), CY + LR * sinf(a), CELL_LABEL, C_LIT, AL_CENTRE);
  }
  drawTitle(s.title, s.titleWarn);
  drawText(s.centre, CX, NY, s.centreCell, s.centreWarn ? C_WARN : C_LIT, AL_CENTRE);
  drawText(s.unit, CX, UNIT_Y, CELL_MED, C_LIT, AL_CENTRE);
  for (int i = 0; i < 2; i++) {
    int y = i ? ROW2_Y : ROW1_Y;
    drawText(s.rowL[i], gLeftEdge, y, s.rowCell, C_LIT, AL_LEFT);
    drawText(s.rowV[i], gRightEdge, y, s.rowCell, s.rowWarn[i] ? C_WARN : C_LIT, AL_RIGHT);
  }
}

static void renderMonitor(const FaceState &s)
{
  drawTitle("MONITOR", false);
  for (int i = 0; i < 6; i++) {
    float y = gTitleY + TH + 34 + i * 40;
    drawText(s.mL[i], 100, y, CELL_MED, C_LIT, AL_LEFT);
    drawText(s.mV[i], 322, y, CELL_MED, s.mW[i] ? C_WARN : C_LIT, AL_RIGHT);
    drawText(s.mU[i], 334, y + 3, CELL_LABEL, s.mW[i] ? C_WARN : C_LIT, AL_LEFT);
  }
}

// Keypad buttons: 2 x 2 grid
#define KB_W 140
#define KB_H 90
static const int KB_X[4] = { 94, 246, 94, 246 };
static const int KB_Y[4] = { 210, 210, 312, 312 };

static void renderKeypad(const FaceState &s)
{
  drawTitle("KEYPAD", false);
  drawText(s.kTgt, CX, gTitleY + TH + 26, CELL_MED, C_LIT, AL_CENTRE);
  for (int b = 0; b < 4; b++) {
    int x = KB_X[b], y = KB_Y[b];
    bool spare = (b == 3 && s.logState == 0);         // LOG slot greys out with no SD card
    bool on = (b == 2 && s.idleOn) || (b == 3 && s.logState == 2) || s.pressed == b + 1;
    lv_color_t col = spare ? C_OFF : C_LIT;
    if (on) fillRect(x, y, x + KB_W - 1, y + KB_H - 1, col);
    else outlineRect(x, y, KB_W, KB_H, 4, col, spare);
    lv_color_t tc = on ? C_BG : col;
    float cx = x + KB_W / 2.0f;
    switch (b) {
      case 0: drawText("BOOST", cx, y + 28, CELL_SMALL, tc, AL_CENTRE); drawText("-", cx, y + 62, 4.5f, tc, AL_CENTRE); break;
      case 1: drawText("BOOST", cx, y + 28, CELL_SMALL, tc, AL_CENTRE); drawText("+", cx, y + 62, 4.5f, tc, AL_CENTRE); break;
      case 2: drawText("ROT IDLE", cx, y + 28, CELL_SMALL, tc, AL_CENTRE); drawText(s.idleOn ? "ON" : "OFF", cx, y + 62, CELL_MED, tc, AL_CENTRE); break;
      case 3:
        drawText("LOG", cx, y + 28, CELL_SMALL, tc, AL_CENTRE);
        drawText(s.logState == 0 ? "NO SD" : s.logState == 2 ? "REC" : "OFF", cx, y + 62, CELL_MED, tc, AL_CENTRE);
        break;
    }
  }
}

static void renderAlerts(const FaceState &s)
{
  drawTitle("ALERTS", false);
  float y = gTitleY + TH + 32;
  if (!s.aN) { drawText("NO ACTIVE WARNINGS", CX, y, 2.75f, C_LIT, AL_CENTRE); y += 36; }
  for (int i = 0; i < s.aN; i++) { drawText(s.aLine[i], CX, y, 2.75f, C_WARN, AL_CENTRE); y += 36; }
}

static void renderPopup(const FaceState &s)
{
  if (s.blink) fillAnnulus(213, 231, C_WARN);
  drawTitle("WARNING", true);
  const float cL = 4.0f, cU = 2.6f, cT = CELL_LABEL;
  float cV = 8.5f;
  while (cV > 4.0f && textW(s.pVal, cV) > 260) cV -= 0.25f;
  const float tapY = 400;
  float hL = 7 * cL, hV = 7 * cV, hU = 7 * cU;
  float top = gTitleY + TH, bot = tapY - 3.5f * cT;
  float g = (bot - top - hL - hV - hU) / 4;
  float yL = top + g + hL / 2, yV = yL + hL / 2 + g + hV / 2, yU = yV + hV / 2 + g + hU / 2;
  lv_color_t cN = gIron ? C_LIT : C_WARN;          // BPD Embers: only the value turns red
  drawText(s.pName, CX, yL, cL, cN, AL_CENTRE);
  drawText(s.pVal, CX, yV, cV, C_WARN, AL_CENTRE);
  drawText(s.pUnit, CX, yU, cU, cN, AL_CENTRE);
  drawText("TAP TO DISMISS", CX, tapY, cT, C_LIT, AL_CENTRE);
  if (s.pN > 1) {
    const int dw = 14, gap = 10;
    int tw = s.pN * dw + (s.pN - 1) * gap, y0 = 436;
    for (int i = 0; i < s.pN; i++) {
      int dx = CX - tw / 2 + i * (dw + gap);
      if (i == s.pIdx) fillRect(dx, y0 - 7, dx + dw - 1, y0 + 6, C_WARN);
      else outlineRect(dx, y0 - 7, dw, 14, 3, C_WARN, false);
    }
  }
}

// ---- Face page: a pixel face that reacts to the car ----
enum { MOOD_SLEEP, MOOD_IDLE, MOOD_CRUISE, MOOD_BOOST, MOOD_REDLINE, MOOD_COLD, MOOD_WARN,
       MOOD_HELLO, MOOD_HOT, MOOD_FAST, MOOD_OVERRUN, MOOD_REVERSE, MOOD_BORED, MOOD_COUNT };
static const char *const EYE_OPEN[]  = { ".#####.", "#.....#", "#.###.#", "#.###.#", "#.....#", ".#####.", NULL };
static const char *const EYE_WIDE[]  = { ".#####.", "#.....#", "#..#..#", "#.###.#", "#..#..#", "#.....#", ".#####.", NULL };
static const char *const EYE_BLINK[] = { ".......", ".......", ".......", "#######", ".......", ".......", NULL };
static const char *const EYE_HAPPY[] = { ".......", "..###..", ".#...#.", "#.....#", ".......", ".......", NULL };
static const char *const EYE_SLEEP[] = { ".......", ".......", "#.....#", ".#####.", ".......", ".......", NULL };
static const char *const EYE_X[]     = { "#.....#", ".#...#.", "..#.#..", "...#...", "..#.#..", ".#...#.", "#.....#", NULL };
static const char *const EYE_LEFT[]  = { "##.....", "..##...", "....##.", "..##...", "##.....", ".......", NULL };
static const char *const EYE_RIGHT[] = { ".....##", "...##..", ".##....", "...##..", ".....##", ".......", NULL };
static const char *const EYE_COLD[]  = { ".......", ".#####.", "#.###.#", ".#####.", ".......", ".......", NULL };
static const char *const EYE_LOOKL[] = { ".#####.", "#.....#", "###...#", "###...#", "#.....#", ".#####.", NULL };
static const char *const EYE_LOOKR[] = { ".#####.", "#.....#", "#...###", "#...###", "#.....#", ".#####.", NULL };
static const char *const EYE_LIDL[]  = { ".......", "#######", "###...#", "###...#", ".#####.", ".......", NULL };
static const char *const EYE_LIDR[]  = { ".......", "#######", "#...###", "#...###", ".#####.", ".......", NULL };
static const char *const EYE_TIRED[] = { ".......", "#######", "#.###.#", ".#####.", ".......", ".......", NULL };
static const char *const EYE_SHADE[] = { "#######", "#######", "#######", ".#####.", ".......", ".......", NULL };
static const char *const M_FLAT[]   = { "...#########...", NULL };
static const char *const M_SMILE[]  = { "#.............#", ".#...........#.", "..###########..", NULL };
static const char *const M_GRIN[]   = { "###############", "#.............#", ".#...........#.", "..###########..", NULL };
static const char *const M_TEETH[]  = { "###############", "#.#.#.#.#.#.#.#", "###############", NULL };
static const char *const M_O[]      = { "..#####..", ".#.....#.", "#.......#", ".#.....#.", "..#####..", NULL };
static const char *const M_SMALLO[] = { ".###.", "#...#", ".###.", NULL };
static const char *const M_SNORE[]  = { "....###....", "...#...#...", "....###....", NULL };
static const char *const M_WOBBLE[] = { "..##..##..##..#", ".#..##..##..##.", NULL };
static const char *const M_SMIRK[]  = { "............##", "..#########...", NULL };
static const char *const M_WAVY[]   = { ".##...##...##..", "#..###..###..##", NULL };
static const char *const DROP[]     = { "..#..", ".###.", "#####", ".###.", NULL };
static const char *const MOOD_NAME[] = { "ZZZ", "IDLE", "CRUISE", "BOOST!", "REDLINE", "COLD", "WARNING",
                                         "HELLO!", "HOT", "FAST", "POP POP", "REVERSE", "BORED" };

static void blitArt(const char *const *art, int cx, int cy, int cell, lv_color_t col)
{
  int h = 0; while (art[h]) h++;
  int w = (int)strlen(art[0]);
  int x0 = cx - w * cell / 2, y0 = cy - h * cell / 2;
  for (int r = 0; r < h; r++)
    for (int k = 0; k < w; k++)
      if (art[r][k] == '#') fillRect(x0 + k * cell, y0 + r * cell, x0 + (k + 1) * cell - 1, y0 + (r + 1) * cell - 1, col);
}

// Face sits with equal space above the eyes (under the title) and below the mouth (above the text)
#define FACE_CELL   12
#define FACE_EYE_H  (7 * FACE_CELL)
#define FACE_GAP    24
#define FACE_MOUTH_H (5 * FACE_CELL)
#define FACE_TEXT_Y 392

static void renderAvatar(const FaceState &s)
{
  const char *const *eL, *const *eR, *const *mo;
  bool alt = s.fZ & 1;                                   // fZ doubles as an animation phase
  switch (s.fMood) {
    case MOOD_SLEEP:   eL = eR = EYE_SLEEP; mo = M_SNORE; break;
    case MOOD_CRUISE:  eL = eR = EYE_HAPPY; mo = M_SMILE; break;
    case MOOD_BOOST:   eL = eR = EYE_WIDE;  mo = M_GRIN; break;
    case MOOD_REDLINE: eL = EYE_LEFT; eR = EYE_RIGHT; mo = M_TEETH; break;
    case MOOD_COLD:    eL = eR = EYE_COLD;  mo = M_WOBBLE; break;
    case MOOD_WARN:    eL = eR = EYE_X;     mo = M_O; break;
    case MOOD_HELLO:   eL = EYE_HAPPY; eR = EYE_BLINK; mo = M_SMILE; break;           // wink
    case MOOD_HOT:     eL = eR = EYE_TIRED; mo = M_WAVY; break;
    case MOOD_FAST:    eL = eR = EYE_SHADE; mo = M_SMIRK; break;                       // sunglasses
    case MOOD_OVERRUN: eL = eR = EYE_HAPPY; mo = alt ? M_SMALLO : M_O; break;          // pop pop
    case MOOD_REVERSE: eL = eR = alt ? EYE_LOOKL : EYE_LOOKR; mo = M_FLAT; break;      // checking behind
    case MOOD_BORED:   eL = eR = alt ? EYE_LIDL : EYE_LIDR; mo = (s.fZ % 16 == 0) ? M_O : M_FLAT; break;  // the odd yawn
    default:           eL = eR = EYE_OPEN;  mo = M_FLAT; break;
  }
  if (s.fBlink) eL = eR = EYE_BLINK;
  lv_color_t col = s.fMood == MOOD_WARN ? C_WARN : C_LIT;
  drawTitle(MOOD_NAME[s.fMood], s.fMood == MOOD_WARN);

  float top = gTitleY + TH, bottom = FACE_TEXT_Y - 3.5f * CELL_MED;
  float gap = (bottom - top - FACE_EYE_H - FACE_GAP - FACE_MOUTH_H) / 2;
  int eyeY = lroundf(top + gap + FACE_EYE_H / 2.0f) + s.fDy;
  int mouthY = lroundf(top + gap + FACE_EYE_H + FACE_GAP + FACE_MOUTH_H / 2.0f) + s.fDy;
  blitArt(eL, 168 + s.fDx, eyeY, FACE_CELL, col);
  blitArt(eR, 312 + s.fDx, eyeY, FACE_CELL, col);
  if (s.fMood == MOOD_FAST) fillRect(168 + 42 + s.fDx, eyeY - 30, 312 - 42 + s.fDx, eyeY - 19, col);   // glasses bridge
  blitArt(mo, 240 + s.fDx, mouthY, FACE_CELL, col);
  if (s.fMood == MOOD_SLEEP) {                     // Zs drifting up
    drawText("Z", 362, eyeY - 30 - (s.fZ % 40), 4.5f, C_LIT, AL_CENTRE);
    drawText("Z", 396, eyeY - 60 - (s.fZ % 40), 3.0f, C_LIT, AL_CENTRE);
  }
  if (s.fMood == MOOD_HOT) blitArt(DROP, 392, eyeY - 20 + (s.fZ % 40), 5, C_LIT);   // sweat drop
  drawText(s.fLine, CX, FACE_TEXT_Y, CELL_MED, C_LIT, AL_CENTRE);   // rpm and boost
}

// ---- BPD Embers: the picture behind the text is built on a 240 x 240 grid by Skin_Iron.cpp,
// shown at 2x, then the text and plaques are drawn on top at full resolution ----
#if LV_COLOR_16_SWAP
  #define IRON_NATIVE(c) ((uint16_t)(((c) >> 8) | ((c) << 8)))
#else
  #define IRON_NATIVE(c) ((uint16_t)(c))
#endif

static void blitIron(void)
{
  const uint16_t *fr = Iron_Frame();
  if (!fr) return;
  for (int y = cv.clip.y1; y <= cv.clip.y2; y++) {
    const uint16_t *src = fr + (y >> 1) * IRON_GRID + (cv.clip.x1 >> 1);
    uint16_t *d = (uint16_t *)px(cv.clip.x1, y);
    int x = cv.clip.x1;
    if (x & 1) { *d++ = IRON_NATIVE(*src); src++; x++; }
    if (((uintptr_t)d & 3) == 0) {
      for (; x < cv.clip.x2; x += 2, src++, d += 2) { uint32_t c = IRON_NATIVE(*src); *(uint32_t *)d = c | (c << 16); }
    } else {
      for (; x < cv.clip.x2; x += 2, src++, d += 2) { uint16_t c = IRON_NATIVE(*src); d[0] = c; d[1] = c; }
    }
    if (x == cv.clip.x2) *d = IRON_NATIVE(*src);
  }
}

static uint32_t dbgCompUs = 0, dbgBlitUs = 0;                     // time in the picture build and in the 2x scaling (debug overlay)
extern volatile uint8_t gDbgFps;

// Build (only what changed) and scale the BPD Embers picture behind the text
static void ironPicture(const uint8_t *seg, int ringMode)
{
  uint32_t t0 = gDbgFps ? micros() : 0;
  Iron_ComposeGauge(seg, ringMode);
  uint32_t t1 = gDbgFps ? micros() : 0;
  blitIron();
  if (gDbgFps) { dbgCompUs += t1 - t0; dbgBlitUs += micros() - t1; }
}

static void renderKnight(const FaceState &s)
{
  uint32_t t0 = gDbgFps ? micros() : 0;
  Iron_ComposeKnight(s.fMood, s.fZ, s.fDx / 2, s.fDy / 2);        // movement is in whole art pixels (2 screen px)
  uint32_t t1 = gDbgFps ? micros() : 0;
  blitIron();
  if (gDbgFps) { dbgCompUs += t1 - t0; dbgBlitUs += micros() - t1; }
  uint8_t f[3], e[3], h[3];
  Iron_MoodPlate(s.fMood, f, e, h);
  ironPlaque(MOOD_NAME[s.fMood], lv_color_make(f[0], f[1], f[2]), lv_color_make(e[0], e[1], e[2]), lv_color_make(h[0], h[1], h[2]));
  if (s.fMood == MOOD_SLEEP) {                                     // Zs drifting up, kept inside the avatar zone
    drawText("Z", 340, 215 - (s.fZ % 40) / 2, 3.0f, C_LIT, AL_CENTRE);
    drawText("Z", 362, 195 - (s.fZ % 40) / 2, 2.25f, C_LIT, AL_CENTRE);
  }
  drawText(s.fLine, CX, FACE_TEXT_Y, CELL_MED, C_LIT, AL_CENTRE);   // rpm and boost, same style as every other text
  if (s.fMood == MOOD_WARN) fillAnnulus(226, 240, C_WARN);        // red band on the very edge
}

static void renderFaceInner(const FaceState &s)
{
  bool knight = s.mode == M_FACE && s.fStyle == FACE_KNIGHT && Iron_Ready();
  bool iron = Iron_Ready() && s.skin == SKIN_IRON;
  setColours(iron);
  if (iron) {
    switch (s.mode) {
      case M_GAUGE:   ironPicture(s.seg, s.ring ? IRON_RING_GAUGE : IRON_RING_NONE); renderGauge(s); break;
      case M_POPUP:   ironPicture(NULL, IRON_RING_NONE); renderPopup(s); break;
      case M_FACE:    renderKnight(s); break;
      default:        ironPicture(NULL, IRON_RING_NONE);
                      if (s.mode == M_MONITOR) renderMonitor(s);
                      else if (s.mode == M_KEYPAD) renderKeypad(s);
                      else if (s.mode == M_ALERTS) renderAlerts(s);
                      break;
    }
    if (s.mode != M_POPUP) drawText("BPD", CX, BPD_Y, CELL_LABEL, C_BRAND, AL_CENTRE);
    if (s.rec && s.mode != M_POPUP) {
      float rx = CX - 48;
      drawText("REC", rx, BPD_Y, CELL_LABEL, C_WARN, AL_RIGHT);
      float dx = rx - textW("REC", CELL_LABEL) - 14;
      fillRect(lroundf(dx), BPD_Y - 5, lroundf(dx) + 9, BPD_Y + 4, C_WARN);
    }
    if (s.shift) fillAnnulus(226, 240, lv_color_hex(ui.shiftRgb));
    return;
  }
  fillRect(cv.clip.x1, cv.clip.y1, cv.clip.x2, cv.clip.y2, C_DIM);
  switch (s.mode) {
    case M_GAUGE:   renderGauge(s); break;
    case M_MONITOR: renderMonitor(s); break;
    case M_KEYPAD:  renderKeypad(s); break;
    case M_ALERTS:  renderAlerts(s); break;
    case M_POPUP:   renderPopup(s); break;
    case M_FACE:    renderAvatar(s); break;
  }
  if (s.mode != M_POPUP) {
    drawText("BPD", CX, BPD_Y, CELL_LABEL, C_BRAND, AL_CENTRE);
  }
  if (s.rec && s.mode != M_POPUP) {                         // REC to the left of the logo
    float rx = CX - 48;
    drawText("REC", rx, BPD_Y, CELL_LABEL, C_WARN, AL_RIGHT);
    float dx = rx - textW("REC", CELL_LABEL) - 14;
    fillRect(lroundf(dx), BPD_Y - 5, lroundf(dx) + 9, BPD_Y + 4, C_WARN);
  }
  if (s.shift) fillAnnulus(226, 240, lv_color_hex(ui.shiftRgb));
  drawGrid();
}

// ---- Debug overlay: frame rate and draw cost, switched on from the app (resets at power-off) ----
// FPS = frames put on the panel per second (a frame is only made when something on screen changes).
// R   = average time to draw one frame, in ms. 1000 / R is the most the drawing could manage.
// W   = average time waiting for the panel to take the finished frame, in ms.
// MAX = slowest single draw call in the last second, in ms.
// C   = of R, time building the picture behind the text.   B = scaling it 2x into the screen.   T = text and everything else.
// Also printed once a second on the serial monitor.
volatile uint8_t gDbgFps = 0;
void Gauge_SetFps(bool on) { gDbgFps = on ? 1 : 0; }
bool Gauge_GetFps(void)    { return gDbgFps != 0; }
void Lvgl_FlushStatsAndReset(uint32_t *frames, uint32_t *avgWaitUs);      // LVGL_Driver.cpp

#define DBG_X1 (CX - 112)
#define DBG_Y1 22
#define DBG_X2 (CX + 111)
#define DBG_Y2 88

static uint32_t dbgSumUs = 0, dbgMaxUs = 0, dbgSinceMs = 0;
static uint32_t shFps = 0, shAvgUs = 0, shMaxUs = 0, shWaitUs = 0, shCompUs = 0, shBlitUs = 0;

static void renderFace(const FaceState &s)
{
  uint32_t t0 = micros();
  renderFaceInner(s);
  if (!gDbgFps) { dbgSinceMs = 0; return; }
  uint32_t dt = micros() - t0, now = millis();
  if (!dbgSinceMs) { dbgSinceMs = now; dbgSumUs = dbgMaxUs = dbgCompUs = dbgBlitUs = 0; uint32_t f, w; Lvgl_FlushStatsAndReset(&f, &w); }
  dbgSumUs += dt; if (dt > dbgMaxUs) dbgMaxUs = dt;
  if (now - dbgSinceMs >= 1000) {
    uint32_t frames, wait; Lvgl_FlushStatsAndReset(&frames, &wait);
    uint32_t span = now - dbgSinceMs;
    shFps = frames * 1000 / span;
    shAvgUs = frames ? dbgSumUs / frames : 0; shCompUs = frames ? dbgCompUs / frames : 0; shBlitUs = frames ? dbgBlitUs / frames : 0;
    shMaxUs = dbgMaxUs; shWaitUs = wait;
    Serial.printf("FPS %lu  draw avg %lu us (picture %lu, scale %lu)  max %lu us  panel wait %lu us\n", (unsigned long)shFps, (unsigned long)shAvgUs,
                  (unsigned long)shCompUs, (unsigned long)shBlitUs, (unsigned long)shMaxUs, (unsigned long)shWaitUs);
    dbgSinceMs = now; dbgSumUs = dbgMaxUs = dbgCompUs = dbgBlitUs = 0;
  }
  char a[16], b[28], c[28];
  float tUs = (float)shAvgUs - shCompUs - shBlitUs; if (tUs < 0) tUs = 0;
  snprintf(a, sizeof(a), "%lu FPS", (unsigned long)shFps);
  snprintf(b, sizeof(b), "R%.1f W%.1f MAX%.0f", shAvgUs / 1000.0f, shWaitUs / 1000.0f, shMaxUs / 1000.0f);
  snprintf(c, sizeof(c), "C%.1f B%.1f T%.1f", shCompUs / 1000.0f, shBlitUs / 1000.0f, tUs / 1000.0f);
  fillRect(DBG_X1, DBG_Y1, DBG_X2, DBG_Y2, lv_color_black());
  drawText(a, CX, 34, CELL_LABEL, lv_color_make(80, 255, 120), AL_CENTRE);
  drawText(b, CX, 54, CELL_LABEL, lv_color_make(80, 255, 120), AL_CENTRE);
  drawText(c, CX, 74, CELL_LABEL, lv_color_make(80, 255, 120), AL_CENTRE);
}

// ---- Partial redraws ----
// On a gauge page most changes are the ring and a number or two. Work out which rectangles those touch, so
// LVGL redraws only them (it keeps both frame buffers in step). Anything not listed here redraws the whole screen.
struct DirtyList { lv_area_t a[16]; int n; };

static void addRect(DirtyList &d, int x1, int y1, int x2, int y2)
{
  if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0; if (x2 > 479) x2 = 479; if (y2 > 479) y2 = 479;
  if (x1 > x2 || y1 > y2 || d.n >= 16) return;
  d.a[d.n].x1 = x1; d.a[d.n].y1 = y1; d.a[d.n].x2 = x2; d.a[d.n].y2 = y2; d.n++;
}

// Rectangle of a text line, wide enough for either the old or the new text
static void addText(DirtyList &d, const char *o, const char *n, float x, float cy, float cell, int align)
{
  float w = max(textW(o, cell), textW(n, cell));
  float x0 = align == AL_CENTRE ? x - w / 2 : align == AL_RIGHT ? x - w : x;
  addRect(d, (int)x0 - 3, (int)(cy - 3.5f * cell) - 3, (int)(x0 + w) + 3, (int)(cy + 3.5f * cell) + 3);
}

// false = redraw everything
static bool dirtyAreas(const FaceState &o, const FaceState &n, DirtyList &d)
{
  d.n = 0;
  if (o.mode != M_GAUGE || n.mode != M_GAUGE) return false;
  FaceState a = o, b = n;
  FaceState *both[2] = { &a, &b };
  for (int k = 0; k < 2; k++) {                       // blank what is handled below; everything else must match
    FaceState &f = *both[k];
    memset(f.seg, 0, sizeof(f.seg)); memset(f.centre, 0, sizeof(f.centre)); f.centreWarn = 0; memset(f.unit, 0, sizeof(f.unit));
    memset(f.rowL, 0, sizeof(f.rowL)); memset(f.rowV, 0, sizeof(f.rowV)); memset(f.rowWarn, 0, sizeof(f.rowWarn)); f.dbg = 0;
  }
  if (memcmp(&a, &b, sizeof(FaceState)) != 0) return false;

  if (memcmp(o.seg, n.seg, sizeof(o.seg)) != 0) {
    bool iron = Iron_Ready() && n.skin == SKIN_IRON;
    if (iron) {
      int x1, y1, x2, y2, r = Iron_GaugeDirty(o.seg, n.seg, &x1, &y1, &x2, &y2);
      if (r == 2) return false;
      if (r == 1) addRect(d, x1 - 2, y1 - 2, x2 + 2, y2 + 2);
    } else {
      int ch = 0;
      for (int i = 0; i < NSEG; i++) if (o.seg[i] != n.seg[i]) ch++;
      if (ch > 6) return false;
      for (int i = 0; i < NSEG; i++)
        if (o.seg[i] != n.seg[i] && segBox[i][2] >= 0) addRect(d, segBox[i][0] - 1, segBox[i][1] - 1, segBox[i][2] + 1, segBox[i][3] + 1);
    }
  }
  if (strcmp(o.centre, n.centre) || o.centreWarn != n.centreWarn) addText(d, o.centre, n.centre, CX, NY, n.centreCell, AL_CENTRE);
  if (strcmp(o.unit, n.unit)) addText(d, o.unit, n.unit, CX, UNIT_Y, CELL_MED, AL_CENTRE);
  for (int i = 0; i < 2; i++) {
    float y = i ? ROW2_Y : ROW1_Y;
    if (strcmp(o.rowL[i], n.rowL[i])) addText(d, o.rowL[i], n.rowL[i], gLeftEdge, y, n.rowCell, AL_LEFT);
    if (strcmp(o.rowV[i], n.rowV[i]) || o.rowWarn[i] != n.rowWarn[i]) addText(d, o.rowV[i], n.rowV[i], gRightEdge, y, n.rowCell, AL_RIGHT);
  }
  if (o.dbg != n.dbg) addRect(d, DBG_X1, DBG_Y1, DBG_X2, DBG_Y2);
  return d.n < 16;
}

// =====================================================================================
//  Building the screen description
// =====================================================================================
static void setScale(FaceState &s, float mn, float step, int dp)
{
  for (int k = 0; k < 5; k++) {
    float v = mn + k * step;
    if (dp < 0) snprintf(s.lbl[k], sizeof(s.lbl[k]), "%g", v);
    else        snprintf(s.lbl[k], sizeof(s.lbl[k]), "%.*f", dp, v);
  }
}

enum { Z_NONE, Z_ABOVE, Z_BELOW };

// Ring: lit up to v, optional single marker segment, lit segments inside the zone turn into big red ones
static void setRing(FaceState &s, float mn, float mx, bool valid, float v, bool hasMk, float mk, int zt, float zth)
{
  s.ring = 1;
  float span = mx - mn;
  int lit = valid ? clampi(lroundf((v - mn) / span * NSEG), 0, NSEG) : 0;
  int mi = hasMk ? lroundf((mk - mn) / span * NSEG) - 1 : -1;
  if (mi >= NSEG) mi = NSEG - 1;
  for (int i = 0; i < NSEG; i++) {
    bool on = i < lit || i == mi;
    if (!on) { s.seg[i] = SEG_OFF; continue; }
    float mid = mn + (i + 0.5f) * span / NSEG;
    // Above-type zones (overboost, redline): the bars turn red as they are reached.
    // Below-type zones (low oil or fuel pressure): red only while the reading is actually low.
    bool z = (zt == Z_ABOVE && mid >= zth) || (zt == Z_BELOW && valid && v < zth && mid < zth);
    s.seg[i] = z ? SEG_WARN : SEG_LIT;
  }
}

static void setRow(FaceState &s, int i, const char *l, const char *v, bool warn)
{
  strncpy(s.rowL[i], l, sizeof(s.rowL[i]) - 1);
  strncpy(s.rowV[i], v, sizeof(s.rowV[i]) - 1);
  s.rowWarn[i] = warn;
}

static bool engineRunning(void) { return Calc_Fresh(hd.t360) && hd.rpm > 400; }

// ---- Warnings ----
enum { W_OILP, W_CLT, W_LEAN, W_BOOST, W_OILT, W_CLTP, W_FUEL, W_IAT, W_BATT, W_DUTY, W_COUNT };
struct WarnItem { const char *name; char val[12]; char unit[8]; };
static WarnItem warns[W_COUNT];
static portMUX_TYPE warnMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint16_t warnMaskShared = 0, warnAckShared = 0;
static volatile bool ackRequest = false;

static void wset(int id, const char *name, const char *val, const char *unit)
{
  portENTER_CRITICAL(&warnMux);
  warns[id].name = name;
  strncpy(warns[id].val, val, sizeof(warns[id].val) - 1);  warns[id].val[sizeof(warns[id].val) - 1] = 0;
  strncpy(warns[id].unit, unit, sizeof(warns[id].unit) - 1); warns[id].unit[sizeof(warns[id].unit) - 1] = 0;
  portEXIT_CRITICAL(&warnMux);
}

static uint16_t evalWarnings(void)
{
  uint16_t m = 0;
  char t[16];
  bool bv;
  float boost = Calc_BoostKpa(&bv);
  bool run = engineRunning();

  if (cfg.oilPressLow.on && Calc_Fresh(hd.t361) && run && hd.rpm > cfg.oilPressMinRpm && hd.oil_press_kpa < cfg.oilPressLow.limit) {
    fmtP(t, sizeof(t), hd.oil_press_kpa, 1); wset(W_OILP, "OIL P", t, pUnit()); m |= 1 << W_OILP;
  }
  if (cfg.coolantHigh.on && Calc_Fresh(hd.t3E0) && hd.coolant_c > cfg.coolantHigh.limit) {
    snprintf(t, sizeof(t), "%ld", tConv(hd.coolant_c)); wset(W_CLT, "WATER", t, tUnit()); m |= 1 << W_CLT;
  }
  if (cfg.leanBoost.on && bv && boost > cfg.leanMinBoostKpa && Calc_Fresh(hd.t368) && hd.lambda > cfg.leanBoost.limit && hd.lambda < 2.0f) {
    if (cfg.afrAsLambda) { snprintf(t, sizeof(t), "%.2f", hd.lambda); wset(W_LEAN, "LEAN", t, "LAMBDA"); }
    else { snprintf(t, sizeof(t), "%.1f", hd.lambda * Calc_Stoich()); wset(W_LEAN, "LEAN", t, "AFR"); }
    m |= 1 << W_LEAN;
  }
  if (cfg.boostHigh.on && bv && boost > cfg.boostHigh.limit) {
    fmtP(t, sizeof(t), boost, 2); wset(W_BOOST, "BOOST", t, pUnit()); m |= 1 << W_BOOST;
  }
  if (cfg.oilTempHigh.on && Calc_Fresh(hd.t3E0) && hd.oil_temp_c > cfg.oilTempHigh.limit) {
    snprintf(t, sizeof(t), "%ld", tConv(hd.oil_temp_c)); wset(W_OILT, "OIL T", t, tUnit()); m |= 1 << W_OILT;
  }
  if (cfg.coolPressHigh.on && Calc_Fresh(hd.t360) && hd.coolant_press_kpa > -50 && hd.coolant_press_kpa >= cfg.coolPressHigh.limit) {
    fmtP(t, sizeof(t), hd.coolant_press_kpa, 2); wset(W_CLTP, "COOL P", t, pUnit()); m |= 1 << W_CLTP;
  }
  if (cfg.fuelDiffLow.on && Calc_Fresh(hd.t361) && bv && run && hd.fuel_press_kpa > -50 && hd.fuel_press_kpa - boost < cfg.fuelDiffLow.limit) {
    fmtP(t, sizeof(t), hd.fuel_press_kpa - boost, 2); wset(W_FUEL, "FUEL", t, pUnit()); m |= 1 << W_FUEL;
  }
  if (cfg.iatHigh.on && Calc_Fresh(hd.t3E0) && hd.iat_c >= cfg.iatHigh.limit) {
    snprintf(t, sizeof(t), "%ld", tConv(hd.iat_c)); wset(W_IAT, "IAT", t, tUnit()); m |= 1 << W_IAT;
  }
  if (cfg.battLow.on && Calc_Fresh(hd.t372) && run && hd.batt_v > 1.0f && hd.batt_v < cfg.battLow.limit) {
    snprintf(t, sizeof(t), "%.1f", hd.batt_v); wset(W_BATT, "BATT", t, "V"); m |= 1 << W_BATT;
  }
  if (cfg.dutyHigh.on && Calc_Fresh(hd.t362) && hd.inj_duty_pct >= cfg.dutyHigh.limit) {
    snprintf(t, sizeof(t), "%ld", lroundf(hd.inj_duty_pct)); wset(W_DUTY, "DUTY", t, "%"); m |= 1 << W_DUTY;
  }
  if (ackRequest) { ackRequest = false; warnAck = m; }        // dismissed from the app
  warnMaskShared = m;
  warnAckShared = warnAck & m;
  return m;
}

// ---- Each page ----
static void buildBoost(FaceState &s)
{
  bool bv; float b = Calc_BoostKpa(&bv);
  char v[16];
  strcpy(s.title, "BOOST");
  float mn = cfg.pressUnit == PU_KPA ? -100 : cfg.pressUnit == PU_BAR ? -1 : -10;
  float st = cfg.pressUnit == PU_KPA ? 100 : cfg.pressUnit == PU_BAR ? 1 : 10;
  setScale(s, mn, st, -1);
  bool hasPk = boostPeakKpa > -999;
  setRing(s, mn, mn + 4.5f * st, bv, pConv(b), hasPk, pConv(boostPeakKpa),
          cfg.boostHigh.on ? Z_ABOVE : Z_NONE, pConv(cfg.boostHigh.limit));
  if (bv) fmtP(s.centre, sizeof(s.centre), b, 2); else strcpy(s.centre, "--");
  strcpy(s.unit, pUnit());
  if (Calc_Fresh(hd.t372)) fmtP(v, sizeof(v), hd.target_boost_kpa, 2); else strcpy(v, "--");
  setRow(s, 0, "TGT", v, false);
  if (hasPk) fmtP(v, sizeof(v), boostPeakKpa, 2); else strcpy(v, "--");
  setRow(s, 1, "PK", v, cfg.boostHigh.on && hasPk && boostPeakKpa > cfg.boostHigh.limit);
}

static void buildOil(FaceState &s)
{
  char v[16];
  strcpy(s.title, "OIL");
  float st = cfg.pressUnit == PU_KPA ? 200 : cfg.pressUnit == PU_BAR ? 2 : 30;
  setScale(s, 0, st, -1);
  bool pv = Calc_Fresh(hd.t361);
  setRing(s, 0, 4.5f * st, pv, pConv(hd.oil_press_kpa), false, 0,
          cfg.oilPressLow.on ? Z_BELOW : Z_NONE, pConv(cfg.oilPressLow.limit));
  bool tv = Calc_Fresh(hd.t3E0);
  bool tw = tv && cfg.oilTempHigh.on && hd.oil_temp_c > cfg.oilTempHigh.limit;
  if (tv) snprintf(s.centre, sizeof(s.centre), "%ld", tConv(hd.oil_temp_c)); else strcpy(s.centre, "--");
  s.centreWarn = tw;
  strcpy(s.unit, tUnit());
  if (pv) fmtP(v, sizeof(v), hd.oil_press_kpa, 1); else strcpy(v, "--");
  setRow(s, 0, "PRES", v, pv && engineRunning() && cfg.oilPressLow.on && hd.oil_press_kpa < cfg.oilPressLow.limit);
  setRow(s, 1, "TEMP", s.centre, tw);
}

static void buildAfr(FaceState &s)
{
  char v[16];
  float st = Calc_Stoich();
  bool lam = cfg.afrAsLambda;
  strcpy(s.title, "AFR");
  if (lam) setScale(s, 0.6f, 0.2f, 1);
  else for (int k = 0; k < 5; k++) snprintf(s.lbl[k], sizeof(s.lbl[k]), "%.1f", (0.6f + k * 0.2f) * st);
  float l = hd.lambda, tl = hd.target_lambda;
  bool av = Calc_Fresh(hd.t368) && l > 0.5f && l < 2.0f;
  bool tv = Calc_Fresh(hd.t3E9) && tl > 0.5f && tl < 2.0f;
  bool bv; float b = Calc_BoostKpa(&bv);
  bool boosting = bv && b > cfg.leanMinBoostKpa;
  bool leanZone = cfg.leanBoost.on && boosting;
  setRing(s, 0.6f, 1.5f, av, l, tv, tl, leanZone ? Z_ABOVE : Z_NONE, cfg.leanBoost.limit);
  if (!av) strcpy(s.centre, "--");
  else if (lam) snprintf(s.centre, sizeof(s.centre), "%.2f", l);
  else snprintf(s.centre, sizeof(s.centre), "%.1f", l * st);
  s.centreWarn = av && leanZone && l > cfg.leanBoost.limit;
  strcpy(s.unit, lam ? "LAMBDA" : "AFR");
  if (!tv) strcpy(v, "--");
  else if (lam) snprintf(v, sizeof(v), "%.2f", tl);
  else snprintf(v, sizeof(v), "%.1f", tl * st);
  setRow(s, 0, "TGT", v, false);
  if (Calc_Fresh(hd.t3E1)) snprintf(v, sizeof(v), "%ld%%", lroundf(hd.ethanol_pct)); else strcpy(v, "--");
  setRow(s, 1, "ETH", v, false);
}

static void buildTach(FaceState &s)
{
  char v[16];
  strcpy(s.title, "RPM");
  int stepK = (int)ceilf(cfg.tachMax / 4.5f / 1000.0f);          // label step in 1000 rpm
  if (stepK < 1) stepK = 1;
  setScale(s, 0, (float)stepK, -1);
  bool rv = Calc_Fresh(hd.t360);
  float shiftK = cfg.shiftLight.limit / 1000.0f;
  setRing(s, 0, 4.5f * stepK, rv, hd.rpm / 1000.0f, false, 0, cfg.shiftLight.on ? Z_ABOVE : Z_NONE, shiftK);
  bool sv; float spd = Calc_SpeedKmh(&sv);
  if (sv) snprintf(s.centre, sizeof(s.centre), "%ld", sConv(spd)); else strcpy(s.centre, "--");
  strcpy(s.unit, sUnit());
  if (rv) snprintf(v, sizeof(v), "%ld", (long)(lroundf(hd.rpm / 10.0f) * 10)); else strcpy(v, "--");
  setRow(s, 0, "RPM", v, rv && cfg.shiftLight.on && hd.rpm >= cfg.shiftLight.limit);
  bool gv; int g = Calc_Gear(&gv);
  if (cfg.gearSource == GEAR_OFF || !gv) strcpy(v, "-");
  else if (g < 0) strcpy(v, "R");
  else if (g == 0) strcpy(v, cfg.gearSource == GEAR_ECU ? "N" : "-");
  else snprintf(v, sizeof(v), "%d", g);
  setRow(s, 1, "GEAR", v, false);
}

static void buildCoolant(FaceState &s)
{
  char v[16];
  strcpy(s.title, "COOLANT");
  float st = cfg.pressUnit == PU_KPA ? 50 : cfg.pressUnit == PU_BAR ? 0.5f : 10;
  setScale(s, 0, st, -1);
  bool pv = Calc_Fresh(hd.t360) && hd.coolant_press_kpa > -50;
  setRing(s, 0, 4.5f * st, pv, pConv(hd.coolant_press_kpa), false, 0,
          cfg.coolPressHigh.on ? Z_ABOVE : Z_NONE, pConv(cfg.coolPressHigh.limit));
  bool tv = Calc_Fresh(hd.t3E0);
  bool tw = tv && cfg.coolantHigh.on && hd.coolant_c > cfg.coolantHigh.limit;
  if (tv) snprintf(s.centre, sizeof(s.centre), "%ld", tConv(hd.coolant_c)); else strcpy(s.centre, "--");
  s.centreWarn = tw;
  strcpy(s.unit, tUnit());
  if (pv) fmtP(v, sizeof(v), hd.coolant_press_kpa, 2); else strcpy(v, "--");
  setRow(s, 0, "PRES", v, pv && cfg.coolPressHigh.on && hd.coolant_press_kpa >= cfg.coolPressHigh.limit);
  setRow(s, 1, "TEMP", s.centre, tw);
}

static void buildFuel(FaceState &s)
{
  char v[16];
  strcpy(s.title, "FUEL");
  float st = cfg.pressUnit == PU_KPA ? 100 : cfg.pressUnit == PU_BAR ? 1 : 20;
  setScale(s, 0, st, -1);
  bool bv; float b = Calc_BoostKpa(&bv);
  bool fv = Calc_Fresh(hd.t361) && hd.fuel_press_kpa > -50;
  bool dv = fv && bv;
  float diff = hd.fuel_press_kpa - b;
  bool low = dv && engineRunning() && cfg.fuelDiffLow.on && diff < cfg.fuelDiffLow.limit;
  setRing(s, 0, 4.5f * st, dv, pConv(diff), false, 0, cfg.fuelDiffLow.on ? Z_BELOW : Z_NONE, pConv(cfg.fuelDiffLow.limit));
  if (dv) fmtP(s.centre, sizeof(s.centre), diff, 2); else strcpy(s.centre, "--");
  s.centreWarn = low;
  snprintf(s.unit, sizeof(s.unit), "DIFF %s", pUnit());
  if (fv) fmtP(v, sizeof(v), hd.fuel_press_kpa, 2); else strcpy(v, "--");
  setRow(s, 0, "FUEL", v, false);
  bool uv = Calc_Fresh(hd.t362);
  if (uv) snprintf(v, sizeof(v), "%ld%%", lroundf(hd.inj_duty_pct)); else strcpy(v, "--");
  setRow(s, 1, "DUTY", v, uv && cfg.dutyHigh.on && hd.inj_duty_pct >= cfg.dutyHigh.limit);
}

static void buildIat(FaceState &s)
{
  char v[16];
  strcpy(s.title, "IAT");
  bool f = cfg.tempUnit == TU_F;
  float mn = f ? 20 : 0, st = f ? 40 : 20;
  setScale(s, mn, st, -1);
  bool tv = Calc_Fresh(hd.t3E0);
  bool hasPk = iatPeakC > -999;
  float lim = (float)tConv(cfg.iatHigh.limit);
  setRing(s, mn, mn + 4.5f * st, tv, (float)tConv(hd.iat_c), hasPk, (float)tConv(iatPeakC),
          cfg.iatHigh.on ? Z_ABOVE : Z_NONE, lim);
  if (tv) snprintf(s.centre, sizeof(s.centre), "%ld", tConv(hd.iat_c)); else strcpy(s.centre, "--");
  s.centreWarn = tv && cfg.iatHigh.on && hd.iat_c >= cfg.iatHigh.limit;
  strcpy(s.unit, tUnit());
  if (hasPk) snprintf(v, sizeof(v), "%ld", tConv(iatPeakC)); else strcpy(v, "--");
  setRow(s, 0, "PK", v, hasPk && cfg.iatHigh.on && iatPeakC >= cfg.iatHigh.limit);
  bool bv; float b = Calc_BoostKpa(&bv);
  if (bv) fmtP(v, sizeof(v), b, 2); else strcpy(v, "--");
  setRow(s, 1, "BOOST", v, false);
}

static void monRow(FaceState &s, int i, const char *l, bool valid, const char *v, const char *u, bool warn)
{
  strncpy(s.mL[i], l, sizeof(s.mL[i]) - 1);
  strncpy(s.mV[i], valid ? v : "--", sizeof(s.mV[i]) - 1);
  strncpy(s.mU[i], u, sizeof(s.mU[i]) - 1);
  s.mW[i] = valid && warn;
}

static void buildMonitor(FaceState &s, uint16_t m)
{
  char v[16];
  bool bv; float b = Calc_BoostKpa(&bv);
  bool t3E0 = Calc_Fresh(hd.t3E0), t361 = Calc_Fresh(hd.t361);
  snprintf(v, sizeof(v), "%ld", tConv(hd.coolant_c));     monRow(s, 0, "WATER", t3E0, v, tUnit(), m & (1 << W_CLT));
  snprintf(v, sizeof(v), "%ld", tConv(hd.oil_temp_c));    monRow(s, 1, "OIL T", t3E0, v, tUnit(), m & (1 << W_OILT));
  fmtP(v, sizeof(v), hd.oil_press_kpa, 1);                 monRow(s, 2, "OIL P", t361, v, pUnit(), m & (1 << W_OILP));
  fmtP(v, sizeof(v), hd.fuel_press_kpa - b, 2);            monRow(s, 3, "FUEL", t361 && bv && hd.fuel_press_kpa > -50, v, pUnit(), m & (1 << W_FUEL));
  snprintf(v, sizeof(v), "%.1f", hd.batt_v);               monRow(s, 4, "BATT", Calc_Fresh(hd.t372), v, "V", m & (1 << W_BATT));
  snprintf(v, sizeof(v), "%ld", tConv(hd.iat_c));          monRow(s, 5, "IAT", t3E0, v, tUnit(), m & (1 << W_IAT));
}

static void fmtLap(char *b, size_t n, uint32_t ms)
{
  if (!ms) { snprintf(b, n, "--"); return; }
  uint32_t m = ms / 60000, sec = (ms / 1000) % 60, t = (ms / 100) % 10;
  snprintf(b, n, "%lu:%02lu.%lu", (unsigned long)m, (unsigned long)sec, (unsigned long)t);
}

static void buildLap(FaceState &s, uint32_t now)
{
  char v[16];
  strcpy(s.title, "LAP");
  for (int k = 0; k < 5; k++) s.lbl[k][0] = 0;          // ring = how far round the lap you are
  LapState l; Lap_Get(l);
  bool fix = Gps_Fresh();
  setRing(s, 0, 255, fix && l.lap > 0 && l.progress > 0, l.progress, false, 0, Z_NONE, 0);
  s.centreCell = 10.0f;
  s.rowCell = 2.5f;                                       // lap times are wider than normal values
  if (fix && l.lap > 0) fmtLap(s.centre, sizeof(s.centre), l.curMs); else strcpy(s.centre, "--");
  if (now < toastUntil) strncpy(s.unit, toast, sizeof(s.unit) - 1);
  else if (!Gps_Enabled()) strcpy(s.unit, "GPS OFF");
  else if (!fix) strcpy(s.unit, "NO GPS");
  else if (!l.haveTrack) strcpy(s.unit, "HOLD = S/F");
  else if (l.lap == 0) strcpy(s.unit, "CROSS S/F");
  else if (l.deltaMs != INT32_MIN) {
    snprintf(s.unit, sizeof(s.unit), "%c%ld.%02ld", l.deltaMs < 0 ? '-' : '+', labs(l.deltaMs) / 1000, (labs(l.deltaMs) % 1000) / 10);
    s.centreWarn = l.deltaMs > 0;                         // slower than best shows red
  }
  else snprintf(s.unit, sizeof(s.unit), "LAP %u", l.lap);
  fmtLap(v, sizeof(v), l.lastMs); setRow(s, 0, "LAST", v, false);
  fmtLap(v, sizeof(v), l.bestMs); setRow(s, 1, "BEST", v, false);
}

static uint32_t nextBlinkMs = 0, blinkEndMs = 0, runSinceMs = 0, idleSinceMs = 0;

static void buildFacePage(FaceState &s, uint16_t m, uint32_t now)
{
  bool run = Calc_Fresh(hd.t360) && hd.rpm >= 100;
  bool bv, sv, gv; float b = Calc_BoostKpa(&bv), spd = Calc_SpeedKmh(&sv); int gear = Calc_Gear(&gv);
  uint16_t red = cfg.shiftLight.on ? (uint16_t)cfg.shiftLight.limit : (uint16_t)(cfg.tachMax * 0.85f);
  bool tFresh = Calc_Fresh(hd.t3E0);
  if (run) { if (!runSinceMs) runSinceMs = now; } else runSinceMs = 0;
  bool idling = run && hd.rpm < 1500 && (!sv || spd < 3);
  if (idling) { if (!idleSinceMs) idleSinceMs = now; } else idleSinceMs = 0;

  if (m)                                                          s.fMood = MOOD_WARN;
  else if (!run)                                                  s.fMood = MOOD_SLEEP;
  else if (now - runSinceMs < 4000)                               s.fMood = MOOD_HELLO;     // just started
  else if (hd.rpm >= red)                                         s.fMood = MOOD_REDLINE;
  else if (bv && b >= 34.5f)                                      s.fMood = MOOD_BOOST;     // 5 psi
  else if (hd.rpm > 2500 && hd.throttle_pct < 3 && bv && b < 0)   s.fMood = MOOD_OVERRUN;   // off the throttle
  else if (sv && spd >= 120)                                      s.fMood = MOOD_FAST;
  else if (gv && gear < 0)                                        s.fMood = MOOD_REVERSE;
  else if (tFresh && hd.coolant_c >= 95)                          s.fMood = MOOD_HOT;
  else if (tFresh && hd.coolant_c < 60)                           s.fMood = MOOD_COLD;
  else if (hd.rpm > 1500)                                         s.fMood = MOOD_CRUISE;
  else if (idleSinceMs && now - idleSinceMs > 60000)              s.fMood = MOOD_BORED;     // a minute sitting still
  else                                                            s.fMood = MOOD_IDLE;

  // little movements: a slow bob, shaking on boost / redline / warning, shivering when cold
  s.fDy = (int8_t)lroundf(sinf(now / 600.0f) * 3);
  int shake = s.fMood == MOOD_WARN ? 3 : s.fMood == MOOD_REDLINE ? 2 : s.fMood == MOOD_BOOST ? 1 : 0;
  if (shake) s.fDx = (int8_t)((int)(esp_random() % (shake * 4 + 1)) - shake * 2);
  if (s.fMood == MOOD_COLD) s.fDx = (int8_t)lroundf(sinf(now / 40.0f) * 3);
  if (s.fMood == MOOD_IDLE || s.fMood == MOOD_CRUISE) {
    if (now > nextBlinkMs) { blinkEndMs = now + 140; nextBlinkMs = now + 2500 + esp_random() % 3000; }
    s.fBlink = now < blinkEndMs && s.fMood == MOOD_IDLE;
  }
  // animation phase: Zs and sweat drift, pop pop / reverse / bored alternate
  if (s.fMood == MOOD_SLEEP || s.fMood == MOOD_HOT) s.fZ = (uint8_t)((now / 25) % 40);
  else if (s.fMood == MOOD_OVERRUN) s.fZ = (uint8_t)((now / 160) % 2);
  else if (s.fMood == MOOD_REVERSE) s.fZ = (uint8_t)((now / 500) % 2);
  else if (s.fMood == MOOD_BORED) s.fZ = (uint8_t)((now / 700) % 32);

  // the avatar belongs to the gauge skin: BPD Embers = Cinder (knight); BPD FC = the original dot-matrix model
  s.fStyle = (ui.gaugeSkin == SKIN_IRON && Iron_Ready()) ? FACE_KNIGHT : FACE_CLASSIC;
  if (s.fStyle == FACE_KNIGHT) {                       // Cinder moves in whole 2 px art pixels
    s.fDx = (int8_t)(lroundf(s.fDx / 2.0f) * 2);
    s.fDy = (int8_t)(lroundf(s.fDy / 2.0f) * 2);
  }

  char t[12], r[8];
  if (bv) fmtP(t, sizeof(t), b, 2); else strcpy(t, "--");
  if (Calc_Fresh(hd.t360)) snprintf(r, sizeof(r), "%u", (unsigned)hd.rpm); else strcpy(r, "--");
  snprintf(s.fLine, sizeof(s.fLine), "%s RPM  %s %s", r, t, pUnit());
}

static void buildKeypad(FaceState &s)
{
  char t[16];
  if (Calc_Fresh(hd.t372)) { fmtP(t, sizeof(t), hd.target_boost_kpa, 2); snprintf(s.kTgt, sizeof(s.kTgt), "TGT %s %s", t, pUnit()); }
  else strcpy(s.kTgt, "TGT --");
  s.idleOn = Keypad_GetHold(KP_BTN_ROT_IDLE);
  s.logState = !Log_SdOk() ? 0 : Log_Recording() ? 2 : 1;
  s.pressed = (uint8_t)(pressedBtn ? pressedBtn : (millis() < flashUntil ? flashBtn : 0));
}

static void buildAlerts(FaceState &s, uint16_t m)
{
  s.aN = 0;
  for (int i = 0; i < W_COUNT && s.aN < 3; i++) {
    if (!(m & (1 << i))) continue;
    snprintf(s.aLine[s.aN++], sizeof(s.aLine[0]), "%s  %s %s", warns[i].name, warns[i].val, warns[i].unit);
  }
}

static void buildState(FaceState &s, int type, uint32_t now)
{
  memset(&s, 0, sizeof(s));
  s.centreCell = CELL_BIG;
  s.rowCell = CELL_MED;
  s.skin = ui.gaugeSkin;
  s.dbg = gDbgFps ? (uint8_t)(1 + (now / 1000) % 250) : 0;      // debug overlay: new value each second so it redraws

  uint16_t m = evalWarnings();
  warnAck &= m;                                     // a warning that clears can pop up again next time
  if (m != lastMask) { lastMask = m; }

  s.rec = Log_Recording() && ((now / 600) % 2 == 0);       // blinking REC while logging
  s.shift = cfg.shiftLight.on && ui.shiftShow && Calc_Fresh(hd.t360) && hd.rpm >= cfg.shiftLight.limit && ((now / 80) % 2 == 0);

  bool popupHere = !(type == PG_FACE && !ui.facePopup);   // option: leave the Face page alone (it shows its own warning face)
  if (popupHere && (m & ~warnAck)) {                // something new: full-screen popup cycling every active warning
    int ids[W_COUNT], n = 0;
    for (int i = 0; i < W_COUNT; i++) if (m & (1 << i)) ids[n++] = i;
    if (now - popSwitchMs >= cfg.alertCycleMs) { popIdx++; popSwitchMs = now; }
    popIdx %= n;
    const WarnItem &w = warns[ids[popIdx]];
    s.mode = M_POPUP;
    strncpy(s.pName, w.name, sizeof(s.pName) - 1);
    strncpy(s.pVal, w.val, sizeof(s.pVal) - 1);
    strncpy(s.pUnit, w.unit, sizeof(s.pUnit) - 1);
    s.pN = n; s.pIdx = popIdx;
    s.blink = (now / 450) % 2 == 0;
    return;
  }

  switch (type) {
    case PG_BOOST:   s.mode = M_GAUGE; buildBoost(s); break;
    case PG_OIL:     s.mode = M_GAUGE; buildOil(s); break;
    case PG_AFR:     s.mode = M_GAUGE; buildAfr(s); break;
    case PG_TACH:    s.mode = M_GAUGE; buildTach(s); break;
    case PG_COOLANT: s.mode = M_GAUGE; buildCoolant(s); break;
    case PG_FUEL:    s.mode = M_GAUGE; buildFuel(s); break;
    case PG_IAT:     s.mode = M_GAUGE; buildIat(s); break;
    case PG_MONITOR: s.mode = M_MONITOR; buildMonitor(s, m); break;
    case PG_KEYPAD:  s.mode = M_KEYPAD; buildKeypad(s); break;
    case PG_ALERTS:  s.mode = M_ALERTS; buildAlerts(s, m); break;
    case PG_LAP:     s.mode = M_GAUGE; buildLap(s, now); break;
    case PG_FACE:    s.mode = M_FACE; buildFacePage(s, m, now); break;
  }
}

// =====================================================================================
//  Face object: drawing and touch
// =====================================================================================
static int keypadHit(lv_point_t p)
{
  for (int b = 0; b < 4; b++)
    if (p.x >= KB_X[b] && p.x < KB_X[b] + KB_W && p.y >= KB_Y[b] && p.y < KB_Y[b] + KB_H) return b + 1;
  return 0;
}

static void faceEvent(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);

  if (code == LV_EVENT_DRAW_MAIN) {
    lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
    cv.buf = (lv_color_t *)dc->buf;
    cv.ba = *dc->buf_area;
    cv.stride = lv_area_get_width(dc->buf_area);
    if (!_lv_area_intersect(&cv.clip, dc->clip_area, dc->buf_area)) return;
    renderFace(fsCur);
    return;
  }

  int type = tileType[activeTile];
  lv_indev_t *ind = lv_indev_get_act();
  lv_point_t p = {0, 0};
  if (ind) lv_indev_get_point(ind, &p);

  if (code == LV_EVENT_PRESSED) {
    pressPt = p;
    pressedBtn = (type == PG_KEYPAD && fsCur.mode == M_KEYPAD) ? keypadHit(p) : 0;
    if (pressedBtn == 4 && !Log_SdOk()) pressedBtn = 0;   // LOG does nothing without a card
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    pressedBtn = 0;
  } else if (code == LV_EVENT_CLICKED) {
    if (wasSwipe()) return;
    if (fsCur.mode == M_POPUP) { warnAck = lastMask; return; }   // dismiss every warning showing now
    if (type == PG_KEYPAD) {
      int b = keypadHit(pressPt);
      if (b >= 1 && b <= 4) { flashBtn = b; flashUntil = millis() + 150; }
      switch (b) {
        case 1: Keypad_Press(KP_BTN_BOOST_DOWN); break;
        case 2: Keypad_Press(KP_BTN_BOOST_UP); break;
        case 3: Keypad_SetHold(KP_BTN_ROT_IDLE, !Keypad_GetHold(KP_BTN_ROT_IDLE)); break;
        case 4: Log_ManualToggle(); break;
        default: break;
      }
    }
  } else if (code == LV_EVENT_LONG_PRESSED) {
    if (wasSwipe() || fsCur.mode == M_POPUP) return;
    if (type == PG_BOOST) boostPeakKpa = -1000;           // hold 2 s to reset the peak
    if (type == PG_IAT)   iatPeakC = -1000;
    if (type == PG_LAP) {                                  // hold 2 s: set start/finish, then sectors
      strncpy(toast, Lap_MarkHere(), sizeof(toast) - 1);
      toastUntil = millis() + 2500;
    }
  }
}

// ---------------- Setup page (normal LVGL widgets) ----------------
static lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, lv_color_t col,
                           lv_coord_t x, lv_coord_t y, const char *txt)
{
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, col, 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(l, txt);
  lv_obj_align(l, LV_ALIGN_CENTER, x, y);
  return l;
}

static void showBleState(void)
{
  int now = BLE_Connected() ? 1 : 0;
  if (now != bleLast) {
    bleLast = now;
    lv_label_set_text_fmt(bleInfo, "Bluetooth %s\nPIN %06lu", now ? "connected" : "ready", (unsigned long)Settings_BlePin());
  }
  int sd = !Log_SdOk() ? 0 : Log_Recording() ? 2 : 1;
  if (sd != sdLast) {
    sdLast = sd;
    lv_label_set_text(sdInfo, sd == 0 ? "No SD card\nLogging off" : sd == 2 ? "SD card OK\nRecording" : "SD card OK\nNot recording");
  }
}

static void createSetupPage(lv_obj_t *parent)
{
  lv_obj_t *title = makeLabel(parent, &lv_font_montserrat_24, C_BG, 0, -110, "SETUP");
  lv_obj_set_style_bg_color(title, C_LIT, 0);
  lv_obj_set_style_bg_opa(title, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_hor(title, 30, 0);
  lv_obj_set_style_pad_ver(title, 4, 0);
  bleInfo = makeLabel(parent, &lv_font_montserrat_24, C_LIT, 0, -30, "");
  sdInfo  = makeLabel(parent, &lv_font_montserrat_16, C_LIT, 0, 60, "");
  makeLabel(parent, &lv_font_montserrat_16, C_LIT, 0, 120, "Settings, logs and keypad\nare in the BPD Gauge app");
  makeLabel(parent, &lv_font_montserrat_16, C_BRAND, 0, BPD_Y - CY, "BPD");
  bleLast = -1; sdLast = -1;
  showBleState();
}

// ---------------- Navigation ----------------
static void showTile(int idx)
{
  activeTile = idx;
  bool setup = tileType[idx] == TILE_SETUP;
  if (setup) { lv_obj_add_flag(face, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(setupCont, LV_OBJ_FLAG_HIDDEN); bleLast = -1; sdLast = -1; showBleState(); }
  else       { lv_obj_clear_flag(face, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(setupCont, LV_OBJ_FLAG_HIDDEN); }
  pressedBtn = 0;
  forceRedraw = true;
  lv_obj_invalidate(lv_scr_act());
}

static void gestureEvent(lv_event_t *e)
{
  (void)e;
  lastSwipeMs = millis();
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  int idx = activeTile;
  if (dir == LV_DIR_LEFT)       idx = (activeTile + 1) % numTiles;
  else if (dir == LV_DIR_RIGHT) idx = (activeTile + numTiles - 1) % numTiles;
  else return;
  showTile(idx);
}

// ---------------- Update timer ----------------
static void updateTimer(lv_timer_t *timer)
{
  (void)timer;
  uint32_t now = millis();

  // Peaks are tracked on every page
  bool bv; float b = Calc_BoostKpa(&bv);
  if (bv && b > boostPeakKpa) boostPeakKpa = b;
  if (Calc_Fresh(hd.t3E0) && hd.iat_c > iatPeakC) iatPeakC = hd.iat_c;

  int type = tileType[activeTile];
  if (type == TILE_SETUP) {
    showBleState();
    evalWarnings();
    return;
  }

  buildState(fsNew, type, now);
  if (forceRedraw || memcmp(&fsNew, &fsCur, sizeof(FaceState)) != 0) {
    static DirtyList dl;
    bool part = CFG_PARTIAL_REDRAW && !forceRedraw && dirtyAreas(fsCur, fsNew, dl);        // must look at the old state, before it is replaced
    forceRedraw = false;
    memcpy(&fsCur, &fsNew, sizeof(FaceState));
    if (part) { for (int i = 0; i < dl.n; i++) lv_obj_invalidate_area(face, &dl.a[i]); }
    else lv_obj_invalidate(face);
  }
}

// ---------------- Build ----------------
static void buildUI(void)
{
  setColours(false);                            // the Setup page is always drawn in the BPD FC colours
  lv_obj_t *scr = lv_scr_act();
  lv_obj_clean(scr);
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  numTiles = 0;
  for (int i = 0; i < NUM_GAUGE_PAGES; i++) {
    int type = cfg.pageOrder[i];
    if (type < 0 || type >= NUM_GAUGE_PAGES || !cfg.pageOn[type]) continue;
    tileType[numTiles++] = type;
  }
  tileType[numTiles++] = TILE_SETUP;

  // Gauge face (all gauge pages)
  face = lv_obj_create(scr);
  lv_obj_remove_style_all(face);
  lv_obj_set_size(face, LV_HOR_RES, LV_VER_RES);
  lv_obj_set_pos(face, 0, 0);
  lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(face, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(face, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(face, faceEvent, LV_EVENT_ALL, NULL);

  // Setup page
  setupCont = lv_obj_create(scr);
  lv_obj_remove_style_all(setupCont);
  lv_obj_set_size(setupCont, LV_HOR_RES, LV_VER_RES);
  lv_obj_set_style_bg_color(setupCont, C_BG, 0);
  lv_obj_set_style_bg_opa(setupCont, LV_OPA_COVER, 0);
  lv_obj_clear_flag(setupCont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(setupCont, LV_OBJ_FLAG_GESTURE_BUBBLE);
  createSetupPage(setupCont);

  lv_obj_add_event_cb(scr, gestureEvent, LV_EVENT_GESTURE, NULL);

  memset(&fsCur, 0, sizeof(fsCur));
  forceRedraw = true;                          // draw the first page as soon as the timer runs
  showTile(0);
}

static void computeLayout(void)
{
  float labelH = 7 * CELL_LABEL;
  gRefBot  = CY + LR * sinf(255.0f * PI / 180.0f) + labelH / 2;     // bottom of the top dial number
  gNumTop  = NY - 3.5f * CELL_BIG;                                   // top of the big number
  gTitleY  = gRefBot + (gNumTop - gRefBot - TH) / 2;                 // title centred in the gap
  gLeftLbl = CX + LR * cosf(135.0f * PI / 180.0f) + textW("-10", CELL_LABEL) / 2;
  gLeftEdge  = gLeftLbl + EDGE_GAP;
  gRightEdge = 480 - gLeftEdge;
}

void Gauge_UI_Init(void)
{
  C_BG = lv_color_hex(HEX_BG);   C_DIM = lv_color_hex(HEX_DIM);  C_LIT = lv_color_hex(HEX_LIT);
  C_WARN = lv_color_hex(HEX_WARN); C_OFF = lv_color_hex(HEX_OFF); C_BRAND = lv_color_hex(CFG_BRAND_COLOUR);
  computeLayout();
  buildSegTable();
  if (!Iron_Init()) Serial.println("UI: no PSRAM for the BPD Embers skin, using BPD FC");
  buildUI();
#if CFG_ENABLE_LIVE_UPDATES
  lv_timer_create(updateTimer, UPDATE_MS, NULL);
#else
  Serial.println("Live screen updates DISABLED (CFG_ENABLE_LIVE_UPDATES 0)");
#endif
}

uint16_t Gauge_WarnMask(void) { return warnMaskShared; }
void Gauge_AckWarnings(void) { ackRequest = true; }

void Gauge_WarnJson(String &out)
{
  WarnItem copy[W_COUNT];
  uint16_t m, a;
  portENTER_CRITICAL(&warnMux);
  memcpy(copy, warns, sizeof(copy));
  m = warnMaskShared; a = warnAckShared;
  portEXIT_CRITICAL(&warnMux);
  out = "[";
  bool first = true;
  for (int i = 0; i < W_COUNT; i++) {
    if (!(m & (1 << i)) || !copy[i].name) continue;
    if (!first) out += ',';
    first = false;
    out += "{\"id\":"; out += i;
    out += ",\"name\":\""; out += copy[i].name;
    out += "\",\"val\":\""; out += copy[i].val;
    out += "\",\"unit\":\""; out += copy[i].unit;
    out += "\",\"ack\":"; out += (a & (1 << i)) ? 1 : 0;
    out += '}';
  }
  out += ']';
}

// Rebuild with the current settings (call from the main loop only)
void Gauge_UI_Rebuild(void)
{
  int keepType = tileType[activeTile];
  buildUI();
  for (int i = 0; i < numTiles; i++) if (tileType[i] == keepType) { showTile(i); break; }
}
