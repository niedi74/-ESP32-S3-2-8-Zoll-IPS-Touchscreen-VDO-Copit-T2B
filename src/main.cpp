// Waveshare ESP32-S3-Touch-LCD-2.8C - VDO Quartz-Zeit Clock
// Main app: clock, touch, WiFi/NTP and WebGUI.
// Display hardware ownership lives in hal_waveshare_28c.h.
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#if __has_include("wifi_secret.h")
  #include "wifi_secret.h"
#else
  #define WIFI_SSID     ""
  #define WIFI_PASSWORD ""
#endif
#include "hal_waveshare_28c.h"
#include "vdo_dial_480_rgb565.h"
#include <sys/time.h>
#include <time.h>

// ---- Touch / I2C Peripherie ----
#define PIN_TOUCH_INT 16

#define GT911_ADDR_PRIMARY 0x5D
#define GT911_ADDR_ALT     0x14
#define GT911_PRODUCT_ID   0x8140
#define GT911_READ_XY      0x814E

// ---- PCF85063 RTC (Echtzeituhr, Batterie-Backup) ----
#define PCF85063_ADDR      0x51
#define PCF85063_CTRL1     0x00
#define PCF85063_SECONDS   0x04   // sec, min, hour, day, wday, month, year (7 Byte)

#ifndef RGB565
#define RGB565(r, g, b) (uint16_t)((((uint16_t)(r) & 0xF8) << 8) | (((uint16_t)(g) & 0xFC) << 3) | ((uint16_t)(b) >> 3))
#endif
#ifndef RGB565_BLACK
#define RGB565_BLACK RGB565(0, 0, 0)
#endif

static uint8_t gt911Addr = GT911_ADDR_PRIMARY;
static bool gt911Found = false;
static uint8_t currentPage = 0;
static bool touchSeen = false;
static char g_ipStr[20] = "---";   // IP-Adresse fuers Menue
static int  g_dialScalePct = 100;  // Zifferblatt-Groesse in % (50..100)
static bool g_redrawClock = false; // Flag: Uhr neu zeichnen (z.B. nach Web-Aenderung)
static WebServer webServer(80);
static void startWebServer();   // Forward-Deklaration (Definition vor setup())

// Build-Zeit-Fallbacks falls inject_time.py nicht lief
#ifndef RTC_BUILD_Y
#define RTC_BUILD_Y 2026
#define RTC_BUILD_MO 1
#define RTC_BUILD_D 1
#define RTC_BUILD_H 12
#define RTC_BUILD_MI 0
#define RTC_BUILD_S 0
#define RTC_BUILD_DOW 4
#define RTC_BUILD_ID 0
#endif

// ---- PCF85063 RTC Helfer ----
static uint8_t decToBcd(uint8_t v) { return (uint8_t)((v / 10 * 16) + (v % 10)); }
static uint8_t bcdToDec(uint8_t v) { return (uint8_t)((v / 16 * 10) + (v % 16)); }

// RTC auslesen in struct tm (lokale Wall-Clock-Zeit).
static bool rtcRead(struct tm *now) {
  Wire.beginTransmission(PCF85063_ADDR);
  Wire.write(PCF85063_SECONDS);
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom((int)PCF85063_ADDR, 7) != 7) return false;
  uint8_t b[7];
  for (int i = 0; i < 7; i++) b[i] = Wire.read();
  now->tm_sec  = bcdToDec(b[0] & 0x7F);
  now->tm_min  = bcdToDec(b[1] & 0x7F);
  now->tm_hour = bcdToDec(b[2] & 0x3F);
  now->tm_mday = bcdToDec(b[3] & 0x3F);
  now->tm_wday = bcdToDec(b[4] & 0x07);
  now->tm_mon  = bcdToDec(b[5] & 0x1F) - 1;     // 1-12 -> 0-11
  now->tm_year = bcdToDec(b[6]) + 100;          // Jahr ab 2000 -> tm_year ab 1900
  return true;
}

// RTC stellen aus struct tm.
static void rtcWrite(const struct tm *t) {
  // CTRL1: Normalbetrieb, 24h, 12.5pF Lastkapazitaet
  Wire.beginTransmission(PCF85063_ADDR);
  Wire.write(PCF85063_CTRL1);
  Wire.write(0x01);  // CAP_SEL=12.5pF, STOP=0 (laeuft)
  Wire.endTransmission();

  Wire.beginTransmission(PCF85063_ADDR);
  Wire.write(PCF85063_SECONDS);
  Wire.write(decToBcd(t->tm_sec));
  Wire.write(decToBcd(t->tm_min));
  Wire.write(decToBcd(t->tm_hour));
  Wire.write(decToBcd(t->tm_mday));
  Wire.write(decToBcd(t->tm_wday));
  Wire.write(decToBcd(t->tm_mon + 1));
  Wire.write(decToBcd((t->tm_year + 1900) - 2000));
  Wire.endTransmission();
}

