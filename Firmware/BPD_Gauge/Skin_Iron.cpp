// BPD Embers skin compositor. See Skin_Iron.h.
//
// Cost per redraw (only when the screen description changes): one 115 KB copy of the dial plate,
// a glow pass over about 25 k pixels, about 4.7 k ring pixels and, on the avatar page, a 62 x 93 helm.
// Nothing here is allocated per frame.

#pragma GCC optimize ("O2")
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "Skin_Iron.h"
#include "IronSkin_Art.h"

#ifdef IRON_HOST
  #define IR_ALLOC(n) malloc(n)
#else
  #include <esp_heap_caps.h>
  static void *irAlloc(size_t n) { void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM); return p ? p : malloc(n); }
  #define IR_ALLOC(n) irAlloc(n)
#endif

#define G   IRON_GRID
#define CEN 120.0f

static uint16_t *plate565 = NULL, *plateFlat = NULL, *comp = NULL;   // plateFlat: same plate with the ring groove filled in, for pages with no dial
static uint16_t pal565[IR_PAL_N];
static uint8_t  hpalRgb[IR_HPAL_N][3];

struct RingPx { uint16_t pos; uint8_t seg, rr; };
struct GlowPx { uint16_t pos, ang; uint8_t rad; };
static RingPx *ringPx = NULL; static int nRing = 0;
static GlowPx *glowPx = NULL; static int nGlow = 0;
static uint16_t litLut[2][IRON_SEGS][16], unlitLut[16];
static float segCx[IRON_SEGS], segCy[IRON_SEGS];
static bool ready = false;

// What comp currently holds, so a redraw only touches what changed
enum { K_NONE = 0, K_GAUGE, K_FLAT, K_KNIGHT };
static int kind = K_NONE;
static uint8_t curSeg[IRON_SEGS];
static uint16_t emberPos[12]; static int nEmber = 0;           // pixels of the embers drawn last time
static uint16_t glowStart[312];                                // glow list sorted by angle bin: bin b = [glowStart[b], glowStart[b+1])
static uint16_t ringStart[IRON_SEGS + 1];                      // ring list sorted by segment
static uint8_t  binBox[311][4], segBox[IRON_SEGS][4];          // x0,y0,x1,y1 on the grid of the pixels of each bin / segment

#define GLOW_BINS 311

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline uint16_t pk(int r, int g, int b)
{
  return (uint16_t)(((clampi(r, 0, 255) & 0xF8) << 8) | ((clampi(g, 0, 255) & 0xFC) << 3) | (clampi(b, 0, 255) >> 3));
}
static inline void unpk(uint16_t c, int &r, int &g, int &b)
{
  r = (c >> 8) & 0xF8; g = (c >> 3) & 0xFC; b = (c << 3) & 0xF8;
}
static inline void addPx(uint16_t *p, int ar, int ag, int ab)
{
  int r, g, b; unpk(*p, r, g, b); *p = pk(r + ar, g + ag, b + ab);
}

static float angleRel(float x, float y)           // degrees clockwise from the ring start (7:30 position), -180..180 wrapped later
{
  float a = atan2f(y + .5f - CEN, x + .5f - CEN) * 57.2957795f;
  if (a < 0) a += 360.0f;
  float rel = a - 135.0f;
  if (rel < 0) rel += 360.0f;
  return rel;
}

bool Iron_Ready(void) { return ready; }
const uint16_t *Iron_Frame(void) { return comp; }

