// Data logging to the TF card. See Data_Log.h for the file format.
// A low-priority task on core 0 samples the live data at the chosen rate, keeps a
// pre-trigger buffer in PSRAM, checks the start/stop triggers and writes rows to the card.

#include "Data_Log.h"
#include <FS.h>
#include <time.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include "TCA9554PWR.h"
#include "Haltech_CAN.h"
#include "Gauge_Calc.h"
#include "Gauge_UI.h"
#include "GPS_Lap.h"

#define SD_CLK_PIN   2
#define SD_CMD_PIN   1
#define SD_D0_PIN    42
#define LOG_DIR      "/logs"
#define LOG_MAGIC    0xB9D10001
#define HDR_SIZE     32
#define MAX_PRE_S    30
#define WBUF_SIZE    8192
#define NO_VALUE     (-32768)

static LogConfig lc;
static bool sdOk = false;
static SemaphoreHandle_t sdMux;

// Recording state (owned by the log task)
static volatile bool recording = false;
static volatile bool manualReq = false;        // toggle requested from the UI or app
static bool     manualSuppress = false;        // after a manual stop: no auto start until conditions clear
static File     logFile;
static char     curName[16] = "";
static uint32_t recStartMs = 0, recRows = 0, recPreMs = 0;
static uint8_t  recRate = 20, recNch = 0;
static uint32_t recMask = 0;
static uint32_t engOffSince = 0, stillSince = 0;
static uint8_t *wbuf = NULL;
static uint32_t wlen = 0, lastFlushMs = 0;

// Pre-trigger ring buffer (rows, same layout as the file)
static uint8_t *ring = NULL;
static uint32_t fileStartEpoch = 0;               // unix start time of the open log
static uint32_t ringRows = 0, ringHead = 0, ringCount = 0, ringRowSize = 0, ringMask = 0, ringRate = 0;

// Card space (refreshed by the log task)
static volatile uint64_t cardTotal = 0, cardUsed = 0;
static uint32_t lastSpaceMs = 0;

// Clock: the app sends the phone's time; we keep it as an offset from millis()
static volatile uint32_t epochAtBoot = 0;      // unix seconds at millis() == 0, 0 = unknown
static int16_t tzMin = 0;                      // local time offset (minutes), from the phone

// ---------------- Config ----------------
static void defaults(LogConfig &c)
{
  memset(&c, 0, sizeof(c));
  c.magic = LOG_MAGIC;
  c.mode = 2; c.rate = 20;
  c.chMask = ((1u << 14) - 1) | (1u << LOG_GPS_BIT);   // first 14 channels and GPS
  c.stRpmOn = 1; c.stRpm = 2500;
  c.stSpd = 10; c.stTpsOn = 1; c.stTps = 50; c.stBst = 35;
  c.spEngOn = 1; c.spEngS = 10; c.spStillS = 60;
  c.preS = 5; c.minS = 15; c.splitMin = 20; c.fullMode = 0;
}

static void sanitise(LogConfig &c)
{
  if (c.mode > 2) c.mode = 2;
  if (c.rate != 10 && c.rate != 20 && c.rate != 50) c.rate = 20;
  c.chMask &= (1u << (LOG_NUM_CH + 1)) - 1;     // int16 channels plus the GPS block
  if (!c.chMask) c.chMask = 1;
  if (c.preS > MAX_PRE_S) c.preS = MAX_PRE_S;
  if (c.splitMin > 240) c.splitMin = 240;
  if (c.fullMode > 1) c.fullMode = 0;
}

static void loadConfig(void)
{
  Preferences p;
  p.begin("bpdlog", true);
  bool ok = p.getBytesLength("cfg") == sizeof(LogConfig) && p.getBytes("cfg", &lc, sizeof(lc)) == sizeof(lc) && lc.magic == LOG_MAGIC;
  p.end();
  if (!ok) defaults(lc);
  sanitise(lc);
}

static void saveConfig(void)
{
  Preferences p;
  p.begin("bpdlog", false);
  p.putBytes("cfg", &lc, sizeof(lc));
  p.end();
}

