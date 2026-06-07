// Waveshare ESP32-S3-Touch-LCD-2.8C — VDO Quartz-Zeit Clock
// PHASE 1: Display Bring-up (Arduino_GFX, ST7701 RGB 480x480)
//
// Display: ST7701 RGB-Parallel. CS/RST des LCD + Touch-RST laufen ueber
// einen PCA9554 I2C-Expander (@0x20). Arduino_GFX kann den Expander-CS
// nicht selbst steuern -> wir halten CS dauerhaft LOW per Expander und
// fuehren die Reset-Sequenz manuell aus, bevor gfx->begin() laeuft.
//
// HINWEIS: Die ST7701-Init-Sequenz ist board-spezifisch. Hier startet sie
// mit st7701_type1_init_operations als Naeherung. Falls das Bild fehlt
// oder Farben/Versatz falsch sind, brauchen wir die exakte Init aus dem
// offiziellen Waveshare-2.8C-Demo (Arduino).

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Arduino_GFX_Library.h>
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "vdo_dial_480_rgb565.h"
#include <sys/time.h>
#include <time.h>

// ---- I2C / PCA9554 Expander ----
#define PIN_I2C_SDA   15
#define PIN_I2C_SCL   7
#define PIN_TOUCH_INT 16
#define PCA9554_ADDR  0x20
#define PCA9554_OUTPUT 0x01
#define PCA9554_CONFIG 0x03
#define EXIO_LCD_RST  (1 << 0)
#define EXIO_TP_RST   (1 << 1)
#define EXIO_LCD_CS   (1 << 2)

#define GT911_ADDR_PRIMARY 0x5D
#define GT911_ADDR_ALT     0x14
#define GT911_PRODUCT_ID   0x8140
#define GT911_READ_XY      0x814E

// ---- LCD ST7701 Init-SPI (3-wire; CS extern via Expander) ----
#define PIN_LCD_SCK   2
#define PIN_LCD_MOSI  1
#define PIN_LCD_BL    6

// ---- RGB Sync ----
#define PIN_DE     40
#define PIN_VSYNC  39
#define PIN_HSYNC  38
#define PIN_PCLK   41

// ---- RGB Daten (5R / 6G / 5B) ----
#define PIN_R0 46
#define PIN_R1 3
#define PIN_R2 8
#define PIN_R3 18
#define PIN_R4 17
#define PIN_G0 14
#define PIN_G1 13
#define PIN_G2 12
#define PIN_G3 11
#define PIN_G4 10
#define PIN_G5 9
#define PIN_B0 5
#define PIN_B1 45
#define PIN_B2 48
#define PIN_B3 47
#define PIN_B4 21

// -------- PCA9554 Helfer --------
static uint8_t pcaOutputState = EXIO_LCD_RST | EXIO_TP_RST | EXIO_LCD_CS;
static uint8_t gt911Addr = GT911_ADDR_PRIMARY;
static bool gt911Found = false;
static uint8_t currentPage = 0;
static bool ntpTimeSynced = false;
static bool touchSeen = false;

static int monthFromBuildName(const char *month) {
  static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *pos = strstr(months, month);
  return pos ? ((pos - months) / 3) : 0;
}

static void setClockFromBuildTime() {
  char monthName[4] = {__DATE__[0], __DATE__[1], __DATE__[2], 0};
  struct tm t = {};
  t.tm_year = atoi(__DATE__ + 7) - 1900;
  t.tm_mon = monthFromBuildName(monthName);
  t.tm_mday = atoi(__DATE__ + 4);
  t.tm_hour = atoi(__TIME__);
  t.tm_min = atoi(__TIME__ + 3);
  t.tm_sec = atoi(__TIME__ + 6);
  time_t epoch = mktime(&t);
  if (epoch > 1700000000) {
    timeval tv = {epoch, 0};
    settimeofday(&tv, nullptr);
  }
}