bool Iron_Init(void)
{
  if (ready) return true;
  plate565 = (uint16_t *)IR_ALLOC(G * G * 2);
  comp     = (uint16_t *)IR_ALLOC(G * G * 2);
  plateFlat = (uint16_t *)IR_ALLOC(G * G * 2);
  ringPx   = (RingPx *)IR_ALLOC(6000 * sizeof(RingPx));
  glowPx   = (GlowPx *)IR_ALLOC(30000 * sizeof(GlowPx));
  if (!plate565 || !plateFlat || !comp || !ringPx || !glowPx) return false;

  for (int i = 0; i < IR_PAL_N; i++) pal565[i] = pk(IR_PAL[i * 3], IR_PAL[i * 3 + 1], IR_PAL[i * 3 + 2]);
  for (int i = 0; i < G * G; i++) plate565[i] = pal565[IR_PLATE[i]];
  // Dial-less plate: every pixel of the ring groove takes the bezel pixel at the same angle, just outside it
  memcpy(plateFlat, plate565, G * G * 2);
  for (int y = 0; y < G; y++)
    for (int x = 0; x < G; x++) {
      float ddx = x + 0.5f - G / 2, ddy = y + 0.5f - G / 2, R = sqrtf(ddx * ddx + ddy * ddy);
      if (R < 91.5f || R > 109.0f) continue;
      int sx = (int)(G / 2 + ddx / R * 110.0f), sy = (int)(G / 2 + ddy / R * 110.0f);
      plateFlat[y * G + x] = plate565[clampi(sy, 0, G - 1) * G + clampi(sx, 0, G - 1)];
    }
  for (int i = 0; i < IR_HPAL_N; i++) for (int k = 0; k < 3; k++) hpalRgb[i][k] = IR_HPAL[i * 3 + k];

  // ring pixels (radius 93..108 on the grid, 270 degrees, 4 of every 6 degrees)
  nRing = 0; nGlow = 0;
  for (int y = 0; y < G; y++)
    for (int x = 0; x < G; x++) {
      float dx = x + .5f - CEN, dy = y + .5f - CEN, R = sqrtf(dx * dx + dy * dy);
      float rel = angleRel((float)x, (float)y);
      if (R >= 93.0f && R < 108.0f && rel < 270.0f) {
        int i = (int)(rel / 6.0f); float w = rel - i * 6.0f;
        if (w >= 1.0f && w < 5.0f && nRing < 6000) {
          ringPx[nRing].pos = (uint16_t)(y * G + x); ringPx[nRing].seg = (uint8_t)i;
          ringPx[nRing].rr = (uint8_t)clampi((int)((R - 93.0f) / 15.0f * 255.0f), 0, 255);
          nRing++;
        }
      }
      if (R >= 79.0f && R < 120.0f) {                       // glow zone, angular position in 1 degree bins (-20..290)
        float r2 = rel > 340.0f ? rel - 360.0f : rel;
        if (r2 >= -20.0f && r2 < 291.0f && nGlow < 30000) {
          float rw = expf(-((R - 100.5f) * (R - 100.5f)) / (2.0f * 49.0f));
          glowPx[nGlow].pos = (uint16_t)(y * G + x); glowPx[nGlow].ang = (uint16_t)(r2 + 20.0f);
          glowPx[nGlow].rad = (uint8_t)(rw * 255.0f);
          if (glowPx[nGlow].rad > 2) nGlow++;
        }
      }
    }
  // Sort the glow list by angle bin and the ring list by segment, so a part of the ring is one slice of each list.
  {
    GlowPx *tg = (GlowPx *)IR_ALLOC(sizeof(GlowPx) * (nGlow ? nGlow : 1));
    RingPx *tr = (RingPx *)IR_ALLOC(sizeof(RingPx) * (nRing ? nRing : 1));
    if (!tg || !tr) { free(tg); free(tr); return false; }
    int cnt[312]; memset(cnt, 0, sizeof(cnt));
    for (int k = 0; k < nGlow; k++) cnt[glowPx[k].ang + 1]++;
    for (int b = 0; b < 311; b++) cnt[b + 1] += cnt[b];
    for (int b = 0; b < 312; b++) glowStart[b] = (uint16_t)cnt[b];
    for (int k = 0; k < nGlow; k++) tg[cnt[glowPx[k].ang]++] = glowPx[k];
    memcpy(glowPx, tg, sizeof(GlowPx) * nGlow);
    int c2[IRON_SEGS + 1]; memset(c2, 0, sizeof(c2));
    for (int k = 0; k < nRing; k++) c2[ringPx[k].seg + 1]++;
    for (int i = 0; i < IRON_SEGS; i++) c2[i + 1] += c2[i];
    for (int i = 0; i <= IRON_SEGS; i++) ringStart[i] = (uint16_t)c2[i];
    for (int k = 0; k < nRing; k++) tr[c2[ringPx[k].seg]++] = ringPx[k];
    memcpy(ringPx, tr, sizeof(RingPx) * nRing);
    free(tg); free(tr);
    for (int b = 0; b < 311; b++) { binBox[b][0] = binBox[b][1] = 255; binBox[b][2] = binBox[b][3] = 0; }
    for (int b = 0; b < 311; b++)
      for (int k = glowStart[b]; k < glowStart[b + 1]; k++) {
        int x = glowPx[k].pos % G, y = glowPx[k].pos / G;
        if (x < binBox[b][0]) binBox[b][0] = (uint8_t)x; if (y < binBox[b][1]) binBox[b][1] = (uint8_t)y;
        if (x > binBox[b][2]) binBox[b][2] = (uint8_t)x; if (y > binBox[b][3]) binBox[b][3] = (uint8_t)y;
      }
    for (int i = 0; i < IRON_SEGS; i++) {
      segBox[i][0] = segBox[i][1] = 255; segBox[i][2] = segBox[i][3] = 0;
      for (int k = ringStart[i]; k < ringStart[i + 1]; k++) {
        int x = ringPx[k].pos % G, y = ringPx[k].pos / G;
        if (x < segBox[i][0]) segBox[i][0] = (uint8_t)x; if (y < segBox[i][1]) segBox[i][1] = (uint8_t)y;
        if (x > segBox[i][2]) segBox[i][2] = (uint8_t)x; if (y > segBox[i][3]) segBox[i][3] = (uint8_t)y;
      }
    }
  }
  for (int i = 0; i < IRON_SEGS; i++) {
    float a = (135.0f + i * 6.0f + 3.0f) * 0.01745329f;
    segCx[i] = CEN + 100.5f * cosf(a); segCy[i] = CEN + 100.5f * sinf(a);
  }

  // colour tables, 16 steps across the width of a segment
  static const float ramp[4][3] = { {120, 22, 10}, {210, 70, 16}, {255, 150, 30}, {255, 214, 80} };
  for (int b = 0; b < 16; b++) {
    float rr = (b + 0.5f) / 16.0f, core = expf(-((rr - 0.5f) / 0.18f) * ((rr - 0.5f) / 0.18f)) * 60.0f;
    float sh = 0.5f + 0.5f * cosf((rr - 0.25f) * 3.0f), k = 0.7f + 0.6f * sh;
    unlitLut[b] = pk((int)(28 * k), (int)(26 * k), (int)(32 * k));
    for (int i = 0; i < IRON_SEGS; i++) {
      for (int w = 0; w < 2; w++) {
        float c0[3], c1[3];
        if (w) { c0[0] = 150; c0[1] = 18; c0[2] = 10; c1[0] = 255; c1[1] = 70; c1[2] = 30; }
        else {
          float f = fminf(1.0f, i / 38.0f) * 3.0f; int j = (int)f; if (j > 2) j = 2; float u = f - j;
          for (int c = 0; c < 3; c++) { c0[c] = ramp[j][c] * (1 - u) + ramp[j + 1][c] * u; c1[c] = fminf(255.0f, c0[c] * 1.25f + 30.0f); }
        }
        int v[3];
        for (int c = 0; c < 3; c++) v[c] = (int)(c0[c] * (1 - rr) + c1[c] * rr + core);
        litLut[w][i][b] = pk(v[0], v[1], v[2]);
      }
    }
  }
  ready = true;
  return true;
}

