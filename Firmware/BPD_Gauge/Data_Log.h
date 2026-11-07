#pragma once
// Data logging to the TF (micro SD) card.
//
// Card: SD_MMC 1-bit mode on the Waveshare ESP32-S3-Touch-LCD-2.1
//   CLK GPIO2, CMD GPIO1, D0 GPIO42, D3 held high through the I/O expander (EXIO4).
//   GPIO1/2 are also the LCD setup lines, which are only used at start-up.
//
// Files: /logs/L00001.BPL, L00002.BPL ... (binary, little endian)
//   Header, 32 bytes:
//     0  "BPDL"        4  version (1)      5  rows per second   6  channel count   7  reserved
//     8  start time (unix seconds, 0 = unknown)   12 channel mask (bit = channel id below)
//     16 pre-trigger length (ms)                  20 flags (bit 0 = recorded from the bench simulator, i.e. fake data)   21-31 reserved
//   Rows: u32 time (ms from the first row), then one int16 per logged channel in channel
//         id order, then (if channel mask bit 18 is set) the GPS block: i32 latitude x1e7,
//         i32 longitude x1e7, u16 GPS speed km/h x10, u16 lap number; then u16 warning mask.
//   Finished logs are renamed with their local start date and time in front, e.g.
//   2026-10-31_1422_L00012.BPL (when the clock is known), so the newest sort to the top.
//   Missing values are stored as -32768.
//
// Channel ids and scaling (stored value = real value x scale):
//   0 RPM x1          1 Boost kPa x10       2 Target boost kPa x10   3 Lambda x1000
//   4 Target lambda x1000                   5 Ethanol % x10          6 Oil pressure kPa x10
//   7 Oil temp C x10  8 Coolant temp C x10  9 Coolant pressure kPa x10
//   10 Fuel pressure kPa x10                11 Injector duty % x10   12 Intake air temp C x10
//   13 Battery V x100 14 Throttle % x10     15 Speed km/h x10        16 Gear x1
//   17 Ignition angle deg x10
#include <Arduino.h>
#include "Settings.h"

#define LOG_NUM_CH 18              // int16 channels
#define LOG_GPS_BIT 18             // channel mask bit for the GPS block
#define LOG_TZ_KEEP (-32768)

struct LogConfig {
  uint32_t magic;
  uint8_t  mode;                 // 0 manual, 1 automatic, 2 both
  uint8_t  rate;                 // rows per second: 10, 20 or 50
  uint32_t chMask;               // which channels to log
  uint8_t  stRpmOn, stSpdOn, stTpsOn, stBstOn;
  uint16_t stRpm;                // start above (rpm)
  float    stSpd, stTps, stBst;  // km/h, %, kPa
  uint8_t  spEngOn, spStillOn;
  uint16_t spEngS, spStillS;     // stop after engine off / stationary for (s)
  uint8_t  preS;                 // pre-trigger buffer (s)
  uint16_t minS;                 // discard logs shorter than (s)
  uint16_t splitMin;             // start a new file every (min), 0 = never
  uint8_t  fullMode;             // card full: 0 stop logging, 1 delete the oldest log
};

void     Log_Init(void);                       // after the I/O expander is set up
bool     Log_SdOk(void);
bool     Log_Recording(void);
uint32_t Log_RecordingSecs(void);
void     Log_ManualToggle(void);               // keypad page / app button
void     Log_SetTime(uint32_t unixSecs, int tzMinutes);   // phone clock or GPS (UTC); tz = LOG_TZ_KEEP to keep the last one

void     Log_StatusJson(String &out);          // config + state, for the app
void     Log_ApplyConfig(SettingGetter get);   // change config from an app command
void     Log_ListJson(String &out);            // files on the card
bool     Log_Delete(const String &name);
int      Log_DeleteAll(void);                  // returns how many were deleted
int32_t  Log_Read(const String &name, uint32_t offset, uint8_t *buf, uint32_t len);  // -1 = no file

// ---- Car profiles on the card (/profiles/<id>.json, text the app writes; .bak keeps the previous copy) ----
// The gauge never needs these to run: settings live in flash. This is a backup the app can restore from.
void     Prof_ListJson(String &out);                                            // ["id","id",...]
bool     Prof_Write(const String &id, uint32_t offset, const uint8_t *d, uint32_t len, bool last);  // offset 0 starts a new file; written to .tmp, swapped in on the last chunk
int32_t  Prof_Read(const String &id, uint32_t offset, uint8_t *buf, uint32_t len);  // -1 = none
bool     Prof_Delete(const String &id);

