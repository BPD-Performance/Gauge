// BLE link: pairing with a PIN, a line-based JSON command channel, and a live data stream.
//
// Commands (write to RX, end each with \n). Replies come back on TX.
//   {"cmd":"info"}                         -> firmware/protocol info
//   {"cmd":"get"}                          -> all settings
//   {"cmd":"set", <any setting fields>}    -> change settings (car must be stopped)
//   {"cmd":"defaults"}                     -> reset settings to defaults (car must be stopped)
//   {"cmd":"key","btn":1}                  -> one keypad B button press (1-15)
//   {"cmd":"hold","btn":3,"on":1}          -> hold / release a keypad B button
//   {"cmd":"sub","ids":[864,865],"hz":20}  -> choose CAN IDs and rate for LIVE ([] = all, hz 1-50)
//   {"cmd":"setupset","state":2,"pid":"abc","pname":"Skyline"}  -> setup state (0 none, 1 basic, 2 configured) and the car profile on the gauge
//   {"cmd":"pls"} / {"cmd":"pput","pid":..,"o":0,"d":"hex","last":1} / {"cmd":"pget","pid":..,"o":0} / {"cmd":"pdel","pid":..}   profiles on the SD card
//   {"cmd":"ping"}                         -> {"ok":true,"pong":1}
//   {"cmd":"warns"}                        -> active warnings [{"id","name","val","unit","ack"}]
//   {"cmd":"ack"}                          -> dismiss the warning popup on the gauge
//   {"cmd":"time","t":1761900000,"tz":600} -> phone clock (unix seconds, UTC) and local offset (minutes), dates the logs
//   {"cmd":"gpsget"} / {"cmd":"gpsset","on":1,"pulse":1,"ppk":5000}   GPS settings (take effect after a restart)
//   {"cmd":"trkget"} / {"cmd":"trkset","name":"...","g":[[lat,lon,bearing,width],...]} / {"cmd":"trkclr"}
//   {"cmd":"lapreset"}                     -> forget the best lap
//   {"cmd":"restart"}                      -> restart the gauge
//   LIVE type 0x04, GPS and lap timer, about 5 times a second:
//     [0x04][seq][fix u8][sats u8][lat i32 x1e7][lon i32 x1e7][speed u16 km/h x10][course u16 x10]
//     [lap u16][current ms u32][last ms u32][best ms u32][delta i32 ms, INT32_MIN = none][sector u8]
//     [progress u8 0-255][gates u8][module talking u8: valid NMEA in the last 3 s, fix or not]
//   {"cmd":"logget"}                       -> logging settings and state
//   {"cmd":"logset", <logging fields>}     -> change logging settings
//   {"cmd":"log","on":1}                   -> start / stop a log by hand
//   {"cmd":"ls"}                           -> logs on the SD card
//   {"cmd":"rm","n":"L00003.BPL"}          -> delete one log;  {"cmd":"rmall"} -> delete every log
//   {"cmd":"rd","n":"L00003.BPL","o":0,"l":8192}
//        -> file bytes sent on LIVE as type 0x03 packets [0x03][0][offset u32][data...],
//           then a TX reply {"ok":true,"o":offset,"l":bytes sent} (l = 0 at the end of the file)
//
// LIVE packets (binary, little endian):
//   type 0x01 CAN frames : [0x01][seq] then repeated { id u16, len u8, data[8] }
//   type 0x02 status     : [0x02][seq][speed_kmh x10 u16][gear i8][warn u8][kp_state u8]
//                          [reserved u8][held_buttons u16][sim u8][warn_mask u16][reserved u8]
//                          [log u8: bit0 SD card ok, bit1 recording]
//   {"cmd":"sim","on":1}                   -> bench simulator on/off (fake Haltech data)
//   {"cmd":"fps","on":1}                   -> show frame rate and draw time on the gauge (debug, not saved)
//   {"cmd":"pclk","mhz":12}                -> test tool: LCD pixel clock 8-18 MHz (not saved). No mhz = report
//   {"cmd":"pclkedge","neg":1}             -> test tool: LCD PCLK edge (saved, gauge restarts). No neg = report
//   {"cmd":"drive","cap":1}                -> test tool: LCD bus drive strength 0-3, default 2 (not saved). No cap = report
//   {"cmd":"lcdinit"}                      -> test tool: re-send the ST7701 panel registers
//   {"cmd":"cachesync","on":0}             -> test tool: cache write-back per frame (default 1, not saved)
//   {"cmd":"cacheflush"}                   -> test tool: one-off cache write-back of both frame buffers
//   {"cmd":"lcdpause","ms":2000}           -> test tool: stop drawing for ms (max 5000)
//   {"cmd":"fbread"}                       -> test tool: read both frame buffers through the cache
//   {"cmd":"cacheinval"}                   -> test tool: write back and invalidate both frame buffers

