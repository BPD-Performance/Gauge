// GPS reader (NMEA RMC + GGA), 10 Hz setup for u-blox M8 and M10, speed pulse output for the
// ECU, and a lap timer with sectors and a live delta to the best lap.

#include "GPS_Lap.h"
#include <Preferences.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <math.h>
#include "Data_Log.h"

#define GPS_RX_PIN     44
#define GPS_TX_PIN     0
#define PULSE_PIN      43
#define GPS_MAGIC      0xB9D20001
#define TRK_MAGIC      0xB9D30001
#define BEST_MAX       6000           // best-lap trace points (10 min at 10 Hz)
#define EARTH_R        6371000.0

struct GpsConfig {
  uint32_t magic;
  uint8_t  on;                // GPS fitted
  uint8_t  pulseOn;           // speed pulses to the ECU
  uint16_t ppk;               // pulses per km
};
struct TrackStore {
  uint32_t magic;
  char     name[24];
  uint8_t  n;
  Gate     g[TRK_MAX_GATES];
};

static GpsConfig gc;
static TrackStore trk;
static HardwareSerial gpsSer(1);
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static GpsState gs;           // shared, guarded by mux
static LapState ls;

// ---------------- Config ----------------
static void loadCfg(void)
{
  Preferences p;
  p.begin("bpdgps", true);
  bool ok = p.getBytes("cfg", &gc, sizeof(gc)) == sizeof(gc) && gc.magic == GPS_MAGIC;
  bool tok = p.getBytes("trk", &trk, sizeof(trk)) == sizeof(trk) && trk.magic == TRK_MAGIC;
  p.end();
  if (!ok) { gc = { GPS_MAGIC, 0, 1, 5000 }; }
  if (gc.ppk < 500 || gc.ppk > 20000) gc.ppk = 5000;
  if (!tok || trk.n > TRK_MAX_GATES) { memset(&trk, 0, sizeof(trk)); trk.magic = TRK_MAGIC; }
}
static void saveCfg(void) { Preferences p; p.begin("bpdgps", false); p.putBytes("cfg", &gc, sizeof(gc)); p.end(); }
static void saveTrk(void) { Preferences p; p.begin("bpdgps", false); p.putBytes("trk", &trk, sizeof(trk)); p.end(); }

bool Gps_Enabled(void) { return gc.on; }

// ---------------- Speed pulse output ----------------
static esp_timer_handle_t pulseTimer = NULL;
static volatile bool pulseLevel = false;
static float pulseHz = 0;

static void IRAM_ATTR pulseCb(void *arg)
{
  pulseLevel = !pulseLevel;
  digitalWrite(PULSE_PIN, pulseLevel);
}

static void setPulseSpeed(float kmh)
{
  if (!gc.pulseOn || !pulseTimer) return;
  float hz = (kmh < 2.0f) ? 0 : kmh * gc.ppk / 3600.0f;
  if (fabsf(hz - pulseHz) < 0.01f * pulseHz + 0.05f) return;   // ignore tiny changes
  pulseHz = hz;
  esp_timer_stop(pulseTimer);
  if (hz <= 0) { pulseLevel = false; digitalWrite(PULSE_PIN, LOW); return; }
  esp_timer_start_periodic(pulseTimer, (uint64_t)(500000.0f / hz));
}