static bool readClockTime(struct tm *now) {
  if (rtcRead(now)) return true;
  // Fallback falls RTC nicht antwortet: Systemzeit
  time_t t = time(nullptr);
  return localtime_r(&t, now) != nullptr;
}

static void initTimeSource() {
  struct tm rtcNow = {};
  bool haveRtc = rtcRead(&rtcNow);
  bool rtcValid = haveRtc && (rtcNow.tm_year + 1900) >= 2024;

  // RTC genau einmal pro Flash stellen (neue Build-ID) oder wenn ungueltig.
  Preferences prefs;
  prefs.begin("clock", false);
  uint32_t savedId = prefs.getUInt("buildid", 0);
  bool newFlash = (savedId != (uint32_t)RTC_BUILD_ID);

  if (!rtcValid || newFlash) {
    struct tm bt = {};
    bt.tm_year = RTC_BUILD_Y - 1900;
    bt.tm_mon  = RTC_BUILD_MO - 1;
    bt.tm_mday = RTC_BUILD_D;
    bt.tm_hour = RTC_BUILD_H;
    bt.tm_min  = RTC_BUILD_MI;
    bt.tm_sec  = RTC_BUILD_S;
    bt.tm_wday = RTC_BUILD_DOW;
    rtcWrite(&bt);
    prefs.putUInt("buildid", (uint32_t)RTC_BUILD_ID);
    Serial.printf("RTC set from build time: %04d-%02d-%02d %02d:%02d:%02d (reason: %s)\n",
                  RTC_BUILD_Y, RTC_BUILD_MO, RTC_BUILD_D, RTC_BUILD_H, RTC_BUILD_MI, RTC_BUILD_S,
                  !rtcValid ? "RTC invalid" : "new flash");
  } else {
    Serial.printf("RTC running: %04d-%02d-%02d %02d:%02d:%02d\n",
                  rtcNow.tm_year + 1900, rtcNow.tm_mon + 1, rtcNow.tm_mday,
                  rtcNow.tm_hour, rtcNow.tm_min, rtcNow.tm_sec);
  }
  prefs.end();
}

