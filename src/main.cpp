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
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <NimBLEDevice.h>
#include <FS.h>
#include <SD_MMC.h>
#if __has_include("wifi_secret.h")
  #include "wifi_secret.h"
#else
  #define WIFI_SSID     ""
  #define WIFI_PASSWORD ""
#endif
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
#define EXIO_SD_D3    (1 << 3)

#define GT911_ADDR_PRIMARY 0x5D
#define GT911_ADDR_ALT     0x14
#define GT911_PRODUCT_ID   0x8140
#define GT911_READ_XY      0x814E

// ---- PCF85063 RTC (Echtzeituhr, Batterie-Backup) ----
#define PCF85063_ADDR      0x51
#define PCF85063_CTRL1     0x00
#define PCF85063_SECONDS   0x04   // sec, min, hour, day, wday, month, year (7 Byte)

// ---- LCD ST7701 Init-SPI (3-wire; CS extern via Expander) ----
#define PIN_LCD_SCK   2
#define PIN_LCD_MOSI  1
#define PIN_LCD_BL    6
#define PIN_SD_CLK    2
#define PIN_SD_CMD    1
#define PIN_SD_D0     42

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
static uint8_t pcaOutputState = EXIO_LCD_RST | EXIO_TP_RST | EXIO_LCD_CS | EXIO_SD_D3;
static uint8_t gt911Addr = GT911_ADDR_PRIMARY;
static bool gt911Found = false;
static uint8_t currentPage = 0;
static bool touchSeen = false;
static uint16_t g_lastTouchX = 0;
static uint16_t g_lastTouchY = 0;
static uint8_t g_lastTouchStatus = 0;
static uint32_t g_lastTouchMs = 0;
static char g_ipStr[20] = "---";   // IP-Adresse fuers Menue
static int  g_dialScalePct = 100;  // Zifferblatt-Groesse in % (50..100)
static int  g_brightnessPct = 100; // LCD-Helligkeit in % (5..100)
static int  g_rotationDeg = 0;      // Display-Rotation in Grad (0..359)
static String g_wifiSsid = "";
static String g_wifiPassword = "";
static bool g_wifiSaved = false;
static bool g_webStarted = false;
static bool g_sdReady = false;
static String g_sdType = "none";
static uint64_t g_sdTotalBytes = 0;
static uint64_t g_sdUsedBytes = 0;
static bool g_redrawClock = false; // Flag: Uhr neu zeichnen (z.B. nach Web-Aenderung)
static bool g_redrawPage = false;  // Flag: aktuelle Display-Seite neu zeichnen
static WebServer webServer(80);
static void startWebServer();   // Forward-Deklaration (Definition vor setup())

// -------- Spartan3-Hub BLE-Client --------
#define SPARTAN_MAC     "30:30:f9:1d:d0:fd"
#define SPARTAN_SVC     "7f510001-5a6b-4d2a-9f20-14a7f3e20000"
#define SPARTAN_STATUS  "7f510002-5a6b-4d2a-9f20-14a7f3e20000"

// Live-Daten vom Hub
static float g_lambda = 0, g_rpm = 0, g_adv = 0, g_map = 0;
static float g_battVolt = 0, g_speedKmh = 0;
static float g_g123Volt = 0, g_g123Temp = 0, g_g123Coil = 0;
static bool  g_lambdaValid = false, g_battValid = false;
static bool  g_speedValid = false, g_g123Valid = false;
static bool  g_bleConn = false;
static uint32_t g_bleLastRx = 0;   // millis des letzten Notify
static uint32_t g_bleRxCnt = 0;

// BLE-Verbindungs-State
static NimBLEClient* bleClient = nullptr;
static NimBLEAddress bleTarget;
static volatile bool bleDoConnect = false;
static uint32_t bleNextScanAt = 0;

// Kompakt-Format parsen: L<lam>R<rpm>A<adv>M<map>[V<volt>][S<kmh>][I<v>T<t>C<c>]
static void parseSpartanPayload(const String& p) {
  if (!(p.startsWith("L") && p.indexOf('R') > 1)) return;
  int posR = p.indexOf('R');
  int posA = p.indexOf('A', posR + 1);
  int posM = p.indexOf('M', posA + 1);
  if (!(posR > 1 && posA > posR && posM > posA)) return;

  g_lambda = p.substring(1, posR).toFloat();        g_lambdaValid = true;
  g_rpm    = p.substring(posR + 1, posA).toFloat();
  g_adv    = p.substring(posA + 1, posM).toFloat();

  int posV = p.indexOf('V', posM + 1);
  int posS = p.indexOf('S', posM + 1);
  int posI = p.indexOf('I', posM + 1);
  int mapEnd = posV > posM ? posV : (posS > posM ? posS : (posI > posM ? posI : p.length()));
  g_map = p.substring(posM + 1, mapEnd).toFloat();

  if (posV > posM) {
    int vEnd = posS > posV ? posS : (posI > posV ? posI : p.length());
    float v = p.substring(posV + 1, vEnd).toFloat();
    if (v > 0.5f) { g_battVolt = v; g_battValid = true; }
  }
  if (posS > posM) {
    int sEnd = posI > posS ? posI : p.length();
    g_speedKmh = p.substring(posS + 1, sEnd).toFloat();
    g_speedValid = true;
  }
  if (posI > posM) {
    int posT = p.indexOf('T', posI + 1);
    int posC = p.indexOf('C', posT > 0 ? posT + 1 : posI + 1);
    if (posT > posI && posC > posT) {
      g_g123Volt = p.substring(posI + 1, posT).toFloat();
      g_g123Temp = p.substring(posT + 1, posC).toFloat();
      g_g123Coil = p.substring(posC + 1).toFloat();
      g_g123Valid = true;
    }
  }
  g_bleRxCnt++;
  g_bleLastRx = millis();
}

