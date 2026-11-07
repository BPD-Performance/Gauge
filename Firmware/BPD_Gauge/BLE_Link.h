#pragma once
// Bluetooth Low Energy link to the phone/PC app.
//
// Service  b9d00001-6a1e-4c2a-9e3d-6b5a8f0c1d01
//   RX    b9d00002-...  write  : commands from the app, JSON text, one command per line ('\n')
//   TX    b9d00003-...  notify : replies to the app, JSON text, one reply per line ('\n')
//   LIVE  b9d00004-...  notify : binary live data (raw CAN frames + gauge status)
// All three need pairing with the 6-digit PIN shown on the gauge's SETUP page.
#include <Arduino.h>

void BLE_Init(void);
bool BLE_Connected(void);
