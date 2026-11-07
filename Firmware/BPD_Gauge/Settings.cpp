#include "Settings.h"
#include <Preferences.h>

#define SETTINGS_MAGIC     0xB9D00005
#define SETTINGS_MAGIC_V4  0xB9D00004     // 11 pages
#define SETTINGS_MAGIC_V3  0xB9D00003     // 10 pages
#define SETTINGS_MAGIC_V2  0xB9D00002     // previous layout (5 pages, no extra warnings)

// Exact copy of the previous settings layout, so saved settings carry over after the update
struct GaugeSettingsV2 {
  uint32_t magic;
  uint8_t numGears;
  float   gear[6];
  float   finalDrive;
  Tyre    rear, front, ref;
  uint8_t speedSource, gearSource;
  uint8_t pressUnit, tempUnit, speedUnit, afrAsLambda, flexStoich;
  float   stoich;
  uint8_t pageOrder[5];
  uint8_t pageOn[5];
  Warn     coolantHigh;
  Warn     oilTempHigh;
  Warn     oilPressLow;
  uint16_t oilPressMinRpm;
  Warn     boostHigh;
  Warn     leanBoost;
  float    leanMinBoostKpa;
  Warn     shiftLight;
  uint8_t  brightness;
  uint16_t tachMax;
};

// Previous layouts that only differ in the number of pages (10 before the lap timer page,
// 11 before the face page), so saved settings carry over after adding a page.
template <int N> struct GaugeSettingsPages {
  uint32_t magic;
  uint8_t numGears;
  float   gear[6];
  float   finalDrive;
  Tyre    rear, front, ref;
  uint8_t speedSource, gearSource;
  uint8_t pressUnit, tempUnit, speedUnit, afrAsLambda, flexStoich;
  float   stoich;
  uint8_t pageOrder[N];
  uint8_t pageOn[N];
  Warn     coolantHigh;
  Warn     oilTempHigh;
  Warn     oilPressLow;
  uint16_t oilPressMinRpm;
  Warn     boostHigh;
  Warn     leanBoost;
  float    leanMinBoostKpa;
  Warn     shiftLight;
  uint8_t  brightness;
  uint16_t tachMax;
  Warn     coolPressHigh;
  Warn     fuelDiffLow;
  Warn     iatHigh;
  Warn     battLow;
  Warn     dutyHigh;
  uint16_t alertCycleMs;
};

template <int N> static void migratePages(const GaugeSettingsPages<N> &o, GaugeSettings &n)
{
  Settings_MakeDefaults(n);                     // new pages get their default on/off
  n.numGears = o.numGears; memcpy(n.gear, o.gear, sizeof(n.gear)); n.finalDrive = o.finalDrive;
  n.rear = o.rear; n.front = o.front; n.ref = o.ref;
  n.speedSource = o.speedSource; n.gearSource = o.gearSource;
  n.pressUnit = o.pressUnit; n.tempUnit = o.tempUnit; n.speedUnit = o.speedUnit;
  n.afrAsLambda = o.afrAsLambda; n.flexStoich = o.flexStoich; n.stoich = o.stoich;
  bool seen[NUM_GAUGE_PAGES] = {false};
  int k = 0;
  for (int i = 0; i < N; i++) if (o.pageOrder[i] < N && !seen[o.pageOrder[i]]) { seen[o.pageOrder[i]] = true; n.pageOrder[k++] = o.pageOrder[i]; }
  for (int p = 0; p < NUM_GAUGE_PAGES; p++) if (!seen[p]) n.pageOrder[k++] = p;   // new pages go at the end
  for (int i = 0; i < N; i++) n.pageOn[i] = o.pageOn[i] ? 1 : 0;
  n.coolantHigh = o.coolantHigh; n.oilTempHigh = o.oilTempHigh; n.oilPressLow = o.oilPressLow;
  n.oilPressMinRpm = o.oilPressMinRpm; n.boostHigh = o.boostHigh; n.leanBoost = o.leanBoost;
  n.leanMinBoostKpa = o.leanMinBoostKpa; n.shiftLight = o.shiftLight;
  n.brightness = o.brightness; n.tachMax = o.tachMax;
  n.coolPressHigh = o.coolPressHigh; n.fuelDiffLow = o.fuelDiffLow; n.iatHigh = o.iatHigh;
  n.battLow = o.battLow; n.dutyHigh = o.dutyHigh; n.alertCycleMs = o.alertCycleMs;
}