static void bleNotifyCB(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  String s;
  s.reserve(len + 1);
  for (size_t i = 0; i < len; i++) s += (char)data[i];
  parseSpartanPayload(s);
}

class SpartanClientCB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient*) override {
    g_bleConn = true;
    Serial.println("BLE: mit Spartan-Hub verbunden");
  }
  void onDisconnect(NimBLEClient*, int reason) override {
    g_bleConn = false;
    Serial.printf("BLE: getrennt (reason=%d), neuer Scan\n", reason);
    bleNextScanAt = millis() + 1500;
  }
  bool onConnParamsUpdateRequest(NimBLEClient*, const ble_gap_upd_params*) override {
    return true;
  }
};

class SpartanScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    String addr = dev->getAddress().toString().c_str();
    addr.toLowerCase();
    String name = dev->getName().c_str();
    if (addr == SPARTAN_MAC || name == SPARTAN_HUB_NAME ||
        dev->isAdvertisingService(NimBLEUUID(SPARTAN_SVC))) {
      bleTarget = dev->getAddress();
      bleDoConnect = true;
      NimBLEDevice::getScan()->stop();
      Serial.printf("BLE: Spartan-Hub gefunden (%s)\n", addr.c_str());
    }
  }
  void onScanEnd(const NimBLEScanResults&, int) override {
    if (!g_bleConn && !bleDoConnect) bleNextScanAt = millis() + 2000;
  }
};

static SpartanClientCB spartanClientCB;
static SpartanScanCB   spartanScanCB;

static void bleStartScan() {
  if (g_bleConn || bleDoConnect) return;
  auto* s = NimBLEDevice::getScan();
  s->setScanCallbacks(&spartanScanCB);
  s->setActiveScan(true);
  s->setInterval(100);
  s->setWindow(99);
  s->start(8000, false);
  Serial.println("BLE: Scan nach Spartan-Hub...");
}

static void bleConnect() {
  bleDoConnect = false;
  if (!bleClient) {
    bleClient = NimBLEDevice::createClient();
    bleClient->setClientCallbacks(&spartanClientCB, false);
  }
  if (!bleClient->connect(bleTarget, true, false, false)) {
    Serial.println("BLE: Connect fehlgeschlagen");
    bleNextScanAt = millis() + 2000;
    return;
  }
  auto* svc = bleClient->getService(SPARTAN_SVC);
  if (!svc) { Serial.println("BLE: kein Service"); bleNextScanAt = millis() + 2000; return; }
  auto* status = svc->getCharacteristic(SPARTAN_STATUS);
  if (!status) { Serial.println("BLE: kein Status-Char"); bleNextScanAt = millis() + 2000; return; }
  bool ok = status->subscribe(true, bleNotifyCB, true);
  Serial.printf("BLE: Subscribe %s\n", ok ? "OK" : "FAIL");
}