// ------------------------------------------------------------------ ring
// The few drifting embers at the head of the bar. Positions depend only on how many segments are lit.
static int emberList(int lit, uint16_t *pos, uint8_t *gcol)
{
  int n = 0;
  if (lit <= 0) return 0;
  uint32_t h = (uint32_t)lit * 2654435761u;
  for (int k = 0; k < 12; k++) {
    h = h * 1664525u + 1013904223u;
    float a = ((135.0f + (lit - 1) * 6.0f + 3.0f) + ((int)((h >> 8) & 31) - 15)) * 0.01745329f;
    float r = 70.0f + (float)((h >> 16) & 63) * 0.75f;
    int x = (int)(CEN + r * cosf(a)), y = (int)(CEN + r * sinf(a)) - (int)((h >> 24) & 7);
    if (x < 0 || x >= G || y < 0 || y >= G) continue;
    float dx = x + .5f - CEN, dy = y + .5f - CEN;
    if (dx * dx + dy * dy < 119.0f * 119.0f) { pos[n] = (uint16_t)(y * G + x); gcol[n] = (uint8_t)(120 + (int)((h >> 4) & 63)); n++; }
  }
  return n;
}

static int litCount(const uint8_t *seg) { int l = 0; while (l < IRON_SEGS && seg[l]) l++; return l; }
static bool anyWarn(const uint8_t *seg) { for (int i = 0; i < IRON_SEGS; i++) if (seg[i] == 2) return true; return false; }

