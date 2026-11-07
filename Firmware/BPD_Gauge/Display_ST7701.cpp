#include "Display_ST7701.h"  
#include <Preferences.h>
#include "esp_cache.h"
      
spi_device_handle_t SPI_handle = NULL;     
esp_lcd_panel_handle_t panel_handle = NULL;            

// ---------------- Tear-free frame swap ----------------
// Given by the panel ISR each time the driver has finished reading a whole frame
// out of PSRAM. After that point a newly queued frame buffer is the one being
// scanned, and the previous buffer is free for LVGL to draw into.
static SemaphoreHandle_t frameSem = NULL;
static volatile uint32_t frameCount = 0;
static void *volatile shownFb = NULL;              // frame buffer the panel is currently showing

// ---------------- Cache write-back (slit fix) ----------------
// LVGL draws into the PSRAM frame buffer through the CPU cache. Changed pixels can sit
// in the cache ("dirty") instead of in PSRAM, while the panel output reads PSRAM. Those
// stale cache lines showed up as short slits along moving arc edges. Writing the cache
// back to PSRAM before each frame is shown guarantees the panel gets what LVGL drew.
#define FB_BYTES  (ESP_PANEL_LCD_WIDTH * ESP_PANEL_LCD_HEIGHT * 2)
static volatile bool cacheSync = true;
static volatile bool flushWanted = false;

