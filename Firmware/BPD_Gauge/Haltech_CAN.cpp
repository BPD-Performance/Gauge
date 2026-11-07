// Haltech CAN task. Runs on core 0 so drawing on core 1 never makes it miss frames.
// Receives the ECU broadcast and emulates Haltech 3x5 CAN Keypad B (node 0x0D):
//   0x18D  button states (3 bytes) every 30 ms
//   0x70D  keepalive (0x05) every 150 ms
//   0x60D -> 0x58D  configuration handshake with the ECU

#include "Haltech_CAN.h"
#include "driver/twai.h"
#include "soc/usb_serial_jtag_reg.h"
#include "soc/soc.h"
#include "Settings.h"
#include "BPD_Config.h"

HaltechData hd = {};

static inline uint16_t be16(const uint8_t *d) { return (uint16_t)((d[0] << 8) | d[1]); }

// ---------------- Latest frame store ----------------
static CanFrame frames[CAN_MAX_FRAMES];
static int numFrames = 0;
static portMUX_TYPE frameMux = portMUX_INITIALIZER_UNLOCKED;

static void storeFrame(const twai_message_t &m)
{
  if (m.extd || m.rtr) return;
  uint16_t id = (uint16_t)m.identifier;
  portENTER_CRITICAL(&frameMux);
  int i;
  for (i = 0; i < numFrames; i++) if (frames[i].id == id) break;
  if (i == numFrames) {
    if (numFrames >= CAN_MAX_FRAMES) { portEXIT_CRITICAL(&frameMux); return; }
    numFrames++;
  }
  frames[i].id  = id;
  frames[i].len = m.data_length_code > 8 ? 8 : m.data_length_code;
  memcpy(frames[i].data, m.data, 8);
  frames[i].ms  = millis();
  portEXIT_CRITICAL(&frameMux);
}

int CAN_CopyFrames(CanFrame *out, int max)
{
  portENTER_CRITICAL(&frameMux);
  int n = numFrames < max ? numFrames : max;
  memcpy(out, frames, n * sizeof(CanFrame));
  portEXIT_CRITICAL(&frameMux);
  return n;
}

bool CAN_GetFrame(uint16_t id, CanFrame *out)
{
  bool found = false;
  portENTER_CRITICAL(&frameMux);
  for (int i = 0; i < numFrames; i++) if (frames[i].id == id) { *out = frames[i]; found = true; break; }
  portEXIT_CRITICAL(&frameMux);
  return found;
}

static void decode(const twai_message_t &m)
{
  const uint8_t *d = m.data;
  const uint32_t now = millis();

  switch (m.identifier) {
    case 0x360:
      hd.rpm         = be16(&d[0]);
      hd.map_kpa_abs = be16(&d[2]) / 10.0f;
      hd.throttle_pct      = be16(&d[4]) / 10.0f;
      hd.coolant_press_kpa = be16(&d[6]) / 10.0f - 101.3f;
      hd.t360 = now;
      break;
    case 0x361:
      hd.fuel_press_kpa = be16(&d[0]) / 10.0f - 101.3f;
      hd.oil_press_kpa  = be16(&d[2]) / 10.0f - 101.3f;
      hd.t361 = now;
      break;
    case 0x362:
      hd.inj_duty_pct  = be16(&d[0]) / 10.0f;
      hd.ign_angle_deg = (int16_t)be16(&d[4]) / 10.0f;
      hd.t362 = now;
      break;
    case 0x368:
      hd.lambda = be16(&d[0]) / 1000.0f;
      hd.t368 = now;
      break;
    case 0x370:
      hd.speed_kmh = be16(&d[0]) / 10.0f;
      hd.t370 = now;
      break;
    case 0x372:
      hd.batt_v               = be16(&d[0]) / 10.0f;
      hd.target_boost_kpa     = be16(&d[4]) / 10.0f;
      hd.baro_kpa             = be16(&d[6]) / 10.0f;
      hd.t372 = now;
      break;
    case 0x3E0:
      hd.coolant_c  = be16(&d[0]) / 10.0f - 273.15f;
      hd.iat_c      = be16(&d[2]) / 10.0f - 273.15f;
      hd.oil_temp_c = be16(&d[6]) / 10.0f - 273.15f;
      hd.t3E0 = now;
      break;
    case 0x3E1:
      hd.ethanol_pct = be16(&d[4]) / 10.0f;
      hd.t3E1 = now;
      break;
    case 0x470:
      hd.ecu_gear = (int8_t)d[7];
      hd.t470 = now;
      break;
    case 0x3E9:
      hd.target_lambda = be16(&d[4]) / 1000.0f;
      hd.t3E9 = now;
      break;
    default:
      break;
  }
}