template <int N> static bool tryLoadPages(Preferences &p, size_t n, uint32_t magic)
{
  if (n != sizeof(GaugeSettingsPages<N>)) return false;
  GaugeSettingsPages<N> *old = (GaugeSettingsPages<N> *)malloc(sizeof(GaugeSettingsPages<N>));
  if (!old) return false;
  p.getBytes("cfg", old, sizeof(GaugeSettingsPages<N>));
  bool ok = old->magic == magic;
  if (ok) migratePages<N>(*old, cfg);
  free(old);
  return ok;
}

static void migrateV2(const GaugeSettingsV2 &o, GaugeSettings &n)
{
  Settings_MakeDefaults(n);                 // new fields get their defaults
  n.numGears = o.numGears;
  memcpy(n.gear, o.gear, sizeof(n.gear));
  n.finalDrive = o.finalDrive;
  n.rear = o.rear; n.front = o.front; n.ref = o.ref;
  n.speedSource = o.speedSource; n.gearSource = o.gearSource;
  n.pressUnit = o.pressUnit; n.tempUnit = o.tempUnit; n.speedUnit = o.speedUnit;
  n.afrAsLambda = o.afrAsLambda; n.flexStoich = o.flexStoich; n.stoich = o.stoich;
  bool seen[NUM_GAUGE_PAGES] = {false};
  int k = 0;
  for (int i = 0; i < 5; i++) if (o.pageOrder[i] < 5 && !seen[o.pageOrder[i]]) { seen[o.pageOrder[i]] = true; n.pageOrder[k++] = o.pageOrder[i]; }
  for (int p = 0; p < NUM_GAUGE_PAGES; p++) if (!seen[p]) n.pageOrder[k++] = p;     // new pages go after the old ones
  for (int i = 0; i < 5; i++) n.pageOn[i] = o.pageOn[i] ? 1 : 0;
  n.coolantHigh = o.coolantHigh; n.oilTempHigh = o.oilTempHigh; n.oilPressLow = o.oilPressLow;
  n.oilPressMinRpm = o.oilPressMinRpm; n.boostHigh = o.boostHigh; n.leanBoost = o.leanBoost;
  n.leanMinBoostKpa = o.leanMinBoostKpa; n.shiftLight = o.shiftLight;
  n.brightness = o.brightness; n.tachMax = o.tachMax;
}

GaugeSettings cfg;
UiPrefs ui = { 0xB9D40001, 1, 1, FACE_CLASSIC, SKIN_BPDFC, 0xFFB000 };
#define UI_MAGIC 0xB9D40001

void Settings_MakeDefaults(GaugeSettings &cfg)
{
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = SETTINGS_MAGIC;

  // Placeholder gearing: stock R154 ratios. Enter your own on the settings page.
  cfg.numGears = 5;
  const float g[6] = { 3.251f, 1.955f, 1.310f, 1.000f, 0.753f, 0.0f };
  memcpy(cfg.gear, g, sizeof(g));
  cfg.finalDrive = 4.100f;
  cfg.rear  = { 265, 35, 18 };
  cfg.front = { 235, 40, 18 };
  cfg.ref   = { 265, 35, 18 };
  cfg.speedSource = SPD_ECU;
  cfg.gearSource  = GEAR_CALC;

  cfg.pressUnit   = PU_PSI;
  cfg.tempUnit    = TU_C;
  cfg.speedUnit   = SU_KMH;
  cfg.afrAsLambda = 1;
  cfg.flexStoich  = 1;
  cfg.stoich      = 14.7f;

  for (int i = 0; i < NUM_GAUGE_PAGES; i++) { cfg.pageOrder[i] = i; cfg.pageOn[i] = 1; }
  cfg.pageOn[PG_LAP] = 0;                       // turn on once the GPS is fitted

  cfg.coolantHigh     = { 1, 105.0f };
  cfg.oilTempHigh     = { 1, 130.0f };
  cfg.oilPressLow     = { 1, 100.0f };
  cfg.oilPressMinRpm  = 2000;
  cfg.boostHigh       = { 1, 220.0f };   // about 32 psi
  cfg.leanBoost       = { 1, 0.85f };
  cfg.leanMinBoostKpa = 50.0f;
  cfg.shiftLight      = { 1, 7000.0f };

  cfg.brightness = 80;
  cfg.tachMax    = 8000;

  cfg.coolPressHigh = { 1, 140.0f };
  cfg.fuelDiffLow   = { 1, 250.0f };
  cfg.iatHigh       = { 1, 60.0f };
  cfg.battLow       = { 1, 12.0f };
  cfg.dutyHigh      = { 1, 85.0f };
  cfg.alertCycleMs  = 2000;
}