// ---------------- Sampling ----------------
static int16_t enc(bool valid, float v, float scale)
{
  if (!valid) return NO_VALUE;
  float s = v * scale;
  if (s > 32767) s = 32767;
  if (s < -32767) s = -32767;
  return (int16_t)lroundf(s);
}

static int16_t sampleCh(int ch)
{
  bool bv;
  switch (ch) {
    case 0:  return enc(Calc_Fresh(hd.t360), hd.rpm, 1);
    case 1:  { float b = Calc_BoostKpa(&bv); return enc(bv, b, 10); }
    case 2:  return enc(Calc_Fresh(hd.t372), hd.target_boost_kpa, 10);
    case 3:  return enc(Calc_Fresh(hd.t368) && hd.lambda > 0.3f && hd.lambda < 3.0f, hd.lambda, 1000);
    case 4:  return enc(Calc_Fresh(hd.t3E9) && hd.target_lambda > 0.3f, hd.target_lambda, 1000);
    case 5:  return enc(Calc_Fresh(hd.t3E1), hd.ethanol_pct, 10);
    case 6:  return enc(Calc_Fresh(hd.t361), hd.oil_press_kpa, 10);
    case 7:  return enc(Calc_Fresh(hd.t3E0), hd.oil_temp_c, 10);
    case 8:  return enc(Calc_Fresh(hd.t3E0), hd.coolant_c, 10);
    case 9:  return enc(Calc_Fresh(hd.t360) && hd.coolant_press_kpa > -50, hd.coolant_press_kpa, 10);
    case 10: return enc(Calc_Fresh(hd.t361) && hd.fuel_press_kpa > -50, hd.fuel_press_kpa, 10);
    case 11: return enc(Calc_Fresh(hd.t362), hd.inj_duty_pct, 10);
    case 12: return enc(Calc_Fresh(hd.t3E0), hd.iat_c, 10);
    case 13: return enc(Calc_Fresh(hd.t372), hd.batt_v, 100);
    case 14: return enc(Calc_Fresh(hd.t360), hd.throttle_pct, 10);
    case 15: { float s = Calc_SpeedKmh(&bv); return enc(bv, s, 10); }
    case 16: { int g = Calc_Gear(&bv); return enc(bv, (float)g, 1); }
    case 17: return enc(Calc_Fresh(hd.t362), hd.ign_angle_deg, 10);
  }
  return NO_VALUE;
}

static uint32_t rowSize(uint32_t mask)
{
  uint32_t ch = mask & ((1u << LOG_NUM_CH) - 1);
  return 4 + 2 * __builtin_popcount(ch) + ((mask & (1u << LOG_GPS_BIT)) ? 12 : 0) + 2;
}

static void buildRow(uint8_t *row, uint32_t tMs, uint32_t mask)
{
  memcpy(row, &tMs, 4);
  int o = 4;
  for (int ch = 0; ch < LOG_NUM_CH; ch++) {
    if (!(mask & (1u << ch))) continue;
    int16_t v = sampleCh(ch);
    memcpy(row + o, &v, 2); o += 2;
  }
  if (mask & (1u << LOG_GPS_BIT)) {
    GpsState g; Gps_Get(g);
    LapState l; Lap_Get(l);
    bool ok = Gps_Fresh();
    int32_t la = ok ? (int32_t)llround(g.lat * 1e7) : INT32_MIN, lo = ok ? (int32_t)llround(g.lon * 1e7) : INT32_MIN;
    uint16_t sp = ok ? (uint16_t)lroundf(g.kmh * 10) : 0xFFFF, lap = l.lap;
    memcpy(row + o, &la, 4); memcpy(row + o + 4, &lo, 4); memcpy(row + o + 8, &sp, 2); memcpy(row + o + 10, &lap, 2);
    o += 12;
  }
  uint16_t w = Gauge_WarnMask();
  memcpy(row + o, &w, 2);
}

static void ringReset(void)
{
  ringRowSize = rowSize(lc.chMask);
  ringMask = lc.chMask;
  ringRate = lc.rate;
  ringRows = (uint32_t)lc.preS * lc.rate;
  ringHead = ringCount = 0;
  if (ring) { heap_caps_free(ring); ring = NULL; }
  if (ringRows) ring = (uint8_t *)heap_caps_malloc(ringRows * ringRowSize, MALLOC_CAP_SPIRAM);
  if (!ring) ringRows = 0;
}