// Nicht-blockierender WiFi/NTP-Handler, wird aus loop() aufgerufen.
// - WiFi verbindet im Hintergrund (in setup gestartet), blockiert nie.
// - Sobald verbunden: IP merken, SNTP einmal starten.
// - Sobald SNTP eine gueltige Zeit liefert: einmal in RTC schreiben.
// - Bei Verbindungsverlust: automatischer Reconnect-Versuch alle 30s.
// Rueckgabe: true wenn gerade frisch synchronisiert (Uhr neu zeichnen).
static bool wifiNtpTick() {
  static bool sntpStarted = false;
  static bool ntpSynced = false;
  static uint32_t lastTry = 0;

  if (strlen(WIFI_SSID) == 0) return false;

  if (WiFi.status() != WL_CONNECTED) {
    // alle 30s erneut versuchen
    if (millis() - lastTry > 30000) {
      lastTry = millis();
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      Serial.println("WiFi: Reconnect-Versuch");
    }
    if (g_ipStr[0] == '-') strcpy(g_ipStr, "...");
    return false;
  }

  // Verbunden -> IP merken
  static char lastIp[20] = "";
  static bool webStarted = false;
  snprintf(g_ipStr, sizeof(g_ipStr), "%s", WiFi.localIP().toString().c_str());
  if (strcmp(lastIp, g_ipStr) != 0) {
    strcpy(lastIp, g_ipStr);
    Serial.printf("WiFi verbunden, IP: %s\n", g_ipStr);
  }
  if (!webStarted) {
    startWebServer();
    webStarted = true;
  }

  if (!sntpStarted) {
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov", "de.pool.ntp.org");
    sntpStarted = true;
    Serial.println("NTP: SNTP gestartet");
  }

  if (!ntpSynced) {
    time_t t = time(nullptr);
    if (t > 1700000000) {  // gueltige Zeit angekommen (nach 2023)
      struct tm now;
      localtime_r(&t, &now);
      rtcWrite(&now);
      ntpSynced = true;
      Serial.printf("NTP: synchronisiert -> RTC gestellt: %04d-%02d-%02d %02d:%02d:%02d\n",
                    now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
                    now.tm_hour, now.tm_min, now.tm_sec);
      return true;  // Uhr neu zeichnen
    }
  }
  return false;
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
  delay(20);
  digitalWrite(PIN_TOUCH_INT, intHigh ? HIGH : LOW);
  delay(5);
  pinMode(PIN_TOUCH_INT, INPUT);
  delay(80);
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

// Display framebuffer access: all hardware init and RGB panel ownership lives in HAL.
static bool ensureFrame() {
  return hal_fb() != nullptr;
}

static void presentFrame() {
  hal_present();
}

// Zifferblatt in den Frame kopieren, skaliert auf g_dialScalePct % und
// zentriert auf schwarzem Grund (Nearest-Neighbor). Bei 100% = 1:1.
static void copyVdoDialToFrame() {
  uint16_t *fb = hal_fb();
  if (!fb) {
    return;
  }
  int pct = g_dialScalePct;
  if (pct < 30) pct = 30;
  if (pct > 100) pct = 100;

  if (pct == 100) {
    for (int i = 0; i < 480 * 480; i++) {
      fb[i] = pgm_read_word(&VDO_DIAL_480_RGB565[i]);
    }
    return;
  }

  // Hintergrund schwarz, dann skaliertes Zifferblatt zentriert einsetzen.
  for (int i = 0; i < 480 * 480; i++) fb[i] = RGB565_BLACK;
  int outSize = (480 * pct) / 100;
  int offset = (480 - outSize) / 2;
  for (int oy = 0; oy < outSize; oy++) {
    int sy = (oy * 480) / outSize;
    int dstRow = (offset + oy) * 480 + offset;
    int srcRow = sy * 480;
    for (int ox = 0; ox < outSize; ox++) {
      int sx = (ox * 480) / outSize;
      fb[dstRow + ox] = pgm_read_word(&VDO_DIAL_480_RGB565[srcRow + sx]);
    }
  }
}

static void setPixel(int x, int y, uint16_t color) {
  uint16_t *fb = hal_fb();
  if (fb && (unsigned)x < 480 && (unsigned)y < 480) {
    fb[y * 480 + x] = color;
  }
}

static void fillFrame(uint16_t color) {
  hal_fill(color);
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
    case '.': { static const uint8_t v[5] = {0x00, 0x00, 0x40, 0x00, 0x00}; g = v; break; }
    case ':': { static const uint8_t v[5] = {0x00, 0x00, 0x24, 0x00, 0x00}; g = v; break; }
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

  // Zeiger + Nabe mit dem Zifferblatt mitskalieren
  float s = g_dialScalePct / 100.0f;
  if (s < 0.30f) s = 0.30f;
  if (s > 1.0f) s = 1.0f;
  #define SC(v) ((int)((v) * s + 0.5f))

  drawHand(hourValue, 12.0f, SC(118), SC(18), RGB565(24, 24, 22));
  drawHand(hourValue, 12.0f, SC(118), SC(13), RGB565(222, 222, 214));
  drawHand(minuteValue, 60.0f, SC(172), SC(15), RGB565(24, 24, 22));
  drawHand(minuteValue, 60.0f, SC(172), SC(10), RGB565(226, 226, 218));
  drawHand(seconds, 60.0f, SC(188), SC(4), RGB565(235, 24, 20));

  fillCircleFast(240, 240, SC(26), RGB565(205, 205, 198));
  fillCircleFast(240, 240, SC(15), RGB565(166, 122, 42));
  fillCircleFast(240, 240, SC(9), RGB565(38, 30, 18));
  fillCircleFast(240, 240, SC(5), RGB565_BLACK);
  #undef SC
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

  // IP-Adresse im Netz unten anzeigen
  char ipLine[32];
  snprintf(ipLine, sizeof(ipLine), "IP %s", g_ipStr);
  drawTextCentered(240, 404, ipLine, RGB565(150, 200, 150), 2);

  presentFrame();
}

// -------- Einstellungen (Preferences) --------
static void loadSettings() {
  Preferences p;
  p.begin("clock", true);
  g_dialScalePct = p.getInt("scale", 100);
  p.end();
  if (g_dialScalePct < 30) g_dialScalePct = 30;
  if (g_dialScalePct > 100) g_dialScalePct = 100;
}

static void saveDialScale(int pct) {
  if (pct < 30) pct = 30;
  if (pct > 100) pct = 100;
  g_dialScalePct = pct;
  Preferences p;
  p.begin("clock", false);
  p.putInt("scale", pct);
  p.end();
}

// -------- Web-GUI --------
static void handleWebRoot() {
  struct tm now = {};
  readClockTime(&now);
  char timeStr[16];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", now.tm_hour, now.tm_min, now.tm_sec);

  String html = F("<!DOCTYPE html><html lang='de'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>VDO Uhr</title><style>"
    "body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:20px;text-align:center}"
    "h1{color:#e0c040;font-weight:600}.card{background:#1c1c1c;border-radius:12px;padding:18px;margin:14px auto;max-width:420px}"
    ".big{font-size:2.2em;letter-spacing:2px}input[type=range]{width:90%}"
    "button{background:#e0c040;border:0;border-radius:8px;padding:12px 20px;font-size:1em;margin:6px;cursor:pointer}"
    "a{color:#8cf}.val{font-size:1.6em;color:#e0c040}</style></head><body>");
  html += F("<h1>VDO Quartz-Zeit</h1>");
  html += "<div class='card'><div class='big'>" + String(timeStr) + "</div>";
  html += "<div>IP " + String(g_ipStr) + "</div></div>";
  html += F("<div class='card'><h3>Zifferblatt-Gr&ouml;&szlig;e</h3>"
    "<form action='/set' method='get'>"
    "<div class='val'><span id='v'>");
  html += String(g_dialScalePct);
  html += F("</span>%</div>"
    "<input type='range' name='scale' min='30' max='100' step='1' value='");
  html += String(g_dialScalePct);
  html += F("' oninput=\"document.getElementById('v').innerText=this.value\">"
    "<br><button type='submit'>&Uuml;bernehmen</button></form>"
    "<div><a href='/set?scale=100'>100%</a> &middot; <a href='/set?scale=90'>90%</a> &middot; "
    "<a href='/set?scale=80'>80%</a> &middot; <a href='/set?scale=70'>70%</a></div></div>");
  html += F("<p style='color:#666'>VW T2b Cockpit &middot; ESP32-S3 2.8\"</p></body></html>");
  webServer.send(200, "text/html", html);
}

static void handleWebSet() {
  if (webServer.hasArg("scale")) {
    saveDialScale(webServer.arg("scale").toInt());
    g_redrawClock = true;
    Serial.printf("Web: Zifferblatt-Groesse = %d%%\n", g_dialScalePct);
  }
  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

static void startWebServer() {
  webServer.on("/", handleWebRoot);
  webServer.on("/set", handleWebSet);
  webServer.begin();
  Serial.println("WebGUI: gestartet auf Port 80");
}

void setup() {
  // Cold-Boot Robustness: give USB power and the expander time to settle.
  delay(500);

  Serial.begin(115200);
  // USB-CDC: Nicht auf Host warten. Wenn kein Serial-Monitor offen ist,
  // soll der Boot trotzdem sofort weiterlaufen.
  uint32_t serialWait = millis();
  while (!Serial && millis() - serialWait < 2000) delay(10);
  Serial.println("\n=== Waveshare 2.8C VDO Clock ===");

  Serial.printf("PSRAM found: %s, size: %u bytes\n", psramFound() ? "yes" : "no", ESP.getPsramSize());

  // Backlight Diagnose-Blink: 2x 50ms ON-OFF, bevor das Panel initialisiert wird.
  hal_backlight(true);
  delay(50);
  hal_backlight(false);
  delay(50);
  hal_backlight(true);
  delay(50);
  hal_backlight(false);

  loadSettings();

  // WiFi startet vor dem RGB-Panel. So passieren eventuelle Cache-Disable-Phasen
  // nicht mitten in der Panel-Initialisierung.
  if (strlen(WIFI_SSID) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("WiFi: Verbindung zu '%s' im Hintergrund gestartet\n", WIFI_SSID);
  }

  Serial.println("Display: HAL init...");
  hal_init();
  gt911Init();
  initTimeSource();
  hal_backlight(true);

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

  // WiFi/NTP im Hintergrund (nicht-blockierend). Bei frischem Sync Uhr neu.
  if (wifiNtpTick() && currentPage == 0) {
    drawVdoClock();
  }

  // Web-GUI bedienen
  if (WiFi.status() == WL_CONNECTED) {
    webServer.handleClient();
  }

  // Neuzeichnen nach Web-Aenderung (z.B. Zifferblatt-Groesse)
  if (g_redrawClock) {
    g_redrawClock = false;
    if (currentPage == 0) drawVdoClock();
  }

  if (currentPage == 0 && millis() - lastClockDraw >= 1000) {
    lastClockDraw = millis();
    drawVdoClock();
  }

  delay(10);
}