static void initTimeSource() {
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  setClockFromBuildTime();

  if (strlen(HOME_WIFI_SSID) == 0) {
    Serial.println("Time: WiFi SSID empty, using build-time fallback");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(HOME_WIFI_SSID, HOME_WIFI_PASSWORD);
  Serial.print("WiFi: connecting for NTP");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi: not connected, using build-time fallback");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return;
  }

  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
  struct tm now = {};
  ntpTimeSynced = getLocalTime(&now, 8000);
  Serial.printf("Time: %s\n", ntpTimeSynced ? "NTP synced" : "NTP timeout, using fallback");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

static bool readClockTime(struct tm *now) {
  time_t t = time(nullptr);
  return localtime_r(&t, now) != nullptr;
}

static uint8_t pca9554WriteOnce(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(PCA9554_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission();
}

// Cold-Boot-Robust: bis zu 5 Versuche mit Delay. Der PCA9554 reagiert
// direkt nach Power-On manchmal nicht auf den ersten I2C-Burst (I2C-Bus
// noch nicht stabil). Bei Replug per USB war genau das der Grund warum
// das Display schwarz blieb: erster expander-write erreichte den Chip
// nicht, CS blieb HIGH, ST7701-Init ging ins Leere.
static void pca9554Write(uint8_t reg, uint8_t val) {
  if (reg == PCA9554_OUTPUT) {
    pcaOutputState = val;
  }
  uint8_t err = 0xFF;
  for (uint8_t attempt = 0; attempt < 5; attempt++) {
    err = pca9554WriteOnce(reg, val);
    if (err == 0) {
      if (attempt > 0) {
        Serial.printf("PCA9554 write reg 0x%02X = 0x%02X -> OK (try %u)\n",
                      reg, val, attempt + 1);
      } else {
        Serial.printf("PCA9554 write reg 0x%02X = 0x%02X -> 0\n", reg, val);
      }
      return;
    }
    delay(10);
  }
  Serial.printf("PCA9554 write reg 0x%02X = 0x%02X -> FAIL (last err %u)\n",
                reg, val, err);
}

static void pcaSetOutputBits(uint8_t mask, bool high) {
  uint8_t next = high ? (pcaOutputState | mask) : (pcaOutputState & ~mask);
  pca9554Write(PCA9554_OUTPUT, next);
}

static void scanI2C() {
  Serial.println("I2C scan:");
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  found 0x%02X\n", addr);
    }
  }
}

// Expander init: Pin0..2 Output, HARTER Reset-Puls Display+Touch.
// Bei USB-Replug bleiben die Caps geladen → ST7701 behaelt korrupten
// internen Zustand. Loesung: RST-Pin SEHR lang LOW halten damit der
// interne State-Machine-Reset sicher greift, dann lange Settle-Zeit.
static void expanderInit() {
  pca9554Write(PCA9554_CONFIG, 0xF8);   // Pin0..2 = Output

  // 1) Alles HIGH (Ausgangs-Zustand)
  pca9554Write(PCA9554_OUTPUT, EXIO_LCD_RST | EXIO_TP_RST | EXIO_LCD_CS);
  delay(50);

  // 2) RST LOW — wie in der funktionierenden Referenz (Boatingwiththebaileys):
  //    kurzer Puls 10ms low, 50ms high reicht.
  pca9554Write(PCA9554_OUTPUT, EXIO_LCD_CS);  // RST LOW, CS HIGH
  delay(10);

  // 3) RST HIGH — 50ms Settle
  pca9554Write(PCA9554_OUTPUT, EXIO_LCD_RST | EXIO_TP_RST | EXIO_LCD_CS);
  delay(50);

  // 4) CS LOW fuer SPI-Init
  pca9554Write(PCA9554_OUTPUT, EXIO_LCD_RST | EXIO_TP_RST);
  delay(10);
}

static bool i2cRegRead16(uint8_t addr, uint16_t reg, uint8_t *data, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission(true) != 0) {
    return false;
  }
  uint8_t got = Wire.requestFrom((int)addr, (int)len);
  if (got != len) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }
  for (uint8_t i = 0; i < len; i++) {
    data[i] = Wire.read();
  }
  return true;
}

static bool i2cRegWrite16(uint8_t addr, uint16_t reg, const uint8_t *data, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  for (uint8_t i = 0; i < len; i++) {
    Wire.write(data[i]);
  }
  return Wire.endTransmission() == 0;
}

static void gt911ResetAddressMode(bool intHigh) {
  pinMode(PIN_TOUCH_INT, OUTPUT);
  digitalWrite(PIN_TOUCH_INT, intHigh ? HIGH : LOW);
  pcaSetOutputBits(EXIO_TP_RST, false);
  delay(10);
  pcaSetOutputBits(EXIO_TP_RST, true);
  delay(200);
  digitalWrite(PIN_TOUCH_INT, intHigh ? HIGH : LOW);
  delay(5);
  pinMode(PIN_TOUCH_INT, INPUT);
  delay(50);
}

static bool gt911Probe() {
  uint8_t id[4] = {0};
  if (i2cRegRead16(GT911_ADDR_PRIMARY, GT911_PRODUCT_ID, id, sizeof(id))) {
    gt911Addr = GT911_ADDR_PRIMARY;
    gt911Found = true;
  } else if (i2cRegRead16(GT911_ADDR_ALT, GT911_PRODUCT_ID, id, sizeof(id))) {
    gt911Addr = GT911_ADDR_ALT;
    gt911Found = true;
  } else {
    gt911Found = false;
    return false;
  }

  Serial.printf("GT911: addr 0x%02X id %c%c%c%c\n", gt911Addr, id[0], id[1], id[2], id[3]);
  uint8_t clear = 0;
  i2cRegWrite16(gt911Addr, GT911_READ_XY, &clear, 1);
  return true;
}