static volatile int32_t lastMsyncErr = 0;          // 0 = ESP_OK
static volatile uint32_t msyncOk = 0, msyncFail = 0;
static void fbWriteBack(void *fb)
{
  if (!fb) return;
  esp_err_t e = esp_cache_msync(fb, FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  if (e != ESP_OK)
    e = esp_cache_msync(fb, FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  lastMsyncErr = e;
  if (e == ESP_OK) msyncOk++; else msyncFail++;
}
void LCD_GetCacheStats(int32_t *lastErr, uint32_t *ok, uint32_t *fail) { *lastErr = lastMsyncErr; *ok = msyncOk; *fail = msyncFail; }
void LCD_SetCacheSync(bool on) { cacheSync = on; Serial.printf("LCD cache write-back %s\n", on ? "ON" : "OFF"); }
bool LCD_GetCacheSync(void)    { return cacheSync; }
void LCD_RequestCacheFlush(void) { flushWanted = true; }

// Slit investigation: copies of what the /screen.bmp request does, one effect each
static volatile uint32_t pauseWantedMs = 0;
static volatile bool readWanted = false, invalWanted = false;
static volatile uint32_t lastReadSum = 0;
void LCD_RequestPause(uint32_t ms) { pauseWantedMs = ms; }
void LCD_RequestFbRead(void)       { readWanted = true; }
void LCD_RequestCacheInval(void)   { invalWanted = true; }
uint32_t LCD_LastReadSum(void)     { return lastReadSum; }
static volatile uint32_t pclkHz = ESP_PANEL_LCD_RGB_TIMING_FREQ_HZ;

static bool IRAM_ATTR onFrameDone(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *edata, void *user_ctx)
{
  BaseType_t woken = pdFALSE;
  frameCount++;
  if (frameSem) xSemaphoreGiveFromISR(frameSem, &woken);
  return woken == pdTRUE;
}

uint32_t LCD_FrameCount(void) { return frameCount; }

// Show a complete frame that LVGL drew directly into one of the panel's own
// frame buffers, and block until the panel has switched over to it.
//   1. draw_bitmap() with a frame buffer pointer does no copy; it only queues
//      that buffer for the next frame.
//   2. Any signal already pending is from before the queue, so drop it. If a frame
//      finished in the gap we lose it and wait one extra frame, which is harmless.
//   3. Wait for the next frame-done: the panel is now reading the new buffer.
// Returns false if no frame-done arrived (panel stalled), so the caller can carry on.
bool LCD_PresentFrame(void *fb)
{
  if (cacheSync) fbWriteBack(fb);      // make PSRAM match what LVGL drew
  esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, ESP_PANEL_LCD_WIDTH, ESP_PANEL_LCD_HEIGHT, fb);
  shownFb = fb;
  if (!frameSem) return false;
  xSemaphoreTake(frameSem, 0);
  return xSemaphoreTake(frameSem, pdMS_TO_TICKS(LCD_FRAME_WAIT_MS)) == pdTRUE;
}

// Frame buffer currently on screen (RGB565, 480 x 480), or NULL before the first frame
const uint16_t *LCD_ShownFrame(void) { return (const uint16_t *)shownFb; }

// Change the pixel clock while running (test tool, not saved; back to 16 MHz on reboot).
// The driver applies it at the next frame.
bool LCD_SetPclk(uint32_t hz)
{
  if (hz < LCD_PCLK_MIN_HZ || hz > LCD_PCLK_MAX_HZ || !panel_handle) return false;
  if (esp_lcd_rgb_panel_set_pclk(panel_handle, hz) != ESP_OK) return false;
  pclkHz = hz;
  Serial.printf("LCD pixel clock now %.1f MHz\n", hz / 1e6f);
  return true;
}
uint32_t LCD_GetPclk(void) { return pclkHz; }

// Re-send the panel setup from the main loop (it shares the I2C expander with touch,
// so it must not run from the Bluetooth task).
static void ST7701_SendInit(bool boot);
static volatile bool reinitWanted = false;
void LCD_RequestReinit(void) { reinitWanted = true; }
void LCD_Service(void)
{
  if (flushWanted) {                  // test tool: one-off write-back of both frame buffers
    flushWanted = false;
    void *a = NULL, *b = NULL;
    esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, &a, &b);
    fbWriteBack(a);
    fbWriteBack(b);
    Serial.println("LCD cache written back");
  }
  if (pauseWantedMs) {                // stop drawing for a while (no reads, no swaps)
    uint32_t ms = pauseWantedMs; pauseWantedMs = 0;
    Serial.printf("LCD pause %lu ms\n", (unsigned long)ms);
    vTaskDelay(pdMS_TO_TICKS(ms));
  }
  if (readWanted || invalWanted) {
    void *a = NULL, *b = NULL;
    esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, &a, &b);
    if (readWanted) {                 // read every pixel of both buffers through the cache
      readWanted = false;
      uint32_t sum = 0;
      const volatile uint32_t *pa = (const volatile uint32_t *)a, *pb = (const volatile uint32_t *)b;
      for (size_t i = 0; i < FB_BYTES / 4; i++) sum += pa[i] + pb[i];
      lastReadSum = sum;
      Serial.printf("LCD frame buffers read, sum %08lx\n", (unsigned long)sum);
    }
    if (invalWanted) {                // write back AND drop both buffers from the cache
      invalWanted = false;
      int fl = ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE;
      esp_cache_msync(a, FB_BYTES, fl);
      esp_cache_msync(b, FB_BYTES, fl);
      Serial.println("LCD frame buffers written back and invalidated");
    }
  }
  if (!reinitWanted) return;
  reinitWanted = false;
  ST7701_SendInit(false);
  Serial.println("LCD panel registers re-sent");
}

