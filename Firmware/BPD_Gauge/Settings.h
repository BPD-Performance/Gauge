#pragma once
// User settings, stored in the gauge's flash (NVS) and edited from the WiFi settings page.
// All values are stored in metric (kPa, C, km/h); the display converts to your chosen units.
#include <Arduino.h>

#define NUM_GAUGE_PAGES 12
enum { PG_BOOST, PG_OIL, PG_AFR, PG_TACH, PG_COOLANT, PG_FUEL, PG_IAT, PG_MONITOR, PG_KEYPAD, PG_ALERTS, PG_LAP, PG_FACE };

enum { PU_KPA, PU_BAR, PU_PSI };           // pressure units
enum { TU_C, TU_F };                       // temperature units
enum { SU_KMH, SU_MPH };                   // speed units
enum { SPD_ECU, SPD_REAR, SPD_FRONT, SPD_GPS };   // where the speed comes from (GPS: the gauge's own GPS)
enum { GEAR_CALC, GEAR_ECU, GEAR_OFF };    // how the gear is worked out

struct Tyre { uint16_t width; uint8_t aspect; uint8_t rim; };   // e.g. 265/35R18
struct Warn { uint8_t on; float limit; };

struct GaugeSettings {
  uint32_t magic;

  // Gearing and tyres
  uint8_t numGears;
  float   gear[6];
  float   finalDrive;
  Tyre    rear, front, ref;      // ref = tyre size the ECU's speed calibration was set up for
  uint8_t speedSource, gearSource;

  // Units
  uint8_t pressUnit, tempUnit, speedUnit, afrAsLambda, flexStoich;
  float   stoich;                // AFR stoich used when flex stoich is off

  // Pages
  uint8_t pageOrder[NUM_GAUGE_PAGES];
  uint8_t pageOn[NUM_GAUGE_PAGES];

  // Warnings (metric)
  Warn     coolantHigh;          // C
  Warn     oilTempHigh;          // C
  Warn     oilPressLow;          // kPa
  uint16_t oilPressMinRpm;       // only warn above this RPM
  Warn     boostHigh;            // kPa (gauge)
  Warn     leanBoost;            // lambda
  float    leanMinBoostKpa;      // only warn above this boost
  Warn     shiftLight;           // RPM

  // Display
  uint8_t  brightness;           // 10-100 %
  uint16_t tachMax;              // RPM

  // Added with the amber skin (older saved settings are migrated, see Settings.cpp)
  Warn     coolPressHigh;        // kPa (gauge)
  Warn     fuelDiffLow;          // kPa (fuel pressure minus boost)
  Warn     iatHigh;              // C
  Warn     battLow;              // V (only while the engine is running)
  Warn     dutyHigh;             // % injector duty
  uint16_t alertCycleMs;         // warning popup: time on each warning when several are active
};

extern GaugeSettings cfg;

// Display extras, saved separately from cfg so older saved settings never need converting
struct UiPrefs {
  uint32_t magic;
  uint8_t  facePopup;        // 1 = warning popup also covers the Face page
  uint8_t  shiftShow;        // 1 = flash the outer ring at the shift point
  uint8_t  faceStyle;        // retired (kept so the saved blob keeps its size): the avatar now belongs to the gauge skin
  uint8_t  gaugeSkin;        // 0 = BPD FC (amber dot matrix), 1 = BPD Embers (16-bit, avatar Cinder)
  uint32_t shiftRgb;         // shift light colour, 0xRRGGBB
};
// faceStyle and gaugeSkin sit in what used to be padding, so the saved blob is still 12 bytes and older saves load as Classic / BPD FC
static_assert(sizeof(UiPrefs) == 12, "UiPrefs size changed: saved display extras would no longer load");
enum { FACE_CLASSIC, FACE_KNIGHT };
enum { SKIN_BPDFC, SKIN_IRON };
extern UiPrefs ui;

#include <functional>

// Reads one setting by name from a web form or a Bluetooth command. Returns false if absent.
typedef std::function<bool(const char *key, String &value)> SettingGetter;

void  Settings_Defaults(void);                                        // reset cfg
void  Settings_MakeDefaults(GaugeSettings &d);
void  Settings_BasicPreset(GaugeSettings &d);                          // cautious starting point for a gauge that isn't set up
bool  Settings_HadSaved(void);                                         // true if settings were loaded from flash                         // fill d with defaults
void  Settings_ToJson(String &out);                                   // all settings as JSON
void  Settings_ApplyFrom(GaugeSettings &n, SettingGetter get);        // copy any supplied fields into n

// Changes from Bluetooth/WiFi are applied by the main loop (screen rebuilt, no restart)
void  Settings_RequestApply(const GaugeSettings &n);
bool  Settings_TakePending(GaugeSettings &out);

uint32_t Settings_BlePin(void);                                       // 6-digit pairing PIN
void  Settings_Load(void);
void  Settings_Save(void);
float Tyre_CircM(const Tyre &t);       // rolling circumference in metres
const char *Page_Name(uint8_t page);