#include "BLE_Link.h"
#include <esp_heap_caps.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLESecurity.h>
#include "Settings.h"
#include "Setup_State.h"
#include "Haltech_CAN.h"
#include "Gauge_Calc.h"
#include "Data_Log.h"
#include "GPS_Lap.h"
#include "Display_ST7701.h"
#include "Gauge_UI.h"

#define BLE_NAME      "BPD Gauge"
#define SVC_UUID      "b9d00001-6a1e-4c2a-9e3d-6b5a8f0c1d01"
#define RX_UUID       "b9d00002-6a1e-4c2a-9e3d-6b5a8f0c1d01"
#define TX_UUID       "b9d00003-6a1e-4c2a-9e3d-6b5a8f0c1d01"
#define LIVE_UUID     "b9d00004-6a1e-4c2a-9e3d-6b5a8f0c1d01"
#define FW_VERSION    "1.2-basic"
#define PROTO_VERSION 1

static BLEServer *server = nullptr;
static BLECharacteristic *txChar = nullptr, *liveChar = nullptr;
static volatile bool connected = false;
static volatile int  nConn = 0;          // phones / head units connected (up to 2 at once)
#define MAX_CLIENTS 2
static String curId;                     // "id" of the command being answered, echoed in the reply
static uint16_t prevHz = 10;             // the other client's update rate, when two are connected

// Live subscription
static portMUX_TYPE subMux = portMUX_INITIALIZER_UNLOCKED;
static uint16_t subIds[CAN_MAX_FRAMES];
static int      subCount = 0;           // 0 = every ID on the bus
static uint16_t subHz = 10;

// Incoming command line buffer (BLE writes can arrive in pieces)
static String rxLine;
static QueueHandle_t cmdQueue;          // complete lines, handled in the BLE task

// ---------------- Helpers ----------------
static uint16_t payloadSize(void)
{
  uint16_t mtu = 23;
  if (server && connected) {
    uint16_t m = server->getPeerMTU(server->getConnId());
    if (m >= 23) mtu = m;
  }
  uint16_t n = mtu - 3;
  return n > 500 ? 500 : n;
}