// Which PCLK edge the panel samples data on. Stored in NVS (namespace "lcd") so it
// can be switched from the app for testing; it only takes effect after a restart.
static bool pclkNeg = false;
bool LCD_GetPclkNeg(void) { return pclkNeg; }
// Output drive strength of the RGB bus pins (16 data + PCLK, DE, HSYNC, VSYNC).
// 0 = weakest .. 3 = strongest; ESP32-S3 default is 2. Weaker = slower, cleaner edges
// (less ringing and crosstalk); stronger = sharper edges. Test tool, not saved.
static const uint8_t lcdBusPins[] = {
  ESP_PANEL_LCD_PIN_NUM_RGB_DATA0,  ESP_PANEL_LCD_PIN_NUM_RGB_DATA1,  ESP_PANEL_LCD_PIN_NUM_RGB_DATA2,
  ESP_PANEL_LCD_PIN_NUM_RGB_DATA3,  ESP_PANEL_LCD_PIN_NUM_RGB_DATA4,  ESP_PANEL_LCD_PIN_NUM_RGB_DATA5,
  ESP_PANEL_LCD_PIN_NUM_RGB_DATA6,  ESP_PANEL_LCD_PIN_NUM_RGB_DATA7,  ESP_PANEL_LCD_PIN_NUM_RGB_DATA8,
  ESP_PANEL_LCD_PIN_NUM_RGB_DATA9,  ESP_PANEL_LCD_PIN_NUM_RGB_DATA10, ESP_PANEL_LCD_PIN_NUM_RGB_DATA11,
  ESP_PANEL_LCD_PIN_NUM_RGB_DATA12, ESP_PANEL_LCD_PIN_NUM_RGB_DATA13, ESP_PANEL_LCD_PIN_NUM_RGB_DATA14,
  ESP_PANEL_LCD_PIN_NUM_RGB_DATA15, ESP_PANEL_LCD_PIN_NUM_RGB_PCLK,   ESP_PANEL_LCD_PIN_NUM_RGB_DE,
  ESP_PANEL_LCD_PIN_NUM_RGB_HSYNC,  ESP_PANEL_LCD_PIN_NUM_RGB_VSYNC,
};
bool LCD_SetDrive(int cap)
{
  if (cap < 0 || cap > 3) return false;
  for (size_t i = 0; i < sizeof(lcdBusPins); i++)
    gpio_set_drive_capability((gpio_num_t)lcdBusPins[i], (gpio_drive_cap_t)cap);
  Serial.printf("LCD bus drive strength now %d\n", cap);
  return true;
}
int LCD_GetDrive(void)
{
  gpio_drive_cap_t c = GPIO_DRIVE_CAP_DEFAULT;
  gpio_get_drive_capability((gpio_num_t)ESP_PANEL_LCD_PIN_NUM_RGB_PCLK, &c);
  return (int)c;
}

void LCD_SavePclkNeg(bool neg)
{
  Preferences p;
  p.begin("lcd", false);
  p.putBool("pclkneg", neg);
  p.end();
}

void ST7701_WriteCommand(uint8_t cmd)
{
  spi_transaction_t spi_tran = {
    .cmd = 0,
    .addr = cmd,
    .length = 0,
    .rxlength = 0,
  };
  spi_device_transmit(SPI_handle, &spi_tran);
}
void ST7701_WriteData(uint8_t data)
{
  spi_transaction_t spi_tran = {
    .cmd = 1,
    .addr = data,
    .length = 0,
    .rxlength = 0,
  };
  spi_device_transmit(SPI_handle, &spi_tran);
}

