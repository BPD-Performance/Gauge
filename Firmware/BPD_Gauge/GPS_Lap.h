#pragma once
// GPS module (Beitian BN-220 / u-blox M8 or M10) and GPS lap timer.
//
// Wiring on the gauge's 4-pin UART header (only live when the USB-to-UART Type-C port is NOT plugged in):
//   GPS TX  -> RXD  (GPIO44)      GPS data in
//   GPS RX  <- IO0  (GPIO0)       configuration out (10 Hz, 115200 baud)
//   GPIO43 (TXD) -> speed pulse output, through an NPN transistor (open collector) to an Elite DPI
//                   input with its pull-up on. Same idea as Haltech's GPS Speed Input Module.
// While the GPS is switched on in the settings, the debug serial output on GPIO43/44 stops.
//
// Lap timer: one start/finish line plus up to 8 sector lines. Each line is a centre point,
// a direction of travel (bearing) and a width. Set them by holding the LAP page for 2 s
// while driving over the spot, or from the app.
#include <Arduino.h>
#include "Settings.h"

#define TRK_MAX_GATES 9           // gate 0 = start/finish, 1..8 = sectors

struct Gate { double lat, lon; float bearing, width; };

struct GpsState {
  bool     fix;                   // valid position
  uint8_t  sats;
  double   lat, lon;
  float    kmh, course;           // ground speed, direction of travel
  uint32_t fixMs;                 // millis() of the last fix
  uint32_t utc;                   // unix seconds from the GPS (0 = unknown)
  uint32_t nmeaMs;                // millis() of the last valid NMEA sentence (0 = none yet): the module is talking, fix or not
};

struct LapState {
  bool     haveTrack;
  uint8_t  nGates;
  uint16_t lap;                   // current lap number (0 = not started)
  uint32_t curMs, lastMs, bestMs; // 0 = none
  int32_t  deltaMs;               // current lap vs best at the same distance (INT32_MIN = unknown)
  uint8_t  sector;                // sector currently being driven (1-based)
  uint8_t  progress;              // 0-255 around the lap, based on the best lap's length
};

void Gps_Init(void);
bool Gps_Enabled(void);
bool Gps_Fresh(void);                    // fix in the last second
bool Gps_Alive(void);                    // module sent valid NMEA in the last 3 s (even with no fix)
void Gps_Get(GpsState &g);
void Lap_Get(LapState &l);

// Hold on the LAP page: first hold sets start/finish, the next ones add sectors.
// Returns a short message for the screen.
const char *Lap_MarkHere(void);

// Bluetooth app
void Gps_ConfigJson(String &out);
void Gps_ApplyConfig(SettingGetter get);
void Lap_TrackJson(String &out);
bool Lap_SetTrack(const String &json);   // {"name":"...","g":[[lat,lon,bearing,width],...]}
void Lap_ClearTrack(void);
void Lap_ResetBest(void);