// ---------------- Card helpers (call with sdMux held) ----------------
static void refreshSpace(void)
{
  cardTotal = SD_MMC.totalBytes();
  cardUsed  = SD_MMC.usedBytes();
}

static uint32_t nextNumber(void)
{
  Preferences p;
  p.begin("bpdlog", false);
  uint32_t n = p.getUInt("next", 1);
  p.putUInt("next", n + 1);
  p.end();
  return n;
}

static bool oldestLog(String &name)                  // lowest-numbered log that isn't open
{
  File dir = SD_MMC.open(LOG_DIR);
  if (!dir) return false;
  uint32_t best = UINT32_MAX;
  File f;
  while ((f = dir.openNextFile())) {
    String n = f.name();
    f.close();
    int s = n.lastIndexOf('/');
    if (s >= 0) n = n.substring(s + 1);
    if (!n.endsWith(".BPL") || n == curName) continue;
    int lp = n.substring(0, n.length() - 4).lastIndexOf('L');      // L00012 part, with or without a date in front
    if (lp < 0) continue;
    uint32_t num = n.substring(lp + 1, n.length() - 4).toInt();
    if (num < best) { best = num; name = n; }
  }
  dir.close();
  return best != UINT32_MAX;
}

static bool makeRoom(void)                           // true if there's room to keep logging
{
  const uint64_t need = 2ULL * 1024 * 1024;
  if (cardTotal == 0 || cardTotal - cardUsed > need) return true;
  if (lc.fullMode != 1) return false;
  String old;
  for (int i = 0; i < 20 && cardTotal - cardUsed <= 4 * need && oldestLog(old); i++) {
    SD_MMC.remove(String(LOG_DIR "/") + old);
    Serial.printf("Log: card full, deleted %s\n", old.c_str());
    refreshSpace();
  }
  return cardTotal - cardUsed > need;
}

static void flushBuf(bool sync)
{
  if (!logFile) { wlen = 0; return; }
  xSemaphoreTake(sdMux, portMAX_DELAY);
  if (wlen) logFile.write(wbuf, wlen);
  wlen = 0;
  if (sync) logFile.flush();
  xSemaphoreGive(sdMux);
}

static void writeRow(const uint8_t *row, uint32_t n)
{
  if (wlen + n > WBUF_SIZE) flushBuf(false);
  memcpy(wbuf + wlen, row, n);
  wlen += n;
}

// ---------------- Start / stop ----------------
static bool startFile(uint32_t now, bool withPre)
{
  if (!sdOk) return false;
  xSemaphoreTake(sdMux, portMAX_DELAY);
  refreshSpace();
  bool room = makeRoom();
  if (room) {
    SD_MMC.mkdir(LOG_DIR);
    snprintf(curName, sizeof(curName), "L%05lu.BPL", (unsigned long)nextNumber());
    logFile = SD_MMC.open(String(LOG_DIR "/") + curName, FILE_WRITE);
  }
  xSemaphoreGive(sdMux);
  if (!room) { Serial.println("Log: card full, not logging"); return false; }
  if (!logFile) { Serial.println("Log: could not create the file"); curName[0] = 0; return false; }

  recRate = lc.rate; recMask = lc.chMask; recNch = __builtin_popcount(recMask & ((1u << LOG_NUM_CH) - 1));
  uint32_t preRows = (withPre && ringMask == recMask && ringRate == recRate) ? ringCount : 0;
  recPreMs = preRows * 1000 / recRate;

  uint8_t h[HDR_SIZE] = {0};
  memcpy(h, "BPDL", 4);
  h[4] = 2; h[5] = recRate; h[6] = recNch; h[7] = (recMask & (1u << LOG_GPS_BIT)) ? 1 : 0;
  uint32_t t0 = epochAtBoot ? epochAtBoot + (now - recPreMs) / 1000 : 0;
  fileStartEpoch = t0;
  memcpy(h + 8, &t0, 4);
  memcpy(h + 12, &recMask, 4);
  memcpy(h + 16, &recPreMs, 4);
  h[20] = Sim_Active() ? 1 : 0;                // flags: bit 0 = recorded from the bench simulator (fake data)
  wlen = 0;
  writeRow(h, HDR_SIZE);

  // Pre-trigger rows first, re-timed to start at 0
  uint32_t rs = rowSize(recMask);
  for (uint32_t i = 0; i < preRows; i++) {
    uint8_t *r = ring + ((ringHead + ringRows - ringCount + i) % ringRows) * rs;
    uint32_t t = i * 1000 / recRate;
    memcpy(r, &t, 4);
    writeRow(r, rs);
  }
  ringCount = 0;
  recStartMs = now - recPreMs;
  recRows = preRows;
  recording = true;
  engOffSince = stillSince = 0;
  lastFlushMs = now;
  Serial.printf("Log: recording %s\n", curName);
  return true;
}