void ST7701_CS_EN(){
  Set_EXIO(EXIO_PIN3,Low);
  vTaskDelay(pdMS_TO_TICKS(10));
}
void ST7701_CS_Dis(){
  Set_EXIO(EXIO_PIN3,High);
  vTaskDelay(pdMS_TO_TICKS(10));
}
void ST7701_Reset(){
  Set_EXIO(EXIO_PIN1,Low);
  vTaskDelay(pdMS_TO_TICKS(10));
  Set_EXIO(EXIO_PIN1,High);
  vTaskDelay(pdMS_TO_TICKS(50));
}
// Panel controller register setup, sent over the 3-wire SPI link.
// boot = true at power-up (includes sleep-out and its delays). boot = false re-sends
// every register while running (test tool: clears any register that got corrupted).
static void ST7701_SendInit(bool boot)
{
  ST7701_CS_EN();
  ST7701_WriteCommand(0xFF);
  ST7701_WriteData(0x77);
  ST7701_WriteData(0x01);
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x10);

  ST7701_WriteCommand(0xC0);
  ST7701_WriteData(0x3B);//Scan line	
  ST7701_WriteData(0x00);

  ST7701_WriteCommand(0xC1);
  ST7701_WriteData(0x0B);	//VBP
  ST7701_WriteData(0x02);

  ST7701_WriteCommand(0xC2);
  ST7701_WriteData(0x07);
  ST7701_WriteData(0x02);

  ST7701_WriteCommand(0xCC);
  ST7701_WriteData(0x10);

  ST7701_WriteCommand(0xCD);//RGB format
  ST7701_WriteData(0x08);

  ST7701_WriteCommand(0xB0); // IPS   
  ST7701_WriteData(0x00); // 255 
  ST7701_WriteData(0x11); // 251    
  ST7701_WriteData(0x16); // 247  down
  ST7701_WriteData(0x0e); // 239    
  ST7701_WriteData(0x11); // 231    
  ST7701_WriteData(0x06); // 203    
  ST7701_WriteData(0x05); // 175 
  ST7701_WriteData(0x09); // 147    
  ST7701_WriteData(0x08); // 108    
  ST7701_WriteData(0x21); // 80  
  ST7701_WriteData(0x06); // 52   
  ST7701_WriteData(0x13); // 24    
  ST7701_WriteData(0x10); // 16    
  ST7701_WriteData(0x29); // 8    down
  ST7701_WriteData(0x31); // 4    
  ST7701_WriteData(0x18); // 0   

  ST7701_WriteCommand(0xB1);//  IPS	   
  ST7701_WriteData(0x00);//  255 
  ST7701_WriteData(0x11);//  251
  ST7701_WriteData(0x16);//  247   down
  ST7701_WriteData(0x0e);//  239
  ST7701_WriteData(0x11);//  231
  ST7701_WriteData(0x07);//  203    
  ST7701_WriteData(0x05);//  175
  ST7701_WriteData(0x09);//  147  
  ST7701_WriteData(0x09);//  108  
  ST7701_WriteData(0x21);//  80 
  ST7701_WriteData(0x05);//  52   
  ST7701_WriteData(0x13);//  24 
  ST7701_WriteData(0x11);//  16 
  ST7701_WriteData(0x2a);//  8  down 
  ST7701_WriteData(0x31);//  4  
  ST7701_WriteData(0x18);//  0  

  ST7701_WriteCommand(0xFF);
  ST7701_WriteData(0x77);
  ST7701_WriteData(0x01);
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x11);

  ST7701_WriteCommand(0xB0);  //VOP  3.5375+ *x 0.0125
  ST7701_WriteData(0x6d);  //5D
  
  ST7701_WriteCommand(0xB1); 	//VCOM amplitude setting  
  ST7701_WriteData(0x37);  //
  
  ST7701_WriteCommand(0xB2); 	//VGH Voltage setting  
  ST7701_WriteData(0x81);	//12V

  ST7701_WriteCommand(0xB3);
  ST7701_WriteData(0x80);

  ST7701_WriteCommand(0xB5); 	//VGL Voltage setting  
  ST7701_WriteData(0x43);	//-8.3V

  ST7701_WriteCommand(0xB7);
  ST7701_WriteData(0x85);

  ST7701_WriteCommand(0xB8);
  ST7701_WriteData(0x20);

  ST7701_WriteCommand(0xC1);
  ST7701_WriteData(0x78);

  ST7701_WriteCommand(0xC2);
  ST7701_WriteData(0x78);

  ST7701_WriteCommand(0xD0);
  ST7701_WriteData(0x88);

  ST7701_WriteCommand(0xE0);
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x02);

  ST7701_WriteCommand(0xE1);
  ST7701_WriteData(0x03);	
  ST7701_WriteData(0xA0);
  ST7701_WriteData(0x00);	
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x04);	
  ST7701_WriteData(0xA0);
  ST7701_WriteData(0x00);	
  ST7701_WriteData(0x00);	
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x20);
  ST7701_WriteData(0x20);

  ST7701_WriteCommand(0xE2);
  ST7701_WriteData(0x00);	
  ST7701_WriteData(0x00);	  
  ST7701_WriteData(0x00);	
  ST7701_WriteData(0x00);	
  ST7701_WriteData(0x00);	
  ST7701_WriteData(0x00);	
  ST7701_WriteData(0x00);	
  ST7701_WriteData(0x00);	
  ST7701_WriteData(0x00);	
  ST7701_WriteData(0x00);	
  ST7701_WriteData(0x00);	
  ST7701_WriteData(0x00);	
  ST7701_WriteData(0x00);

  ST7701_WriteCommand(0xE3);
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x11);
  ST7701_WriteData(0x00);

  ST7701_WriteCommand(0xE4);
  ST7701_WriteData(0x22);
  ST7701_WriteData(0x00);

  ST7701_WriteCommand(0xE5);		
  ST7701_WriteData(0x05);	
  ST7701_WriteData(0xEC);	
  ST7701_WriteData(0xA0);
  ST7701_WriteData(0xA0);
  ST7701_WriteData(0x07);	
  ST7701_WriteData(0xEE);	
  ST7701_WriteData(0xA0);
  ST7701_WriteData(0xA0);
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x00);	
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x00);	
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x00);

  ST7701_WriteCommand(0xE6);
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x11);
  ST7701_WriteData(0x00);

  ST7701_WriteCommand(0xE7);
  ST7701_WriteData(0x22);
  ST7701_WriteData(0x00);

  ST7701_WriteCommand(0xE8);		
  ST7701_WriteData(0x06);	
  ST7701_WriteData(0xED);	
  ST7701_WriteData(0xA0);
  ST7701_WriteData(0xA0);
  ST7701_WriteData(0x08);	
  ST7701_WriteData(0xEF);	
  ST7701_WriteData(0xA0); 
  ST7701_WriteData(0xA0);
  ST7701_WriteData(0x00);	
  ST7701_WriteData(0x00);	
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x00);	
  ST7701_WriteData(0x00);	
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x00);

  ST7701_WriteCommand(0xEB);
  ST7701_WriteData(0x00); 	
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x40);
  ST7701_WriteData(0x40);
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x00);  

  ST7701_WriteCommand(0xED);  
  ST7701_WriteData(0xFF); 
  ST7701_WriteData(0xFF);  
  ST7701_WriteData(0xFF); 	
  ST7701_WriteData(0xBA); 		
  ST7701_WriteData(0x0A); 	
  ST7701_WriteData(0xBF); 	
  ST7701_WriteData(0x45); 	
  ST7701_WriteData(0xFF); 
  ST7701_WriteData(0xFF);  
  ST7701_WriteData(0x54); 	
  ST7701_WriteData(0xFB); 	
  ST7701_WriteData(0xA0); 	
  ST7701_WriteData(0xAB); 	
  ST7701_WriteData(0xFF); 
  ST7701_WriteData(0xFF); 
  ST7701_WriteData(0xFF); 

  ST7701_WriteCommand(0xEF);
  ST7701_WriteData(0x10); 
  ST7701_WriteData(0x0D); 
  ST7701_WriteData(0x04); 
  ST7701_WriteData(0x08); 
  ST7701_WriteData(0x3F); 
  ST7701_WriteData(0x1F);

  ST7701_WriteCommand(0xFF);
  ST7701_WriteData(0x77);
  ST7701_WriteData(0x01);
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x13);

  ST7701_WriteCommand(0xEF);
  ST7701_WriteData(0x08);

  ST7701_WriteCommand(0xFF);
  ST7701_WriteData(0x77);
  ST7701_WriteData(0x01);
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x00);
  ST7701_WriteData(0x00);


  ST7701_WriteCommand(0x36);
  ST7701_WriteData(0x00);

  ST7701_WriteCommand(0x3A);
  ST7701_WriteData(0x66);

  if (boot) {
    ST7701_WriteCommand(0x11);        // sleep out
    vTaskDelay(pdMS_TO_TICKS(480));
  }
  ST7701_WriteCommand(0x20);          // inversion off
  if (boot) vTaskDelay(pdMS_TO_TICKS(120));
  ST7701_WriteCommand(0x29);          // display on
  ST7701_CS_Dis();
}