// ---------------- Keypad B emulation ----------------
#define KP_NODE          0x0D
#define KP_BUTTON_ID     (0x180 + KP_NODE)   // 0x18D
#define KP_SDO_TX_ID     (0x580 + KP_NODE)   // 0x58D  keypad -> ECU
#define KP_SDO_RX_ID     (0x600 + KP_NODE)   // 0x60D  ECU -> keypad
#define KP_KEEPALIVE_ID  (0x700 + KP_NODE)   // 0x70D
#define KP_BUTTON_MS     30
#define KP_KEEPALIVE_MS  150
#define KP_PRESS_MS      150                 // how long a momentary press is held
#define KP_GAP_MS        100                 // release time between queued presses
#define KP_NUM_BUTTONS   15

static portMUX_TYPE kpMux = portMUX_INITIALIZER_UNLOCKED;

// Debug counters (printed once a second)
static uint32_t dbgSdoRx = 0, dbgNmtRx = 0, dbgSdoLogged = 0, dbgBackoffs = 0;

// CANopen state of the emulated keypad, reported in every keepalive (heartbeat)
#define NMT_BOOTUP   0x00
#define NMT_STOPPED  0x04
#define NMT_OPER     0x05
#define NMT_PREOP    0x7F
static volatile uint8_t kpState = NMT_PREOP;
static uint32_t dbgNodeLogged = 0;

// Transmit guard: only send while the ECU is broadcasting, and back off if our
// frames are not being acknowledged, so the gauge can never flood the bus.
static uint32_t txBackoffUntil = 0;
static uint16_t kpHoldMask = 0;              // buttons held on by the UI
static uint8_t  kpPending[KP_NUM_BUTTONS];   // queued momentary presses
static uint8_t  kpPhase[KP_NUM_BUTTONS];     // 0 idle, 1 pressed, 2 gap
static uint32_t kpPhaseEnd[KP_NUM_BUTTONS];

// ---------------- Simulator state ----------------
static volatile uint32_t lastRealMs = 0;       // last 0x360 from the real ECU
static volatile bool simOn = false;
static volatile int  simTrim = 6;              // simulated boost trim position (0-8)

void Keypad_Press(uint8_t button)
{
  if (simOn) {                                 // in the simulator, + / - move the simulated trim
    if (button == KP_BTN_BOOST_UP   && simTrim < 8) simTrim++;
    if (button == KP_BTN_BOOST_DOWN && simTrim > 0) simTrim--;
  }
  if (button < 1 || button > KP_NUM_BUTTONS) return;
  Serial.printf("Touch: keypad button %u pressed (keypad state %02X)\n", button, kpState);
  portENTER_CRITICAL(&kpMux);
  if (kpPending[button - 1] < 10) kpPending[button - 1]++;
  portEXIT_CRITICAL(&kpMux);
}

void Keypad_SetHold(uint8_t button, bool on)
{
  if (button < 1 || button > KP_NUM_BUTTONS) return;
  portENTER_CRITICAL(&kpMux);
  if (on) kpHoldMask |=  (1u << (button - 1));
  else    kpHoldMask &= ~(1u << (button - 1));
  portEXIT_CRITICAL(&kpMux);
}

bool Keypad_GetHold(uint8_t button)
{
  if (button < 1 || button > KP_NUM_BUTTONS) return false;
  return (kpHoldMask >> (button - 1)) & 1;
}

static void sendFrame(uint32_t id, const uint8_t *data, uint8_t len)
{
  twai_message_t m = {};
  m.identifier = id;
  m.data_length_code = len;
  // Normal retries (not single-shot): single-shot frames are dropped whenever they
  // lose arbitration to higher-priority ECU traffic, so the keepalive never got through.
  // Flooding is prevented by the backoff guard in can_task instead.
  memcpy(m.data, data, len);
  twai_transmit(&m, 0);     // never block the task; drop if the TX queue is full
}