static void gt911Init() {
  Serial.println("GT911: reset/probe INT low");
  gt911ResetAddressMode(false);
  if (gt911Probe()) {
    return;
  }

  Serial.println("GT911: reset/probe INT high");
  gt911ResetAddressMode(true);
  if (gt911Probe()) {
    return;
  }

  Serial.println("GT911: not found on 0x5D/0x14");
}

static bool readTouch(uint16_t *x, uint16_t *y) {
  uint8_t status = 0;
  if (!i2cRegRead16(gt911Addr, GT911_READ_XY, &status, 1)) {
    uint8_t otherAddr = (gt911Addr == GT911_ADDR_PRIMARY) ? GT911_ADDR_ALT : GT911_ADDR_PRIMARY;
    if (!i2cRegRead16(otherAddr, GT911_READ_XY, &status, 1)) {
      return false;
    }
    gt911Addr = otherAddr;
    gt911Found = true;
  }
  if ((status & 0x80) == 0) {
    uint8_t clear = 0;
    i2cRegWrite16(gt911Addr, GT911_READ_XY, &clear, 1);
    return false;
  }

  uint8_t clear = 0;
  uint8_t points = status & 0x0F;
  if (points == 0 || points > 5) {
    i2cRegWrite16(gt911Addr, GT911_READ_XY, &clear, 1);
    return false;
  }

  uint8_t point[8] = {0};
  bool ok = i2cRegRead16(gt911Addr, GT911_READ_XY + 1, point, sizeof(point));
  i2cRegWrite16(gt911Addr, GT911_READ_XY, &clear, 1);
  if (!ok) {
    return false;
  }

  *x = (uint16_t)point[2] | ((uint16_t)point[3] << 8);
  *y = (uint16_t)point[4] | ((uint16_t)point[5] << 8);
  return true;
}

// ST7701 Init-Sequenz: nur noch in nativeSt7701Init() (native SPI).

// HINWEIS: Arduino_GFX globale Objekte (bus/rgbpanel/gfx) wurden entfernt.
// Sie liefen ihre Konstruktoren VOR setup() und konfigurierten GPIO-Pins
// (SPI + RGB), was mit dem nativen ESP-IDF Init kollidierte.
// Das war die Ursache fuer "Display schwarz nach Replug".

static spi_device_handle_t nativeSpi = nullptr;
static esp_lcd_panel_handle_t nativePanel = nullptr;
static uint16_t *nativeFrame = nullptr;

static void nativeWriteCommand(uint8_t cmd) {
  if (!nativeSpi) return;
  spi_transaction_t t = {};
  t.cmd = 0;
  t.addr = cmd;
  spi_device_transmit(nativeSpi, &t);
}

static void nativeWriteData(uint8_t data) {
  if (!nativeSpi) return;
  spi_transaction_t t = {};
  t.cmd = 1;
  t.addr = data;
  spi_device_transmit(nativeSpi, &t);
}