static void stopFile(bool keepShort)
{
  if (!recording) return;
  recording = false;
  flushBuf(true);
  uint32_t secs = recRows / (recRate ? recRate : 1);
  String path = String(LOG_DIR "/") + curName;
  xSemaphoreTake(sdMux, portMAX_DELAY);
  logFile.close();
  if (!keepShort && secs < lc.minS) { SD_MMC.remove(path); Serial.printf("Log: %s too short, discarded\n", curName); }
  else {
    uint32_t t0 = fileStartEpoch ? fileStartEpoch : (epochAtBoot ? epochAtBoot + recStartMs / 1000 : 0);
    if (t0) {                                  // finished: put the local date and time in front of the name
      time_t lt = (time_t)t0 + tzMin * 60;
      struct tm tmv; gmtime_r(&lt, &tmv);
      char nn[40];
      snprintf(nn, sizeof(nn), "%04d-%02d-%02d_%02d%02d_%s", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, curName);
      if (SD_MMC.rename(path, String(LOG_DIR "/") + nn)) path = String(LOG_DIR "/") + nn;
    }
    Serial.printf("Log: saved %s (%lu s)\n", path.c_str(), (unsigned long)secs);
  }
  refreshSpace();
  xSemaphoreGive(sdMux);
  curName[0] = 0;
}

// ---------------- Triggers ----------------
static bool startCondition(void)
{
  bool bv, sv;
  float boost = Calc_BoostKpa(&bv);
  float spd = Calc_SpeedKmh(&sv);
  bool rpmOk = Calc_Fresh(hd.t360);
  if (lc.stRpmOn && rpmOk && hd.rpm > lc.stRpm) return true;
  if (lc.stSpdOn && sv && spd > lc.stSpd) return true;
  if (lc.stTpsOn && rpmOk && hd.throttle_pct > lc.stTps) return true;
  if (lc.stBstOn && bv && boost > lc.stBst) return true;
  return false;
}

static bool stopCondition(uint32_t now)
{
  bool sv;
  float spd = Calc_SpeedKmh(&sv);
  bool engOff = !Calc_Fresh(hd.t360) || hd.rpm < 300;
  bool still = !sv || spd < 2.0f;
  engOffSince = engOff ? (engOffSince ? engOffSince : now) : 0;
  stillSince  = still  ? (stillSince  ? stillSince  : now) : 0;
  if (lc.spEngOn && engOffSince && now - engOffSince >= lc.spEngS * 1000UL) return true;
  if (lc.spStillOn && stillSince && now - stillSince >= lc.spStillS * 1000UL) return true;
  return false;
}