// Basic view: same screens, conservative choices. Haltech sends every channel even when the sensor
// isn't fitted (zeros), so pages that depend on optional sensors stay off until the app sets them up.
void Settings_BasicPreset(GaugeSettings &d)
{
  for (int i = 0; i < NUM_GAUGE_PAGES; i++) d.pageOn[i] = 0;
  d.pageOn[PG_BOOST] = d.pageOn[PG_TACH] = d.pageOn[PG_COOLANT] = d.pageOn[PG_IAT] = 1;
  d.pageOn[PG_ALERTS] = 1;
  d.oilTempHigh.on = d.oilPressLow.on = d.boostHigh.on = d.leanBoost.on = 0;
  d.coolPressHigh.on = d.fuelDiffLow.on = d.iatHigh.on = d.battLow.on = d.dutyHigh.on = 0;
  d.shiftLight.on = 0;
  d.coolantHigh.on = 1;
}

static bool hadSaved = false;
bool Settings_HadSaved(void) { return hadSaved; }

void Settings_Defaults(void) { Settings_MakeDefaults(cfg); }

void Settings_Load(void)
{
  Preferences p;
  p.begin("bpd", true);
  { UiPrefs u; if (p.getBytes("ui", &u, sizeof(u)) == sizeof(u) && u.magic == UI_MAGIC) ui = u; }
  ui.faceStyle = FACE_CLASSIC;                                           // retired: the avatar belongs to the skin
  ui.gaugeSkin = SKIN_BPDFC;                                             // basic build: BPD FC only (BPD Embers removed)
  size_t n = p.getBytesLength("cfg");
  bool ok = false, migrated = false;
  if (n == sizeof(GaugeSettings)) {
    p.getBytes("cfg", &cfg, sizeof(cfg));
    ok = (cfg.magic == SETTINGS_MAGIC);
  } else if (tryLoadPages<11>(p, n, SETTINGS_MAGIC_V4) || tryLoadPages<10>(p, n, SETTINGS_MAGIC_V3)) {
    ok = migrated = true;
  } else if (n == sizeof(GaugeSettingsV2)) {
    GaugeSettingsV2 *old = (GaugeSettingsV2 *)malloc(sizeof(GaugeSettingsV2));
    if (old) {
      p.getBytes("cfg", old, sizeof(GaugeSettingsV2));
      if (old->magic == SETTINGS_MAGIC_V2) { migrateV2(*old, cfg); ok = migrated = true; }
      free(old);
    }
  }
  p.end();
  if (migrated) { Serial.println("Settings: carried over from the previous version"); Settings_Save(); }
  if (!ok) {
    Serial.println("Settings: using defaults");
    Settings_Defaults();
    Settings_BasicPreset(cfg);
  }
  hadSaved = ok;
  // Basic sanity limits
  cfg.afrAsLambda = 1;                           // lambda only: the AFR / stoich option was removed to keep things simple
  if (cfg.numGears < 4 || cfg.numGears > 6) cfg.numGears = 5;
  if (cfg.brightness < 10 || cfg.brightness > 100) cfg.brightness = 80;
  if (cfg.tachMax < 4000 || cfg.tachMax > 12000) cfg.tachMax = 8000;
  if (cfg.alertCycleMs < 1000 || cfg.alertCycleMs > 5000) cfg.alertCycleMs = 2000;
}

void Settings_Save(void)
{
  cfg.magic = SETTINGS_MAGIC;
  Preferences p;
  p.begin("bpd", false);
  p.putBytes("cfg", &cfg, sizeof(cfg));
  p.putBytes("ui", &ui, sizeof(ui));
  p.end();
  Serial.println("Settings saved");
}