static void nativeSt7701Init() {
  // Bei Warm-Reset (USB replug -> rst:0x15) kann der SPI-Bus noch vom
  // vorherigen Lauf belegt sein. Erst freigeben, dann neu init.
  if (nativeSpi) {
    spi_bus_remove_device(nativeSpi);
    nativeSpi = nullptr;
  }
  spi_bus_free(SPI2_HOST);  // ignore error if not initialized

  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num = PIN_LCD_MOSI;
  buscfg.miso_io_num = -1;
  buscfg.sclk_io_num = PIN_LCD_SCK;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = 64;
  esp_err_t err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
  Serial.printf("SPI bus init: %s\n", esp_err_to_name(err));

  spi_device_interface_config_t devcfg = {};
  devcfg.command_bits = 1;
  devcfg.address_bits = 8;
  devcfg.mode = SPI_MODE0;
  devcfg.clock_speed_hz = 40000000;
  devcfg.spics_io_num = -1;
  devcfg.queue_size = 1;
  err = spi_bus_add_device(SPI2_HOST, &devcfg, &nativeSpi);
  Serial.printf("SPI device add: %s\n", esp_err_to_name(err));

  pca9554Write(PCA9554_OUTPUT, EXIO_LCD_RST | EXIO_TP_RST); // CS low
  delay(10);

  // WICHTIG: KEIN SWRESET, KEIN frueher Sleep-Out!
  // Die funktionierende Referenz (Boatingwiththebaileys / Waveshare BSP)
  // startet direkt mit der Page-13-Sequenz. Ein SWRESET hier laesst das
  // Panel in einem Zustand zurueck wo die Init nicht greift -> Display
  // wird rosa/rot statt das Bild zu zeigen.
  nativeWriteCommand(0xFF); nativeWriteData(0x77); nativeWriteData(0x01); nativeWriteData(0x00); nativeWriteData(0x00); nativeWriteData(0x13);
  nativeWriteCommand(0xEF); nativeWriteData(0x08);
  nativeWriteCommand(0xFF); nativeWriteData(0x77); nativeWriteData(0x01); nativeWriteData(0x00); nativeWriteData(0x00); nativeWriteData(0x10);
  nativeWriteCommand(0xC0); nativeWriteData(0x3B); nativeWriteData(0x00);
  nativeWriteCommand(0xC1); nativeWriteData(0x10); nativeWriteData(0x0C);
  nativeWriteCommand(0xC2); nativeWriteData(0x07); nativeWriteData(0x0A);
  nativeWriteCommand(0xC7); nativeWriteData(0x00);
  nativeWriteCommand(0xCC); nativeWriteData(0x10);
  nativeWriteCommand(0xCD); nativeWriteData(0x08);
  nativeWriteCommand(0xB0);
  for (uint8_t v : {0x05,0x12,0x98,0x0E,0x0F,0x07,0x07,0x09,0x09,0x23,0x05,0x52,0x0F,0x67,0x2C,0x11}) nativeWriteData(v);
  nativeWriteCommand(0xB1);
  for (uint8_t v : {0x0B,0x11,0x97,0x0C,0x12,0x06,0x06,0x08,0x08,0x22,0x03,0x51,0x11,0x66,0x2B,0x0F}) nativeWriteData(v);
  nativeWriteCommand(0xFF); nativeWriteData(0x77); nativeWriteData(0x01); nativeWriteData(0x00); nativeWriteData(0x00); nativeWriteData(0x11);
  nativeWriteCommand(0xB0); nativeWriteData(0x5D);
  nativeWriteCommand(0xB1); nativeWriteData(0x3E);
  nativeWriteCommand(0xB2); nativeWriteData(0x81);
  nativeWriteCommand(0xB3); nativeWriteData(0x80);
  nativeWriteCommand(0xB5); nativeWriteData(0x4E);
  nativeWriteCommand(0xB7); nativeWriteData(0x85);
  nativeWriteCommand(0xB8); nativeWriteData(0x20);
  nativeWriteCommand(0xC1); nativeWriteData(0x78);
  nativeWriteCommand(0xC2); nativeWriteData(0x78);
  nativeWriteCommand(0xD0); nativeWriteData(0x88);
  nativeWriteCommand(0xE0); nativeWriteData(0x00); nativeWriteData(0x00); nativeWriteData(0x02);
  nativeWriteCommand(0xE1);
  for (uint8_t v : {0x06,0x30,0x08,0x30,0x05,0x30,0x07,0x30,0x00,0x33,0x33}) nativeWriteData(v);
  nativeWriteCommand(0xE2);
  for (uint8_t v : {0x11,0x11,0x33,0x33,0xF4,0x00,0x00,0x00,0xF4,0x00,0x00,0x00}) nativeWriteData(v);
  nativeWriteCommand(0xE3); nativeWriteData(0x00); nativeWriteData(0x00); nativeWriteData(0x11); nativeWriteData(0x11);
  nativeWriteCommand(0xE4); nativeWriteData(0x44); nativeWriteData(0x44);
  nativeWriteCommand(0xE5);
  for (uint8_t v : {0x0D,0xF5,0x30,0xF0,0x0F,0xF7,0x30,0xF0,0x09,0xF1,0x30,0xF0,0x0B,0xF3,0x30,0xF0}) nativeWriteData(v);
  nativeWriteCommand(0xE6); nativeWriteData(0x00); nativeWriteData(0x00); nativeWriteData(0x11); nativeWriteData(0x11);
  nativeWriteCommand(0xE7); nativeWriteData(0x44); nativeWriteData(0x44);
  nativeWriteCommand(0xE8);
  for (uint8_t v : {0x0C,0xF4,0x30,0xF0,0x0E,0xF6,0x30,0xF0,0x08,0xF0,0x30,0xF0,0x0A,0xF2,0x30,0xF0}) nativeWriteData(v);
  nativeWriteCommand(0xE9); nativeWriteData(0x36); nativeWriteData(0x01);
  nativeWriteCommand(0xEB); nativeWriteData(0x00); nativeWriteData(0x01); nativeWriteData(0xE4); nativeWriteData(0xE4); nativeWriteData(0x44); nativeWriteData(0x88); nativeWriteData(0x40);
  nativeWriteCommand(0xED);
  for (uint8_t v : {0xFF,0x10,0xAF,0x76,0x54,0x2B,0xCF,0xFF,0xFF,0xFC,0xB2,0x45,0x67,0xFA,0x01,0xFF}) nativeWriteData(v);
  nativeWriteCommand(0xEF); nativeWriteData(0x08); nativeWriteData(0x08); nativeWriteData(0x08); nativeWriteData(0x45); nativeWriteData(0x3F); nativeWriteData(0x54);
  nativeWriteCommand(0xFF); nativeWriteData(0x77); nativeWriteData(0x01); nativeWriteData(0x00); nativeWriteData(0x00); nativeWriteData(0x00);
  nativeWriteCommand(0x11);
  delay(120);
  nativeWriteCommand(0x3A); nativeWriteData(0x66);
  nativeWriteCommand(0x36); nativeWriteData(0x00);
  nativeWriteCommand(0x35); nativeWriteData(0x00);
  nativeWriteCommand(0x29);
  pca9554Write(PCA9554_OUTPUT, EXIO_LCD_RST | EXIO_TP_RST | EXIO_LCD_CS); // CS high
  delay(10);
}