// Nicht-blockierend aus loop() aufrufen.
static void bleTick() {
  if (bleDoConnect) { bleConnect(); return; }
  if (!g_bleConn && bleNextScanAt != 0 && millis() >= bleNextScanAt) {
    bleNextScanAt = 0;
    bleStartScan();
  }
}

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

  if (g_wifiSsid.length() == 0) return false;

  if (WiFi.status() != WL_CONNECTED) {
    // alle 30s erneut versuchen
    if (millis() - lastTry > 30000) {
      lastTry = millis();
      WiFi.disconnect();
      WiFi.begin(g_wifiSsid.c_str(), g_wifiPassword.c_str());
      Serial.printf("WiFi: Reconnect-Versuch zu '%s'\n", g_wifiSsid.c_str());
    }
    if (g_ipStr[0] == '-') strcpy(g_ipStr, "...");
    return false;
  }

  // Verbunden -> IP merken
  static char lastIp[20] = "";
  snprintf(g_ipStr, sizeof(g_ipStr), "%s", WiFi.localIP().toString().c_str());
  if (strcmp(lastIp, g_ipStr) != 0) {
    strcpy(lastIp, g_ipStr);
    Serial.printf("WiFi verbunden, IP: %s\n", g_ipStr);
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
  pca9554Write(PCA9554_CONFIG, 0xF0);   // Pin0..3 = Output

  // 1) Alles HIGH (Ausgangs-Zustand)
  pca9554Write(PCA9554_OUTPUT, EXIO_LCD_RST | EXIO_TP_RST | EXIO_LCD_CS | EXIO_SD_D3);
  delay(120);

  // 2) RST LOW — wie in der funktionierenden Referenz (Boatingwiththebaileys):
  //    kurzer Puls 10ms low, 50ms high reicht.
  pca9554Write(PCA9554_OUTPUT, EXIO_LCD_CS | EXIO_SD_D3);  // RST LOW, CS HIGH
  delay(180);

  // 3) RST HIGH — 50ms Settle
  pca9554Write(PCA9554_OUTPUT, EXIO_LCD_RST | EXIO_TP_RST | EXIO_LCD_CS | EXIO_SD_D3);
  delay(220);

  // 4) CS LOW fuer SPI-Init
  pca9554Write(PCA9554_OUTPUT, EXIO_LCD_RST | EXIO_TP_RST | EXIO_SD_D3);
  delay(40);
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
  pinMode(PIN_TOUCH_INT, INPUT_PULLUP);
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
  g_lastTouchStatus = status;
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
  g_lastTouchX = *x;
  g_lastTouchY = *y;
  g_lastTouchMs = millis();
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
static uint16_t *presentFrameBuf = nullptr;

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

  pca9554Write(PCA9554_OUTPUT, EXIO_LCD_RST | EXIO_TP_RST | EXIO_SD_D3); // CS low
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
  pca9554Write(PCA9554_OUTPUT, EXIO_LCD_RST | EXIO_TP_RST | EXIO_LCD_CS | EXIO_SD_D3); // CS high
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
    int rot = g_rotationDeg % 360;
    if (rot < 0) rot += 360;
    if (rot == 0) {
      esp_lcd_panel_draw_bitmap(nativePanel, 0, 0, 480, 480, nativeFrame);
      return;
    }
    if (!presentFrameBuf) {
      presentFrameBuf = (uint16_t *)heap_caps_malloc(480 * 480 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      Serial.printf("presentFrameBuf: %p\n", presentFrameBuf);
    }
    if (!presentFrameBuf) {
      esp_lcd_panel_draw_bitmap(nativePanel, 0, 0, 480, 480, nativeFrame);
      return;
    }
    const float rad = rot * PI / 180.0f;
    const float ca = cosf(rad);
    const float sa = sinf(rad);
    constexpr float center = 239.5f;
    for (int y = 0; y < 480; y++) {
      const float dy = y - center;
      for (int x = 0; x < 480; x++) {
        const float dx = x - center;
        const int sx = static_cast<int>(ca * dx + sa * dy + center + 0.5f);
        const int sy = static_cast<int>(-sa * dx + ca * dy + center + 0.5f);
        presentFrameBuf[y * 480 + x] =
            ((unsigned)sx < 480 && (unsigned)sy < 480) ? nativeFrame[sy * 480 + sx] : RGB565_BLACK;
      }
    }
    esp_lcd_panel_draw_bitmap(nativePanel, 0, 0, 480, 480, presentFrameBuf);
  }
}