float Tyre_CircM(const Tyre &t)
{
  float diaMm = t.rim * 25.4f + 2.0f * t.width * t.aspect / 100.0f;
  return PI * diaMm / 1000.0f;
}

const char *Page_Name(uint8_t page)
{
  switch (page) {
    case PG_BOOST:   return "Boost";
    case PG_OIL:     return "Oil pressure";
    case PG_AFR:     return "AFR";
    case PG_TACH:    return "RPM and speed";
    case PG_COOLANT: return "Coolant";
    case PG_FUEL:    return "Fuel pressure";
    case PG_IAT:     return "Intake air temp";
    case PG_MONITOR: return "Monitor";
    case PG_KEYPAD:  return "CAN keypad";
    case PG_ALERTS:  return "Alerts";
    case PG_LAP:     return "Lap timer";
    case PG_FACE:    return "Face";
  }
  return "?";
}

// ---------------- JSON out ----------------
static void jNum(String &s, const char *k, float v, int dp = 3) { s += '"'; s += k; s += "\":"; s += String(v, dp); s += ','; }
static void jInt(String &s, const char *k, long v)              { s += '"'; s += k; s += "\":"; s += v; s += ','; }

void Settings_ToJson(String &s)
{
  s = "{";
  jInt(s, "numGears", cfg.numGears);
  for (int i = 0; i < 6; i++) { char k[4]; snprintf(k, sizeof(k), "g%d", i + 1); jNum(s, k, cfg.gear[i]); }
  jNum(s, "fd", cfg.finalDrive);
  jInt(s, "rw", cfg.rear.width);  jInt(s, "ra", cfg.rear.aspect);  jInt(s, "rr", cfg.rear.rim);
  jInt(s, "fw", cfg.front.width); jInt(s, "fa", cfg.front.aspect); jInt(s, "fr", cfg.front.rim);
  jInt(s, "cw", cfg.ref.width);   jInt(s, "ca", cfg.ref.aspect);   jInt(s, "cr", cfg.ref.rim);
  jInt(s, "spdSrc", cfg.speedSource); jInt(s, "gearSrc", cfg.gearSource);
  jInt(s, "pu", cfg.pressUnit); jInt(s, "tu", cfg.tempUnit); jInt(s, "su", cfg.speedUnit);
  jInt(s, "lam", cfg.afrAsLambda); jInt(s, "flex", cfg.flexStoich); jNum(s, "stoich", cfg.stoich, 2);
  s += "\"order\":[";
  for (int i = 0; i < NUM_GAUGE_PAGES; i++) { s += cfg.pageOrder[i]; if (i < NUM_GAUGE_PAGES - 1) s += ','; }
  s += "],\"on\":[";
  for (int i = 0; i < NUM_GAUGE_PAGES; i++) { s += cfg.pageOn[i]; if (i < NUM_GAUGE_PAGES - 1) s += ','; }
  s += "],";
  jInt(s, "w_clt_on", cfg.coolantHigh.on);  jNum(s, "w_clt", cfg.coolantHigh.limit, 1);
  jInt(s, "w_ot_on",  cfg.oilTempHigh.on);  jNum(s, "w_ot",  cfg.oilTempHigh.limit, 1);
  jInt(s, "w_op_on",  cfg.oilPressLow.on);  jNum(s, "w_op",  cfg.oilPressLow.limit, 1); jInt(s, "w_op_rpm", cfg.oilPressMinRpm);
  jInt(s, "w_bst_on", cfg.boostHigh.on);    jNum(s, "w_bst", cfg.boostHigh.limit, 1);
  jInt(s, "w_lean_on", cfg.leanBoost.on);   jNum(s, "w_lean", cfg.leanBoost.limit, 3); jNum(s, "w_lean_bst", cfg.leanMinBoostKpa, 1);
  jInt(s, "w_shift_on", cfg.shiftLight.on); jInt(s, "w_shift", lroundf(cfg.shiftLight.limit));
  jInt(s, "w_cp_on", cfg.coolPressHigh.on);  jNum(s, "w_cp", cfg.coolPressHigh.limit, 1);
  jInt(s, "w_fd_on", cfg.fuelDiffLow.on);    jNum(s, "w_fd", cfg.fuelDiffLow.limit, 1);
  jInt(s, "w_iat_on", cfg.iatHigh.on);       jNum(s, "w_iat", cfg.iatHigh.limit, 1);
  jInt(s, "w_bat_on", cfg.battLow.on);       jNum(s, "w_bat", cfg.battLow.limit, 1);
  jInt(s, "w_duty_on", cfg.dutyHigh.on);     jNum(s, "w_duty", cfg.dutyHigh.limit, 1);
  jInt(s, "acycle", cfg.alertCycleMs);
  jInt(s, "face_popup", ui.facePopup);
  jInt(s, "shift_show", ui.shiftShow);
  jInt(s, "shift_rgb", (long)ui.shiftRgb);
  jInt(s, "gauge_skin", ui.gaugeSkin);
  jInt(s, "bright", cfg.brightness);
  s += "\"tachmax\":"; s += cfg.tachMax; s += '}';
}