static void nativePanelInit() {
  // Warm-Reset: altes Panel zerstoeren falls vorhanden
  if (nativePanel) {
    esp_lcd_panel_del(nativePanel);
    nativePanel = nullptr;
  }
  // Frame-Buffer freigeben (wird in ensureFrame() neu alloziert)
  if (nativeFrame) {
    heap_caps_free(nativeFrame);
    nativeFrame = nullptr;
  }

  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_PLL160M;
  cfg.timings.pclk_hz = 8000000;
  cfg.timings.h_res = 480;
  cfg.timings.v_res = 480;
  cfg.timings.hsync_pulse_width = 8;
  cfg.timings.hsync_back_porch = 10;
  cfg.timings.hsync_front_porch = 50;
  cfg.timings.vsync_pulse_width = 2;
  cfg.timings.vsync_back_porch = 18;
  cfg.timings.vsync_front_porch = 8;
  cfg.timings.flags.pclk_active_neg = 0;
  cfg.data_width = 16;
  cfg.bits_per_pixel = 16;
  cfg.num_fbs = 1;   // single fb wie funktionierende Referenz
  cfg.bounce_buffer_size_px = 0;
  cfg.psram_trans_align = 64;
  cfg.hsync_gpio_num = PIN_HSYNC;
  cfg.vsync_gpio_num = PIN_VSYNC;
  cfg.de_gpio_num = PIN_DE;
  cfg.pclk_gpio_num = PIN_PCLK;
  cfg.disp_gpio_num = GPIO_NUM_NC;
  cfg.data_gpio_nums[0] = PIN_B0;
  cfg.data_gpio_nums[1] = PIN_B1;
  cfg.data_gpio_nums[2] = PIN_B2;
  cfg.data_gpio_nums[3] = PIN_B3;
  cfg.data_gpio_nums[4] = PIN_B4;
  cfg.data_gpio_nums[5] = PIN_G0;
  cfg.data_gpio_nums[6] = PIN_G1;
  cfg.data_gpio_nums[7] = PIN_G2;
  cfg.data_gpio_nums[8] = PIN_G3;
  cfg.data_gpio_nums[9] = PIN_G4;
  cfg.data_gpio_nums[10] = PIN_G5;
  cfg.data_gpio_nums[11] = PIN_R0;
  cfg.data_gpio_nums[12] = PIN_R1;
  cfg.data_gpio_nums[13] = PIN_R2;
  cfg.data_gpio_nums[14] = PIN_R3;
  cfg.data_gpio_nums[15] = PIN_R4;
  cfg.flags.fb_in_psram = true;

  esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &nativePanel);
  Serial.printf("RGB panel create: %s\n", esp_err_to_name(err));
  if (err != ESP_OK) return;
  err = esp_lcd_panel_reset(nativePanel);
  Serial.printf("RGB panel reset: %s\n", esp_err_to_name(err));
  err = esp_lcd_panel_init(nativePanel);
  Serial.printf("RGB panel init: %s\n", esp_err_to_name(err));
}

