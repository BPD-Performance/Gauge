#pragma once
// Values worked out from the CAN data and the user settings (speed, gear, stoich)
#include <Arduino.h>

bool  Calc_Fresh(uint32_t t);          // true if a message arrived in the last second
float Calc_SpeedKmh(bool *valid);      // speed corrected for the tyres fitted
int   Calc_Gear(bool *valid);          // -1 R, 0 N/clutch in, 1-6
float Calc_Stoich(void);               // AFR stoich (follows ethanol content if enabled)
float Calc_BoostKpa(bool *valid);      // MAP minus baro