static void step(uint32_t now)
{
  bool autoOn = lc.mode != 0;

  if (manualReq) {
    manualReq = false;
    if (recording) { stopFile(true); manualSuppress = true; }   // manual stop always allowed
    else if (lc.mode != 1) startFile(now, true);                // manual start: manual or both modes
  }

  if (!recording) {
    if (ringMask != lc.chMask || ringRate != lc.rate || ringRows != (uint32_t)lc.preS * lc.rate) ringReset();
    if (ring && ringRows) {                                // keep the last few seconds
      uint8_t *r = ring + ringHead * ringRowSize;
      buildRow(r, 0, ringMask);
      ringHead = (ringHead + 1) % ringRows;
      if (ringCount < ringRows) ringCount++;
    }
    bool cond = startCondition();
    if (!cond) manualSuppress = false;
    if (autoOn && cond && !manualSuppress && sdOk) startFile(now, true);
    return;
  }

  // Recording
  uint8_t row[4 + 2 * LOG_NUM_CH + 12 + 2];
  uint32_t t = recRows * 1000 / recRate;
  buildRow(row, t, recMask);
  writeRow(row, rowSize(recMask));
  recRows++;

  if (now - lastFlushMs >= 2000) { flushBuf(true); lastFlushMs = now; }   // little lost if power drops

  if (now - lastSpaceMs >= 30000) {
    lastSpaceMs = now;
    xSemaphoreTake(sdMux, portMAX_DELAY);
    refreshSpace();
    bool room = makeRoom();
    xSemaphoreGive(sdMux);
    if (!room) { Serial.println("Log: card full, stopped"); stopFile(true); return; }
  }

  if (autoOn && stopCondition(now)) { stopFile(false); return; }

  if (lc.splitMin && recRows >= (uint32_t)lc.splitMin * 60 * recRate) {   // long session: carry on in a new file
    stopFile(true);
    startFile(now, false);
  }
}

static void logTask(void *arg)
{
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    uint8_t rate = recording ? recRate : lc.rate;
    vTaskDelayUntil(&last, pdMS_TO_TICKS(1000 / rate));
    step(millis());
  }
}

// ---------------- Public ----------------
void Log_Init(void)
{
  sdMux = xSemaphoreCreateMutex();
  loadConfig();
  { Preferences p; p.begin("bpdlog", true); tzMin = p.getShort("tz", 0); p.end(); }
  wbuf = (uint8_t *)heap_caps_malloc(WBUF_SIZE, MALLOC_CAP_SPIRAM);

  Set_EXIO(EXIO_PIN4, High);                   // SD D3 high = SD mode
  delay(10);
  if (SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN) && SD_MMC.begin("/sdcard", true) && SD_MMC.cardType() != CARD_NONE) {
    sdOk = true;
    SD_MMC.mkdir(LOG_DIR);
    refreshSpace();
    Serial.printf("SD card: %llu MB, %llu MB used\n", cardTotal >> 20, cardUsed >> 20);
  } else {
    Serial.println("SD card: none (logging off)");
  }
  ringReset();
  xTaskCreatePinnedToCore(logTask, "log", 6144, NULL, 2, NULL, 0);
}

bool Log_SdOk(void) { return sdOk; }
bool Log_Recording(void) { return recording; }
uint32_t Log_RecordingSecs(void) { return recording ? (millis() - recStartMs) / 1000 : 0; }
void Log_ManualToggle(void) { if (sdOk) manualReq = true; }
void Log_SetTime(uint32_t unixSecs, int tz)
{
  if (unixSecs > 1600000000UL) epochAtBoot = unixSecs - millis() / 1000;
  if (tz != LOG_TZ_KEEP && tz > -900 && tz < 900 && tz != tzMin) {
    tzMin = tz;
    Preferences p; p.begin("bpdlog", false); p.putShort("tz", tzMin); p.end();
  }
}

static void jI(String &s, const char *k, long v) { s += '"'; s += k; s += "\":"; s += v; s += ','; }
static void jF(String &s, const char *k, float v) { s += '"'; s += k; s += "\":"; s += String(v, 1); s += ','; }