// Which part of the ring a change from oldSeg to newSeg can touch: glow bins [bLo,bHi] and segments [sLo,sHi].
// Returns 0 = nothing changed, 1 = a part, 2 = everything (the glow colour changed).
static int changeRange(const uint8_t *o, const uint8_t *n, int *bLo, int *bHi, int *sLo, int *sHi)
{
  if (anyWarn(o) != anyWarn(n)) { *bLo = 0; *bHi = 310; *sLo = 0; *sHi = IRON_SEGS - 1; return 2; }
  int cmin = 99, cmax = -1;
  for (int i = 0; i < IRON_SEGS; i++) if (o[i] != n[i]) { if (i < cmin) cmin = i; if (i > cmax) cmax = i; }
  if (cmax < 0) return 0;
  int ho = litCount(o) - 1, hn = litCount(n) - 1;                 // the head of the bar changes colour and moves the embers
  if (ho >= 0) { if (ho < cmin) cmin = ho; if (ho > cmax) cmax = ho; }
  if (hn >= 0) { if (hn < cmin) cmin = hn; if (hn > cmax) cmax = hn; }
  *bLo = clampi(cmin * 6 + 23 - 26, 0, 310); *bHi = clampi(cmax * 6 + 23 + 26, 0, 310);
  *sLo = IRON_SEGS; *sHi = -1;
  for (int i = 0; i < IRON_SEGS; i++)
    if (i * 6 + 25 >= *bLo && i * 6 + 21 <= *bHi) { if (i < *sLo) *sLo = i; if (i > *sHi) *sHi = i; }
  return 1;
}

// Draw glow bins [bLo,bHi], ring segments [sLo,sHi], the hot head and the embers on top of what comp holds there.
static void drawRingPart(const uint8_t *seg, int bLo, int bHi, int sLo, int sHi)
{
  bool glowOn = false, warnGlow = false;
  int lit = litCount(seg);

  // glow first, so the ring pixels are drawn on top of it
  uint8_t g8[GLOW_BINS];
  {
    float A[GLOW_BINS]; memset(A, 0, sizeof(A));
    for (int i = 0; i < IRON_SEGS; i++) {
      if (!seg[i]) continue;
      glowOn = true; if (seg[i] == 2) warnGlow = true;
      float c = i * 6.0f + 3.0f + 20.0f;                   // bin position of the segment centre
      for (int b = (int)c - 14; b <= (int)c + 14; b++) {
        if (b < 0 || b >= GLOW_BINS) continue;
        float d = (b + 0.5f) - c; A[b] += expf(-(d * d) / 32.0f);
      }
    }
    for (int b = 0; b < GLOW_BINS; b++) g8[b] = (uint8_t)(fminf(1.4f, A[b] * 0.55f) * 0.55f * 255.0f);
  }
  if (glowOn) {
    const int tr = 255, tg = warnGlow ? 50 : 110, tb = warnGlow ? 25 : 30;
    for (int k = glowStart[bLo]; k < glowStart[bHi + 1]; k++) {
      int v = (g8[glowPx[k].ang] * glowPx[k].rad) >> 8;
      if (v < 2) continue;
      addPx(&comp[glowPx[k].pos], (v * tr) >> 8, (v * tg) >> 8, (v * tb) >> 8);
    }
  }

  for (int i = sLo; i <= sHi; i++) {
    int st = seg[i];
    for (int k = ringStart[i]; k < ringStart[i + 1]; k++) {
      int b = ringPx[k].rr >> 4;
      comp[ringPx[k].pos] = st ? litLut[st == 2 ? 1 : 0][i][b] : unlitLut[b];
    }
  }
  if (lit > 0 && seg[lit - 1] != 2 && lit - 1 >= sLo && lit - 1 <= sHi) {          // hot head of the bar
    uint16_t hot = pk(255, 240, 200);
    for (int k = ringStart[lit - 1]; k < ringStart[lit]; k++) comp[ringPx[k].pos] = hot;
  }
  uint8_t gc[12];
  nEmber = emberList(lit, emberPos, gc);
  for (int n = 0; n < nEmber; n++) comp[emberPos[n]] = pk(255, gc[n], 40);
}