static void nativeFill(uint16_t color) {
  if (!nativeFrame) {
    nativeFrame = (uint16_t *)heap_caps_malloc(480 * 480 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    Serial.printf("nativeFrame: %p\n", nativeFrame);
  }
  if (!nativeFrame) {
    return;
  }
  for (int i = 0; i < 480 * 480; i++) {
    nativeFrame[i] = color;
  }
  if (nativePanel) esp_lcd_panel_draw_bitmap(nativePanel, 0, 0, 480, 480, nativeFrame);
}

static bool ensureFrame() {
  if (!nativeFrame) {
    nativeFrame = (uint16_t *)heap_caps_malloc(480 * 480 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    Serial.printf("nativeFrame: %p\n", nativeFrame);
  }
  return nativeFrame != nullptr;
}

static void presentFrame() {
  if (nativeFrame && nativePanel) {
    esp_lcd_panel_draw_bitmap(nativePanel, 0, 0, 480, 480, nativeFrame);
  }
}

static void copyVdoDialToFrame() {
  if (!ensureFrame()) {
    return;
  }
  for (int i = 0; i < 480 * 480; i++) {
    nativeFrame[i] = pgm_read_word(&VDO_DIAL_480_RGB565[i]);
  }
}

static void setPixel(int x, int y, uint16_t color) {
  if ((unsigned)x < 480 && (unsigned)y < 480) {
    nativeFrame[y * 480 + x] = color;
  }
}

static void fillFrame(uint16_t color) {
  if (!ensureFrame()) {
    return;
  }
  for (int i = 0; i < 480 * 480; i++) {
    nativeFrame[i] = color;
  }
}

static void fillRectFast(int x, int y, int w, int h, uint16_t color) {
  for (int yy = y; yy < y + h; yy++) {
    for (int xx = x; xx < x + w; xx++) {
      setPixel(xx, yy, color);
    }
  }
}

static void drawCircleLine(int cx, int cy, int radius, int thickness, uint16_t color) {
  int outer = radius * radius;
  int innerRadius = radius - thickness;
  int inner = innerRadius * innerRadius;
  for (int y = cy - radius; y <= cy + radius; y++) {
    for (int x = cx - radius; x <= cx + radius; x++) {
      int dx = x - cx;
      int dy = y - cy;
      int d = dx * dx + dy * dy;
      if (d <= outer && d >= inner) {
        setPixel(x, y, color);
      }
    }
  }
}

static void fillCircleFast(int cx, int cy, int radius, uint16_t color) {
  int r2 = radius * radius;
  for (int y = cy - radius; y <= cy + radius; y++) {
    for (int x = cx - radius; x <= cx + radius; x++) {
      int dx = x - cx;
      int dy = y - cy;
      if (dx * dx + dy * dy <= r2) {
        setPixel(x, y, color);
      }
    }
  }
}

static void drawLineFast(int x0, int y0, int x1, int y1, uint16_t color, int thickness) {
  int dx = abs(x1 - x0);
  int sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0);
  int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  int radius = thickness / 2;

  while (true) {
    fillCircleFast(x0, y0, radius, color);
    if (x0 == x1 && y0 == y1) {
      break;
    }
    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

static void drawHand(float value, float maxValue, int length, int thickness, uint16_t color) {
  float angle = (value / maxValue) * 2.0f * PI - PI / 2.0f;
  int x = 240 + (int)(cosf(angle) * length);
  int y = 240 + (int)(sinf(angle) * length);
  drawLineFast(240, 240, x, y, color, thickness);
}

static void drawVdoLogo(uint16_t color) {
  fillRectFast(177, 138, 8, 34, color);
  drawLineFast(185, 172, 207, 138, color, 7);
  drawLineFast(207, 138, 229, 172, color, 7);

  fillRectFast(238, 138, 8, 34, color);
  fillCircleFast(252, 155, 17, color);
  fillCircleFast(252, 155, 9, RGB565_BLACK);
  fillRectFast(238, 138, 14, 34, RGB565_BLACK);
  fillRectFast(238, 138, 8, 34, color);

  drawCircleLine(303, 155, 18, 7, color);
}

static uint8_t glyphColumn(char c, uint8_t col) {
  static const uint8_t blank[5] = {0, 0, 0, 0, 0};
  const uint8_t *g = blank;
  switch (c) {
    case 'A': { static const uint8_t v[5] = {0x7E, 0x11, 0x11, 0x11, 0x7E}; g = v; break; }
    case 'B': { static const uint8_t v[5] = {0x7F, 0x49, 0x49, 0x49, 0x36}; g = v; break; }
    case 'D': { static const uint8_t v[5] = {0x7F, 0x41, 0x41, 0x22, 0x1C}; g = v; break; }
    case 'E': { static const uint8_t v[5] = {0x7F, 0x49, 0x49, 0x49, 0x41}; g = v; break; }
    case 'H': { static const uint8_t v[5] = {0x7F, 0x08, 0x08, 0x08, 0x7F}; g = v; break; }
    case 'I': { static const uint8_t v[5] = {0x00, 0x41, 0x7F, 0x41, 0x00}; g = v; break; }
    case 'L': { static const uint8_t v[5] = {0x7F, 0x40, 0x40, 0x40, 0x40}; g = v; break; }
    case 'M': { static const uint8_t v[5] = {0x7F, 0x02, 0x0C, 0x02, 0x7F}; g = v; break; }
    case 'N': { static const uint8_t v[5] = {0x7F, 0x04, 0x08, 0x10, 0x7F}; g = v; break; }
    case 'O': { static const uint8_t v[5] = {0x3E, 0x41, 0x41, 0x41, 0x3E}; g = v; break; }
    case 'P': { static const uint8_t v[5] = {0x7F, 0x09, 0x09, 0x09, 0x06}; g = v; break; }
    case 'Q': { static const uint8_t v[5] = {0x3E, 0x41, 0x51, 0x21, 0x5E}; g = v; break; }
    case 'R': { static const uint8_t v[5] = {0x7F, 0x09, 0x19, 0x29, 0x46}; g = v; break; }
    case 'S': { static const uint8_t v[5] = {0x46, 0x49, 0x49, 0x49, 0x31}; g = v; break; }
    case 'T': { static const uint8_t v[5] = {0x01, 0x01, 0x7F, 0x01, 0x01}; g = v; break; }
    case 'U': { static const uint8_t v[5] = {0x3F, 0x40, 0x40, 0x40, 0x3F}; g = v; break; }
    case 'V': { static const uint8_t v[5] = {0x1F, 0x20, 0x40, 0x20, 0x1F}; g = v; break; }
    case 'Y': { static const uint8_t v[5] = {0x07, 0x08, 0x70, 0x08, 0x07}; g = v; break; }
    case 'Z': { static const uint8_t v[5] = {0x61, 0x51, 0x49, 0x45, 0x43}; g = v; break; }
    case '-': { static const uint8_t v[5] = {0x08, 0x08, 0x08, 0x08, 0x08}; g = v; break; }
    case '0': { static const uint8_t v[5] = {0x3E, 0x51, 0x49, 0x45, 0x3E}; g = v; break; }
    case '1': { static const uint8_t v[5] = {0x00, 0x42, 0x7F, 0x40, 0x00}; g = v; break; }
    case '2': { static const uint8_t v[5] = {0x62, 0x51, 0x49, 0x49, 0x46}; g = v; break; }
    case '3': { static const uint8_t v[5] = {0x22, 0x41, 0x49, 0x49, 0x36}; g = v; break; }
    case '4': { static const uint8_t v[5] = {0x18, 0x14, 0x12, 0x7F, 0x10}; g = v; break; }
    case '5': { static const uint8_t v[5] = {0x27, 0x45, 0x45, 0x45, 0x39}; g = v; break; }
    case '6': { static const uint8_t v[5] = {0x3C, 0x4A, 0x49, 0x49, 0x30}; g = v; break; }
    case '7': { static const uint8_t v[5] = {0x01, 0x71, 0x09, 0x05, 0x03}; g = v; break; }
    case '8': { static const uint8_t v[5] = {0x36, 0x49, 0x49, 0x49, 0x36}; g = v; break; }
    case '9': { static const uint8_t v[5] = {0x06, 0x49, 0x49, 0x29, 0x1E}; g = v; break; }
    default: break;
  }
  return g[col];
}

static void drawTextSmall(int x, int y, const char *text, uint16_t color, int scale) {
  int cursor = x;
  while (*text) {
    char c = *text++;
    if (c == ' ') {
      cursor += 4 * scale;
      continue;
    }
    for (int col = 0; col < 5; col++) {
      uint8_t bits = glyphColumn(c, col);
      for (int row = 0; row < 7; row++) {
        if (bits & (1 << row)) {
          fillRectFast(cursor + col * scale, y + row * scale, scale, scale, color);
        }
      }
    }
    cursor += 6 * scale;
  }
}

static int textWidthSmall(const char *text, int scale) {
  int width = 0;
  while (*text) {
    width += (*text++ == ' ') ? 4 * scale : 6 * scale;
  }
  return width;
}

static void drawTextCentered(int cx, int y, const char *text, uint16_t color, int scale) {
  drawTextSmall(cx - textWidthSmall(text, scale) / 2, y, text, color, scale);
}

static void drawGlyphPixelRotated(int x, int y, int localX, int localY, int w, int h, int scale, int rotation, uint16_t color) {
  int rx = localX;
  int ry = localY;
  if (rotation == 1) {
    rx = h - 1 - localY;
    ry = localX;
  } else if (rotation == 2) {
    rx = w - 1 - localX;
    ry = h - 1 - localY;
  } else if (rotation == 3) {
    rx = localY;
    ry = w - 1 - localX;
  }
  fillRectFast(x + rx * scale, y + ry * scale, scale, scale, color);
}

static void drawTextRotated(int x, int y, const char *text, uint16_t color, int scale, int rotation) {
  int w = textWidthSmall(text, 1);
  int h = 7;
  int cursor = 0;
  while (*text) {
    char c = *text++;
    if (c == ' ') {
      cursor += 4;
      continue;
    }
    for (int col = 0; col < 5; col++) {
      uint8_t bits = glyphColumn(c, col);
      for (int row = 0; row < 7; row++) {
        if (bits & (1 << row)) {
          drawGlyphPixelRotated(x, y, cursor + col, row, w, h, scale, rotation, color);
        }
      }
    }
    cursor += 6;
  }
}

static void drawTextCenteredRotated(int cx, int cy, const char *text, uint16_t color, int scale, int rotation) {
  int w = textWidthSmall(text, 1) * scale;
  int h = 7 * scale;
  int rw = (rotation == 1 || rotation == 3) ? h : w;
  int rh = (rotation == 1 || rotation == 3) ? w : h;
  drawTextRotated(cx - rw / 2, cy - rh / 2, text, color, scale, rotation);
}

static void drawDialText(int cx, int cy, const char *text, uint16_t color, int scale, int rotation) {
  if (rotation == 0) {
    drawTextCentered(cx, cy - (7 * scale) / 2, text, color, scale);
  } else {
    drawTextCenteredRotated(cx, cy, text, color, scale, rotation);
  }
}

static void drawVdoClock() {
  if (!ensureFrame()) {
    return;
  }

  copyVdoDialToFrame();

  struct tm now = {};
  readClockTime(&now);
  float seconds = now.tm_sec;
  float minuteValue = now.tm_min + seconds / 60.0f;
  float hourValue = (now.tm_hour % 12) + minuteValue / 60.0f;

  drawHand(hourValue, 12.0f, 118, 18, RGB565(24, 24, 22));
  drawHand(hourValue, 12.0f, 118, 13, RGB565(222, 222, 214));
  drawHand(minuteValue, 60.0f, 172, 15, RGB565(24, 24, 22));
  drawHand(minuteValue, 60.0f, 172, 10, RGB565(226, 226, 218));
  drawHand(seconds, 60.0f, 188, 4, RGB565(235, 24, 20));

  fillCircleFast(240, 240, 26, RGB565(205, 205, 198));
  fillCircleFast(240, 240, 15, RGB565(166, 122, 42));
  fillCircleFast(240, 240, 9, RGB565(38, 30, 18));
  fillCircleFast(240, 240, 5, RGB565_BLACK);
  presentFrame();
}

static void drawMenuTile(int x, int y, int w, int h, const char *label, uint16_t accent) {
  fillRectFast(x, y, w, h, RGB565(18, 18, 18));
  fillRectFast(x, y, 8, h, accent);
  drawLineFast(x, y, x + w, y, RGB565(70, 70, 70), 2);
  drawLineFast(x, y + h, x + w, y + h, RGB565(55, 55, 55), 2);
  drawTextSmall(x + 24, y + 22, label, RGB565(235, 235, 225), 4);
}

static void drawMenuOverview() {
  if (!ensureFrame()) {
    return;
  }
  fillFrame(RGB565_BLACK);
  drawCircleLine(240, 240, 232, 3, RGB565(80, 80, 75));
  drawTextSmall(78, 52, "MENU", RGB565(235, 235, 225), 8);

  drawMenuTile(60, 120, 360, 58, "UHR", RGB565(200, 40, 35));
  drawMenuTile(60, 190, 360, 58, "MOTOR", RGB565(40, 150, 210));
  drawMenuTile(60, 260, 360, 58, "LAMBDA", RGB565(60, 185, 90));
  drawMenuTile(60, 330, 360, 58, "SETUP", RGB565(210, 170, 45));
  presentFrame();
}

void setup() {
  // Cold-Boot Robustness: erst 250ms warten damit die Versorgung sauber
  // hochlaeuft, bevor wir I2C/Display anpacken. Bei direktem USB-Plug-in
  // war das Display sonst manchmal schwarz weil der PCA9554 noch nicht
  // sicher antwortet und der erste expander-Write ins Leere ging.
  delay(250);

  Serial.begin(115200);
  // USB-CDC: Nicht auf Host warten. Wenn kein Serial-Monitor offen ist,
  // soll der Boot trotzdem sofort weiterlaufen.
  uint32_t serialWait = millis();
  while (!Serial && millis() - serialWait < 2000) delay(10);
  Serial.println("\n=== Waveshare 2.8C VDO Clock ===");

  Serial.printf("PSRAM found: %s, size: %u bytes\n", psramFound() ? "yes" : "no", ESP.getPsramSize());

  // Backlight Diagnose-Blink: 2x 50ms ON-OFF, damit man sieht dass der
  // Chip bootet und GPIO 6 schaltbar ist — selbst wenn der Panel-Init
  // spaeter scheitert, sieht man wenigstens "die Hardware lebt".
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);
  delay(50);
  digitalWrite(PIN_LCD_BL, LOW);
  delay(50);
  digitalWrite(PIN_LCD_BL, HIGH);
  delay(50);
  digitalWrite(PIN_LCD_BL, LOW);   // wieder aus, sonst sieht man Init-Muell

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);
  delay(20);  // I2C-Bus erstmal still lassen
  scanI2C();
  Serial.println("PCA9554: init + reset");
  expanderInit();

  Serial.println("Display: native ST7701 init...");
  nativeSt7701Init();
  nativePanelInit();
  Serial.println("Display: native panel OK");

  gt911Init();
  initTimeSource();

  // Backlight an
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  drawVdoClock();
  Serial.println("VDO clock drawn.");
}

void loop() {
  static uint32_t lastTouch = 0;
  static uint32_t lastClockDraw = 0;
  uint16_t x = 0;
  uint16_t y = 0;

  if (readTouch(&x, &y) && millis() - lastTouch > 350) {
    lastTouch = millis();
    touchSeen = true;
    Serial.printf("touch x=%u y=%u page=%u\n", x, y, currentPage);

    if (currentPage == 0) {
      currentPage = 1;
      drawMenuOverview();
      Serial.println("page: menu");
    } else {
      currentPage = 0;
      drawVdoClock();
      Serial.println("page: clock");
    }
  }

  if (currentPage == 0 && millis() - lastClockDraw >= 1000) {
    lastClockDraw = millis();
    drawVdoClock();
  }

  delay(10);
}