void Log_StatusJson(String &s)
{
  s = "{\"cfg\":{";
  jI(s, "mode", lc.mode); jI(s, "rate", lc.rate); jI(s, "ch", lc.chMask);
  jI(s, "st_rpm_on", lc.stRpmOn); jI(s, "st_rpm", lc.stRpm);
  jI(s, "st_spd_on", lc.stSpdOn); jF(s, "st_spd", lc.stSpd);
  jI(s, "st_tps_on", lc.stTpsOn); jF(s, "st_tps", lc.stTps);
  jI(s, "st_bst_on", lc.stBstOn); jF(s, "st_bst", lc.stBst);
  jI(s, "sp_eng_on", lc.spEngOn); jI(s, "sp_eng", lc.spEngS);
  jI(s, "sp_still_on", lc.spStillOn); jI(s, "sp_still", lc.spStillS);
  jI(s, "pre", lc.preS); jI(s, "min", lc.minS); jI(s, "split", lc.splitMin);
  s += "\"full\":"; s += lc.fullMode;
  s += "},\"st\":{";
  jI(s, "sd", sdOk); jI(s, "rec", recording); jI(s, "secs", Log_RecordingSecs());
  s += "\"file\":\""; s += recording ? curName : ""; s += "\",";
  s += "\"total\":"; s += String((double)cardTotal, 0); s += ",\"used\":"; s += String((double)cardUsed, 0);
  s += "}}";
}

void Log_ApplyConfig(SettingGetter get)
{
  String v;
  LogConfig n = lc;
  auto gi = [&](const char *k, long d) -> long { return get(k, v) ? v.toInt() : d; };
  auto gf = [&](const char *k, float d) -> float { return get(k, v) ? v.toFloat() : d; };
  n.mode = gi("mode", n.mode); n.rate = gi("rate", n.rate); n.chMask = gi("ch", n.chMask);
  n.stRpmOn = gi("st_rpm_on", n.stRpmOn) ? 1 : 0; n.stRpm = gi("st_rpm", n.stRpm);
  n.stSpdOn = gi("st_spd_on", n.stSpdOn) ? 1 : 0; n.stSpd = gf("st_spd", n.stSpd);
  n.stTpsOn = gi("st_tps_on", n.stTpsOn) ? 1 : 0; n.stTps = gf("st_tps", n.stTps);
  n.stBstOn = gi("st_bst_on", n.stBstOn) ? 1 : 0; n.stBst = gf("st_bst", n.stBst);
  n.spEngOn = gi("sp_eng_on", n.spEngOn) ? 1 : 0; n.spEngS = gi("sp_eng", n.spEngS);
  n.spStillOn = gi("sp_still_on", n.spStillOn) ? 1 : 0; n.spStillS = gi("sp_still", n.spStillS);
  n.preS = gi("pre", n.preS); n.minS = gi("min", n.minS); n.splitMin = gi("split", n.splitMin);
  n.fullMode = gi("full", n.fullMode);
  sanitise(n);
  lc = n;                                      // picked up by the log task on its next step
  saveConfig();
}

void Log_ListJson(String &s)
{
  s = "[";
  if (!sdOk) { s += "]"; return; }
  xSemaphoreTake(sdMux, portMAX_DELAY);
  File dir = SD_MMC.open(LOG_DIR);
  bool first = true;
  if (dir) {
    File f;
    while ((f = dir.openNextFile())) {
      String n = f.name();
      int sl = n.lastIndexOf('/');
      if (sl >= 0) n = n.substring(sl + 1);
      if (!f.isDirectory() && n.endsWith(".BPL")) {
        uint8_t h[HDR_SIZE] = {0};
        f.read(h, HDR_SIZE);
        uint32_t t0, mask; memcpy(&t0, h + 8, 4); memcpy(&mask, h + 12, 4);
        if (!first) s += ',';
        first = false;
        s += "{\"n\":\""; s += n; s += "\",\"size\":"; s += (unsigned long)f.size();
        s += ",\"t0\":"; s += (unsigned long)t0; s += ",\"rate\":"; s += h[5];
        s += ",\"ch\":"; s += (unsigned long)mask; s += ",\"open\":"; s += (n == curName) ? 1 : 0;
        s += ",\"sim\":"; s += (h[20] & 1) ? 1 : 0; s += '}';
      }
      f.close();
    }
    dir.close();
  }
  xSemaphoreGive(sdMux);
  s += "]";
}

static bool safeName(const String &n) { return n.length() > 4 && n.length() < 40 && n.endsWith(".BPL") && n.indexOf('/') < 0 && n.indexOf("..") < 0; }

bool Log_Delete(const String &name)
{
  if (!sdOk || !safeName(name) || name == curName) return false;
  xSemaphoreTake(sdMux, portMAX_DELAY);
  bool ok = SD_MMC.remove(String(LOG_DIR "/") + name);
  refreshSpace();
  xSemaphoreGive(sdMux);
  return ok;
}