// ---------------- Apply from form / command ----------------
static void parseList(const String &v, uint8_t *out, int n)
{
  int idx = 0, start = 0;
  for (int i = 0; i <= (int)v.length() && idx < n; i++) {
    if (i == (int)v.length() || v[i] == ',') { out[idx++] = v.substring(start, i).toInt(); start = i + 1; }
  }
}

void Settings_ApplyFrom(GaugeSettings &n, SettingGetter get)
{
  String v;
  auto getF = [&](const char *k, float def) -> float { return get(k, v) ? v.toFloat() : def; };
  auto getI = [&](const char *k, long def)  -> long  { return get(k, v) ? v.toInt()   : def; };

  n.numGears = constrain(getI("numGears", n.numGears), 4, 6);
  for (int i = 0; i < 6; i++) { char k[4]; snprintf(k, sizeof(k), "g%d", i + 1); n.gear[i] = getF(k, n.gear[i]); }
  n.finalDrive = getF("fd", n.finalDrive);
  n.rear  = { (uint16_t)getI("rw", n.rear.width),  (uint8_t)getI("ra", n.rear.aspect),  (uint8_t)getI("rr", n.rear.rim) };
  n.front = { (uint16_t)getI("fw", n.front.width), (uint8_t)getI("fa", n.front.aspect), (uint8_t)getI("fr", n.front.rim) };
  n.ref   = { (uint16_t)getI("cw", n.ref.width),   (uint8_t)getI("ca", n.ref.aspect),   (uint8_t)getI("cr", n.ref.rim) };
  n.speedSource = constrain(getI("spdSrc", n.speedSource), 0, 3);
  n.gearSource  = constrain(getI("gearSrc", n.gearSource), 0, 2);

  n.pressUnit   = constrain(getI("pu", n.pressUnit), 0, 2);
  n.tempUnit    = constrain(getI("tu", n.tempUnit), 0, 1);
  n.speedUnit   = constrain(getI("su", n.speedUnit), 0, 1);
  n.afrAsLambda = getI("lam", n.afrAsLambda) ? 1 : 0;
  n.flexStoich  = getI("flex", n.flexStoich) ? 1 : 0;
  n.stoich      = constrain(getF("stoich", n.stoich), 5.0f, 20.0f);

  uint8_t order[NUM_GAUGE_PAGES], on[NUM_GAUGE_PAGES];
  memcpy(order, n.pageOrder, sizeof(order));
  memcpy(on, n.pageOn, sizeof(on));
  if (get("order", v)) { v.replace("[", ""); v.replace("]", ""); parseList(v, order, NUM_GAUGE_PAGES); }
  if (get("on", v))    { v.replace("[", ""); v.replace("]", ""); parseList(v, on, NUM_GAUGE_PAGES); }
  bool seen[NUM_GAUGE_PAGES] = {false}, okOrder = true;       // must contain each page exactly once
  for (int i = 0; i < NUM_GAUGE_PAGES; i++) {
    if (order[i] >= NUM_GAUGE_PAGES || seen[order[i]]) okOrder = false; else seen[order[i]] = true;
  }
  if (okOrder) memcpy(n.pageOrder, order, sizeof(order));
  for (int i = 0; i < NUM_GAUGE_PAGES; i++) n.pageOn[i] = on[i] ? 1 : 0;

  n.coolantHigh = { (uint8_t)(getI("w_clt_on", n.coolantHigh.on) ? 1 : 0), getF("w_clt", n.coolantHigh.limit) };
  n.oilTempHigh = { (uint8_t)(getI("w_ot_on",  n.oilTempHigh.on) ? 1 : 0), getF("w_ot",  n.oilTempHigh.limit) };
  n.oilPressLow = { (uint8_t)(getI("w_op_on",  n.oilPressLow.on) ? 1 : 0), getF("w_op",  n.oilPressLow.limit) };
  n.oilPressMinRpm = getI("w_op_rpm", n.oilPressMinRpm);
  n.boostHigh   = { (uint8_t)(getI("w_bst_on", n.boostHigh.on) ? 1 : 0),   getF("w_bst", n.boostHigh.limit) };
  n.leanBoost   = { (uint8_t)(getI("w_lean_on", n.leanBoost.on) ? 1 : 0),  getF("w_lean", n.leanBoost.limit) };
  n.leanMinBoostKpa = getF("w_lean_bst", n.leanMinBoostKpa);
  n.shiftLight  = { (uint8_t)(getI("w_shift_on", n.shiftLight.on) ? 1 : 0), (float)getI("w_shift", lroundf(n.shiftLight.limit)) };

  n.coolPressHigh = { (uint8_t)(getI("w_cp_on", n.coolPressHigh.on) ? 1 : 0),  getF("w_cp", n.coolPressHigh.limit) };
  n.fuelDiffLow   = { (uint8_t)(getI("w_fd_on", n.fuelDiffLow.on) ? 1 : 0),    getF("w_fd", n.fuelDiffLow.limit) };
  n.iatHigh       = { (uint8_t)(getI("w_iat_on", n.iatHigh.on) ? 1 : 0),       getF("w_iat", n.iatHigh.limit) };
  n.battLow       = { (uint8_t)(getI("w_bat_on", n.battLow.on) ? 1 : 0),       getF("w_bat", n.battLow.limit) };
  n.dutyHigh      = { (uint8_t)(getI("w_duty_on", n.dutyHigh.on) ? 1 : 0),     getF("w_duty", n.dutyHigh.limit) };
  n.alertCycleMs  = constrain(getI("acycle", n.alertCycleMs), 1000, 5000);

  ui.facePopup = getI("face_popup", ui.facePopup) ? 1 : 0;      // display extras apply straight away
  ui.shiftShow  = getI("shift_show", ui.shiftShow) ? 1 : 0;
  ui.shiftRgb   = (uint32_t)getI("shift_rgb", (long)ui.shiftRgb) & 0xFFFFFF;
  ui.gaugeSkin  = SKIN_BPDFC;                                            // basic build: BPD FC only

  n.brightness = constrain(getI("bright", n.brightness), 10, 100);
  n.tachMax    = constrain(getI("tachmax", n.tachMax), 4000, 12000);
}

// ---------------- Pending apply (handed to the main loop) ----------------
static portMUX_TYPE pendMux = portMUX_INITIALIZER_UNLOCKED;
static GaugeSettings pending;
static volatile bool hasPending = false;

void Settings_RequestApply(const GaugeSettings &n)
{
  portENTER_CRITICAL(&pendMux);
  pending = n;
  hasPending = true;
  portEXIT_CRITICAL(&pendMux);
}

bool Settings_TakePending(GaugeSettings &out)
{
  if (!hasPending) return false;
  portENTER_CRITICAL(&pendMux);
  out = pending;
  hasPending = false;
  portEXIT_CRITICAL(&pendMux);
  return true;
}

// ---------------- Bluetooth PIN ----------------
uint32_t Settings_BlePin(void)
{
  static uint32_t pin = 0;
  if (pin) return pin;
  Preferences p;
  p.begin("bpd", false);
  pin = p.getUInt("blepin", 0);
  if (pin < 100000 || pin > 999999) {               // first boot: make a random PIN and keep it
    pin = 100000 + (esp_random() % 900000);
    p.putUInt("blepin", pin);
  }
  p.end();
  return pin;
}