// Work out which buttons are down right now (held + momentary presses)
static uint16_t keypadMask(uint32_t now)
{
  uint16_t mask;
  portENTER_CRITICAL(&kpMux);
  mask = kpHoldMask;
  for (int i = 0; i < KP_NUM_BUTTONS; i++) {
    switch (kpPhase[i]) {
      case 0:   // idle: start the next queued press
        if (kpPending[i]) { kpPending[i]--; kpPhase[i] = 1; kpPhaseEnd[i] = now + KP_PRESS_MS; }
        break;
      case 1:   // pressed
        if ((int32_t)(now - kpPhaseEnd[i]) >= 0) { kpPhase[i] = 2; kpPhaseEnd[i] = now + KP_GAP_MS; }
        break;
      case 2:   // released gap
        if ((int32_t)(now - kpPhaseEnd[i]) >= 0) kpPhase[i] = 0;
        break;
    }
    if (kpPhase[i] == 1) mask |= (1u << i);
  }
  portEXIT_CRITICAL(&kpMux);
  return mask;
}

static void sendButtons(uint32_t now)
{
  static uint16_t lastMask = 0;
  uint16_t mask = keypadMask(now);
  if (mask != lastMask) {                     // log every change in what we send
    Serial.printf("KP->ECU 0x18D buttons: %02X %02X\n", mask & 0xFF, mask >> 8);
    lastMask = mask;
  }
  uint8_t d[3] = { (uint8_t)(mask & 0xFF), (uint8_t)(mask >> 8), 0 };  // buttons 1-8, 9-15
  sendFrame(KP_BUTTON_ID, d, 3);
}

static void sendKeepalive(void)
{
  const uint8_t d[1] = { kpState };
  sendFrame(KP_KEEPALIVE_ID, d, 1);
}

static void sendBootup(void)
{
  const uint8_t d[1] = { NMT_BOOTUP };
  sendFrame(KP_KEEPALIVE_ID, d, 1);
}

// Network management commands from the ECU (CAN ID 0x000): [command, node]
static void handleNmt(const twai_message_t &m)
{
  uint8_t cmd = m.data[0], node = m.data[1];
  dbgNmtRx++;
  Serial.printf("NMT: %02X %02X\n", cmd, node);
  if (node != KP_NODE && node != 0) return;      // 0 = all nodes

  uint8_t old = kpState;
  switch (cmd) {
    case 0x01: kpState = NMT_OPER;    break;     // start
    case 0x02: kpState = NMT_STOPPED; break;     // stop
    case 0x80: kpState = NMT_PREOP;   break;     // enter pre-operational
    case 0x81:                                   // reset node
    case 0x82:                                   // reset communication
      kpState = NMT_PREOP;
      sendBootup();
      break;
  }
  if (kpState != old) Serial.printf("Keypad state -> %02X\n", kpState);
}

// Log the first frames the ECU sends to node 0x0D on any channel (LEDs, settings...)
static void logNodeFrame(const twai_message_t &m)
{
  if (dbgNodeLogged >= 40) return;
  dbgNodeLogged++;
  Serial.printf("ECU->KP 0x%03lX [%d]:", m.identifier, m.data_length_code);
  for (int i = 0; i < m.data_length_code; i++) Serial.printf(" %02X", m.data[i]);
  Serial.println();
}