// Screen rectangle (inclusive) that Iron_ComposeGauge will touch when the bar goes from oldSeg to newSeg.
// Returns 0 = nothing changes, 1 = rectangle in x1..y2, 2 = redraw the whole screen.
int Iron_GaugeDirty(const uint8_t *oldSeg, const uint8_t *newSeg, int *x1, int *y1, int *x2, int *y2)
{
  int bLo, bHi, sLo, sHi;
  int r = changeRange(oldSeg, newSeg, &bLo, &bHi, &sLo, &sHi);
  if (r != 1) return r;
  int ax = 255, ay = 255, bx = 0, by = 0;
  #define ADD(a0, a1, a2, a3) { if ((a0) < ax) ax = (a0); if ((a1) < ay) ay = (a1); if ((a2) > bx) bx = (a2); if ((a3) > by) by = (a3); }
  for (int b = bLo; b <= bHi; b++) if (binBox[b][2] >= binBox[b][0]) ADD(binBox[b][0], binBox[b][1], binBox[b][2], binBox[b][3]);
  for (int i = sLo; i <= sHi; i++) if (segBox[i][2] >= segBox[i][0]) ADD(segBox[i][0], segBox[i][1], segBox[i][2], segBox[i][3]);
  uint16_t ep[12]; uint8_t gc[12];
  for (int pass = 0; pass < 2; pass++) {
    int n = emberList(litCount(pass ? newSeg : oldSeg), ep, gc);
    for (int k = 0; k < n; k++) { int x = ep[k] % G, y = ep[k] / G; ADD(x, y, x, y); }
  }
  #undef ADD
  *x1 = ax * 2; *y1 = ay * 2; *x2 = bx * 2 + 1; *y2 = by * 2 + 1;
  return 1;
}

void Iron_ComposeGauge(const uint8_t *seg, int ringMode)
{
  if (!ready) return;
  if (ringMode == IRON_RING_NONE || !seg) {                    // pages without a dial: the plain plate, once
    if (kind != K_FLAT) { memcpy(comp, plateFlat, G * G * 2); kind = K_FLAT; }
    return;
  }
  if (kind == K_GAUGE) {
    int bLo, bHi, sLo, sHi;
    int r = changeRange(curSeg, seg, &bLo, &bHi, &sLo, &sHi);
    if (r == 0) return;                                        // nothing changed: comp is already right
    if (r == 1) {                                              // only part of the ring changed: redo just that part
      for (int n = 0; n < nEmber; n++) comp[emberPos[n]] = plate565[emberPos[n]];
      for (int k = glowStart[bLo]; k < glowStart[bHi + 1]; k++) comp[glowPx[k].pos] = plate565[glowPx[k].pos];
      drawRingPart(seg, bLo, bHi, sLo, sHi);
      memcpy(curSeg, seg, IRON_SEGS);
      return;
    }
  }
  memcpy(comp, plate565, G * G * 2);                           // first time, or the glow colour changed: everything
  drawRingPart(seg, 0, 310, 0, IRON_SEGS - 1);
  memcpy(curSeg, seg, IRON_SEGS);
  kind = K_GAUGE;
}