// Zifferblatt in den Frame kopieren, skaliert auf g_dialScalePct % und
// zentriert auf schwarzem Grund (Nearest-Neighbor). Bei 100% = 1:1.
static void copyVdoDialToFrame() {
  if (!ensureFrame()) {
    return;
  }
  int pct = g_dialScalePct;
  if (pct < 30) pct = 30;
  if (pct > 100) pct = 100;

  if (pct == 100) {
    for (int i = 0; i < 480 * 480; i++) {
      nativeFrame[i] = pgm_read_word(&VDO_DIAL_480_RGB565[i]);
    }
    return;
  }

  // Hintergrund schwarz, dann skaliertes Zifferblatt zentriert einsetzen.
  for (int i = 0; i < 480 * 480; i++) nativeFrame[i] = RGB565_BLACK;
  int outSize = (480 * pct) / 100;
  int offset = (480 - outSize) / 2;
  for (int oy = 0; oy < outSize; oy++) {
    int sy = (oy * 480) / outSize;
    int dstRow = (offset + oy) * 480 + offset;
    int srcRow = sy * 480;
    for (int ox = 0; ox < outSize; ox++) {
      int sx = (ox * 480) / outSize;
      nativeFrame[dstRow + ox] = pgm_read_word(&VDO_DIAL_480_RGB565[srcRow + sx]);
    }
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
    case 'C': { static const uint8_t v[5] = {0x3E, 0x41, 0x41, 0x41, 0x22}; g = v; break; }
    case 'D': { static const uint8_t v[5] = {0x7F, 0x41, 0x41, 0x22, 0x1C}; g = v; break; }
    case 'E': { static const uint8_t v[5] = {0x7F, 0x49, 0x49, 0x49, 0x41}; g = v; break; }
    case 'F': { static const uint8_t v[5] = {0x7F, 0x09, 0x09, 0x09, 0x01}; g = v; break; }
    case 'G': { static const uint8_t v[5] = {0x3E, 0x41, 0x49, 0x49, 0x7A}; g = v; break; }
    case 'H': { static const uint8_t v[5] = {0x7F, 0x08, 0x08, 0x08, 0x7F}; g = v; break; }
    case 'I': { static const uint8_t v[5] = {0x00, 0x41, 0x7F, 0x41, 0x00}; g = v; break; }
    case 'K': { static const uint8_t v[5] = {0x7F, 0x08, 0x14, 0x22, 0x41}; g = v; break; }
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
    case 'W': { static const uint8_t v[5] = {0x3F, 0x40, 0x38, 0x40, 0x3F}; g = v; break; }
    case 'X': { static const uint8_t v[5] = {0x63, 0x14, 0x08, 0x14, 0x63}; g = v; break; }
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
  drawCircleLine(240, 240, 216, 3, RGB565(80, 80, 75));
  drawTextCentered(240, 54, "MENU", RGB565(235, 235, 225), 7);

  drawMenuTile(88, 116, 304, 46, "UHR", RGB565(200, 40, 35));
  drawMenuTile(88, 172, 304, 46, "MOTOR", RGB565(40, 150, 210));
  drawMenuTile(88, 228, 304, 46, "LAMBDA", RGB565(60, 185, 90));
  drawMenuTile(88, 284, 304, 46, "HUB", RGB565(190, 90, 210));
  drawMenuTile(88, 340, 304, 46, "SETUP", RGB565(210, 170, 45));

  // IP-Adresse im Netz unten anzeigen
  char ipLine[32];
  snprintf(ipLine, sizeof(ipLine), "IP %s", g_ipStr);
  drawTextCentered(240, 404, ipLine, RGB565(150, 200, 150), 2);

  presentFrame();
}

// Daten gelten als frisch, wenn verbunden und Notify < 3s her.
static bool bleFresh() {
  return g_bleConn && (millis() - g_bleLastRx < 3000);
}

// Eine Datenzeile "LABEL  WERT" zeichnen.
static void drawDataRow(int y, const char* label, const char* value, uint16_t col) {
  drawTextSmall(92, y, label, RGB565(160, 160, 160), 2);
  drawTextSmall(244, y, value, col, 2);
}

static void drawMotorPage() {
  if (!ensureFrame()) return;
  fillFrame(RGB565_BLACK);
  drawCircleLine(240, 240, 216, 3, RGB565(40, 110, 160));
  drawTextCentered(240, 52, "MOTOR", RGB565(60, 170, 230), 5);

  bool fresh = bleFresh();
  char buf[16];
  uint16_t cv = fresh ? RGB565(235, 235, 225) : RGB565(110, 60, 60);
  const char* na = "---";

  if (fresh) { snprintf(buf, sizeof(buf), "%d", (int)g_rpm); drawDataRow(112, "RPM", buf, cv); }
  else drawDataRow(112, "RPM", na, cv);
  if (fresh) { snprintf(buf, sizeof(buf), "%d", (int)g_adv); drawDataRow(152, "ADV", buf, cv); }
  else drawDataRow(152, "ADV", na, cv);
  if (fresh) { snprintf(buf, sizeof(buf), "%d", (int)g_map); drawDataRow(192, "MAP", buf, cv); }
  else drawDataRow(192, "MAP", na, cv);

  if (fresh && g_g123Valid) { snprintf(buf, sizeof(buf), "%.1fV", g_g123Volt); drawDataRow(238, "123V", buf, cv); }
  else drawDataRow(238, "123V", na, cv);
  if (fresh && g_g123Valid) { snprintf(buf, sizeof(buf), "%dC", (int)g_g123Temp); drawDataRow(278, "123T", buf, cv); }
  else drawDataRow(278, "123T", na, cv);
  if (fresh && g_battValid) { snprintf(buf, sizeof(buf), "%.1fV", g_battVolt); drawDataRow(318, "BATT", buf, cv); }
  else drawDataRow(318, "BATT", na, cv);

  const char* st = g_bleConn ? (fresh ? "LIVE" : "WARTE") : "KEIN HUB";
  drawTextCentered(240, 370, st, g_bleConn && fresh ? RGB565(60, 200, 90) : RGB565(200, 120, 50), 2);
  presentFrame();
}

static void drawLambdaPage() {
  if (!ensureFrame()) return;
  fillFrame(RGB565_BLACK);
  drawCircleLine(240, 240, 216, 3, RGB565(45, 150, 70));
  drawTextCentered(240, 58, "LAMBDA", RGB565(70, 200, 100), 5);

  bool fresh = bleFresh() && g_lambdaValid;
  char buf[16];
  if (fresh) snprintf(buf, sizeof(buf), "%.2f", g_lambda);
  else strcpy(buf, "----");
  // grosse Lambda-Zahl mittig
  uint16_t col = RGB565(240, 240, 230);
  if (fresh) {
    if (g_lambda < 0.97f) col = RGB565(235, 120, 40);       // fett
    else if (g_lambda > 1.03f) col = RGB565(80, 160, 240);  // mager
    else col = RGB565(70, 210, 100);                        // ok
  } else col = RGB565(110, 60, 60);
  drawTextCentered(240, 198, buf, col, 8);

  if (fresh && g_speedValid) {
    char sp[16]; snprintf(sp, sizeof(sp), "%d km/h", (int)g_speedKmh);
    drawTextCentered(240, 318, sp, RGB565(180, 180, 180), 3);
  }

  const char* st = g_bleConn ? (bleFresh() ? "LIVE" : "WARTE") : "KEIN HUB";
  drawTextCentered(240, 370, st, g_bleConn && bleFresh() ? RGB565(60, 200, 90) : RGB565(200, 120, 50), 2);
  presentFrame();
}

static void drawHubPage() {
  if (!ensureFrame()) return;
  fillFrame(RGB565_BLACK);
  drawCircleLine(240, 240, 216, 3, RGB565(150, 70, 180));
  drawTextCentered(240, 54, "HUB", RGB565(205, 120, 230), 5);

  char buf[24];
  drawDataRow(112, "BLE", g_bleConn ? "OK" : "SCAN", g_bleConn ? RGB565(60, 210, 100) : RGB565(220, 130, 50));
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)g_bleRxCnt);
  drawDataRow(152, "RX", buf, RGB565(235, 235, 225));
  snprintf(buf, sizeof(buf), "%lu MS", g_bleLastRx ? (unsigned long)(millis() - g_bleLastRx) : 0UL);
  drawDataRow(192, "AGE", buf, RGB565(235, 235, 225));
  snprintf(buf, sizeof(buf), "%.1f V", g_battVolt);
  drawDataRow(232, "BATT", g_battValid ? buf : "---", RGB565(235, 235, 225));
  snprintf(buf, sizeof(buf), "%.0f KMH", g_speedKmh);
  drawDataRow(272, "SPEED", g_speedValid ? buf : "---", RGB565(235, 235, 225));
  drawDataRow(312, "IP", g_ipStr, RGB565(150, 200, 150));

  drawTextCentered(240, 370, "TIP MENU", RGB565(180, 180, 170), 2);
  presentFrame();
}

