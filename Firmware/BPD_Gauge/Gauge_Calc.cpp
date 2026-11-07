#include "Gauge_Calc.h"
#include "GPS_Lap.h"
#include "Haltech_CAN.h"
#include "Settings.h"

#define STALE_MS 1000

bool Calc_Fresh(uint32_t t) { return t != 0 && (millis() - t) < STALE_MS; }

float Calc_SpeedKmh(bool *valid)
{
  if (cfg.speedSource == SPD_GPS && Gps_Fresh()) {           // the gauge's own GPS
    GpsState g; Gps_Get(g);
    *valid = true;
    return g.kmh;
  }
  *valid = Calc_Fresh(hd.t370);
  float v = hd.speed_kmh;
  float ref = Tyre_CircM(cfg.ref);
  if (ref > 0.5f) {
    if (cfg.speedSource == SPD_REAR)  v *= Tyre_CircM(cfg.rear)  / ref;
    if (cfg.speedSource == SPD_FRONT) v *= Tyre_CircM(cfg.front) / ref;
  }
  return v;
}

int Calc_Gear(bool *valid)
{
  *valid = false;
  if (cfg.gearSource == GEAR_OFF) return 0;

  if (cfg.gearSource == GEAR_ECU) {
    *valid = Calc_Fresh(hd.t470);
    return hd.ecu_gear;
  }

  // Calculated: compare engine RPM with rear wheel RPM
  bool sv;
  float v = Calc_SpeedKmh(&sv);
  if (!sv || !Calc_Fresh(hd.t360)) return 0;
  *valid = true;
  if (v < 4.0f || hd.rpm < 400) return 0;

  float circ = Tyre_CircM(cfg.rear);
  if (circ < 0.5f || cfg.finalDrive < 0.5f) { *valid = false; return 0; }
  float wheelRpm = (v * 1000.0f / 60.0f) / circ;
  float boxRatio = hd.rpm / wheelRpm / cfg.finalDrive;

  int best = 0;
  float bestErr = 1e9f;
  for (int i = 0; i < cfg.numGears; i++) {
    if (cfg.gear[i] < 0.1f) continue;
    float err = fabsf(boxRatio - cfg.gear[i]) / cfg.gear[i];
    if (err < bestErr) { bestErr = err; best = i + 1; }
  }
  return (bestErr < 0.08f) ? best : 0;     // no close match = clutch in / neutral
}

float Calc_Stoich(void)
{
  if (cfg.flexStoich && Calc_Fresh(hd.t3E1)) {
    float e = constrain(hd.ethanol_pct, 0.0f, 100.0f) / 100.0f;
    return 14.7f - (14.7f - 9.0f) * e;          // petrol 14.7 to pure ethanol 9.0
  }
  return cfg.stoich;
}

float Calc_BoostKpa(bool *valid)
{
  *valid = Calc_Fresh(hd.t360);
  float baro = Calc_Fresh(hd.t372) ? hd.baro_kpa : 101.3f;
  return hd.map_kpa_abs - baro;
}