// ------------------------------------------------------------------ knight avatar
struct Mood {
  uint8_t eye[3], vent[3];
  float glow, aperture, angry, stem, cracks;
  uint8_t plate[3][3];
  uint8_t cold, warn;
};
// order = MOOD_* in Gauge_UI.cpp: SLEEP IDLE CRUISE BOOST REDLINE COLD WARN HELLO HOT FAST OVERRUN REVERSE BORED
static const Mood MOODS[13] = {
  { {110,40,20},  {60,20,12},   0.25f, 0.12f, 0.0f, 0.15f, 0.0f, {{40,44,56},{14,16,22},{110,120,140}}, 0, 0 },
  { {255,150,40}, {190,80,20},  0.50f, 0.55f, 0.0f, 0.45f, 0.1f, {{70,50,30},{16,10,6},{180,130,70}},    0, 0 },
  { {255,170,50}, {210,100,25}, 0.65f, 0.60f, 0.0f, 0.55f, 0.2f, {{110,64,14},{18,10,4},{220,150,70}},   0, 0 },
  { {255,225,100},{255,150,40}, 0.90f, 1.00f, 0.0f, 0.85f, 0.5f, {{150,70,12},{18,8,2},{255,170,70}},    0, 0 },
  { {255,246,220},{255,200,120},1.25f, 1.00f, 0.0f, 1.00f, 1.0f, {{150,18,10},{16,2,2},{255,110,70}},    0, 0 },
  { {170,220,255},{120,170,220},0.50f, 0.40f, 0.0f, 0.40f, 0.0f, {{40,70,110},{8,14,24},{150,200,255}},  1, 0 },
  { {255,60,40},  {255,40,25},  1.00f, 1.00f, 0.0f, 1.00f, 0.9f, {{140,14,10},{14,2,2},{255,100,70}},    0, 1 },
  { {255,215,90}, {255,170,50}, 1.00f, 0.90f, 0.0f, 0.70f, 0.3f, {{130,76,10},{18,10,2},{255,190,80}},   0, 0 },
  { {255,130,40}, {255,100,30}, 0.80f, 0.45f, 0.0f, 0.70f, 0.35f,{{130,50,14},{16,6,2},{255,130,60}},   0, 0 },
  { {255,225,100},{255,170,60}, 0.95f, 0.80f, 0.5f, 0.90f, 0.6f, {{150,70,12},{18,8,2},{255,170,70}},    0, 0 },
  { {255,150,40}, {255,110,30}, 0.70f, 0.60f, 0.0f, 0.60f, 0.3f, {{120,60,20},{16,8,4},{230,140,60}},    0, 0 },
  { {255,190,60}, {200,120,30}, 0.70f, 0.65f, 0.0f, 0.50f, 0.15f,{{100,70,24},{16,10,4},{210,160,70}},   0, 0 },
  { {200,110,40}, {120,60,20},  0.35f, 0.35f, 0.0f, 0.20f, 0.0f, {{60,48,34},{12,10,8},{150,124,90}},    0, 0 },
};

void Iron_MoodPlate(int mood, uint8_t fill[3], uint8_t edge[3], uint8_t hl[3])
{
  const Mood &m = MOODS[clampi(mood, 0, 12)];
  for (int k = 0; k < 3; k++) { fill[k] = m.plate[0][k]; edge[k] = m.plate[1][k]; hl[k] = m.plate[2][k]; }
}

static inline int helmIdx(int gx, int gy, int ox, int oy)        // sprite index at a grid position, 255 outside
{
  int x = gx - IR_HX0 - ox, y = gy - IR_HY0 - oy;
  if (x < 0 || y < 0 || x >= IR_HW || y >= IR_HH) return IR_HCLEAR;
  return IR_HELM[y * IR_HW + x];
}