void ST7701_Init()
{
  {
    Preferences p;
    p.begin("lcd", true);
    pclkNeg = p.getBool("pclkneg", false);
    p.end();
    Serial.printf("LCD PCLK edge: %s\n", pclkNeg ? "falling (neg)" : "rising (default)");
  }
  spi_bus_config_t buscfg = {
    .mosi_io_num = LCD_MOSI_PIN,
    .miso_io_num = -1,
    .sclk_io_num = LCD_CLK_PIN,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = 64, // ESP32 S3 max size is 64Kbytes
  };
  spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
  spi_device_interface_config_t devcfg = {
    .command_bits = 1,
    .address_bits = 8,
    .mode = SPI_MODE0,
    .clock_speed_hz = LCD_SPI_HZ,     // was 40 MHz (Waveshare demo); setup link only, speed doesn't matter
    .spics_io_num = -1,                      
    .queue_size = 1,            // Not using queues
  };
  spi_bus_add_device(SPI2_HOST, &devcfg, &SPI_handle);            

  ST7701_SendInit(true);

  //  RGB
  esp_lcd_rgb_panel_config_t rgb_config = {
    .clk_src = LCD_CLK_SRC_DEFAULT,
    .timings =  {
      .pclk_hz = ESP_PANEL_LCD_RGB_TIMING_FREQ_HZ,
      .h_res = ESP_PANEL_LCD_HEIGHT,
      .v_res = ESP_PANEL_LCD_WIDTH,
      .hsync_pulse_width = ESP_PANEL_LCD_RGB_TIMING_HPW,
      .hsync_back_porch = ESP_PANEL_LCD_RGB_TIMING_HBP,
      .hsync_front_porch = ESP_PANEL_LCD_RGB_TIMING_HFP,
      .vsync_pulse_width = ESP_PANEL_LCD_RGB_TIMING_VPW,
      .vsync_back_porch = ESP_PANEL_LCD_RGB_TIMING_VBP,
      .vsync_front_porch = ESP_PANEL_LCD_RGB_TIMING_VFP,
      .flags = {
        .hsync_idle_low = 0,  /*!< The hsync signal is low in IDLE state */
        .vsync_idle_low = 0,  /*!< The vsync signal is low in IDLE state */
        .de_idle_high = 0,    /*!< The de signal is high in IDLE state */
        .pclk_active_neg = pclkNeg,   // default false; switchable from the app for testing
        .pclk_idle_high = 0,  /*!< The PCLK stays at high level in IDLE phase */
      },
    },
    .data_width = ESP_PANEL_LCD_RGB_DATA_WIDTH,
    .bits_per_pixel = ESP_PANEL_LCD_RGB_PIXEL_BITS,
    .num_fbs = ESP_PANEL_LCD_RGB_FRAME_BUF_NUM,
    .bounce_buffer_size_px = ESP_PANEL_LCD_RGB_BOUNCE_BUF_SIZE,
    .psram_trans_align = 64,
    .hsync_gpio_num = ESP_PANEL_LCD_PIN_NUM_RGB_HSYNC,
    .vsync_gpio_num = ESP_PANEL_LCD_PIN_NUM_RGB_VSYNC,
    .de_gpio_num = ESP_PANEL_LCD_PIN_NUM_RGB_DE,
    .pclk_gpio_num = ESP_PANEL_LCD_PIN_NUM_RGB_PCLK,
    .disp_gpio_num = ESP_PANEL_LCD_PIN_NUM_RGB_DISP,
    .data_gpio_nums = {
      ESP_PANEL_LCD_PIN_NUM_RGB_DATA0,
      ESP_PANEL_LCD_PIN_NUM_RGB_DATA1,
      ESP_PANEL_LCD_PIN_NUM_RGB_DATA2,
      ESP_PANEL_LCD_PIN_NUM_RGB_DATA3,
      ESP_PANEL_LCD_PIN_NUM_RGB_DATA4,
      ESP_PANEL_LCD_PIN_NUM_RGB_DATA5,
      ESP_PANEL_LCD_PIN_NUM_RGB_DATA6,
      ESP_PANEL_LCD_PIN_NUM_RGB_DATA7,
      ESP_PANEL_LCD_PIN_NUM_RGB_DATA8,
      ESP_PANEL_LCD_PIN_NUM_RGB_DATA9,
      ESP_PANEL_LCD_PIN_NUM_RGB_DATA10,
      ESP_PANEL_LCD_PIN_NUM_RGB_DATA11,
      ESP_PANEL_LCD_PIN_NUM_RGB_DATA12,
      ESP_PANEL_LCD_PIN_NUM_RGB_DATA13,
      ESP_PANEL_LCD_PIN_NUM_RGB_DATA14,
      ESP_PANEL_LCD_PIN_NUM_RGB_DATA15,
    },
    .flags = {
      .disp_active_low = 0,
      .refresh_on_demand = 0,
      .fb_in_psram = true,
      .double_fb = true,
      .no_fb = 0,
      .bb_invalidate_cache = 0,
    },
  };
  esp_lcd_new_rgb_panel(&rgb_config, &panel_handle); 

  // Frame-done signal for tear-free buffer swaps (see LCD_PresentFrame).
  // With bounce buffers the driver reads the frame buffer ahead of the panel, so the
  // correct event is "bounce buffer finished copying the frame", not VSYNC.
  frameSem = xSemaphoreCreateBinary();
  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
#if ESP_PANEL_LCD_RGB_BOUNCE_BUF_SIZE > 0
  cbs.on_bounce_frame_finish = onFrameDone;
#else
  cbs.on_vsync = onFrameDone;
#endif
  esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &cbs, NULL);
  esp_lcd_panel_reset(panel_handle);
  esp_lcd_panel_init(panel_handle);
}

