// Setup_State: has this gauge been set up yet, and which car profile is on it.
//   0 = not set up   (brand-new gauge: runs the cautious Basic preset)
//   1 = basic        (user chose to skip setup: Basic preset kept on purpose)
//   2 = configured   (set up from the app, or a gauge that already had saved settings)
// Stored in its own flash namespace so it survives a settings reset and never changes the settings struct.
#pragma once
#include <Arduino.h>

enum { SETUP_NONE = 0, SETUP_BASIC = 1, SETUP_DONE = 2 };

void     Setup_Begin(bool hadSavedSettings);       // call after Settings_Load()
uint8_t  Setup_State(void);
void     Setup_SetState(uint8_t s);
String   Setup_ProfileId(void);                    // "" when none
String   Setup_ProfileName(void);
void     Setup_SetProfile(const String &id, const String &name);
String   Setup_BleName(void);                      // "BPD Gauge XXXX", XXXX = last 2 bytes of the MAC