// Reply to the ECU's configuration requests the same way a real Haltech 3x5 keypad does
static void handleSdo(const twai_message_t &m)
{
  // The Elite sends these requests with 4-8 data bytes; pad the rest with zeros
  uint8_t r[8] = {0};
  memcpy(r, m.data, m.data_length_code > 8 ? 8 : m.data_length_code);
  uint8_t t[8] = {0};

  dbgSdoRx++;

  if (r[0] == 0x22) {                       // ECU writes a setting -> acknowledge
    t[0] = 0x60; t[1] = r[1]; t[2] = r[2]; t[3] = r[3];
  } else if (r[0] == 0x40 || r[0] == 0x42) { // ECU reads a setting -> answer
    t[0] = 0x43; t[1] = r[1]; t[2] = r[2]; t[3] = r[3];
    uint16_t index = r[1] | (r[2] << 8);
    uint8_t  sub   = r[3];
    if (index == 0x1018 && sub == 1)      { t[4] = 0x07; t[5] = 0x04; }                         // vendor
    else if (index == 0x1018 && sub == 2) { t[4] = 0x4B; t[5] = 0x33; }                         // product
    else if (index == 0x1018 && sub == 3) { t[4] = 0x01; }                                      // revision
    else if (index == 0x1018 && sub == 4) { t[4] = 0xA6; t[5] = 0xB8; t[6] = 0x19; t[7] = 0x0C; } // serial
    else if (index == 0x1800 && sub == 1) { t[4] = 0x8D; t[5] = 0x01; t[7] = 0x40; }            // button PDO ID
  } else if (r[0] == 0x00 && r[7] == 0xC8) {
    t[0] = 0x80; t[4] = 0x01; t[6] = 0x04; t[7] = 0x05;
  } else {
    return;                                  // unknown request: stay quiet
  }
  sendFrame(KP_SDO_TX_ID, t, 8);
  Serial.printf("KP->ECU 0x58D: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]);

  if (hd.tKeypadEcu == 0) Serial.println("ECU is talking to keypad B");
  hd.tKeypadEcu = millis();
}

// ---------------- Simulator: realistic fake Haltech data for bench testing ----------------
static void putBE(uint8_t *d, int at, int32_t v) { d[at] = (v >> 8) & 0xFF; d[at + 1] = v & 0xFF; }

static void simFrame(uint16_t id, const uint8_t *data)
{
  twai_message_t m = {};
  m.identifier = id;
  m.data_length_code = 8;
  memcpy(m.data, data, 8);
  storeFrame(m);
  decode(m);
}

static void simStep(uint32_t now)
{
  static uint32_t start = 0, t20 = 0, t50 = 0, t100 = 0, t200 = 0;
  if (!start) start = now;

  // 24 s loop: idle, a run through gears 1-4 with boost, cruise down, idle
  uint32_t t = (now - start) % 24000;
  float rpm, boost, tps, lam;
  int gear = 0;
  float target = 96.5f + simTrim * 13.79f;                 // 14 psi + 2 psi per trim step
  if (t < 4000 || t >= 20000) {                            // idle
    rpm = 900 + 25 * sinf(now / 300.0f); boost = -66; tps = 0; lam = 1.00f;
  } else if (t < 16000) {                                  // full throttle run, 3 s per gear
    uint32_t tt = t - 4000;
    gear = 1 + tt / 3000;
    float k = (tt % 3000) / 3000.0f;
    rpm = 2600 + k * 4600;
    boost = rpm < 3200 ? -20 + (rpm - 2600) / 600.0f * 40 : min(target, 20 + (rpm - 3200) / 1300.0f * target);
    tps = 100; lam = boost > 30 ? 0.78f : 0.92f;
  } else {                                                 // easing off in 4th
    float k = (t - 16000) / 4000.0f;
    gear = 4; rpm = 3200 - k * 1900; boost = -50; tps = 8; lam = 1.00f;
  }
  if (gear && rpm < 1400) gear = 0;

  // Road speed from the gearing and tyres on the settings page, then turned into what
  // the ECU would report so the gauge's tyre correction and gear calculation are exercised
  float spd = 0;
  if (gear && cfg.gear[gear - 1] > 0.1f && cfg.finalDrive > 0.1f)
    spd = rpm / (cfg.gear[gear - 1] * cfg.finalDrive) * Tyre_CircM(cfg.rear) * 60.0f / 1000.0f;
  float ecuSpd = spd, ref = Tyre_CircM(cfg.ref);
  if (cfg.speedSource == SPD_REAR  && Tyre_CircM(cfg.rear)  > 0.5f) ecuSpd = spd * ref / Tyre_CircM(cfg.rear);
  if (cfg.speedSource == SPD_FRONT && Tyre_CircM(cfg.front) > 0.5f) ecuSpd = spd * ref / Tyre_CircM(cfg.front);

  float clt = 84 + 6 * sinf(now / 20000.0f), oilT = 96 + 4 * sinf(now / 25000.0f);
  float oilP = 110 + rpm * 0.075f;
  uint8_t d[8];

  if (now - t20 >= 20) {                                   // 50 Hz
    t20 = now;
    float cltP = 70 + 25 * sinf(now / 15000.0f);                       // coolant pressure, kPa
    float fuelP = 300 + boost - (rpm > 6500 ? (rpm - 6500) * 0.04f : 0); // 1:1 regulator, slight droop at the top
    memset(d, 0, 8); putBE(d, 0, rpm); putBE(d, 2, (101.3f + boost) * 10); putBE(d, 4, tps * 10); putBE(d, 6, (101.3f + cltP) * 10);
    simFrame(0x360, d);
    memset(d, 0, 8); putBE(d, 0, (101.3f + fuelP) * 10); putBE(d, 2, (101.3f + oilP) * 10); putBE(d, 4, tps * 10); putBE(d, 6, (101.3f + boost * 0.6f) * 10);
    simFrame(0x361, d);
    memset(d, 0, 8); putBE(d, 0, rpm / 7200.0f * (boost > 0 ? 75 : 20) * 10); putBE(d, 4, (boost > 30 ? 16 : 14) * 10);
    simFrame(0x362, d);
  }
  if (now - t50 >= 50) {                                   // 20 Hz
    t50 = now;
    memset(d, 0, 8); putBE(d, 0, lam * 1000);
    simFrame(0x368, d);
    memset(d, 0, 8); putBE(d, 0, ecuSpd * 10);
    simFrame(0x370, d);
    memset(d, 0, 8); putBE(d, 2, boost > 0 ? min(90.0f, boost / 2) * 10 : 0);
    simFrame(0x36F, d);
    memset(d, 0, 8); putBE(d, 4, (boost > 30 ? 0.78f : 1.0f) * 1000);
    simFrame(0x3E9, d);
    memset(d, 0, 8); putBE(d, 0, lam * 1000); putBE(d, 2, lam * 1000); putBE(d, 4, lam * 1000); d[7] = (uint8_t)(int8_t)gear;
    simFrame(0x470, d);
  }
  if (now - t100 >= 100) {                                 // 10 Hz
    t100 = now;
    memset(d, 0, 8); putBE(d, 0, 139); putBE(d, 4, target * 10); putBE(d, 6, 1013);
    simFrame(0x372, d);
    memset(d, 0, 8); putBE(d, 0, (273.15f + 400 + rpm / 10) * 10); putBE(d, 2, (273.15f + 395 + rpm / 10) * 10);
    simFrame(0x373, d);
  }
  if (now - t200 >= 200) {                                 // 5 Hz
    t200 = now;
    static float iat = 32;                                               // heat soak: creeps up on boost, recovers off boost
    iat += ((boost > 0 ? 32 + boost * 0.15f : 32) - iat) * 0.02f;
    memset(d, 0, 8); putBE(d, 0, (273.15f + clt) * 10); putBE(d, 2, (273.15f + iat) * 10); putBE(d, 4, (273.15f + 28) * 10); putBE(d, 6, (273.15f + oilT) * 10);
    simFrame(0x3E0, d);
    memset(d, 0, 8); putBE(d, 0, (273.15f + 70) * 10); putBE(d, 4, 720);       // ethanol 72 %
    simFrame(0x3E1, d);
    memset(d, 0, 8); putBE(d, 0, 420);                                        // 42 L fuel
    simFrame(0x3E2, d);
    memset(d, 0, 8); d[4] = (uint8_t)simTrim;                                 // rotary trim pot 1 = trim position
    simFrame(0x3E4, d);
  }
}

void Sim_Set(bool on)
{
  if (on && lastRealMs && millis() - lastRealMs < 1000) { Serial.println("Simulator refused: real ECU data present"); return; }
  simOn = on;
  Serial.printf("Simulator %s\n", on ? "ON" : "OFF");
}
bool Sim_Active(void) { return simOn; }

// GPIO19/20 are also the ESP32-S3's built-in USB pins (D-/D+). At power-up the
// internal USB PHY owns those pads and has a pull-up on D+ (GPIO20). Receiving on
// GPIO19 still works, but the CAN TX signal on GPIO20 may never reach the pin.
// Hand both pads back to normal GPIO use before starting CAN.
static void releaseUsbPads(void)
{
  SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PAD_PULL_OVERRIDE);
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP);
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DM_PULLUP);
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
}