int Log_DeleteAll(void)
{
  if (!sdOk) return 0;
  int n = 0;
  String name;
  xSemaphoreTake(sdMux, portMAX_DELAY);
  while (oldestLog(name) && n < 2000) { if (!SD_MMC.remove(String(LOG_DIR "/") + name)) break; n++; }
  refreshSpace();
  xSemaphoreGive(sdMux);
  return n;
}

int32_t Log_Read(const String &name, uint32_t offset, uint8_t *buf, uint32_t len)
{
  if (!sdOk || !safeName(name)) return -1;
  // The log being recorded can be read too; it's written out to the card every 2 s.
  xSemaphoreTake(sdMux, portMAX_DELAY);
  File f = SD_MMC.open(String(LOG_DIR "/") + name, FILE_READ);
  int32_t n = -1;
  if (f) {
    if (offset >= f.size()) n = 0;
    else { f.seek(offset); n = f.read(buf, len); }
    f.close();
  }
  xSemaphoreGive(sdMux);
  return n;
}

// ---------------- Car profiles ----------------
#define PROF_DIR "/profiles"
static bool profId(const String &id)
{
  if (id.length() < 1 || id.length() > 24) return false;
  for (size_t i = 0; i < id.length(); i++) { char c = id[i]; if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) return false; }
  return true;
}

void Prof_ListJson(String &out)
{
  out = "[";
  if (sdOk) {
    xSemaphoreTake(sdMux, portMAX_DELAY);
    File d = SD_MMC.open(PROF_DIR);
    bool first = true;
    if (d) {
      for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        String n = f.name();
        int sl = n.lastIndexOf('/'); if (sl >= 0) n = n.substring(sl + 1);
        if (n.endsWith(".json")) { if (!first) out += ","; first = false; out += "\"" + n.substring(0, n.length() - 5) + "\""; }
        f.close();
      }
      d.close();
    }
    xSemaphoreGive(sdMux);
  }
  out += "]";
}

bool Prof_Write(const String &id, uint32_t offset, const uint8_t *dta, uint32_t len, bool last)
{
  if (!sdOk || !profId(id) || len > 4096) return false;
  String base = String(PROF_DIR "/") + id, tmp = base + ".tmp";
  bool ok = false;
  xSemaphoreTake(sdMux, portMAX_DELAY);
  if (!SD_MMC.exists(PROF_DIR)) SD_MMC.mkdir(PROF_DIR);
  File f = SD_MMC.open(tmp, offset == 0 ? FILE_WRITE : FILE_APPEND);
  if (f) {
    if (offset != 0 && (uint32_t)f.size() != offset) { f.close(); }          // chunk out of order: refuse
    else {
      ok = f.write(dta, len) == len;
      f.close();
      if (ok && last) {
        String json = base + ".json", bak = base + ".bak";
        if (SD_MMC.exists(bak)) SD_MMC.remove(bak);
        if (SD_MMC.exists(json)) SD_MMC.rename(json, bak);
        ok = SD_MMC.rename(tmp, json);
      }
    }
  }
  xSemaphoreGive(sdMux);
  return ok;
}

int32_t Prof_Read(const String &id, uint32_t offset, uint8_t *buf, uint32_t len)
{
  if (!sdOk || !profId(id)) return -1;
  xSemaphoreTake(sdMux, portMAX_DELAY);
  File f = SD_MMC.open(String(PROF_DIR "/") + id + ".json", FILE_READ);
  int32_t n = -1;
  if (f) {
    if (offset >= f.size()) n = 0; else { f.seek(offset); n = f.read(buf, len); }
    f.close();
  }
  xSemaphoreGive(sdMux);
  return n;
}

bool Prof_Delete(const String &id)
{
  if (!sdOk || !profId(id)) return false;
  xSemaphoreTake(sdMux, portMAX_DELAY);
  bool ok = SD_MMC.remove(String(PROF_DIR "/") + id + ".json");
  xSemaphoreGive(sdMux);
  return ok;
}