// ---------------- u-blox setup (10 Hz, 115200 baud, only RMC + GGA) ----------------
static void ubx(uint8_t cls, uint8_t id, const uint8_t *pl, uint16_t len)
{
  uint8_t ca = 0, cb = 0;
  uint8_t h[6] = { 0xB5, 0x62, cls, id, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
  for (int i = 2; i < 6; i++) { ca += h[i]; cb += ca; }
  for (int i = 0; i < len; i++) { ca += pl[i]; cb += ca; }
  gpsSer.write(h, 6); gpsSer.write(pl, len); gpsSer.write(ca); gpsSer.write(cb);
  gpsSer.flush();
  delay(30);
}

static void configureModule(uint32_t atBaud)
{
  gpsSer.updateBaudRate(atBaud);
  delay(50);
  // u-blox M8: UBX-CFG-PRT (UART1, 8N1, 115200, UBX+NMEA in, NMEA out)
  const uint8_t prt[20] = { 1, 0, 0, 0, 0xD0, 0x08, 0, 0, 0x00, 0xC2, 0x01, 0x00, 0x07, 0, 0x02, 0, 0, 0, 0, 0 };
  ubx(0x06, 0x00, prt, 20);
  // u-blox M10: UBX-CFG-VALSET (RAM) CFG-UART1-BAUDRATE = 115200
  const uint8_t vb[12] = { 0, 1, 0, 0, 0x01, 0x00, 0x52, 0x40, 0x00, 0xC2, 0x01, 0x00 };
  ubx(0x06, 0x8A, vb, 12);
  gpsSer.updateBaudRate(115200);
  delay(100);
  // 10 Hz: CFG-RATE (M8) and CFG-RATE-MEAS = 100 ms (M10)
  const uint8_t rate[6] = { 100, 0, 1, 0, 1, 0 };
  ubx(0x06, 0x08, rate, 6);
  const uint8_t vr[10] = { 0, 1, 0, 0, 0x01, 0x00, 0x21, 0x30, 100, 0 };
  ubx(0x06, 0x8A, vr, 10);
  // Turn off the NMEA sentences we don't use (GLL, GSA, GSV, VTG) to keep the link quiet
  const uint8_t offIds[4] = { 0x01, 0x02, 0x03, 0x05 };
  for (int i = 0; i < 4; i++) { uint8_t m[3] = { 0xF0, offIds[i], 0 }; ubx(0x06, 0x01, m, 3); }
  // M10 equivalents: CFG-MSGOUT-NMEA_ID_GLL/GSA/GSV/VTG_UART1 = 0
  const uint8_t vo[4 + 4 * 5] = { 0, 1, 0, 0,
    0xCA, 0x00, 0x91, 0x20, 0,   0xC0, 0x00, 0x91, 0x20, 0,   0xC5, 0x00, 0x91, 0x20, 0,   0xB1, 0x00, 0x91, 0x20, 0 };
  ubx(0x06, 0x8A, vo, sizeof(vo));
}

// ---------------- NMEA ----------------
static double nmeaDeg(const char *s, char hemi)
{
  if (!*s) return NAN;
  double v = atof(s);
  int d = (int)(v / 100);
  double deg = d + (v - d * 100) / 60.0;
  return (hemi == 'S' || hemi == 'W') ? -deg : deg;
}

static uint32_t toUnix(int y, int mo, int d, int h, int mi, int s)
{
  // days from civil (Howard Hinnant)
  y -= mo <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097L + (long)doe - 719468L;
  return (uint32_t)(days * 86400L + h * 3600L + mi * 60L + s);
}

static void onFix(double lat, double lon, float kmh, float course, uint32_t now);

static void parseLine(char *line, uint32_t now)
{
  // checksum
  char *star = strchr(line, '*');
  if (!star || line[0] != '$') return;
  uint8_t cs = 0;
  for (char *p = line + 1; p < star; p++) cs ^= (uint8_t)*p;
  if (strtol(star + 1, NULL, 16) != cs) return;
  *star = 0;
  portENTER_CRITICAL(&mux); gs.nmeaMs = now ? now : 1; portEXIT_CRITICAL(&mux);   // checksum passed: the module is alive

  char *f[20]; int n = 0;
  for (char *p = line; n < 20; ) { f[n++] = p; char *c = strchr(p, ','); if (!c) break; *c = 0; p = c + 1; }
  const char *t = f[0] + 3;                         // skip $GP / $GN / $GL

  if (!strcmp(t, "RMC") && n >= 10) {
    bool valid = f[2][0] == 'A';
    if (strlen(f[1]) >= 6 && strlen(f[9]) == 6) {
      int hh = (f[1][0] - '0') * 10 + f[1][1] - '0', mm = (f[1][2] - '0') * 10 + f[1][3] - '0', ss = (f[1][4] - '0') * 10 + f[1][5] - '0';
      int dd = (f[9][0] - '0') * 10 + f[9][1] - '0', mo = (f[9][2] - '0') * 10 + f[9][3] - '0', yy = 2000 + (f[9][4] - '0') * 10 + f[9][5] - '0';
      uint32_t u = toUnix(yy, mo, dd, hh, mm, ss);
      portENTER_CRITICAL(&mux); gs.utc = u; portEXIT_CRITICAL(&mux);
      static uint32_t lastSet = 0;
      if (valid && now - lastSet > 60000) { lastSet = now; Log_SetTime(u, LOG_TZ_KEEP); }   // dates the logs without the phone
    }
    if (!valid) { portENTER_CRITICAL(&mux); gs.fix = false; portEXIT_CRITICAL(&mux); setPulseSpeed(0); return; }
    double lat = nmeaDeg(f[3], f[4][0]), lon = nmeaDeg(f[5], f[6][0]);
    float kmh = atof(f[7]) * 1.852f, course = atof(f[8]);
    if (isnan(lat) || isnan(lon)) return;
    onFix(lat, lon, kmh, course, now);
  } else if (!strcmp(t, "GGA") && n >= 8) {
    portENTER_CRITICAL(&mux); gs.sats = atoi(f[7]); portEXIT_CRITICAL(&mux);
  }
}

// ---------------- Lap timer ----------------
struct Pt { float d; uint32_t t; };              // distance (m) and time (ms) into the lap
static Pt *bestTrace = NULL, *curTrace = NULL;
static uint32_t bestN = 0, curN = 0;
static float lapDist = 0, bestDist = 0;
static uint32_t lapStartMs = 0;
static double prevLat = NAN, prevLon = NAN;
static uint32_t prevMs = 0;
static float lastCourse = NAN;

static void localXY(double lat0, double lon0, double lat, double lon, double &x, double &y)
{
  x = (lon - lon0) * (M_PI / 180.0) * EARTH_R * cos(lat0 * M_PI / 180.0);
  y = (lat - lat0) * (M_PI / 180.0) * EARTH_R;
}

// Fraction (0-1) along p1->p2 where it crosses the gate in the direction of travel, or -1
static float crossing(const Gate &g, double la1, double lo1, double la2, double lo2)
{
  double x1, y1, x2, y2;
  localXY(g.lat, g.lon, la1, lo1, x1, y1);
  localXY(g.lat, g.lon, la2, lo2, x2, y2);
  double b = g.bearing * M_PI / 180.0;
  double fx = sin(b), fy = cos(b);                // forward direction
  double s1 = x1 * fx + y1 * fy, s2 = x2 * fx + y2 * fy;   // signed distance along the direction
  if (!(s1 < 0 && s2 >= 0)) return -1;             // must cross from behind to in front
  double k = s1 / (s1 - s2);
  double cx = x1 + (x2 - x1) * k, cy = y1 + (y2 - y1) * k;
  double lateral = cx * fy - cy * fx;              // distance along the line from its centre
  if (fabs(lateral) > g.width / 2) return -1;
  return (float)k;
}

static void lapCrossed(uint32_t tCross)
{
  if (ls.lap > 0 && lapStartMs) {
    uint32_t lapMs = tCross - lapStartMs;
    if (lapMs > 5000) {                            // ignore silly short laps (pit lane wobble)
      ls.lastMs = lapMs;
      if (!ls.bestMs || lapMs < ls.bestMs) {
        ls.bestMs = lapMs;
        memcpy(bestTrace, curTrace, curN * sizeof(Pt));
        bestN = curN; bestDist = lapDist;
      }
    }
  }
  ls.lap++;
  lapStartMs = tCross;
  lapDist = 0; curN = 0;
  ls.sector = 1;
}

static void onFix(double lat, double lon, float kmh, float course, uint32_t now)
{
  portENTER_CRITICAL(&mux);
  gs.fix = true; gs.lat = lat; gs.lon = lon; gs.kmh = kmh; gs.course = course; gs.fixMs = now;
  portEXIT_CRITICAL(&mux);
  setPulseSpeed(kmh);
  if (kmh > 8) lastCourse = course;

  if (!isnan(prevLat) && trk.n && bestTrace) {
    double dx, dy;
    localXY(prevLat, prevLon, lat, lon, dx, dy);
    float step = (float)sqrt(dx * dx + dy * dy);
    if (step < 200) {                              // ignore jumps
      // gates: start/finish first, then the next sector in order
      float k = crossing(trk.g[0], prevLat, prevLon, lat, lon);
      if (k >= 0) {
        lapCrossed(prevMs + (uint32_t)((now - prevMs) * k));
      } else if (ls.lap > 0 && ls.sector < trk.n) {
        float ks = crossing(trk.g[ls.sector], prevLat, prevLon, lat, lon);
        if (ks >= 0) ls.sector++;
      }
      if (ls.lap > 0) {
        lapDist += step;
        if (curN < BEST_MAX) curTrace[curN++] = { lapDist, now - lapStartMs };
      }
    }
  }
  prevLat = lat; prevLon = lon; prevMs = now;

  // live numbers
  ls.curMs = (ls.lap > 0 && lapStartMs) ? now - lapStartMs : 0;
  ls.deltaMs = INT32_MIN;
  ls.progress = 0;
  if (ls.lap > 0 && bestN > 1 && bestDist > 10) {
    uint32_t lo = 0, hi = bestN - 1;               // best-lap time at this distance
    while (lo < hi) { uint32_t m = (lo + hi) / 2; if (bestTrace[m].d < lapDist) lo = m + 1; else hi = m; }
    if (lo > 0 && bestTrace[lo].d > bestTrace[lo - 1].d) {
      const Pt &a = bestTrace[lo - 1], &b = bestTrace[lo];
      float f = (lapDist - a.d) / (b.d - a.d);
      int32_t tb = a.t + (int32_t)((int32_t)b.t - (int32_t)a.t) * f;
      ls.deltaMs = (int32_t)ls.curMs - tb;
    }
    ls.progress = (uint8_t)fminf(255.0f, lapDist / bestDist * 255.0f);
  }
  ls.haveTrack = trk.n > 0;
  ls.nGates = trk.n;
}

static void gpsTask(void *arg)
{
  static char line[128];
  int len = 0;
  uint32_t lastData = millis(), lastCfg = 0;
  configureModule(9600);
  for (;;) {
    while (gpsSer.available()) {
      char c = gpsSer.read();
      lastData = millis();
      if (c == '\n' || c == '\r') { if (len > 6) { line[len] = 0; parseLine(line, millis()); } len = 0; }
      else if (len < (int)sizeof(line) - 1) line[len++] = c;
      else len = 0;
    }
    uint32_t now = millis();
    // No sentences for a while: the module may have restarted at 9600 baud, set it up again
    if (now - lastData > 3000 && now - lastCfg > 5000) { lastCfg = now; configureModule(9600); }
    if (gs.fix && now - gs.fixMs > 1500) { portENTER_CRITICAL(&mux); gs.fix = false; portEXIT_CRITICAL(&mux); setPulseSpeed(0); }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void Gps_Init(void)
{
  loadCfg();
  memset(&gs, 0, sizeof(gs));
  memset(&ls, 0, sizeof(ls));
  ls.haveTrack = trk.n > 0; ls.nGates = trk.n;
  if (!gc.on) { Serial.println("GPS: off (turn it on in the app)"); return; }
  Serial.println("GPS: on. Debug serial output stops here (the GPS uses the UART header).");
  delay(50);
  Serial.end();
  bestTrace = (Pt *)heap_caps_malloc(BEST_MAX * sizeof(Pt), MALLOC_CAP_SPIRAM);
  curTrace  = (Pt *)heap_caps_malloc(BEST_MAX * sizeof(Pt), MALLOC_CAP_SPIRAM);
  gpsSer.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  if (gc.pulseOn) {
    pinMode(PULSE_PIN, OUTPUT);
    digitalWrite(PULSE_PIN, LOW);
    esp_timer_create_args_t a = {};
    a.callback = pulseCb;
    a.name = "spdpulse";
    esp_timer_create(&a, &pulseTimer);
  }
  xTaskCreatePinnedToCore(gpsTask, "gps", 4096, NULL, 3, NULL, 0);
}

bool Gps_Fresh(void) { return gc.on && gs.fix && millis() - gs.fixMs < 1000; }
bool Gps_Alive(void) { GpsState g; Gps_Get(g); return g.nmeaMs && millis() - g.nmeaMs < 3000; }
void Gps_Get(GpsState &g) { portENTER_CRITICAL(&mux); g = gs; portEXIT_CRITICAL(&mux); }
void Lap_Get(LapState &l)
{
  l = ls;
  if (ls.lap > 0 && lapStartMs && Gps_Fresh()) l.curMs = millis() - lapStartMs;   // smooth running clock
}

const char *Lap_MarkHere(void)
{
  if (!gc.on) return "GPS OFF";
  GpsState g; Gps_Get(g);
  if (!Gps_Fresh()) return "NO GPS FIX";
  if (isnan(lastCourse)) return "DRIVE TO SET";
  if (trk.n >= TRK_MAX_GATES) return "SECTORS FULL";
  Gate gate = { g.lat, g.lon, lastCourse, 25.0f };
  if (trk.n == 0) {
    trk.g[0] = gate; trk.n = 1;
    snprintf(trk.name, sizeof(trk.name), "Track");
    ls.lap = 0; ls.bestMs = ls.lastMs = 0; bestN = 0;
    saveTrk();
    ls.haveTrack = true; ls.nGates = 1;
    return "S/F SET";
  }
  trk.g[trk.n++] = gate;
  saveTrk();
  ls.nGates = trk.n;
  static char msg[16];
  snprintf(msg, sizeof(msg), "SECTOR %d SET", trk.n - 1);
  return msg;
}

// ---------------- App commands ----------------
void Gps_ConfigJson(String &s)
{
  GpsState g; Gps_Get(g);
  s = "{\"on\":"; s += gc.on; s += ",\"pulse\":"; s += gc.pulseOn; s += ",\"ppk\":"; s += gc.ppk;
  s += ",\"alive\":"; s += Gps_Alive() ? 1 : 0; s += ",\"fix\":"; s += Gps_Fresh() ? 1 : 0; s += ",\"sats\":"; s += g.sats;
  s += ",\"lat\":"; s += String(g.lat, 7); s += ",\"lon\":"; s += String(g.lon, 7); s += '}';
}

void Gps_ApplyConfig(SettingGetter get)
{
  String v;
  if (get("on", v)) gc.on = v.toInt() ? 1 : 0;
  if (get("pulse", v)) gc.pulseOn = v.toInt() ? 1 : 0;
  if (get("ppk", v)) gc.ppk = constrain(v.toInt(), 500, 20000);
  saveCfg();                                       // takes effect after a restart (the UART header changes role)
}

void Lap_TrackJson(String &s)
{
  s = "{\"name\":\""; s += trk.name; s += "\",\"g\":[";
  for (int i = 0; i < trk.n; i++) {
    if (i) s += ',';
    s += '['; s += String(trk.g[i].lat, 7); s += ','; s += String(trk.g[i].lon, 7); s += ',';
    s += String(trk.g[i].bearing, 1); s += ','; s += String(trk.g[i].width, 1); s += ']';
  }
  s += "],\"best\":"; s += ls.bestMs; s += '}';
}

bool Lap_SetTrack(const String &js)
{
  TrackStore t; memset(&t, 0, sizeof(t)); t.magic = TRK_MAGIC;
  int ni = js.indexOf("\"name\":\"");
  if (ni >= 0) { int e = js.indexOf('"', ni + 8); String nm = js.substring(ni + 8, e); strncpy(t.name, nm.c_str(), sizeof(t.name) - 1); }
  int gi = js.indexOf("\"g\":[");
  if (gi < 0) return false;
  const char *p = js.c_str() + gi + 5;
  while (*p && t.n < TRK_MAX_GATES) {
    while (*p && *p != '[' && *p != ']') p++;
    if (*p != '[') break;
    p++;
    double v[4]; int k = 0;
    char *end;
    while (k < 4) { v[k++] = strtod(p, &end); p = end; while (*p == ',' || *p == ' ') p++; }
    while (*p && *p != ']') p++;
    if (*p) p++;
    t.g[t.n++] = { v[0], v[1], (float)v[2], (float)v[3] };
  }
  trk = t;
  saveTrk();
  ls.lap = 0; ls.bestMs = ls.lastMs = 0; bestN = 0;
  ls.haveTrack = trk.n > 0; ls.nGates = trk.n;
  return true;
}

void Lap_ClearTrack(void) { memset(&trk, 0, sizeof(trk)); trk.magic = TRK_MAGIC; saveTrk(); ls = {}; bestN = 0; }
void Lap_ResetBest(void) { ls.bestMs = 0; ls.lastMs = 0; bestN = 0; }