static bool canHwOk = false;

// Install and start the CAN controller. Called from the CAN task so that the driver's
// interrupt is allocated on core 0, away from the display refill interrupt on core 1.
static bool startCanHw(void)
{
#if CFG_ENABLE_CAN_HW
  releaseUsbPads();
  // NORMAL mode: needed now that the gauge transmits keypad frames
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
  g.rx_queue_len = 256;
  g.tx_queue_len = 16;
  twai_timing_config_t t = TWAI_TIMING_CONFIG_1MBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if (twai_driver_install(&g, &t, &f) != ESP_OK || twai_start() != ESP_OK) {
    Serial.println("CAN start FAILED");
    return false;
  }
  Serial.printf("CAN running (1 Mbit/s) on core %d, emulating Haltech keypad B\n", xPortGetCoreID());
  return true;
#else
  Serial.println("CAN hardware DISABLED (CFG_ENABLE_CAN_HW 0)");
  return false;
#endif
}

static void can_task(void *arg)
{
  twai_message_t msg;
  uint32_t lastCheck = 0, lastButtons = 0, lastKeepalive = 0, lastGuard = 0;
  canHwOk = startCanHw();

  while (1) {
    if (!canHwOk) vTaskDelay(pdMS_TO_TICKS(5));
    else if (twai_receive(&msg, pdMS_TO_TICKS(5)) == ESP_OK && !msg.rtr) {
      storeFrame(msg);
      if (msg.identifier == 0x360) {
        lastRealMs = millis();
        if (simOn) { simOn = false; Serial.println("Simulator OFF: real ECU data seen"); }
      }
      uint32_t id = msg.identifier;
      bool toKeypad = (id & 0x7F) == KP_NODE && id >= 0x200 && id <= 0x67F && id != KP_SDO_TX_ID;
      if (id == 0x000 && msg.data_length_code >= 2) handleNmt(msg);
      else if (id == KP_SDO_RX_ID) { logNodeFrame(msg); if (msg.data_length_code >= 4) handleSdo(msg); }
      else if (toKeypad) logNodeFrame(msg);
      else if (msg.data_length_code == 8) decode(msg);
    }

    uint32_t now = millis();
    if (simOn) simStep(now);
    // Keypad frames only go out while the real ECU is talking (never because of the simulator)
    bool ecuAlive = lastRealMs != 0 && (now - lastRealMs) < 500;
    bool txOk = ecuAlive && (int32_t)(now - txBackoffUntil) >= 0;

    static bool bootSent = false;
    if (txOk && !bootSent) {                       // CANopen boot-up message, once
      sendBootup();
      bootSent = true;
    }
    // Button states (PDO) are only sent once the ECU has started the keypad
    if (txOk && kpState == NMT_OPER && now - lastButtons >= KP_BUTTON_MS)      { lastButtons = now;   sendButtons(now); }
    if (txOk && now - lastKeepalive >= KP_KEEPALIVE_MS) { lastKeepalive = now; sendKeepalive(); }

    if (!canHwOk) continue;

    // Every 200 ms: if frames are piling up unacknowledged, abort them and pause 2 s
    if (now - lastGuard >= 200) {
      lastGuard = now;
      twai_status_info_t g;
      if (twai_get_status_info(&g) == ESP_OK && g.msgs_to_tx >= 3 && g.tx_error_counter >= 128) {
        twai_stop();                 // aborts the frame stuck retrying
        twai_start();
        txBackoffUntil = now + 2000;
        dbgBackoffs++;
      }
    }

    // Once a second, make sure the controller is still running
    if (millis() - lastCheck > 1000) {
      lastCheck = millis();
      twai_status_info_t s;
      if (twai_get_status_info(&s) == ESP_OK) {
        Serial.printf("KP state:%d tx_err:%lu to_tx:%lu bus_err:%lu | ecu:%s backoffs:%lu | kp:%02X sdo_rx:%lu nmt_rx:%lu\n",
                      s.state, s.tx_error_counter, s.msgs_to_tx, s.bus_error_count,
                      (hd.t360 && millis() - hd.t360 < 500) ? "yes" : "no",
                      dbgBackoffs, kpState, dbgSdoRx, dbgNmtRx);
        if (s.state == TWAI_STATE_BUS_OFF)      twai_initiate_recovery();
        else if (s.state == TWAI_STATE_STOPPED) twai_start();
      }
    }
  }
}

void Haltech_CAN_Init(void)
{
  hd.baro_kpa = 101.3f;
  // The task starts the CAN controller itself, so its interrupt lands on core 0
  xTaskCreatePinnedToCore(can_task, "haltech_can", 4096, NULL, 5, NULL, 0);
}