static void drawSetupPage() {
  if (!ensureFrame()) return;
  fillFrame(RGB565_BLACK);
  drawCircleLine(240, 240, 216, 3, RGB565(185, 150, 45));
  drawTextCentered(240, 54, "SETUP", RGB565(230, 190, 70), 5);

  char buf[28];
  snprintf(buf, sizeof(buf), "%d %%", g_dialScalePct);
  drawDataRow(112, "UHR", buf, RGB565(235, 235, 225));
  snprintf(buf, sizeof(buf), "%d %%", g_brightnessPct);
  drawDataRow(152, "HELL", buf, RGB565(235, 235, 225));
  snprintf(buf, sizeof(buf), "%d DEG", g_rotationDeg);
  drawDataRow(192, "ROT", buf, RGB565(235, 235, 225));
  drawDataRow(232, "TOUCH", touchSeen ? "AKTIV" : "WARTET", touchSeen ? RGB565(60, 210, 100) : RGB565(220, 130, 50));
  drawDataRow(272, "WIFI", WiFi.status() == WL_CONNECTED ? "OK" : "---", WiFi.status() == WL_CONNECTED ? RGB565(60, 210, 100) : RGB565(220, 130, 50));
  drawDataRow(312, "SD", g_sdReady ? g_sdType.c_str() : "---", g_sdReady ? RGB565(60, 210, 100) : RGB565(220, 130, 50));

  drawTextCentered(240, 370, "TIP MENU", RGB565(180, 180, 170), 2);
  presentFrame();
}

static void drawCurrentPage() {
  if (currentPage == 0) drawVdoClock();
  else if (currentPage == 1) drawMenuOverview();
  else if (currentPage == 2) drawMotorPage();
  else if (currentPage == 3) drawLambdaPage();
  else if (currentPage == 4) drawHubPage();
  else if (currentPage == 5) drawSetupPage();
}

// -------- Einstellungen (Preferences) --------
static void loadSettings() {
  Preferences p;
  p.begin("clock", true);
  g_dialScalePct = p.getInt("scale", 100);
  g_brightnessPct = p.getInt("bright", 100);
  g_rotationDeg = p.getInt("rotdeg", p.getUChar("rot", 0) * 90);
  p.end();
  if (g_dialScalePct < 30) g_dialScalePct = 30;
  if (g_dialScalePct > 100) g_dialScalePct = 100;
  if (g_brightnessPct < 5) g_brightnessPct = 5;
  if (g_brightnessPct > 100) g_brightnessPct = 100;
  g_rotationDeg %= 360;
  if (g_rotationDeg < 0) g_rotationDeg += 360;

  Preferences wifi;
  wifi.begin("wifi", true);
  g_wifiSsid = wifi.getString("ssid", WIFI_SSID);
  g_wifiPassword = wifi.getString("pass", WIFI_PASSWORD);
  g_wifiSaved = wifi.isKey("ssid");
  wifi.end();
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

static void applyBrightness()
{
  int pct = g_brightnessPct;
  if (pct < 5) pct = 5;
  if (pct > 100) pct = 100;
  const int duty = map(pct, 0, 100, 0, 255);
  analogWrite(PIN_LCD_BL, duty);
}

static void saveBrightness(int pct) {
  if (pct < 5) pct = 5;
  if (pct > 100) pct = 100;
  g_brightnessPct = pct;
  Preferences p;
  p.begin("clock", false);
  p.putInt("bright", pct);
  p.end();
  applyBrightness();
}

static void saveRotation(int deg) {
  deg %= 360;
  if (deg < 0) deg += 360;
  g_rotationDeg = deg;
  Preferences p;
  p.begin("clock", false);
  p.putInt("rotdeg", g_rotationDeg);
  p.end();
}

static void saveWifi(const String &ssid, const String &password) {
  g_wifiSsid = ssid;
  g_wifiPassword = password;
  g_wifiSaved = ssid.length() > 0;
  Preferences p;
  p.begin("wifi", false);
  if (g_wifiSaved) {
    p.putString("ssid", g_wifiSsid);
    p.putString("pass", g_wifiPassword);
  } else {
    p.clear();
  }
  p.end();
  WiFi.disconnect();
  if (g_wifiSsid.length() > 0) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(g_wifiSsid.c_str(), g_wifiPassword.c_str());
    snprintf(g_ipStr, sizeof(g_ipStr), "...");
  } else {
    snprintf(g_ipStr, sizeof(g_ipStr), "---");
  }
}

