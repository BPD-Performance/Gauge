#include "Setup_State.h"
#include <Preferences.h>
#include <esp_mac.h>

static uint8_t st = SETUP_NONE;
static String  pid, pname;

void Setup_Begin(bool hadSavedSettings)
{
  Preferences p;
  p.begin("bpdsetup", false);
  if (p.isKey("state")) st = p.getUChar("state", SETUP_NONE);
  else {
    // First run of this firmware. A gauge that already had saved settings counts as configured.
    st = hadSavedSettings ? SETUP_DONE : SETUP_NONE;
    p.putUChar("state", st);
  }
  if (st > SETUP_DONE) st = SETUP_NONE;
  pid = p.getString("pid", "");
  pname = p.getString("pname", "");
  p.end();
}

uint8_t Setup_State(void) { return st; }

void Setup_SetState(uint8_t s)
{
  if (s > SETUP_DONE) return;
  st = s;
  Preferences p; p.begin("bpdsetup", false); p.putUChar("state", st); p.end();
}

String Setup_ProfileId(void) { return pid; }
String Setup_ProfileName(void) { return pname; }

void Setup_SetProfile(const String &id, const String &name)
{
  pid = id.substring(0, 24); pname = name.substring(0, 24);
  Preferences p; p.begin("bpdsetup", false);
  p.putString("pid", pid); p.putString("pname", pname); p.end();
}

String Setup_BleName(void)
{
  uint8_t m[6] = {0};
  esp_read_mac(m, ESP_MAC_BT);
  char b[24];
  snprintf(b, sizeof(b), "BPD Gauge %02X%02X", m[4], m[5]);
  return String(b);
}
