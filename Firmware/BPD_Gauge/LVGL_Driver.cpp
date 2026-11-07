// LVGL glue for the Waveshare ESP32-S3-Touch-LCD-2.1
// Two PSRAM frame buffers owned by the RGB panel driver. LVGL draws a full frame
// straight into the buffer that is NOT on screen, then LCD_PresentFrame() swaps the
// panel to it and waits until the panel is actually reading it before LVGL is
// allowed to start drawing into the other buffer. This removes the tearing seen
// when values change (LVGL used to start drawing into the buffer still on screen).

#include "LVGL_Driver.h"
#include "BPD_Config.h"

lv_disp_drv_t disp_drv;

static lv_disp_draw_buf_t draw_buf;
void* buf1 = NULL;
void* buf2 = NULL;

void Lvgl_print(const char * buf) { }

// Set to 1 to print display timing once a second on the serial monitor (115200)
#define LVGL_DEBUG_FPS  0

#if LVGL_DEBUG_FPS
static uint32_t dbgFlushes = 0, dbgMisses = 0, dbgWaitUs = 0, dbgLastMs = 0, dbgLastFrames = 0;
#endif

// Time spent waiting for the panel to take each finished frame (read by the debug overlay in Gauge_UI.cpp)
static uint32_t flushSumUs = 0, flushN = 0;
void Lvgl_FlushStatsAndReset(uint32_t *frames, uint32_t *avgWaitUs)
{
  *frames = flushN;
  *avgWaitUs = flushN ? flushSumUs / flushN : 0;
  flushSumUs = 0; flushN = 0;
}

// Hand the finished frame to the RGB panel. In direct mode LVGL calls this once for every redrawn area of a frame;
// only the last call of a frame swaps the buffers.
void Lvgl_Display_LCD(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
#if LVGL_DEBUG_FPS
  uint32_t t0 = micros();
#endif
  bool ok = true;
  if (lv_disp_flush_is_last(disp_drv)) {
    uint32_t f0 = micros();
    ok = LCD_PresentFrame(color_p);
    flushSumUs += micros() - f0; flushN++;
  }
#if LVGL_DEBUG_FPS
  dbgWaitUs += micros() - t0;
  dbgFlushes++;
  if (!ok) dbgMisses++;
  uint32_t now = millis();
  if (now - dbgLastMs >= 1000) {
    uint32_t frames = LCD_FrameCount();
    Serial.printf("LCD: panel %lu Hz, LVGL %lu fps, avg wait %lu us, missed %lu\n",
                  (unsigned long)(frames - dbgLastFrames), (unsigned long)dbgFlushes,
                  (unsigned long)(dbgFlushes ? dbgWaitUs / dbgFlushes : 0), (unsigned long)dbgMisses);
    dbgLastFrames = frames; dbgLastMs = now;
    dbgFlushes = dbgMisses = dbgWaitUs = 0;
  }
#else
  (void)ok;
#endif
  lv_disp_flush_ready(disp_drv);
}

// Read the touch controller
void Lvgl_Touchpad_Read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data)
{
  Touch_Read_Data();
  if (touch_data.points != 0x00) {
    data->point.x = touch_data.x;
    data->point.y = touch_data.y;
    data->state = LV_INDEV_STATE_PR;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
  touch_data.x = 0;
  touch_data.y = 0;
  touch_data.points = 0;
  touch_data.gesture = NONE;
}

void example_increase_lvgl_tick(void *arg)
{
  lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

void Lvgl_Init(void)
{
  lv_init();
  esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, &buf1, &buf2);
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, ESP_PANEL_LCD_WIDTH * ESP_PANEL_LCD_HEIGHT);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = LCD_HEIGHT;
  disp_drv.ver_res = LCD_WIDTH;
  disp_drv.flush_cb = Lvgl_Display_LCD;
#if CFG_PARTIAL_REDRAW
  disp_drv.direct_mode = 1;      // LVGL draws only the changed areas straight into the screen-sized buffer and keeps the two buffers in step
  disp_drv.full_refresh = 0;
#else
  disp_drv.full_refresh = 1;     // every frame is drawn complete, so both buffers always hold a whole image
#endif
  disp_drv.draw_buf = &draw_buf;
  lv_disp_t *disp = lv_disp_drv_register(&disp_drv);
  lv_timer_set_period(_lv_disp_get_refr_timer(disp), 15);   // check for changes every 15 ms; actual rate is capped by the panel (~58 Hz)

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = Lvgl_Touchpad_Read;
  indev_drv.long_press_time = 2000;        // 2 s hold for boost +/- and the WiFi button
  lv_indev_drv_register(&indev_drv);
  lv_timer_set_period(indev_drv.read_timer, 10);             // read touch every 10 ms (default 30 ms)

  const esp_timer_create_args_t lvgl_tick_timer_args = {
    .callback = &example_increase_lvgl_tick,
    .name = "lvgl_tick"
  };
  esp_timer_handle_t lvgl_tick_timer = NULL;
  esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer);
  esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000);
}

void Lvgl_Loop(void)
{
  LCD_Service();               // panel register re-send, if requested
  lv_timer_handler();
}