static String formatStorageMb(uint64_t bytes) {
  return String((unsigned long)(bytes / (1024ULL * 1024ULL))) + " MB";
}

static void initSdCard() {
  g_sdReady = false;
  g_sdType = "none";
  g_sdTotalBytes = 0;
  g_sdUsedBytes = 0;

  Serial.println("SD: init 1-bit SD_MMC...");
  pcaSetOutputBits(EXIO_SD_D3, true);
  delay(10);

  if (!SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0, -1, -1, -1)) {
    Serial.println("SD: setPins fehlgeschlagen");
    return;
  }

  bool mounted = SD_MMC.begin("/sdcard", true, false, 40000);
  if (!mounted) {
    Serial.println("SD: 40 MHz fehlgeschlagen, retry 20 MHz");
    SD_MMC.end();
    delay(50);
    mounted = SD_MMC.begin("/sdcard", true, false, 20000);
  }
  if (!mounted) {
    Serial.println("SD: mount fehlgeschlagen");
    return;
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("SD: keine Karte erkannt");
    SD_MMC.end();
    return;
  }

  if (cardType == CARD_MMC) g_sdType = "MMC";
  else if (cardType == CARD_SD) g_sdType = "SDSC";
  else if (cardType == CARD_SDHC) g_sdType = "SDHC";
  else g_sdType = "unknown";

  g_sdTotalBytes = SD_MMC.totalBytes();
  g_sdUsedBytes = SD_MMC.usedBytes();
  g_sdReady = true;
  SD_MMC.mkdir("/logs");

  File f = SD_MMC.open("/logs/boot.txt", FILE_APPEND);
  if (f) {
    f.printf("boot millis=%lu sd=%s total=%luMB used=%luMB\n",
             (unsigned long)millis(),
             g_sdType.c_str(),
             (unsigned long)(g_sdTotalBytes / (1024ULL * 1024ULL)),
             (unsigned long)(g_sdUsedBytes / (1024ULL * 1024ULL)));
    f.close();
  }

  Serial.printf("SD: OK type=%s total=%luMB used=%luMB\n",
                g_sdType.c_str(),
                (unsigned long)(g_sdTotalBytes / (1024ULL * 1024ULL)),
                (unsigned long)(g_sdUsedBytes / (1024ULL * 1024ULL)));
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
  html += F("<div class='card'><h3>Display-Seite</h3>"
    "<a href='/page?p=0'><button>Uhr</button></a>"
    "<a href='/page?p=1'><button>Menu</button></a>"
    "<a href='/page?p=2'><button>Motor</button></a>"
    "<a href='/page?p=3'><button>Lambda</button></a>"
    "<a href='/page?p=4'><button>Hub</button></a>"
    "<a href='/page?p=5'><button>Setup</button></a></div>");
  html += F("<div class='card'><h3>Hub Live</h3>");
  html += "<div>BLE: " + String(g_bleConn ? "verbunden" : "scan") + "</div>";
  html += "<div>RX: " + String((unsigned long)g_bleRxCnt) + "</div>";
  html += "<div>Lambda: " + String(g_lambdaValid ? String(g_lambda, 2) : String("---")) + "</div>";
  html += "<div>RPM: " + String((int)g_rpm) + " &nbsp; ADV: " + String(g_adv, 1) + "</div>";
  html += "<div>Batt: " + String(g_battValid ? String(g_battVolt, 1) + " V" : String("---")) + "</div></div>";
  html += F("<div class='card'><h3>Touch</h3>");
  html += "<div>Chip: " + String(gt911Found ? "GT911 OK" : "nicht gefunden") + " @0x" + String(gt911Addr, HEX) + "</div>";
  html += "<div>Status: 0x" + String(g_lastTouchStatus, HEX) + "</div>";
  html += "<div>Letzter Punkt: X " + String(g_lastTouchX) + " / Y " + String(g_lastTouchY) + "</div>";
  html += "<div>Alter: " + String(g_lastTouchMs ? (millis() - g_lastTouchMs) : 0) + " ms</div></div>";
  html += F("<div class='card'><h3>microSD</h3>");
  html += "<div>Status: " + String(g_sdReady ? "bereit" : "nicht bereit") + "</div>";
  html += "<div>Typ: " + g_sdType + "</div>";
  html += "<div>Groesse: " + formatStorageMb(g_sdTotalBytes) + "</div>";
  html += "<div>Benutzt: " + formatStorageMb(g_sdUsedBytes) + "</div>";
  html += "<div>Log-Test: /logs/boot.txt</div></div>";
  html += F("<div class='card'><h3>WLAN</h3>");
  html += "<div>STA: " + String(WiFi.status() == WL_CONNECTED ? WiFi.SSID() + " / " + WiFi.localIP().toString() : String("nicht verbunden")) + "</div>";
  html += "<div>Gespeichert: " + String(g_wifiSaved ? g_wifiSsid : String("-")) + "</div>";
  html += "<div>Setup-AP: VDO-Clock-Setup / " + WiFi.softAPIP().toString() + "</div>";
  html += F("<form action='/wifi' method='get'>"
    "<p><select id='wifiPreset'>"
    "<option value='' data-pass=''>Manuell</option>"
    "<option value='Android-AP1' data-pass='Frankfurt1'>S24 Hotspot Android-AP1</option>"
    "<option value='Z00-Station' data-pass=''>Z00-Station</option>"
    "</select></p>"
    "<p><input id='ssid' name='ssid' placeholder='SSID' style='width:88%;padding:10px' value='");
  html += g_wifiSsid;
  html += F("'></p><p><input id='pass' name='pass' placeholder='Passwort' type='password' style='width:88%;padding:10px'></p>"
    "<button type='submit'>WLAN speichern</button></form>"
    "<form action='/wifi_clear' method='get'><button type='submit'>WLAN loeschen</button></form>"
    "<script>document.getElementById('wifiPreset').addEventListener('change',e=>{let o=e.target.selectedOptions[0];document.getElementById('ssid').value=o.value||'';document.getElementById('pass').value=o.dataset.pass||'';});</script>"
    "</div>");
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
  html += F("<div class='card'><h3>Helligkeit</h3>"
    "<form action='/set' method='get'>"
    "<div class='val'><span id='b'>");
  html += String(g_brightnessPct);
  html += F("</span>%</div>"
    "<input type='range' name='bright' min='5' max='100' step='1' value='");
  html += String(g_brightnessPct);
  html += F("' oninput=\"document.getElementById('b').innerText=this.value\">"
    "<br><button type='submit'>&Uuml;bernehmen</button></form>"
    "<div><a href='/set?bright=100'>100%</a> &middot; <a href='/set?bright=70'>70%</a> &middot; "
    "<a href='/set?bright=40'>40%</a> &middot; <a href='/set?bright=15'>15%</a></div></div>");
  html += F("<div class='card'><h3>Rotation</h3>");
  html += F("<form action='/set' method='get'><div class='val'><span id='r'>");
  html += String(g_rotationDeg);
  html += F("</span>&deg;</div><input type='range' name='rot' min='0' max='359' step='1' value='");
  html += String(g_rotationDeg);
  html += F("' oninput=\"document.getElementById('r').innerText=this.value\">"
    "<br><button type='submit'>&Uuml;bernehmen</button></form>"
    "<div><a href='/set?rot=0'>0&deg;</a> &middot; <a href='/set?rot=90'>90&deg;</a> &middot; "
    "<a href='/set?rot=180'>180&deg;</a> &middot; <a href='/set?rot=270'>270&deg;</a></div></div>");
  html += F("<p style='color:#666'>VW T2b Cockpit &middot; ESP32-S3 2.8\"</p></body></html>");
  webServer.send(200, "text/html", html);
}