// Send a text reply on TX, split to fit the connection, ending with '\n'
static void sendLine(const String &json)
{
  if (!connected) return;
  // With two apps connected, both hear every reply, so each reply carries the command's id
  String s = (curId.length() && json.startsWith("{")) ? "{\"id\":" + curId + "," + json.substring(1) + "\n" : json + "\n";
  uint16_t chunk = payloadSize();
  for (size_t i = 0; i < s.length(); i += chunk) {
    size_t n = min((size_t)chunk, s.length() - i);
    txChar->setValue((uint8_t *)s.c_str() + i, n);
    txChar->notify();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// Minimal reader for our flat JSON commands: "key":value  (value may be a number, "text" or [list])
static bool jsonGet(const String &j, const char *key, String &out)
{
  String pat = String("\"") + key + "\"";
  int k = j.indexOf(pat);
  if (k < 0) return false;
  int c = j.indexOf(':', k + pat.length());
  if (c < 0) return false;
  int i = c + 1;
  while (i < (int)j.length() && j[i] == ' ') i++;
  if (i >= (int)j.length()) return false;
  int e;
  if (j[i] == '"')      { e = j.indexOf('"', i + 1); if (e < 0) return false; out = j.substring(i + 1, e); return true; }
  if (j[i] == '[')      { e = j.indexOf(']', i);     if (e < 0) return false; out = j.substring(i + 1, e); return true; }
  e = i;
  while (e < (int)j.length() && j[e] != ',' && j[e] != '}' && j[e] != ' ') e++;
  out = j.substring(i, e);
  return true;
}

// Profile ids and names go back out inside JSON, so keep only safe characters.
static String cleanId(const String &s)
{
  String o;
  for (size_t i = 0; i < s.length() && o.length() < 24; i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ') o += c;
  }
  return o;
}


// ---------------- Commands ----------------
static uint32_t quietUntil = 0;              // live data paused until this time (log downloads)

static void handleCommandInner(const String &line);
static void handleCommand(const String &line)
{
  String id;
  curId = (jsonGet(line, "id", id) && id.length() && id.length() < 12 && isDigit(id[0])) ? id : String();
  handleCommandInner(line);
  curId = "";
}

static void handleCommandInner(const String &line)
{
  String cmd, v;
  if (!jsonGet(line, "cmd", cmd)) { sendLine("{\"ok\":false,\"err\":\"no cmd\"}"); return; }

  if (cmd == "ping") {
    sendLine("{\"ok\":true,\"pong\":1}");
  }
  else if (cmd == "info") {
    String s = "{\"ok\":true,\"fw\":\"" FW_VERSION "\",\"proto\":";
    s += PROTO_VERSION;
    s += ",\"name\":\"" + Setup_BleName() + "\",\"maxIds\":";
    s += CAN_MAX_FRAMES;
    s += ",\"setup\":"; s += Setup_State();
    s += ",\"pid\":\"" + cleanId(Setup_ProfileId()) + "\",\"pname\":\"" + cleanId(Setup_ProfileName()) + "\"";
    s += "}";
    sendLine(s);
  }
  else if (cmd == "setupset") {
    // state 0/1/2 and (optionally) which car profile is now on the gauge. PIN and BLE name are never part of a profile.
    if (jsonGet(line, "state", v)) {
      int st = v.toInt();
      if (st < 0 || st > 2) { sendLine("{\"ok\":false,\"err\":\"state 0-2\"}"); return; }
      Setup_SetState((uint8_t)st);
    }
    String id, nm;
    if (jsonGet(line, "pid", id)) { jsonGet(line, "pname", nm); Setup_SetProfile(cleanId(id), cleanId(nm)); }
    sendLine("{\"ok\":true}");
  }
  else if (cmd == "get") {
    String s;
    Settings_ToJson(s);
    sendLine("{\"ok\":true,\"settings\":" + s + "}");
  }
  else if (cmd == "set") {
    GaugeSettings n = cfg;
    Settings_ApplyFrom(n, [&](const char *k, String &out) -> bool { return jsonGet(line, k, out); });
    Settings_RequestApply(n);
    sendLine("{\"ok\":true,\"saved\":1}");
  }
  else if (cmd == "defaults") {
    GaugeSettings d;
    Settings_MakeDefaults(d);
    if (Setup_State() != SETUP_DONE) Settings_BasicPreset(d);
    Settings_RequestApply(d);
    sendLine("{\"ok\":true,\"saved\":1}");
  }
  else if (cmd == "key") {
    int b = jsonGet(line, "btn", v) ? v.toInt() : 0;
    if (b < 1 || b > 15) { sendLine("{\"ok\":false,\"err\":\"btn 1-15\"}"); return; }
    Keypad_Press(b);
    sendLine("{\"ok\":true}");
  }
  else if (cmd == "hold") {
    int b = jsonGet(line, "btn", v) ? v.toInt() : 0;
    bool on = jsonGet(line, "on", v) ? v.toInt() != 0 : false;
    if (b < 1 || b > 15) { sendLine("{\"ok\":false,\"err\":\"btn 1-15\"}"); return; }
    Keypad_SetHold(b, on);
    sendLine("{\"ok\":true}");
  }
  else if (cmd == "warns") {
    String w;
    Gauge_WarnJson(w);
    sendLine("{\"ok\":true,\"warns\":" + w + "}");
  }
  else if (cmd == "ack") {
    Gauge_AckWarnings();
    sendLine("{\"ok\":true}");
  }
  else if (cmd == "time") {
    String tz;
    int tzMin = jsonGet(line, "tz", tz) ? tz.toInt() : LOG_TZ_KEEP;
    if (jsonGet(line, "t", v)) Log_SetTime((uint32_t)strtoul(v.c_str(), NULL, 10), tzMin);
    sendLine("{\"ok\":true}");
  }
  else if (cmd == "logget") {
    String s;
    Log_StatusJson(s);
    sendLine("{\"ok\":true,\"log\":" + s + "}");
  }
  else if (cmd == "logset") {
    Log_ApplyConfig([&](const char *k, String &out) -> bool { return jsonGet(line, k, out); });
    sendLine("{\"ok\":true}");
  }
  else if (cmd == "log") {
    bool on = jsonGet(line, "on", v) ? v.toInt() != 0 : !Log_Recording();
    if (!Log_SdOk()) { sendLine("{\"ok\":false,\"err\":\"no SD card\"}"); return; }
    if (on != Log_Recording()) Log_ManualToggle();
    sendLine("{\"ok\":true}");
  }
  else if (cmd == "ls") {
    String s;
    Log_ListJson(s);
    sendLine("{\"ok\":true,\"files\":" + s + "}");
  }
  else if (cmd == "rm") {
    String n;
    jsonGet(line, "n", n);
    sendLine(Log_Delete(n) ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"could not delete\"}");
  }
  else if (cmd == "rmall") {
    String s = "{\"ok\":true,\"deleted\":";
    s += Log_DeleteAll(); s += "}";
    sendLine(s);
  }
  else if (cmd == "rd") {
    String n;
    jsonGet(line, "n", n);
    uint32_t off = jsonGet(line, "o", v) ? strtoul(v.c_str(), NULL, 10) : 0;
    uint32_t len = jsonGet(line, "l", v) ? constrain(v.toInt(), 1, 16384) : 8192;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!buf) { sendLine("{\"ok\":false,\"err\":\"no memory\"}"); return; }
    int32_t got = Log_Read(n, off, buf, len);
    if (got < 0) { heap_caps_free(buf); sendLine("{\"ok\":false,\"err\":\"no such log\"}"); return; }
    uint16_t chunk = payloadSize() - 6;
    uint8_t pkt[512];
    for (int32_t i = 0; i < got; i += chunk) {
      uint32_t n2 = min((int32_t)chunk, got - i), o = off + i;
      pkt[0] = 0x03; pkt[1] = 0;
      memcpy(pkt + 2, &o, 4);
      memcpy(pkt + 6, buf + i, n2);
      liveChar->setValue(pkt, n2 + 6);
      liveChar->notify();
      vTaskDelay(pdMS_TO_TICKS(8));             // give the Bluetooth stack time to send; it drops packets when rushed
    }
    quietUntil = millis() + 400;                // hold back live data while a download is running
    heap_caps_free(buf);
    String s = "{\"ok\":true,\"o\":";
    s += (unsigned long)off; s += ",\"l\":"; s += got; s += "}";
    sendLine(s);
  }
  else if (cmd == "pls") {
    String s; Prof_ListJson(s);
    sendLine(String("{\"ok\":true,\"sd\":") + (Log_SdOk() ? 1 : 0) + ",\"profiles\":" + s + "}");
  }
  else if (cmd == "pput") {                      // chunked upload: pid, o = byte offset, d = hex, last = 1 on the final chunk
    String id, hex;
    jsonGet(line, "pid", id); jsonGet(line, "d", hex);
    uint32_t off = jsonGet(line, "o", v) ? strtoul(v.c_str(), NULL, 10) : 0;
    bool last = jsonGet(line, "last", v) && v.toInt() != 0;
    uint8_t b[512]; uint32_t n = 0;
    for (size_t i = 0; i + 1 < hex.length() && n < sizeof(b); i += 2) b[n++] = (uint8_t)strtoul(hex.substring(i, i + 2).c_str(), NULL, 16);
    sendLine(Prof_Write(id, off, b, n, last) ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"could not write\"}");
  }
  else if (cmd == "pget") {                      // chunked download: pid, o = offset; replies d = hex (up to 400 bytes), n = bytes
    String id; jsonGet(line, "pid", id);
    uint32_t off = jsonGet(line, "o", v) ? strtoul(v.c_str(), NULL, 10) : 0;
    uint8_t b[400];
    int32_t got = Prof_Read(id, off, b, sizeof(b));
    if (got < 0) { sendLine("{\"ok\":false,\"err\":\"no such profile\"}"); return; }
    String s = "{\"ok\":true,\"n\":" + String(got) + ",\"d\":\"";
    char h[3];
    for (int32_t i = 0; i < got; i++) { snprintf(h, sizeof(h), "%02x", b[i]); s += h; }
    s += "\"}";
    sendLine(s);
  }
  else if (cmd == "pdel") {
    String id; jsonGet(line, "pid", id);
    sendLine(Prof_Delete(id) ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"could not delete\"}");
  }
  else if (cmd == "gpsget") {
    String s; Gps_ConfigJson(s);
    sendLine("{\"ok\":true,\"gps\":" + s + "}");
  }
  else if (cmd == "gpsset") {
    Gps_ApplyConfig([&](const char *k, String &out) -> bool { return jsonGet(line, k, out); });
    sendLine("{\"ok\":true,\"restart\":1}");
  }
  else if (cmd == "trkget") {
    String s; Lap_TrackJson(s);
    sendLine("{\"ok\":true,\"track\":" + s + "}");
  }
  else if (cmd == "trkset") {
    sendLine(Lap_SetTrack(line) ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"bad track\"}");
  }
  else if (cmd == "trkclr") { Lap_ClearTrack(); sendLine("{\"ok\":true}"); }
  else if (cmd == "lapreset") { Lap_ResetBest(); sendLine("{\"ok\":true}"); }
  else if (cmd == "restart") {                 // e.g. after turning the GPS on or off
    sendLine("{\"ok\":true,\"restarting\":1}");
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP.restart();
  }
  else if (cmd == "fps") {
    Gauge_SetFps(jsonGet(line, "on", v) ? v.toInt() != 0 : false);
    sendLine("{\"ok\":true}");
  }
  else if (cmd == "sim") {
    bool on = jsonGet(line, "on", v) ? v.toInt() != 0 : false;
    Sim_Set(on);
    sendLine(Sim_Active() == on ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"real ECU data present\"}");
  }
  else if (cmd == "sub") {
    uint16_t ids[CAN_MAX_FRAMES];
    int n = 0;
    if (jsonGet(line, "ids", v)) {
      int start = 0;
      for (int i = 0; i <= (int)v.length() && n < CAN_MAX_FRAMES; i++) {
        if (i == (int)v.length() || v[i] == ',') {
          String t = v.substring(start, i); t.trim();
          if (t.length()) ids[n++] = (uint16_t)t.toInt();
          start = i + 1;
        }
      }
    }
    int hz = jsonGet(line, "hz", v) ? constrain(v.toInt(), 1, 50) : subHz;
    portENTER_CRITICAL(&subMux);
    memcpy(subIds, ids, n * sizeof(uint16_t));
    subCount = n;
    prevHz = subHz;
    subHz = hz;
    portEXIT_CRITICAL(&subMux);
    String s = "{\"ok\":true,\"ids\":";
    s += n; s += ",\"hz\":"; s += hz; s += "}";
    sendLine(s);
  }
  else if (cmd == "pclk") {
    String low = line; low.toLowerCase();                // phone keyboards like to capitalise MHz
    if (jsonGet(low, "mhz", v)) {
      uint32_t hz = (uint32_t)lroundf(v.toFloat() * 1000000.0f);
      if (!LCD_SetPclk(hz)) { sendLine("{\"ok\":false,\"err\":\"mhz 8-18\"}"); return; }
    } else if (low.indexOf("mhz") >= 0) {
      sendLine("{\"ok\":false,\"err\":\"could not read mhz, use {\\\"cmd\\\":\\\"pclk\\\",\\\"mhz\\\":12}\"}");
      return;
    }
    String s = "{\"ok\":true,\"pclk_mhz\":";
    s += String(LCD_GetPclk() / 1e6f, 1);
    s += "}";
    sendLine(s);
  }
  else if (cmd == "pclkedge") {
    String low = line; low.toLowerCase();
    if (jsonGet(low, "neg", v)) {
      bool neg = v.toInt() != 0;
      LCD_SavePclkNeg(neg);
      sendLine(String("{\"ok\":true,\"pclk_neg\":") + (neg ? 1 : 0) + ",\"restarting\":1}");
      vTaskDelay(pdMS_TO_TICKS(500));
      ESP.restart();
    }
    sendLine(String("{\"ok\":true,\"pclk_neg\":") + (LCD_GetPclkNeg() ? 1 : 0) + "}");
  }
  else if (cmd == "cachesync") {
    if (jsonGet(line, "on", v)) LCD_SetCacheSync(v.toInt() != 0);
    int32_t err; uint32_t okN, failN;
    LCD_GetCacheStats(&err, &okN, &failN);
    sendLine(String("{\"ok\":true,\"cachesync\":") + (LCD_GetCacheSync() ? 1 : 0) +
             ",\"wb_ok\":" + okN + ",\"wb_fail\":" + failN + ",\"wb_err\":" + err + "}");
  }
  else if (cmd == "cacheflush") {
    LCD_RequestCacheFlush();
    sendLine("{\"ok\":true,\"cacheflush\":1}");
  }
  else if (cmd == "lcdpause") {
    int ms = jsonGet(line, "ms", v) ? constrain(v.toInt(), 100, 5000) : 2000;
    LCD_RequestPause(ms);
    sendLine(String("{\"ok\":true,\"pause_ms\":") + ms + "}");
  }
  else if (cmd == "fbread") {
    LCD_RequestFbRead();
    sendLine("{\"ok\":true,\"fbread\":1}");
  }
  else if (cmd == "cacheinval") {
    LCD_RequestCacheInval();
    sendLine("{\"ok\":true,\"cacheinval\":1}");
  }
  else if (cmd == "lcdinit") {
    if (Log_SdOk()) { sendLine("{\"ok\":false,\"err\":\"not while the SD card is in use (shares the LCD setup pins)\"}"); return; }
    LCD_RequestReinit();
    sendLine("{\"ok\":true,\"lcdinit\":1}");
  }
  else if (cmd == "drive") {
    String low = line; low.toLowerCase();
    if (jsonGet(low, "cap", v) && !LCD_SetDrive(v.toInt())) { sendLine("{\"ok\":false,\"err\":\"cap 0-3\"}"); return; }
    sendLine(String("{\"ok\":true,\"drive\":") + LCD_GetDrive() + ",\"pclk_mhz\":" + String(LCD_GetPclk() / 1e6f, 1) +
             ",\"pclk_neg\":" + (LCD_GetPclkNeg() ? 1 : 0) + "}");
  }
  else {
    sendLine("{\"ok\":false,\"err\":\"unknown cmd\"}");
  }
}

// ---------------- BLE callbacks ----------------
class ServerCB : public BLEServerCallbacks {
  // Up to two apps at once (e.g. the head unit and a phone): keep advertising until both slots are used
  void onConnect(BLEServer *s) override {
    nConn++; connected = true;
    Serial.printf("BLE: connected (%d)\n", (int)nConn);
    if (nConn < MAX_CLIENTS) BLEDevice::startAdvertising();
  }
  void onDisconnect(BLEServer *s) override {
    if (nConn > 0) nConn--;
    connected = nConn > 0;
    Serial.printf("BLE: disconnected (%d left)\n", (int)nConn);
    BLEDevice::startAdvertising();
  }
};

// A command ends at a newline, or when its { } braces balance (handy when typing
// commands into nRF Connect, which can't easily send a newline).
static int braceDepth = 0;

static void queueLine(void)
{
  rxLine.trim();
  if (rxLine.length()) {
    String *line = new String(rxLine);
    if (xQueueSend(cmdQueue, &line, 0) != pdTRUE) delete line;
  }
  rxLine = "";
  braceDepth = 0;
}

class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String part = c->getValue();
    for (size_t i = 0; i < part.length(); i++) {
      char ch = part[i];
      if (ch == '\n' || ch == '\r') { queueLine(); continue; }
      if (rxLine.length() >= 2048) { rxLine = ""; braceDepth = 0; }
      rxLine += ch;
      if (ch == '{') braceDepth++;
      if (ch == '}' && --braceDepth <= 0) queueLine();
    }
  }
};

