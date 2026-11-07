#pragma once
// Haltech CAN: receives the ECU broadcast (Haltech CAN Broadcast Protocol V2.35,
// 1 Mbit/s, 11-bit, big endian) and emulates a Haltech 3x5 CAN keypad (Keypad B).
#include <Arduino.h>

#define CAN_TX_PIN  GPIO_NUM_20   // 12-pin header pin 4 -> transceiver TX
#define CAN_RX_PIN  GPIO_NUM_19   // 12-pin header pin 3 -> transceiver RX

// ---- Keypad B button assignments (set the same buttons in NSP) ----
#define KP_BTN_BOOST_UP     1     // Boost trim up   (momentary press)
#define KP_BTN_BOOST_DOWN   2     // Boost trim down (momentary press)
#define KP_BTN_ROT_IDLE     3     // Rotational idle (held on while enabled on the gauge)

struct HaltechData {
  // Values converted to normal units
  volatile uint16_t rpm;            // 0x360 b0-1
  volatile float    map_kpa_abs;    // 0x360 b2-3
  volatile float    oil_press_kpa;  // 0x361 b2-3 (gauge pressure)
  volatile float    lambda;         // 0x368 b0-1 (wideband sensor 1)
  volatile float    speed_kmh;      // 0x370 b0-1
  volatile float    target_boost_kpa;     // 0x372 b4-5 (gauge pressure, kPa)
  volatile float    baro_kpa;       // 0x372 b6-7
  volatile float    coolant_c;      // 0x3E0 b0-1
  volatile float    oil_temp_c;     // 0x3E0 b6-7
  volatile float    target_lambda;  // 0x3E9 b4-5
  volatile float    ethanol_pct;    // 0x3E1 b4-5 (fuel composition)
  volatile int8_t   ecu_gear;       // 0x470 b7 (-1 R, 0 N, 1-6)
  volatile float    coolant_press_kpa;  // 0x360 b6-7 (gauge pressure)
  volatile float    fuel_press_kpa;     // 0x361 b0-1 (gauge pressure)
  volatile float    inj_duty_pct;       // 0x362 b0-1 (injection stage 1 duty)
  volatile float    batt_v;             // 0x372 b0-1
  volatile float    iat_c;              // 0x3E0 b2-3 (air temperature)
  volatile float    throttle_pct;       // 0x360 b4-5
  volatile float    ign_angle_deg;      // 0x362 b4-5 (signed)

  // millis() when each message was last received (0 = never)
  volatile uint32_t t360, t361, t362, t368, t370, t372, t3E0, t3E9, t3E1, t470;

  // millis() when the ECU last talked to the emulated keypad (0 = never)
  volatile uint32_t tKeypadEcu;
};

extern HaltechData hd;

void Haltech_CAN_Init(void);

// Keypad emulation (safe to call from the UI)
void Keypad_Press(uint8_t button);              // one momentary press (~150 ms)
void Keypad_SetHold(uint8_t button, bool on);   // hold a button down until released
bool Keypad_GetHold(uint8_t button);

// Latest copy of every CAN message seen on the bus (for the Bluetooth app)
#define CAN_MAX_FRAMES 96
struct CanFrame { uint16_t id; uint8_t len; uint8_t data[8]; uint32_t ms; };
int  CAN_CopyFrames(CanFrame *out, int max);            // returns how many were copied
bool CAN_GetFrame(uint16_t id, CanFrame *out);

// Bench-test simulator: generates realistic fake Haltech data (switches itself off if the real ECU appears)
void Sim_Set(bool on);
bool Sim_Active(void);