static void handleWebSet() {
  if (webServer.hasArg("scale")) {
    saveDialScale(webServer.arg("scale").toInt());
    g_redrawPage = true;
    Serial.printf("Web: Zifferblatt-Groesse = %d%%\n", g_dialScalePct);
  }
  if (webServer.hasArg("bright")) {
    saveBrightness(webServer.arg("bright").toInt());
    g_redrawPage = true;
    Serial.printf("Web: Helligkeit = %d%%\n", g_brightnessPct);
  }
  if (webServer.hasArg("rot")) {
    saveRotation(webServer.arg("rot").toInt());
    g_redrawPage = true;
    Serial.printf("Web: Rotation = %d deg\n", g_rotationDeg);
  }
  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

static void handleWebWifi() {
  String ssid = webServer.arg("ssid");
  String pass = webServer.arg("pass");
  ssid.trim();
  saveWifi(ssid, pass);
  Serial.printf("Web: WLAN gespeichert '%s'\n", g_wifiSsid.c_str());
  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

static void handleWebWifiClear() {
  saveWifi("", "");
  Serial.println("Web: WLAN geloescht");
  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

static void handleWebPage() {
  if (webServer.hasArg("p")) {
    int page = webServer.arg("p").toInt();
    if (page < 0) page = 0;
    if (page > 5) page = 5;
    currentPage = static_cast<uint8_t>(page);
    g_redrawPage = true;
    Serial.printf("Web: page=%u\n", currentPage);
  }
  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

static void startWebServer() {
  webServer.on("/", handleWebRoot);
  webServer.on("/set", handleWebSet);
  webServer.on("/page", handleWebPage);
  webServer.on("/wifi", handleWebWifi);
  webServer.on("/wifi_clear", handleWebWifiClear);
  webServer.begin();
  g_webStarted = true;
  Serial.println("WebGUI: gestartet auf Port 80");
}

static void coldBootDisplayRetry() {
  Serial.println("Display: cold-boot retry init...");
  digitalWrite(PIN_LCD_BL, LOW);
  delay(120);
  expanderInit();
  nativeSt7701Init();
  nativePanelInit();
  applyBrightness();
  drawCurrentPage();
  Serial.println("Display: cold-boot retry drawn.");
}

void setup() {
  // Cold-Boot Robustness: erst 250ms warten damit die Versorgung sauber
  // hochlaeuft, bevor wir I2C/Display anpacken. Bei direktem USB-Plug-in
  // war das Display sonst manchmal schwarz weil der PCA9554 noch nicht
  // sicher antwortet und der erste expander-Write ins Leere ging.
  delay(1000);

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
  loadSettings();
  initTimeSource();

  // Backlight mit gespeicherter Helligkeit einschalten.
  pinMode(PIN_LCD_BL, OUTPUT);
  applyBrightness();

  drawVdoClock();
  Serial.println("VDO clock drawn.");
  delay(700);
  coldBootDisplayRetry();
  initSdCard();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("VDO-Clock-Setup", "vdoclock");
  Serial.printf("Setup-AP: VDO-Clock-Setup / %s\n", WiFi.softAPIP().toString().c_str());
  startWebServer();

  // WiFi-Verbindung im Hintergrund starten. NTP/IP laufen im loop() weiter.
  if (g_wifiSsid.length() > 0) {
    WiFi.begin(g_wifiSsid.c_str(), g_wifiPassword.c_str());
    Serial.printf("WiFi: Verbindung zu '%s' im Hintergrund gestartet\n", g_wifiSsid.c_str());
  }

  // BLE-Client fuer Spartan3-Hub (Motor-/Lambda-Daten). WiFi-Modem-Sleep
  // bleibt aktiv (Default) fuer WiFi+BLE-Koexistenz auf dem ESP32-S3.
  NimBLEDevice::init("VDO-Clock");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  bleNextScanAt = millis() + 2000;  // kurz nach Boot ersten Scan starten
  Serial.println("BLE: Client initialisiert");
}

void loop() {
  static uint32_t lastTouch = 0;
  static uint32_t lastClockDraw = 0;
  uint16_t x = 0;
  uint16_t y = 0;

  if (readTouch(&x, &y) && millis() - lastTouch > 350) {
    lastTouch = millis();
    touchSeen = true;
    const uint16_t rawX = x;
    const uint16_t rawY = y;
    int rot = g_rotationDeg % 360;
    if (rot < 0) rot += 360;
    if (rot != 0) {
      const float rad = rot * PI / 180.0f;
      const float ca = cosf(rad);
      const float sa = sinf(rad);
      constexpr float center = 239.5f;
      const float dx = rawX - center;
      const float dy = rawY - center;
      int lx = static_cast<int>(ca * dx + sa * dy + center + 0.5f);
      int ly = static_cast<int>(-sa * dx + ca * dy + center + 0.5f);
      if (lx < 0) lx = 0;
      if (lx > 479) lx = 479;
      if (ly < 0) ly = 0;
      if (ly > 479) ly = 479;
      x = static_cast<uint16_t>(lx);
      y = static_cast<uint16_t>(ly);
    }
    Serial.printf("touch raw=%u/%u logical=%u/%u rot=%d page=%u\n", rawX, rawY, x, y, g_rotationDeg, currentPage);

    if (currentPage == 0) {
      // Uhr -> Menue
      currentPage = 1;
      drawMenuOverview();
      Serial.println("page: menu");
    } else if (currentPage == 1) {
      // Menue: Kachel nach y-Position waehlen
      if (y >= 100 && y < 166) { currentPage = 0; drawVdoClock(); Serial.println("page: clock"); }
      else if (y >= 166 && y < 222) { currentPage = 2; drawMotorPage(); Serial.println("page: motor"); }
      else if (y >= 222 && y < 278) { currentPage = 3; drawLambdaPage(); Serial.println("page: lambda"); }
      else if (y >= 278 && y < 334) { currentPage = 4; drawHubPage(); Serial.println("page: hub"); }
      else if (y >= 334 && y < 405) { currentPage = 5; drawSetupPage(); Serial.println("page: setup"); }
      else { currentPage = 2; drawMotorPage(); Serial.println("page: motor fallback"); }
    } else {
      // Daten-Seiten: einfacher Blaettermodus, unabhaengig von Touch-Koordinaten.
      currentPage++;
      if (currentPage > 5) currentPage = 0;
      drawCurrentPage();
      Serial.printf("page: next %u\n", currentPage);
    }
  }

  // WiFi/NTP im Hintergrund (nicht-blockierend). Bei frischem Sync Uhr neu.
  if (wifiNtpTick() && currentPage == 0) {
    drawVdoClock();
  }

  // BLE-Client (Spartan-Hub) nicht-blockierend bedienen
  bleTick();

  // Web-GUI bedienen
  if (g_webStarted) {
    webServer.handleClient();
  }

  // Neuzeichnen nach Web-Aenderung (z.B. Zifferblatt-Groesse)
  if (g_redrawClock || g_redrawPage) {
    g_redrawClock = false;
    g_redrawPage = false;
    drawCurrentPage();
  }

  if (currentPage == 0 && millis() - lastClockDraw >= 1000) {
    lastClockDraw = millis();
    drawVdoClock();
  }
  // Daten-Seiten live aktualisieren (2x/s)
  if ((currentPage == 2 || currentPage == 3 || currentPage == 4 || currentPage == 5) && millis() - lastClockDraw >= 500) {
    lastClockDraw = millis();
    drawCurrentPage();
  }

  delay(10);
}