// ---------------- Live stream ----------------
static void sendLive(uint8_t &seq)
{
  static CanFrame all[CAN_MAX_FRAMES];
  int n = CAN_CopyFrames(all, CAN_MAX_FRAMES);

  uint16_t ids[CAN_MAX_FRAMES];
  int sc;
  portENTER_CRITICAL(&subMux);
  sc = nConn > 1 ? 0 : subCount;                // two apps connected: send everything, each picks what it needs
  memcpy(ids, subIds, sc * sizeof(uint16_t));
  portEXIT_CRITICAL(&subMux);

  uint16_t maxLen = payloadSize();
  uint8_t pkt[512];
  int len = 0;
  auto flush = [&]() {
    if (len > 2) { liveChar->setValue(pkt, len); liveChar->notify(); }
    len = 0;
  };

  for (int i = 0; i < n; i++) {
    if (sc) {                                   // only subscribed IDs
      bool want = false;
      for (int k = 0; k < sc; k++) if (ids[k] == all[i].id) { want = true; break; }
      if (!want) continue;
    }
    if (millis() - all[i].ms > 2000) continue;  // skip messages that have stopped
    if (len == 0) { pkt[0] = 0x01; pkt[1] = seq++; len = 2; }
    if (len + 11 > maxLen) { flush(); pkt[0] = 0x01; pkt[1] = seq++; len = 2; }
    pkt[len++] = all[i].id & 0xFF;
    pkt[len++] = all[i].id >> 8;
    pkt[len++] = all[i].len;
    memcpy(&pkt[len], all[i].data, 8);
    len += 8;
  }
  flush();

  // Gauge status
  bool sv, gv;
  float spd = Calc_SpeedKmh(&sv);
  int g = Calc_Gear(&gv);
  uint16_t s10 = sv ? (uint16_t)lroundf(spd * 10) : 0xFFFF;
  uint16_t held = 0;
  for (int b = 1; b <= 15; b++) if (Keypad_GetHold(b)) held |= (1u << (b - 1));
  uint8_t st[15];
  st[14] = (Log_SdOk() ? 1 : 0) | (Log_Recording() ? 2 : 0);
  uint16_t wm = Gauge_WarnMask();
  st[11] = wm & 0xFF; st[12] = wm >> 8;
  st[13] = 0;                                   // was dyno mode (removed)
  st[10] = Sim_Active() ? 1 : 0;
  st[0] = 0x02; st[1] = seq++;
  st[2] = s10 & 0xFF; st[3] = s10 >> 8;
  st[4] = gv ? (uint8_t)(int8_t)g : 0x7F;
  st[5] = 0;                                    // warning code (reserved for the app)
  st[6] = 0;                                    // keypad state (reserved)
  st[7] = 0;                                    // was WiFi (removed)
  st[8] = held & 0xFF; st[9] = held >> 8;
  liveChar->setValue(st, 15);
  liveChar->notify();

  // GPS and lap timer
  static uint32_t lastGps = 0;
  if (Gps_Enabled() && millis() - lastGps >= 200) {
    lastGps = millis();
    GpsState gg; Gps_Get(gg);
    LapState l; Lap_Get(l);
    uint8_t p[40];
    int32_t la = (int32_t)llround(gg.lat * 1e7), lo = (int32_t)llround(gg.lon * 1e7);
    uint16_t sp = (uint16_t)lroundf(gg.kmh * 10), co = (uint16_t)lroundf(gg.course * 10);
    p[0] = 0x04; p[1] = seq++; p[2] = Gps_Fresh() ? 1 : 0; p[3] = gg.sats;
    memcpy(p + 4, &la, 4); memcpy(p + 8, &lo, 4); memcpy(p + 12, &sp, 2); memcpy(p + 14, &co, 2);
    memcpy(p + 16, &l.lap, 2); memcpy(p + 18, &l.curMs, 4); memcpy(p + 22, &l.lastMs, 4); memcpy(p + 26, &l.bestMs, 4);
    memcpy(p + 30, &l.deltaMs, 4); p[34] = l.sector; p[35] = l.progress; p[36] = l.nGates; p[37] = Gps_Alive() ? 1 : 0;
    liveChar->setValue(p, 38);
    liveChar->notify();
  }
}