void LCD_Init() {
  ST7701_Reset();
  ST7701_Init();
  Touch_Init();
  Backlight_Init();
}

void LCD_addWindow(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend,uint8_t* color) {
  Xend = Xend + 1;      // esp_lcd_panel_draw_bitmap: x_end End index on x-axis (x_end not included)
  Yend = Yend + 1;      // esp_lcd_panel_draw_bitmap: y_end End index on y-axis (y_end not included)
  if (Xend >= ESP_PANEL_LCD_WIDTH)
    Xend = ESP_PANEL_LCD_WIDTH;
  if (Yend >= ESP_PANEL_LCD_HEIGHT)
    Yend = ESP_PANEL_LCD_HEIGHT;
   
  esp_lcd_panel_draw_bitmap(panel_handle, Xstart, Ystart, Xend, Yend, color);                     // x_end End index on x-axis (x_end not included)
}


// backlight
uint8_t LCD_Backlight = 50;
void Backlight_Init()
{
  ledcAttach(LCD_Backlight_PIN, Frequency, Resolution);    
  Set_Backlight(LCD_Backlight);      //0~100               
}

void Set_Backlight(uint8_t Light)                        //
{
  if(Light > Backlight_MAX || Light < 0)
    printf("Set Backlight parameters in the range of 0 to 100 \r\n");
  else{
    uint32_t Backlight = Light*10;
    if(Backlight == 1000)
      Backlight = 1024;
    ledcWrite(LCD_Backlight_PIN, Backlight);
  }
}


