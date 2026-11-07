// BPD Gauge - amber Commander-style swipe pages with live Haltech CAN data, a CAN keypad page
// (Haltech keypad B emulation: boost trim, rotational idle), full-screen warnings,
// data logging to the TF card, and a Bluetooth link to the phone/PC app.
// Board: Waveshare ESP32-S3-Touch-LCD-2.1
//
// Tools: Board "Waveshare ESP32-S3-Touch-LCD-2.1", Flash Mode QIO 80MHz, PSRAM Enabled,
//        USB CDC On Boot Disabled, Erase All Flash Disabled,
//        Partition Scheme "16M Flash (3MB APP/9.9MB FATFS)".
// Flash and monitor through the USB-to-UART Type-C port (CAN uses GPIO19/20).

#include "LVGL_Driver.h"
#include "Haltech_CAN.h"
#include "Settings.h"
#include "Setup_State.h"
#include "Gauge_UI.h"
#include "Data_Log.h"
#include "GPS_Lap.h"
#include "BLE_Link.h"
#include "BPD_Config.h"

void setup()
{
  Serial.begin(115200);
  Serial.println("\nBPD gauge starting");

  Settings_Load();
  Setup_Begin(Settings_HadSaved());            // saved settings (or defaults on first run)

  I2C_Init();
  TCA9554PWR_Init(0x00);      // all expander pins as outputs
  Set_EXIO(EXIO_PIN8, Low);   // buzzer off

  LCD_Init();                 // ST7701 panel, touch, backlight
  Set_Backlight(cfg.brightness);
  Lvgl_Init();
  Gauge_UI_Init();
  Log_Init();                 // TF card and data logging (after the LCD: shares its setup pins)
  Gps_Init();                 // GPS on the UART header, if switched on in the app

  Haltech_CAN_Init();         // CAN task on core 0
#if CFG_ENABLE_BLE
  BLE_Init();                 // Bluetooth link to the app
#else
  Serial.println("Bluetooth DISABLED (CFG_ENABLE_BLE 0)");
#endif
#if CFG_AUTO_SIM
  Sim_Set(true);              // bench simulator from power-up
#endif
}

void loop()
{
  // Settings changed from the app: save and rebuild the screen
  GaugeSettings n;
  if (Settings_TakePending(n)) {
    cfg = n;
    Settings_Save();
    Set_Backlight(cfg.brightness);
    Gauge_UI_Rebuild();
  }

  Lvgl_Loop();                // LVGL drawing on core 1
  vTaskDelay(pdMS_TO_TICKS(5));
}