static void bleTask(void *arg)
{
  // (quietUntil is set by log downloads)
  uint8_t seq = 0;
  uint32_t lastLive = 0;
  while (1) {
    String *line;
    while (xQueueReceive(cmdQueue, &line, 0) == pdTRUE) {
      handleCommand(*line);
      delete line;
    }
    if (connected) {
      uint32_t period = 1000 / (nConn > 1 ? max(subHz, prevHz) : subHz);
      if (millis() - lastLive >= period && (int32_t)(millis() - quietUntil) >= 0) { lastLive = millis(); sendLive(seq); }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ---------------- Setup ----------------
void BLE_Init(void)
{
  cmdQueue = xQueueCreate(8, sizeof(String *));

  static String bleName = Setup_BleName();   // unique per gauge, so two gauges never look the same
  BLEDevice::init(bleName.c_str());
  BLEDevice::setMTU(517);

  // Pairing: the gauge shows the PIN (display only), the phone/PC types it in.
  // Bonded devices are remembered, so the PIN is only needed the first time.
  BLESecurity *sec = new BLESecurity();
  sec->setPassKey(true, Settings_BlePin());
  sec->setCapability(ESP_IO_CAP_OUT);
  sec->setAuthenticationMode(true, true, true);     // bonding, MITM (PIN), secure connections

  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCB());
  server->advertiseOnDisconnect(true);

  BLEService *svc = server->createService(SVC_UUID);
  const uint32_t ENC_R = BLECharacteristic::PROPERTY_READ_ENC;
  const uint32_t ENC_W = BLECharacteristic::PROPERTY_WRITE_ENC;

  BLECharacteristic *rx = svc->createCharacteristic(RX_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR | ENC_W);
  rx->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
  rx->setCallbacks(new RxCB());

  txChar = svc->createCharacteristic(TX_UUID,
      BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ | ENC_R);
  txChar->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);

  liveChar = svc->createCharacteristic(LIVE_UUID,
      BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ | ENC_R);
  liveChar->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);

  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();

  xTaskCreatePinnedToCore(bleTask, "ble_link", 6144, NULL, 3, NULL, 0);
  Serial.printf("BLE: advertising as \"%s\", PIN %06lu\n", bleName.c_str(), (unsigned long)Settings_BlePin());
}

bool BLE_Connected(void) { return connected; }