void Iron_ComposeKnight(int moodId, int phase, int ox, int oy)
{
  if (!ready) return;
  Mood m = MOODS[clampi(moodId, 0, 12)];
  if (moodId == 10) m.glow = (phase & 1) ? 1.0f : 0.4f;                 // pop pop: flashes
  if (moodId == 11) m.aperture = (phase & 1) ? 0.8f : 0.5f;             // reverse: looks about
  if (moodId == 12) m.aperture = ((phase >> 3) & 1) ? 0.45f : 0.25f;    // bored: slow droop
  memcpy(comp, plateFlat, G * G * 2);
  kind = K_KNIGHT;

  // back-light behind the helm, so the silhouette separates from the dial
  const int BW = IR_HW + 2 * IR_BACK_M, BH = IR_HH + 2 * IR_BACK_M;
  for (int my = 0; my < BH; my++) {
    int gy = IR_HY0 + oy - IR_BACK_M + my; if (gy < 0 || gy >= G) continue;
    for (int mx = 0; mx < BW; mx++) {
      int v = IR_BACK[my * BW + mx]; if (v < 3) continue;
      int gx = IR_HX0 + ox - IR_BACK_M + mx; if (gx < 0 || gx >= G) continue;
      if (helmIdx(gx, gy, ox, oy) != IR_HCLEAR) continue;
      float k = v / 255.0f * 0.2f * m.glow;
      addPx(&comp[gy * G + gx], (int)(m.vent[0] * k), (int)(m.vent[1] * k), (int)(m.vent[2] * k));
    }
  }
  // helm sprite
  uint16_t hp[IR_HPAL_N];
  for (int i = 0; i < IR_HPAL_N; i++) {
    float r = hpalRgb[i][0], g = hpalRgb[i][1], b = hpalRgb[i][2];
    if (m.cold) { r *= 0.85f; g *= 0.97f; b *= 1.12f; }
    hp[i] = pk((int)r, (int)g, (int)b);
  }
  for (int y = 0; y < IR_HH; y++) {
    int gy = IR_HY0 + oy + y; if (gy < 0 || gy >= G) continue;
    for (int x = 0; x < IR_HW; x++) {
      uint8_t v = IR_HELM[y * IR_HW + x]; if (v == IR_HCLEAR) continue;
      int gx = IR_HX0 + ox + x; if (gx < 0 || gx >= G) continue;
      comp[gy * G + gx] = hp[v];
    }
  }
  // glow around the visor, scaled by how open the eyes and the mouth are
  const int HW2 = IR_HW + 2 * IR_HALO_M;
  for (int my = 0; my < IR_HH + 2 * IR_HALO_M; my++) {
    int gy = IR_HY0 + oy - IR_HALO_M + my; if (gy < 87 || gy > 186) continue;
    bool arm = (my - IR_HALO_M) < IR_ARM_END + 2;
    const uint8_t *col = arm ? m.eye : m.vent;
    float amt = (arm ? m.aperture : m.stem) * 1.3f * m.glow;
    for (int mx = 0; mx < HW2; mx++) {
      int v = IR_HALO[my * HW2 + mx]; if (v < 4) continue;
      int gx = IR_HX0 + ox - IR_HALO_M + mx; if (gx < 0 || gx >= G) continue;
      float dx = gx + .5f - CEN, dy = gy + .5f - CEN; if (dx * dx + dy * dy > 96.0f * 96.0f) continue;
      float k = v / 255.0f * amt * (helmIdx(gx, gy, ox, oy) != IR_HCLEAR ? 1.0f : 0.35f);
      addPx(&comp[gy * G + gx], (int)(col[0] * k), (int)(col[1] * k), (int)(col[2] * k));
    }
  }
  // lit visor
  const int ah = IR_ARM_END - IR_ARM0; const float tcx = (IR_TX0 + IR_TX1) * 0.5f, hw = (IR_TX1 - IR_TX0) * 0.5f;
  const float cyArm = IR_ARM0 + ah * 0.5f, half = fmaxf(0.5f, m.aperture * ah * 0.5f);
  uint16_t eyeC = pk(m.eye[0], m.eye[1], m.eye[2]), ventC = pk(m.vent[0], m.vent[1], m.vent[2]);
  for (int k = 0; k < IR_NT; k++) {
    int x = IR_T[k * 2], y = IR_T[k * 2 + 1];
    if (y < IR_ARM0) continue;
    bool on; uint16_t c;
    if (y < IR_ARM_END) {
      float cut = m.angry > 0 ? (1.0f - fabsf(x - tcx) / hw) * ah * 0.55f * m.angry : 0.0f;
      on = fabsf(y - cyArm) <= half && y >= IR_ARM0 + cut; c = eyeC;
    } else { on = (y - IR_ARM_END) <= m.stem * (IR_TY1 - IR_ARM_END); c = ventC; }
    if (!on) continue;
    int gx = IR_HX0 + ox + x, gy = IR_HY0 + oy + y;
    if (gx >= 0 && gx < G && gy >= 0 && gy < G) comp[gy * G + gx] = c;
  }
  // ember cracks
  int nc = (int)(m.cracks * IR_NCRACK + 0.5f);
  uint16_t cc = m.warn ? pk(255, 70, 35) : m.cold ? pk(150, 200, 255) : pk(255, 140, 40);
  for (int i = 0; i < nc && i < IR_NCRACK; i++)
    for (int k = IR_CRACK_START[i]; k < IR_CRACK_START[i + 1]; k++) {
      int gx = IR_HX0 + ox + IR_CRACK[k] % IR_HW, gy = IR_HY0 + oy + IR_CRACK[k] / IR_HW;
      if (gx >= 0 && gx < G && gy >= 0 && gy < G) comp[gy * G + gx] = cc;
    }
  // ash drifting in the dial, kept inside the avatar zone
  uint32_t h = 12345u;
  for (int n = 0; n < 46; n++) {
    h = h * 1664525u + 1013904223u; int x = 30 + (int)((h >> 8) % 180);
    h = h * 1664525u + 1013904223u; int y = 87 + (int)((h >> 8) % 99);
    float dx = x + .5f - CEN, dy = y + .5f - CEN;
    if (dx * dx + dy * dy < 94.0f * 94.0f && helmIdx(x, y, ox, oy) == IR_HCLEAR) comp[y * G + x] = pk(130, 130, 140);
  }
}
