/*
 * ESP32-S3 BLE HID Central -> Wired USB Composite HID Bridge (v3.0 FIXED)
 * - Умная индикация LED / WS2812 RGB: Поиск (4 Гц), Нет девайсов (маячок 1.6 с), Подключение (3 вспышки + горит), Отключение (5 вспышек)
 * - Индивидуальный профиль раскодировки (fmt) и чувствительности (scale) для КАЖДОГО MAC-адреса в NVS
 * - Полная поддержка джойстика Google Stadia Controller (BLE режим: 10 байт, стики, триггеры L2/R2, 15 кнопок, D-Pad)
 * - Два режима джойстика: Нативный USBHIDGamepad ИЛИ аппаратная эмуляция Клавиатура + Мышь (WASD + Курсор)
 * - Автоматический парсер HID Report Map (0x2A4B) для 12/16-битных мышей и клавиатуры Logitech MX Keys Mini
 * - Обновление прошивки прямо из браузера по USB WebSerial OTA (Update.h)
 *
 * Настройки Arduino IDE -> Tools:
 * 1. Board: "ESP32S3 Dev Module"
 * 2. USB Mode: "USB-OTG (TinyUSB)"
 * 3. USB CDC On Boot: "Enabled"
 */

#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDMouse.h"
#include "USBHIDConsumerControl.h"
#if __has_include("USBHIDGamepad.h")
  #include "USBHIDGamepad.h"
  #define BRIDGE_HAS_USB_GAMEPAD 1
#else
  #define BRIDGE_HAS_USB_GAMEPAD 0
#endif
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "soc/rtc_cntl_reg.h"

#if !ARDUINO_USB_MODE
  #if ARDUINO_USB_CDC_ON_BOOT
    #define CONSOLE Serial
  #else
    USBCDC USBSerial;
    #define CONSOLE USBSerial
  #endif
#else
  #define CONSOLE Serial
#endif

USBHIDKeyboard Keyboard;
USBHIDMouse Mouse;
USBHIDConsumerControl ConsumerControl;
#if BRIDGE_HAS_USB_GAMEPAD
USBHIDGamepad Gamepad;
#endif
Preferences prefs;

static constexpr uint8_t WS2812_PIN = 21;
#define HID_PACKET_DEBUG 0

static NimBLEUUID hidServiceUUID((uint16_t)0x1812);
static NimBLEUUID reportMapUUID((uint16_t)0x2A4B);
static NimBLEUUID protoModeUUID((uint16_t)0x2A4E);
static NimBLEUUID reportUUID((uint16_t)0x2A4D);
static NimBLEUUID bootKbUUID((uint16_t)0x2A22);
static NimBLEUUID bootMouseUUID((uint16_t)0x2A33);
static NimBLEUUID reportRefUUID((uint16_t)0x2908);

// Один атомарный USB HID report вместо отдельных press/release/move.
void sendUsbMouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel, int8_t pan) {
  hid_mouse_report_t report;
  report.buttons = buttons;
  report.x = x;
  report.y = y;
  report.wheel = wheel;
  report.pan = pan;
  Mouse.sendReport(report);
}

struct BridgeConfig {
  uint8_t mouseScale10 = 10; // 10 = 1.0x
  uint8_t scrollScale = 1;
  bool swapMouseBtns = false;
  bool invertWheel = false;
  bool invertXY = false;
  bool swapWinAlt = false;
  bool bootProtocol = false;
  bool autoReconnect = true;
  uint8_t ledMode = 2;
  uint8_t ledPin = 48;
  uint8_t ledType = 2;    // 0=Digital LED, 1=WS2812 RGB на ledPin, 2=Авто-Комбо (Digital ledPin + WS2812 на GPIO 21)
  uint8_t mouseFmt = 0;   // Глобальный по умолчанию: 0=Auto(0x2A4B), 1=16b_1B, 2=16b_2B, 3=12b_1B, 4=12b_2B, 5=8b
  uint8_t kbFmt = 0;      // Глобальный по умолчанию: 0=Auto(0x2A4B), 1=Boot(0x2A22), 2=Std8B, 3=NoRes, 4=Bitmap
  uint8_t gpMode = 0;     // 0=Нативный USB Gamepad, 1=Эмуляция Клавиатура(WASD) + Мышь
  uint8_t gpFmt = 0;      // 0=Google Stadia BLE (10 байт), 1=Generic BLE Gamepad
  uint8_t gpDeadzone = 10;
} cfg;

enum ReportKind : uint8_t {
  KIND_UNKNOWN = 0,
  KIND_KEYBOARD = 1,
  KIND_MOUSE = 2,
  KIND_CONSUMER = 3,
  KIND_VENDOR = 4,
  KIND_GAMEPAD = 5
};

/*
 * Коды индивидуального профиля раскодировки (SavedBond.fmt) для каждого устройства:
 * 0  = Авто (по дескриптору 0x2A4B + глобальные настройки)
 * 1  = Мышь: 16-бит X/Y + 1 байт кнопок ([Btn, X_lo, X_hi, Y_lo, Y_hi, Wheel])
 * 2  = Мышь: 16-бит X/Y + 2 байта кнопок Logitech ([BtnL, BtnH, X_lo, X_hi, Y_lo, Y_hi, Wheel])
 * 3  = Мышь: 12-бит упакованные X/Y + 1 байт кнопок
 * 4  = Мышь: 12-бит упакованные X/Y + 2 байта кнопок Logitech
 * 5  = Мышь: 8-бит классический Boot (3-4 байта)
 * 6  = Клавиатура: Принудительный Boot Protocol (0x2A22)
 * 7  = Клавиатура: Стандартный 8-байтный ([Mod, Reserved, K0..K5])
 * 8  = Клавиатура: Без пустого байта ([Mod, K0..K6] — Logitech MX Keys Mini)
 * 9  = Клавиатура: Битовая маска NKRO (Logitech BLE Bitmap)
 * 10 = Джойстик: Google Stadia Controller BLE (10 байт: Hat, 15 кнопок, стики, курки L2/R2)
 * 11 = Джойстик: Универсальный BLE Gamepad / Xbox
 */

struct ReportLayout {
  bool valid = false;
  uint8_t reportId = 0;
  uint8_t kind = KIND_UNKNOWN;
  uint16_t bitOffset = 0;
  // Мышь:
  uint16_t btnOff = 0;
  uint8_t  btnSz = 0;
  uint16_t xOff = 0;
  uint8_t  xSz = 0;
  uint16_t yOff = 0;
  uint8_t  ySz = 0;
  uint16_t wOff = 0;
  uint8_t  wSz = 0;
  uint16_t panOff = 0;
  uint8_t  panSz = 0;
  // Клавиатура:
  uint16_t modOff = 0;
  bool     hasMod = false;
  uint8_t  keyByteOff = 2;
  uint8_t  keyCount = 0;
  bool     hasBitmap = false;
  uint16_t bmpBitOff = 0;
  uint16_t bmpBitCnt = 0;
  uint8_t  bmpUsageMin = 0;
};

// Явный прототип нужен Arduino-препроцессору: без него автоматически
// созданное объявление функции оказывалось выше определения ReportLayout.
int parseHidReportMap(const uint8_t* desc, size_t len, ReportLayout* outLayouts, int maxLayouts);

struct SubscribedCharInfo {
  uint16_t connHandle = 0xFFFF;
  uint16_t charHandle = 0;
  uint8_t  devRole = 0;
  int8_t   bondIdx = -1; // Индекс устройства в savedBonds[] для индивидуальной раскодировки
  ReportLayout layout;
};

#define MAX_SUB_CHARS 20
SubscribedCharInfo subChars[MAX_SUB_CHARS];
int subCharCount = 0;

#define MAX_BONDS 5
struct SavedBond {
  String mac;
  uint8_t addrType;
  String name;
  bool connected;
  uint8_t fmt;   // Индивидуальная раскодировка (0..11)
  uint8_t scale; // Индивидуальный множитель скорости для этой мыши/стика (10 = 1.0x)
};

SavedBond savedBonds[MAX_BONDS];
int savedCount = 0;

bool isScanning = false;
bool pendingConnect = false;
String targetConnectMac = "";
uint8_t targetConnectType = 1;
String targetConnectName = "BLE HID";

uint32_t packetCounter = 0;
uint32_t lastHidInputMs = 0;
uint32_t lastReconnectTry = 0;
uint32_t lastStatusPush = 0;
String serialBuffer = "";

// Переменные состояния USB WebSerial OTA
bool otaInProgress = false;
size_t otaTotalSize = 0;
size_t otaWritten = 0;

size_t decodeBase64(const char* in, size_t inLen, uint8_t* out) {
  static const int8_t T[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
  };
  size_t outLen = 0;
  uint32_t val = 0;
  int valb = -8;
  for (size_t i = 0; i < inLen; i++) {
    uint8_t c = (uint8_t)in[i];
    if (c == '=') break;
    int8_t d = T[c];
    if (d == -1) continue;
    val = (val << 6) + d;
    valb += 6;
    if (valb >= 0) {
      out[outLen++] = (uint8_t)((val >> valb) & 0xFF);
      valb -= 8;
    }
  }
  return outLen;
}

// Неблокирующий движок светодиодной индикации (Digital LED + WS2812 RGB NeoPixel)
enum LedEvent : uint8_t {
  LED_EV_NONE = 0,
  LED_EV_CONNECTED = 1,    // 3 приветственные вспышки при подключении (Зелёный)
  LED_EV_DISCONNECTED = 2, // 5 быстрых тревожных вспышек при потери связи (Красный)
  LED_EV_ACTIVITY = 3      // Короткое подмигивание при передаче пакета HID
};

uint8_t  currentLedEvent = LED_EV_NONE;
uint32_t ledEventStartMs = 0;
uint32_t lastConnCheckMs = 0;
bool     lastLedPhysOn   = false;
uint8_t  lastRgbR = 255, lastRgbG = 255, lastRgbB = 255;

void writeRgbSafe(uint8_t pin, uint8_t r, uint8_t g, uint8_t b) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  rgbLedWrite(pin, r, g, b);
#else
  neopixelWrite(pin, r, g, b);
#endif
}

void setLedHardware(bool on, uint8_t r, uint8_t g, uint8_t b) {
  if (cfg.ledMode == 0) {
    on = false; r = 0; g = 0; b = 0;
  }
  uint8_t rr = on ? r : 0;
  uint8_t gg = on ? g : 0;
  uint8_t bb = on ? b : 0;

  if (on != lastLedPhysOn || rr != lastRgbR || gg != lastRgbG || bb != lastRgbB) {
    lastLedPhysOn = on;
    lastRgbR = rr; lastRgbG = gg; lastRgbB = bb;

    if ((cfg.ledType == 0 || cfg.ledType == 2) && cfg.ledPin != WS2812_PIN) {
      digitalWrite(cfg.ledPin, on ? HIGH : LOW);
    }
    if (cfg.ledType == 1) {
      writeRgbSafe(WS2812_PIN, rr, gg, bb);
    } else if (cfg.ledType == 2) {
      writeRgbSafe(WS2812_PIN, rr, gg, bb);
    }
  }
}

void triggerLedEvent(uint8_t ev) {
  if (ev == LED_EV_ACTIVITY && (currentLedEvent == LED_EV_CONNECTED || currentLedEvent == LED_EV_DISCONNECTED)) {
    return;
  }
  currentLedEvent = ev;
  ledEventStartMs = millis();
}

void pulseLed() {
  if (cfg.ledMode == 2) {
    triggerLedEvent(LED_EV_ACTIVITY);
  }
}

int16_t extractSignedBits(const uint8_t* data, size_t len, uint16_t bitOffset, uint8_t bitSize) {
  if (bitSize == 0 || (bitOffset + bitSize) > len * 8) return 0;
  uint32_t raw = 0;
  for (uint8_t i = 0; i < bitSize; i++) {
    uint16_t bIdx = (bitOffset + i) >> 3;
    uint8_t  bShift = (bitOffset + i) & 7;
    if (data[bIdx] & (1 << bShift)) {
      raw |= (1UL << i);
    }
  }
  if (raw & (1UL << (bitSize - 1))) {
    raw |= (~0UL << bitSize);
  }
  return (int16_t)raw;
}

uint16_t extractUnsignedBits(const uint8_t* data, size_t len, uint16_t bitOffset, uint8_t bitSize) {
  if (bitSize == 0 || (bitOffset + bitSize) > len * 8) return 0;
  if (bitSize > 16) bitSize = 16;
  uint16_t raw = 0;
  for (uint8_t i = 0; i < bitSize; i++) {
    uint16_t bIdx = (bitOffset + i) >> 3;
    uint8_t  bShift = (bitOffset + i) & 7;
    if (data[bIdx] & (1 << bShift)) {
      raw |= (1U << i);
    }
  }
  return raw;
}

// Автоматический парсер дескриптора HID Report Map (0x2A4B)
int parseHidReportMap(const uint8_t* desc, size_t len, ReportLayout* outLayouts, int maxLayouts) {
  int count = 0;
  uint16_t usagePage = 0;
  uint8_t  reportId = 0;
  uint16_t reportSize = 0;
  uint16_t reportCount = 0;
  uint16_t usageMin = 0;
  uint16_t usages[16];
  int      usageCnt = 0;
  int      collDepth = 0;
  uint8_t  appKind = KIND_UNKNOWN;

  auto getOrAddLayout = [&](uint8_t rId, uint8_t defKind) -> ReportLayout* {
    for (int j = 0; j < count; j++) {
      if (outLayouts[j].reportId == rId) {
        if (outLayouts[j].kind == KIND_UNKNOWN && defKind != KIND_UNKNOWN) {
          outLayouts[j].kind = defKind;
        }
        return &outLayouts[j];
      }
    }
    if (count >= maxLayouts) return nullptr;
    ReportLayout& nl = outLayouts[count++];
    nl = ReportLayout();
    nl.valid = true;
    nl.reportId = rId;
    nl.kind = defKind;
    return &nl;
  };

  size_t i = 0;
  while (i < len) {
    uint8_t b = desc[i++];
    if (b == 0xFE) {
      if (i + 2 <= len) i += 2 + desc[i];
      continue;
    }
    uint8_t bSize = b & 0x03;
    if (bSize == 3) bSize = 4;
    uint8_t bType = (b >> 2) & 0x03;
    uint8_t bTag  = (b >> 4) & 0x0F;
    if (i + bSize > len) break;

    uint32_t val = 0;
    for (uint8_t k = 0; k < bSize; k++) val |= ((uint32_t)desc[i + k] << (8 * k));
    i += bSize;

    if (bType == 1) { // Global
      if (bTag == 0x0) usagePage = val;
      else if (bTag == 0x7) reportSize = val;
      else if (bTag == 0x8) { reportId = (uint8_t)val; getOrAddLayout(reportId, appKind); }
      else if (bTag == 0x9) reportCount = val;
    } else if (bType == 2) { // Local
      if (bTag == 0x0 && usageCnt < 16) usages[usageCnt++] = val;
      else if (bTag == 0x1) usageMin = val;
      else if (bTag == 0x2) { /* Usage Maximum не нужен текущему декодеру */ }
    } else if (bType == 0) { // Main
      if (bTag == 0xA) { // Collection
        if (collDepth == 0) {
          uint16_t u = (usageCnt > 0) ? usages[0] : 0;
          if (usagePage == 0x01 && u == 0x06) appKind = KIND_KEYBOARD;
          else if (usagePage == 0x01 && (u == 0x02 || u == 0x01)) appKind = KIND_MOUSE;
          else if (usagePage == 0x01 && (u == 0x05 || u == 0x04)) appKind = KIND_GAMEPAD;
          else if (usagePage == 0x0C) appKind = KIND_CONSUMER;
          else if (usagePage >= 0xFF00) appKind = KIND_VENDOR;
          else appKind = KIND_UNKNOWN;
        }
        collDepth++;
      } else if (bTag == 0xC) { // End Collection
        if (collDepth > 0) collDepth--;
        if (collDepth == 0) appKind = KIND_UNKNOWN;
      } else if (bTag == 0x8) { // Input
        ReportLayout* lay = getOrAddLayout(reportId, appKind);
        if (lay) {
          bool isConst = (val & 0x01) != 0;
          bool isVar   = (val & 0x02) != 0;
          uint16_t curOff = lay->bitOffset;

          if (!isConst && lay->kind != KIND_VENDOR) {
            if (usagePage == 0x09) { // Buttons
              if (lay->kind == KIND_UNKNOWN) lay->kind = KIND_MOUSE;
              if (lay->btnSz == 0) {
                lay->btnOff = curOff;
                lay->btnSz  = reportSize * reportCount;
              }
            } else if (usagePage == 0x01) { // Generic Desktop
              for (uint16_t idx = 0; idx < reportCount; idx++) {
                uint16_t u = (idx < usageCnt) ? usages[idx] : (usageCnt > 0 ? usages[usageCnt - 1] : 0);
                uint16_t fOff = curOff + idx * reportSize;
                if (u == 0x39 || u == 0x32 || u == 0x35) {
                  lay->kind = KIND_GAMEPAD;
                } else if (u == 0x30 && lay->kind != KIND_GAMEPAD) {
                  lay->xOff = fOff; lay->xSz = reportSize;
                  if (lay->kind == KIND_UNKNOWN) lay->kind = KIND_MOUSE;
                } else if (u == 0x31 && lay->kind != KIND_GAMEPAD) {
                  lay->yOff = fOff; lay->ySz = reportSize;
                  if (lay->kind == KIND_UNKNOWN) lay->kind = KIND_MOUSE;
                } else if (u == 0x38) {
                  lay->wOff = fOff; lay->wSz = reportSize;
                }
              }
            } else if (usagePage == 0x02 && (appKind == KIND_GAMEPAD || lay->kind == KIND_GAMEPAD)) {
              lay->kind = KIND_GAMEPAD; // Accelerator / Brake triggers (Google Stadia L2/R2)
            } else if (usagePage == 0x0C) { // Consumer Page
              uint16_t u = (usageCnt > 0) ? usages[0] : 0;
              if (u == 0x0238 || lay->kind == KIND_MOUSE) {
                lay->panOff = curOff;
                lay->panSz  = reportSize;
              } else if (lay->kind == KIND_UNKNOWN) {
                lay->kind = KIND_CONSUMER;
              }
            } else if (usagePage == 0x07) { // Keyboard / Keypad Page
              lay->kind = KIND_KEYBOARD;
              if (isVar && reportSize == 1) {
                if (usageMin == 0xE0 || (reportCount == 8 && !lay->hasMod)) {
                  lay->modOff = curOff;
                  lay->hasMod = true;
                } else if (reportCount >= 16) {
                  lay->hasBitmap   = true;
                  lay->bmpBitOff   = curOff;
                  lay->bmpBitCnt   = reportCount;
                  lay->bmpUsageMin = (uint8_t)usageMin;
                }
              } else if (!isVar && reportSize == 8) {
                lay->keyByteOff = curOff / 8;
                lay->keyCount   = reportCount;
              }
            }
          }
          lay->bitOffset += reportSize * reportCount;
        }
      }
      usageCnt = 0;
      usageMin = 0;
    }
  }
  return count;
}

void saveConfigToNVS() {
  prefs.begin("hidbridge", false);
  prefs.putUChar("mScale", cfg.mouseScale10);
  prefs.putUChar("sScale", cfg.scrollScale);
  prefs.putBool("swapBtn", cfg.swapMouseBtns);
  prefs.putBool("invWhl", cfg.invertWheel);
  prefs.putBool("invXY", cfg.invertXY);
  prefs.putBool("swapWA", cfg.swapWinAlt);
  prefs.putBool("bootP", cfg.bootProtocol);
  prefs.putBool("autoRec", cfg.autoReconnect);
  prefs.putUChar("ledM", cfg.ledMode);
  prefs.putUChar("ledP", cfg.ledPin);
  prefs.putUChar("ledT", cfg.ledType);
  prefs.putUChar("mFmt", cfg.mouseFmt);
  prefs.putUChar("kFmt", cfg.kbFmt);
  prefs.putUChar("gpMode", cfg.gpMode);
  prefs.putUChar("gpFmt", cfg.gpFmt);
  prefs.putUChar("gpDz", cfg.gpDeadzone);
  prefs.end();
}

void loadConfigFromNVS() {
  prefs.begin("hidbridge", true);
  cfg.mouseScale10  = prefs.getUChar("mScale", 10);
  cfg.scrollScale   = prefs.getUChar("sScale", 1);
  cfg.swapMouseBtns = prefs.getBool("swapBtn", false);
  cfg.invertWheel   = prefs.getBool("invWhl", false);
  cfg.invertXY      = prefs.getBool("invXY", false);
  cfg.swapWinAlt    = prefs.getBool("swapWA", false);
  cfg.bootProtocol  = prefs.getBool("bootP", false);
  cfg.autoReconnect = prefs.getBool("autoRec", true);
  cfg.ledMode       = prefs.getUChar("ledM", 2);
  cfg.ledPin        = prefs.getUChar("ledP", 48);
  cfg.ledType       = prefs.getUChar("ledT", 2);
  cfg.mouseFmt      = prefs.getUChar("mFmt", 0);
  cfg.kbFmt         = prefs.getUChar("kFmt", 0);
  cfg.gpMode        = prefs.getUChar("gpMode", 0);
  cfg.gpFmt         = prefs.getUChar("gpFmt", 0);
  cfg.gpDeadzone    = prefs.getUChar("gpDz", 10);

  savedCount = prefs.getInt("bCount", 0);
  if (savedCount > MAX_BONDS) savedCount = MAX_BONDS;
  for (int i = 0; i < savedCount; i++) {
    savedBonds[i].mac      = prefs.getString(("bMac" + String(i)).c_str(), "");
    savedBonds[i].addrType = prefs.getUChar(("bTyp" + String(i)).c_str(), 1);
    savedBonds[i].name     = prefs.getString(("bNam" + String(i)).c_str(), "BLE HID");
    savedBonds[i].fmt      = prefs.getUChar(("bFmt" + String(i)).c_str(), 0);
    savedBonds[i].scale    = prefs.getUChar(("bScl" + String(i)).c_str(), 10);
    savedBonds[i].connected = false;
  }
  prefs.end();
}

void saveBondsToNVS() {
  prefs.begin("hidbridge", false);
  prefs.putInt("bCount", savedCount);
  for (int i = 0; i < savedCount; i++) {
    prefs.putString(("bMac" + String(i)).c_str(), savedBonds[i].mac);
    prefs.putUChar(("bTyp" + String(i)).c_str(), savedBonds[i].addrType);
    prefs.putString(("bNam" + String(i)).c_str(), savedBonds[i].name);
    prefs.putUChar(("bFmt" + String(i)).c_str(), savedBonds[i].fmt);
    prefs.putUChar(("bScl" + String(i)).c_str(), savedBonds[i].scale);
  }
  prefs.end();
}

int findOrAddBond(const String& mac, uint8_t type, const String& name, bool isConn, uint8_t autoFmt = 0) {
  for (int i = 0; i < savedCount; i++) {
    if (savedBonds[i].mac.equalsIgnoreCase(mac)) {
      savedBonds[i].connected = isConn;
      if (name.length() > 0 && name != "BLE Устройство") savedBonds[i].name = name;
      if (savedBonds[i].fmt == 0 && autoFmt != 0) savedBonds[i].fmt = autoFmt;
      saveBondsToNVS();
      return i;
    }
  }
  if (savedCount < MAX_BONDS) {
    int idx = savedCount++;
    savedBonds[idx] = {mac, type, name.length() ? name : "BLE HID", isConn, autoFmt, 10};
    saveBondsToNVS();
    return idx;
  }
  return -1;
}

void sendStatusJson() {
  if (otaInProgress) return;
  const esp_partition_t* running = esp_ota_get_running_partition();
  const char* partLabel = running ? running->label : "app0";
  CONSOLE.print("{\"ev\":\"status\",\"ver\":\"3.0-FIXED\",\"part\":\"");
  CONSOLE.print(partLabel);
  CONSOLE.print("\",\"otaFree\":");
  CONSOLE.print(ESP.getFreeSketchSpace());
  CONSOLE.print(",\"uptime\":");
  CONSOLE.print(millis() / 1000);
  CONSOLE.print(",\"heap\":");
  CONSOLE.print(ESP.getFreeHeap());
  CONSOLE.print(",\"pkts\":");
  CONSOLE.print(packetCounter);
  CONSOLE.print(",\"scanning\":");
  CONSOLE.print(NimBLEDevice::getScan()->isScanning() ? "true" : "false");
  CONSOLE.print(",\"bonds\":[");
  for (int i = 0; i < savedCount; i++) {
    if (i > 0) CONSOLE.print(",");
    CONSOLE.print("{\"mac\":\"" + savedBonds[i].mac + "\",\"type\":" + String(savedBonds[i].addrType) +
                  ",\"name\":\"" + savedBonds[i].name + "\",\"fmt\":" + String(savedBonds[i].fmt) +
                  ",\"scale\":" + String(savedBonds[i].scale) +
                  ",\"connected\":" + (savedBonds[i].connected ? "true" : "false") + "}");
  }
  CONSOLE.println("]}");
}

void processKeyboardReport(const uint8_t* pData, size_t length, const ReportLayout* lay, uint8_t overrideKbFmt = 0) {
  if (length < 2) return;
  KeyReport report = {0, 0, {0, 0, 0, 0, 0, 0}};
  uint8_t mods = pData[0];

  uint8_t mode = (overrideKbFmt > 0) ? overrideKbFmt : cfg.kbFmt; // 0=Auto, 1=Boot, 2=Std8B, 3=NoRes, 4=Bitmap
  if (mode == 4 || (mode == 0 && ((lay && lay->hasBitmap && lay->keyCount == 0) || length >= 12))) {
    uint16_t bitOff = (lay && lay->hasBitmap) ? lay->bmpBitOff : 8;
    uint16_t bitCnt = (lay && lay->hasBitmap) ? lay->bmpBitCnt : (length - 1) * 8;
    uint8_t  uMin   = (lay && lay->hasBitmap) ? lay->bmpUsageMin : 0;
    int kIdx = 0;
    for (uint16_t b = 0; b < bitCnt && (bitOff + b) < length * 8; b++) {
      if (pData[(bitOff + b) >> 3] & (1 << ((bitOff + b) & 7))) {
        uint8_t kc = uMin + b;
        if (kc >= 0xE0 && kc <= 0xE7) {
          mods |= (1 << (kc - 0xE0));
        } else if (kc >= 0x04 && kIdx < 6) {
          report.keys[kIdx++] = kc;
        }
      }
    }
  } else {
    uint8_t keyStart = 2;
    if (mode == 3) {
      keyStart = 1;
    } else if (mode == 2 || mode == 1) {
      keyStart = 2;
    } else {
      if (lay && lay->valid && lay->keyCount > 0) {
        keyStart = lay->keyByteOff;
      } else if (length == 7 || (length >= 8 && pData[1] >= 0x04 && pData[2] == 0x00)) {
        keyStart = 1;
      } else {
        keyStart = 2;
      }
    }
    int kIdx = 0;
    for (size_t i = keyStart; i < length && kIdx < 6; i++) {
      if (pData[i] != 0x00) {
        report.keys[kIdx++] = pData[i];
      }
    }
  }

  if (cfg.swapWinAlt) {
    uint8_t lAlt = (mods & 0x04) ? 0x08 : 0;
    uint8_t lGui = (mods & 0x08) ? 0x04 : 0;
    uint8_t rAlt = (mods & 0x40) ? 0x80 : 0;
    uint8_t rGui = (mods & 0x80) ? 0x40 : 0;
    mods = (mods & 0x33) | lAlt | lGui | rAlt | rGui;
  }

  report.modifiers = mods;
  Keyboard.sendReport(&report);
#if HID_PACKET_DEBUG
  CONSOLE.printf("{\"ev\":\"hid_kb\",\"mod\":%d,\"k0\":%d,\"k1\":%d}\n", mods, report.keys[0], report.keys[1]);
#endif
}

void processMouseReport(const uint8_t* pData, size_t length, const ReportLayout* lay, uint8_t overrideMouseFmt = 0, uint8_t overrideScale10 = 0) {
  if (length < 3) return;
  uint8_t buttons = pData[0];
  int16_t rawX = 0, rawY = 0, rawW = 0, rawPan = 0;

  uint8_t mode = (overrideMouseFmt > 0) ? overrideMouseFmt : cfg.mouseFmt;
  uint8_t scale10 = (overrideScale10 > 0) ? overrideScale10 : cfg.mouseScale10;

  if (mode == 0 && lay && lay->valid && lay->xSz > 0 && lay->ySz > 0) {
    buttons = (uint8_t)extractUnsignedBits(pData, length, lay->btnOff, (lay->btnSz > 8) ? 8 : lay->btnSz);
    rawX    = extractSignedBits(pData, length, lay->xOff, lay->xSz);
    rawY    = extractSignedBits(pData, length, lay->yOff, lay->ySz);
    rawW    = (lay->wSz > 0) ? extractSignedBits(pData, length, lay->wOff, lay->wSz) : 0;
    rawPan  = (lay->panSz > 0) ? extractSignedBits(pData, length, lay->panOff, lay->panSz) : 0;
  } else {
    if (mode == 0) {
      if (length <= 4) mode = 5;
      else if (length == 5) mode = 3;
      else mode = 1;
    }

    if (mode == 1 && length >= 5) {
      buttons = pData[0];
      rawX = (int16_t)(pData[1] | (pData[2] << 8));
      rawY = (int16_t)(pData[3] | (pData[4] << 8));
      rawW = (length >= 6) ? (int8_t)pData[5] : 0;
      rawPan = (length >= 7) ? (int8_t)pData[6] : 0;
    } else if (mode == 2 && length >= 6) {
      buttons = pData[0];
      rawX = (int16_t)(pData[2] | (pData[3] << 8));
      rawY = (int16_t)(pData[4] | (pData[5] << 8));
      rawW = (length >= 7) ? (int8_t)pData[6] : 0;
      rawPan = (length >= 8) ? (int8_t)pData[7] : 0;
    } else if (mode == 3 && length >= 4) {
      buttons = pData[0];
      rawX = extractSignedBits(pData, length, 8, 12);
      rawY = extractSignedBits(pData, length, 20, 12);
      rawW = (length >= 5) ? (int8_t)pData[4] : 0;
    } else if (mode == 4 && length >= 5) {
      buttons = pData[0];
      rawX = extractSignedBits(pData, length, 16, 12);
      rawY = extractSignedBits(pData, length, 28, 12);
      rawW = (length >= 6) ? (int8_t)pData[5] : 0;
    } else {
      buttons = pData[0];
      rawX = (int8_t)pData[1];
      rawY = (int8_t)pData[2];
      rawW = (length >= 4) ? (int8_t)pData[3] : 0;
    }
  }

  if (cfg.swapMouseBtns) {
    uint8_t bL = (buttons & 0x01) ? 0x02 : 0;
    uint8_t bR = (buttons & 0x02) ? 0x01 : 0;
    buttons = (buttons & 0xFC) | bL | bR;
  }

  if (cfg.invertXY) { rawX = -rawX; rawY = -rawY; }
  if (cfg.invertWheel) { rawW = -rawW; }

  int8_t x = (int8_t)constrain((rawX * scale10) / 10, -127, 127);
  int8_t y = (int8_t)constrain((rawY * scale10) / 10, -127, 127);
  int8_t wheel = (int8_t)constrain(rawW * cfg.scrollScale, -127, 127);
  int8_t pan = (int8_t)constrain(rawPan, -127, 127);

  sendUsbMouse(buttons, x, y, wheel, pan);

#if HID_PACKET_DEBUG
  CONSOLE.printf("{\"ev\":\"hid_ms\",\"btn\":%d,\"x\":%d,\"y\":%d,\"w\":%d}\n", buttons, x, y, wheel);
#endif
}

// Обработчик джойстика Google Stadia Controller (BLE Report 0x03) и универсальных BLE-геймпадов
void processGamepadReport(const uint8_t* pData, size_t length, uint8_t overrideGpFmt = 0, uint8_t scale10 = 10) {
  // Если пришёл 11-байтный пакет Google Stadia с префиксом Report ID 0x03 в начале — сдвигаем на 1 байт
  if (length == 11 && pData[0] == 0x03) {
    pData++;
    length--;
  }
  if (length < 7) return;

  // Формат Google Stadia BLE (10 байт):
  // Byte 0: Hat Switch (0..7 = N..NW, 8 = Center)
  // Byte 1..2: 15-битная маска кнопок (Bit14=A, Bit13=B, Bit12=X, Bit11=Y, Bit10=L1, Bit9=R1, Bit8=L3, Bit7=R3, Bit6=Options, Bit5=Menu, Bit4=Stadia, Bit3=R2, Bit2=L2, Bit1=Assistant, Bit0=Capture)
  // Byte 3..6: Оси стиков LX, LY, RX, RY (1..255, центр 128)
  // Byte 7..8: Аналоговые курки L2 (Brake) и R2 (Gas) (0..255)
  uint8_t rawHat = pData[0] & 0x0F;
  uint16_t rawBtns = pData[1] | ((uint16_t)pData[2] << 8);
  int16_t lx = (int16_t)pData[3] - 128;
  int16_t ly = (int16_t)pData[4] - 128;
  int16_t rx = (int16_t)pData[5] - 128;
  int16_t ry = (int16_t)pData[6] - 128;
  uint8_t l2 = (length >= 8) ? pData[7] : 0;
  uint8_t r2 = (length >= 9) ? pData[8] : 0;

  // Применяем мёртвую зону (Deadzone) против дрифта стиков
  int16_t dz = cfg.gpDeadzone;
  if (abs(lx) < dz) lx = 0;
  if (abs(ly) < dz) ly = 0;
  if (abs(rx) < dz) rx = 0;
  if (abs(ry) < dz) ry = 0;

  bool isStadiaLayout = (overrideGpFmt == 10) || (overrideGpFmt == 0 && cfg.gpFmt == 0);
  uint32_t usbBtns = 0;
  if (isStadiaLayout) {
    if (rawBtns & (1 << 14)) usbBtns |= (1UL << 0);  // A -> Button 1
    if (rawBtns & (1 << 13)) usbBtns |= (1UL << 1);  // B -> Button 2
    if (rawBtns & (1 << 12)) usbBtns |= (1UL << 2);  // X -> Button 3
    if (rawBtns & (1 << 11)) usbBtns |= (1UL << 3);  // Y -> Button 4
    if (rawBtns & (1 << 10)) usbBtns |= (1UL << 4);  // L1 -> Button 5
    if (rawBtns & (1 << 9))  usbBtns |= (1UL << 5);  // R1 -> Button 6
    if ((rawBtns & (1 << 2)) || l2 > 60) usbBtns |= (1UL << 6); // L2 -> Button 7
    if ((rawBtns & (1 << 3)) || r2 > 60) usbBtns |= (1UL << 7); // R2 -> Button 8
    if (rawBtns & (1 << 6))  usbBtns |= (1UL << 8);  // Options -> Button 9
    if (rawBtns & (1 << 5))  usbBtns |= (1UL << 9);  // Menu -> Button 10
    if (rawBtns & (1 << 8))  usbBtns |= (1UL << 10); // L3 -> Button 11
    if (rawBtns & (1 << 7))  usbBtns |= (1UL << 11); // R3 -> Button 12
    if (rawBtns & (1 << 4))  usbBtns |= (1UL << 12); // Stadia -> Button 13
    if (rawBtns & (1 << 1))  usbBtns |= (1UL << 13); // Assistant -> Button 14
    if (rawBtns & (1 << 0))  usbBtns |= (1UL << 14); // Capture -> Button 15
  } else {
    usbBtns = rawBtns;
  }

#if BRIDGE_HAS_USB_GAMEPAD
  if (cfg.gpMode == 0) {
    // Режим 0: Нативный USB HID Геймпад (TinyUSB Gamepad)
    uint8_t usbHat = (rawHat <= 7) ? (rawHat + 1) : 0; // TinyUSB: 0=Center, 1=N .. 8=NW
    Gamepad.send(
      (int8_t)constrain(lx, -127, 127),
      (int8_t)constrain(ly, -127, 127),
      (int8_t)constrain(rx, -127, 127),
      (int8_t)constrain(ry, -127, 127),
      (int8_t)((int16_t)l2 - 128),
      (int8_t)((int16_t)r2 - 128),
      usbHat,
      usbBtns
    );
  } else
#endif
  {
    // Режим 1: Аппаратная эмуляция Клавиатура (WASD) + Мышь (Правый стик + L2/R2)
    KeyReport kRpt = {0, 0, {0, 0, 0, 0, 0, 0}};
    int kIdx = 0;
    auto pushKey = [&](uint8_t kc) { if (kIdx < 6) kRpt.keys[kIdx++] = kc; };

    // Левый стик и крестовина D-Pad -> W (0x1A), A (0x04), S (0x16), D (0x07)
    if (ly < -35 || rawHat == 0 || rawHat == 1 || rawHat == 7) pushKey(0x1A); // W
    if (ly >  35 || rawHat == 3 || rawHat == 4 || rawHat == 5) pushKey(0x16); // S
    if (lx < -35 || rawHat == 5 || rawHat == 6 || rawHat == 7) pushKey(0x04); // A
    if (lx >  35 || rawHat == 1 || rawHat == 2 || rawHat == 3) pushKey(0x07); // D

    if (usbBtns & (1UL << 0)) pushKey(0x2C); // A -> Пробел (Space)
    if (usbBtns & (1UL << 1)) pushKey(0x06); // B -> C (Присесть)
    if (usbBtns & (1UL << 2)) pushKey(0x15); // X -> R (Перезарядка)
    if (usbBtns & (1UL << 3)) pushKey(0x08); // Y -> E (Действие)
    if (usbBtns & (1UL << 9)) pushKey(0x28); // Menu -> Enter
    if (usbBtns & (1UL << 8)) pushKey(0x29); // Options -> Esc
    if (usbBtns & (1UL << 10)) kRpt.modifiers |= 0x02; // L3 -> Left Shift (Бег)

    Keyboard.sendReport(&kRpt);

    // Правый стик -> Курсор мыши; R2/R1 -> ЛКМ; L2/L1 -> ПКМ
    uint8_t mBtns = 0;
    if ((usbBtns & (1UL << 7)) || (usbBtns & (1UL << 5))) mBtns |= 0x01; // R2 / R1 -> ЛКМ
    if ((usbBtns & (1UL << 6)) || (usbBtns & (1UL << 4))) mBtns |= 0x02; // L2 / L1 -> ПКМ
    if (usbBtns & (1UL << 11)) mBtns |= 0x04; // R3 -> СКМ

    int8_t mx = (int8_t)constrain((rx * scale10) / 80, -127, 127);
    int8_t my = (int8_t)constrain((ry * scale10) / 80, -127, 127);
    sendUsbMouse(mBtns, mx, my, 0, 0);
  }

#if HID_PACKET_DEBUG
  CONSOLE.printf("{\"ev\":\"hid_gp\",\"hat\":%d,\"btn\":%u,\"lx\":%d,\"ly\":%d,\"rx\":%d,\"ry\":%d,\"l2\":%d,\"r2\":%d}\n",
                 rawHat, (unsigned int)usbBtns, lx, ly, rx, ry, l2, r2);
#endif
}

// Обработчик входящих BLE HID пакетов
void notifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  if (length == 0) return;
  packetCounter++;
  lastHidInputMs = millis();
  pulseLed();

  uint16_t cHandle = pChar->getHandle();
  uint16_t connH   = pChar->getClient() ? pChar->getClient()->getConnHandle() : 0xFFFF;

  // Ищем привязанный SubscribedCharInfo и индивидуальный профиль устройства (bondFmt / bondScale)
  const ReportLayout* matchedLay = nullptr;
  uint8_t devRole = 0;
  uint8_t bondFmt = 0;
  uint8_t bondScale = cfg.mouseScale10;

  for (int i = 0; i < subCharCount; i++) {
    if (subChars[i].connHandle == connH && subChars[i].charHandle == cHandle) {
      devRole = subChars[i].devRole;
      if (subChars[i].layout.valid) matchedLay = &subChars[i].layout;
      int8_t bIdx = subChars[i].bondIdx;
      if (bIdx >= 0 && bIdx < savedCount) {
        bondFmt   = savedBonds[bIdx].fmt;
        bondScale = savedBonds[bIdx].scale;
      }
      break;
    }
  }

  // 1) Принудительный индивидуальный профиль раскодировки (заданный пользователем для конкретного MAC)
  if (bondFmt >= 1 && bondFmt <= 5) {
    processMouseReport(pData, length, matchedLay, bondFmt, bondScale);
    return;
  }
  if (bondFmt >= 6 && bondFmt <= 9) {
    processKeyboardReport(pData, length, matchedLay, bondFmt - 5);
    return;
  }
  if (bondFmt == 10 || bondFmt == 11) {
    processGamepadReport(pData, length, bondFmt, bondScale);
    return;
  }

  // 2) Проверка прямых Boot характеристик (0x2A22 / 0x2A33)
  if (pChar->getUUID() == bootKbUUID) {
    processKeyboardReport(pData, length, nullptr, 0);
    return;
  }
  if (pChar->getUUID() == bootMouseUUID) {
    processMouseReport(pData, length, nullptr, 0, bondScale);
    return;
  }

  // 3) Автоматический роутинг по дескриптору 0x2A4B (ReportLayout)
  if (matchedLay) {
    uint16_t expBytes = (matchedLay->bitOffset + 7) / 8;
    if (length == expBytes + 1 && pData[0] == matchedLay->reportId) {
      pData++;
      length--;
    }
    if (matchedLay->kind == KIND_GAMEPAD) {
      processGamepadReport(pData, length, 0, bondScale);
      return;
    } else if (matchedLay->kind == KIND_KEYBOARD) {
      processKeyboardReport(pData, length, matchedLay, 0);
      return;
    } else if (matchedLay->kind == KIND_MOUSE) {
      processMouseReport(pData, length, matchedLay, 0, bondScale);
      return;
    } else if (matchedLay->kind == KIND_CONSUMER && length >= 2) {
      uint16_t mediaCode = pData[0] | (pData[1] << 8);
      if (mediaCode != 0) ConsumerControl.press(mediaCode);
      else ConsumerControl.release();
      return;
    } else if (matchedLay->kind == KIND_VENDOR) {
      return;
    }
  }

  // 4) Fallback по роли устройства (Геймпад / Мышь / Клавиатура)
  if (devRole == KIND_GAMEPAD || length == 10 || (length == 11 && pData[0] == 0x03)) {
    processGamepadReport(pData, length, 0, bondScale);
  } else if (devRole == KIND_MOUSE) {
    processMouseReport(pData, length, nullptr, 0, bondScale);
  } else if (devRole == KIND_KEYBOARD) {
    if (length == 2) {
      uint16_t mediaCode = pData[0] | (pData[1] << 8);
      if (mediaCode != 0) ConsumerControl.press(mediaCode);
      else ConsumerControl.release();
    } else if (length >= 7) {
      processKeyboardReport(pData, length, nullptr, 0);
    }
  } else {
    if (length >= 8) processKeyboardReport(pData, length, nullptr, 0);
    else if (length >= 3 && length <= 7) processMouseReport(pData, length, nullptr, 0, bondScale);
  }
}

void reportDiscoveredDevice(const NimBLEAdvertisedDevice* dev) {
  uint16_t app = dev->getAppearance();
  bool isHid = (dev->haveServiceUUID() && dev->isAdvertisingService(hidServiceUUID)) ||
               (app >= 960 && app <= 968);
  String name = dev->getName().c_str();
  name.replace("\"", "");
  if (name.length() == 0) name = isHid ? "HID Устройство (0x1812)" : "BLE Устройство";
  String mac = dev->getAddress().toString().c_str();
  uint8_t aType = dev->getAddress().getType();
  int rssi = dev->getRSSI();

  CONSOLE.printf("{\"ev\":\"scan\",\"name\":\"%s\",\"mac\":\"%s\",\"type\":%d,\"rssi\":%d,\"hid\":%s,\"app\":%d}\n",
                 name.c_str(), mac.c_str(), aType, rssi, isHid ? "true" : "false", app);
}

class ScanCallbacks: public NimBLEScanCallbacks {
  void onDiscovered(const NimBLEAdvertisedDevice* dev) override {
    reportDiscoveredDevice(dev);
  }
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    reportDiscoveredDevice(dev);
  }
  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    isScanning = false;
    CONSOLE.println("{\"ev\":\"scan_end\"}");
  }
};

class BridgeClientCallbacks: public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient* pClient, int reason) override {
    String peer = pClient->getPeerAddress().toString().c_str();
    for (int i = 0; i < savedCount; i++) {
      if (savedBonds[i].mac.equalsIgnoreCase(peer)) {
        savedBonds[i].connected = false;
        break;
      }
    }

    // Не оставляем на компьютере зажатую клавишу или кнопку после потери BLE.
    Keyboard.releaseAll();
    sendUsbMouse(0, 0, 0, 0, 0);
    ConsumerControl.release();
    triggerLedEvent(LED_EV_DISCONNECTED);
  }
};

BridgeClientCallbacks bridgeClientCallbacks;

bool connectToBleDevice(const String& mac, uint8_t addrType, const String& name) {
  NimBLEDevice::getScan()->stop();
  isScanning = false;

  NimBLEAddress addr(mac.c_str(), addrType);
  NimBLEClient* pClient = NimBLEDevice::getClientByPeerAddress(addr);
  if (!pClient) {
    pClient = NimBLEDevice::createClient();
  }

  pClient->setClientCallbacks(&bridgeClientCallbacks, false);
  // 6..12 * 1.25 мс = 7.5..15 мс, latency=0.
  pClient->setConnectionParams(6, 12, 0, 400);
  pClient->setConnectTimeout(2500);
  if (!pClient->connect(addr)) {
    CONSOLE.printf("{\"ev\":\"err\",\"msg\":\"Не удалось подключиться к %s\"}\n", mac.c_str());
    return false;
  }

  pClient->updateConnParams(6, 12, 0, 400);

  pClient->secureConnection();
  NimBLERemoteService* pService = pClient->getService(hidServiceUUID);
  if (!pService) {
    CONSOLE.printf("{\"ev\":\"err\",\"msg\":\"Служба HID 0x1812 не найдена на %s\"}\n", mac.c_str());
    pClient->disconnect();
    return false;
  }

  // 1) Читаем и парсим дескриптор HID Report Map (0x2A4B)
  ReportLayout parsedLayouts[12];
  int parsedCount = 0;
  uint8_t detectedDeviceRole = 0;

  if (name.indexOf("Stadia") >= 0 || name.indexOf("Gamepad") >= 0 || name.indexOf("Controller") >= 0) {
    detectedDeviceRole = KIND_GAMEPAD;
  }

  NimBLERemoteCharacteristic* pMapChar = pService->getCharacteristic(reportMapUUID);
  if (pMapChar && pMapChar->canRead()) {
    NimBLEAttValue mapVal = pMapChar->readValue();
    if (mapVal.size() > 0) {
      parsedCount = parseHidReportMap(mapVal.data(), mapVal.size(), parsedLayouts, 12);
      for (int i = 0; i < parsedCount; i++) {
        if (parsedLayouts[i].kind == KIND_GAMEPAD) detectedDeviceRole = KIND_GAMEPAD;
        else if (parsedLayouts[i].kind == KIND_MOUSE && detectedDeviceRole != KIND_GAMEPAD) detectedDeviceRole = KIND_MOUSE;
        else if (parsedLayouts[i].kind == KIND_KEYBOARD && detectedDeviceRole == 0) detectedDeviceRole = KIND_KEYBOARD;

        CONSOLE.printf("{\"ev\":\"rmap\",\"id\":%d,\"kind\":%d,\"bits\":%d,\"xSz\":%d,\"ySz\":%d,\"keyOff\":%d,\"bmp\":%d}\n",
                       parsedLayouts[i].reportId, parsedLayouts[i].kind, parsedLayouts[i].bitOffset,
                       parsedLayouts[i].xSz, parsedLayouts[i].ySz, parsedLayouts[i].keyByteOff, parsedLayouts[i].hasBitmap ? 1 : 0);
      }
    }
  }

  String lowerName = name;
  lowerName.toLowerCase();
  uint8_t autoFmt = 0;
  if (lowerName.indexOf("stadia") >= 0) {
    autoFmt = 10;
  } else if (lowerName.indexOf("mx keys") >= 0) {
    // Для MX Keys Mini используем стабильный Boot report 0x2A22.
    autoFmt = 6;
  }
  int bIdx = findOrAddBond(mac, addrType, name, true, autoFmt);
  uint8_t bondFmt = (bIdx >= 0) ? savedBonds[bIdx].fmt : 0;

  // 2) Если для этой клавиатуры выбран Boot Protocol (или глобальный) — пишем 0x00 в 0x2A4E
  bool useBootMode = (bondFmt == 6) || (bondFmt == 0 && (cfg.kbFmt == 1 || cfg.bootProtocol) && detectedDeviceRole == KIND_KEYBOARD);
  NimBLERemoteCharacteristic* pProtoChar = pService->getCharacteristic(protoModeUUID);
  if (pProtoChar && pProtoChar->canWrite()) {
    uint8_t modeByte = useBootMode ? 0x00 : 0x01;
    pProtoChar->writeValue(&modeByte, 1, false);
  }

  uint16_t connH = pClient->getConnHandle();
  for (int i = 0; i < subCharCount; ) {
    if (subChars[i].connHandle == connH) {
      subChars[i] = subChars[--subCharCount];
    } else {
      i++;
    }
  }

  // 3) Подписываемся на характеристики ввода
  int subCount = 0;
  const auto& chars = pService->getCharacteristics(true);
  for (auto* pChar : chars) {
    if (!pChar->canNotify()) continue;
    NimBLEUUID u = pChar->getUUID();

    if (u == bootKbUUID) {
      if (detectedDeviceRole == 0) detectedDeviceRole = KIND_KEYBOARD;
      if (!useBootMode && parsedCount > 0) continue;
      if (pChar->subscribe(true, notifyCallback)) {
        subCount++;
        if (subCharCount < MAX_SUB_CHARS) {
          subChars[subCharCount++] = { connH, pChar->getHandle(), detectedDeviceRole, (int8_t)bIdx, ReportLayout() };
        }
      }
      continue;
    }
    if (u == bootMouseUUID) {
      detectedDeviceRole = KIND_MOUSE;
      if (!useBootMode && parsedCount > 0) continue;
      if (pChar->subscribe(true, notifyCallback)) {
        subCount++;
        if (subCharCount < MAX_SUB_CHARS) {
          subChars[subCharCount++] = { connH, pChar->getHandle(), detectedDeviceRole, (int8_t)bIdx, ReportLayout() };
        }
      }
      continue;
    }

    if (u == reportUUID) {
      if (useBootMode && detectedDeviceRole == KIND_KEYBOARD) continue;

      uint8_t repId = 0;
      uint8_t repType = 1;
      pChar->getDescriptors(true);
      NimBLERemoteDescriptor* pRef = pChar->getDescriptor(reportRefUUID);
      if (pRef) {
        NimBLEAttValue refVal = pRef->readValue();
        if (refVal.size() >= 2) {
          repId   = refVal.data()[0];
          repType = refVal.data()[1];
        }
      }
      if (repType != 1) continue;

      ReportLayout matched;
      for (int i = 0; i < parsedCount; i++) {
        if (parsedLayouts[i].reportId == repId || (repId == 0 && i == 0)) {
          matched = parsedLayouts[i];
          break;
        }
      }
      if (matched.valid && matched.kind == KIND_VENDOR) continue;

      if (pChar->subscribe(true, notifyCallback)) {
        subCount++;
        if (subCharCount < MAX_SUB_CHARS) {
          subChars[subCharCount++] = { connH, pChar->getHandle(), detectedDeviceRole, (int8_t)bIdx, matched };
        }
      }
    }
  }

  if (subCount == 0) {
    for (auto* pChar : chars) {
      if (pChar->canNotify() && pChar->subscribe(true, notifyCallback)) {
        subCount++;
        if (subCharCount < MAX_SUB_CHARS) {
          subChars[subCharCount++] = { connH, pChar->getHandle(), detectedDeviceRole, (int8_t)bIdx, ReportLayout() };
        }
      }
    }
  }

  triggerLedEvent(LED_EV_CONNECTED);
  CONSOLE.printf("{\"ev\":\"connected\",\"mac\":\"%s\",\"subs\":%d}\n", mac.c_str(), subCount);
  sendStatusJson();
  return true;
}

String extractJsonStr(const String& json, const char* key) {
  String pat = String("\"") + key + "\":\"";
  int s = json.indexOf(pat);
  if (s < 0) return "";
  s += pat.length();
  int e = json.indexOf("\"", s);
  return (e > s) ? json.substring(s, e) : "";
}

int extractJsonInt(const String& json, const char* key, int defVal) {
  String pat = String("\"") + key + "\":";
  int s = json.indexOf(pat);
  if (s < 0) return defVal;
  s += pat.length();
  return json.substring(s).toInt();
}

void updateLedEngine() {
  uint32_t now = millis();

  // Каждые 150 мс проверяем реальный статус соединения всех привязанных устройств для мгновенной индикации отключения
  if (now - lastConnCheckMs >= 150) {
    lastConnCheckMs = now;
    for (int i = 0; i < savedCount; i++) {
      NimBLEAddress addr(savedBonds[i].mac.c_str(), savedBonds[i].addrType);
      NimBLEClient* pCli = NimBLEDevice::getClientByPeerAddress(addr);
      bool isConn = (pCli && pCli->isConnected());
      if (savedBonds[i].connected && !isConn) {
        savedBonds[i].connected = false;
        triggerLedEvent(LED_EV_DISCONNECTED);
        CONSOLE.printf("{\"ev\":\"disconnected\",\"mac\":\"%s\",\"name\":\"%s\"}\n",
                       savedBonds[i].mac.c_str(), savedBonds[i].name.c_str());
        sendStatusJson();
      }
    }
  }

  if (cfg.ledMode == 0) {
    setLedHardware(false, 0, 0, 0);
    return;
  }

  // 1) Приоритетные вспышки событий: Подключение (3 вспышки), Отключение (5 вспышек), Ввод пакета (подмигивание)
  if (currentLedEvent == LED_EV_CONNECTED) {
    uint32_t el = now - ledEventStartMs;
    if (el < 720) {
      bool blinkOn = ((el / 120) % 2) == 0;
      setLedHardware(blinkOn, 0, 90, 15); // Ярко-зелёный (3 вспышки)
      return;
    }
    currentLedEvent = LED_EV_NONE;
  } else if (currentLedEvent == LED_EV_DISCONNECTED) {
    uint32_t el = now - ledEventStartMs;
    if (el < 1000) {
      bool blinkOn = ((el / 100) % 2) == 0;
      setLedHardware(blinkOn, 95, 0, 0); // Ярко-красный (5 быстрых вспышек)
      return;
    }
    currentLedEvent = LED_EV_NONE;
  } else if (currentLedEvent == LED_EV_ACTIVITY) {
    uint32_t el = now - ledEventStartMs;
    if (el < 35) {
      setLedHardware(false, 0, 0, 0); // Короткая пауза-подмигивание на фоне свечения
      return;
    }
    currentLedEvent = LED_EV_NONE;
  }

  // 2) Состояние Поиска (BLE Scan): частое мигание 4 раза в секунду (125 мс ВКЛ / 125 мс ВЫКЛ, Синий)
  if (isScanning || NimBLEDevice::getScan()->isScanning()) {
    bool scanBlink = ((now / 125) % 2) == 0;
    setLedHardware(scanBlink, 0, 30, 85);
    return;
  }

  // 3) Проверяем количество активных подключений
  int activeCount = 0;
  for (int i = 0; i < savedCount; i++) {
    if (savedBonds[i].connected) activeCount++;
  }

  if (activeCount > 0) {
    // На связи: постоянное свечение (Зелёный)
    setLedHardware(true, 0, 35, 8);
  } else {
    // Нет девайсов (Ожидание): короткая одиночная вспышка раз в 1.6 секунды (160 мс ВКЛ / 1440 мс ВЫКЛ, Янтарный)
    bool idlePulse = (now % 1600) < 160;
    setLedHardware(idlePulse, 55, 22, 0);
  }
}

void startBleScan() {
  isScanning = true;
  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->clearResults();
  pScan->start(10000, false, true);
  sendStatusJson();
}

void handleSerialCommand(const String& line) {
  if (line.indexOf("\"cmd\":\"status\"") >= 0) {
    sendStatusJson();
  } else if (line.indexOf("\"cmd\":\"scan\"") >= 0) {
    startBleScan();
  } else if (line.indexOf("\"cmd\":\"connect\"") >= 0) {
    targetConnectMac = extractJsonStr(line, "mac");
    targetConnectType = extractJsonInt(line, "type", 1);
    targetConnectName = extractJsonStr(line, "name");
    if (targetConnectMac.length() > 0) pendingConnect = true;
  } else if (line.indexOf("\"cmd\":\"set_bond_cfg\"") >= 0) {
    String bMac = extractJsonStr(line, "mac");
    int newFmt  = extractJsonInt(line, "fmt", 0);
    int newScl  = extractJsonInt(line, "scale", 10);
    for (int i = 0; i < savedCount; i++) {
      if (savedBonds[i].mac.equalsIgnoreCase(bMac)) {
        savedBonds[i].fmt   = (uint8_t)newFmt;
        savedBonds[i].scale = (uint8_t)newScl;
        saveBondsToNVS();
        break;
      }
    }
    CONSOLE.println("{\"ev\":\"cfg_saved\"}");
    sendStatusJson();
  } else if (line.indexOf("\"cmd\":\"led_test\"") >= 0) {
    int ev = extractJsonInt(line, "ev", 1);
    triggerLedEvent((uint8_t)ev);
    CONSOLE.printf("{\"ev\":\"led_tested\",\"mode\":%d}\n", ev);
  } else if (line.indexOf("\"cmd\":\"forget\"") >= 0) {
    String fMac = extractJsonStr(line, "mac");
    for (int i = 0; i < savedCount; i++) {
      if (savedBonds[i].mac.equalsIgnoreCase(fMac)) {
        for (int j = i; j < savedCount - 1; j++) savedBonds[j] = savedBonds[j + 1];
        savedCount--;
        saveBondsToNVS();
        break;
      }
    }
    sendStatusJson();
  } else if (line.indexOf("\"cmd\":\"forget_all\"") >= 0) {
    NimBLEDevice::deleteAllBonds();
    savedCount = 0;
    saveBondsToNVS();
    sendStatusJson();
  } else if (line.indexOf("\"cmd\":\"set_config\"") >= 0) {
    cfg.mouseScale10  = extractJsonInt(line, "mScale", cfg.mouseScale10);
    cfg.scrollScale   = extractJsonInt(line, "sScale", cfg.scrollScale);
    cfg.swapMouseBtns = extractJsonInt(line, "swapBtn", 0) == 1;
    cfg.invertWheel   = extractJsonInt(line, "invWhl", 0) == 1;
    cfg.invertXY      = extractJsonInt(line, "invXY", 0) == 1;
    cfg.swapWinAlt    = extractJsonInt(line, "swapWA", 0) == 1;
    cfg.bootProtocol  = extractJsonInt(line, "bootP", 0) == 1;
    cfg.ledMode       = extractJsonInt(line, "ledM", cfg.ledMode);
    cfg.ledPin        = extractJsonInt(line, "ledP", cfg.ledPin);
    cfg.ledType       = extractJsonInt(line, "ledT", cfg.ledType);
    if (cfg.ledPin != WS2812_PIN && (cfg.ledType == 0 || cfg.ledType == 2)) {
      pinMode(cfg.ledPin, OUTPUT);
    }
    cfg.mouseFmt      = extractJsonInt(line, "mFmt", cfg.mouseFmt);
    cfg.kbFmt         = extractJsonInt(line, "kFmt", cfg.kbFmt);
    cfg.gpMode        = extractJsonInt(line, "gpMode", cfg.gpMode);
    cfg.gpFmt         = extractJsonInt(line, "gpFmt", cfg.gpFmt);
    cfg.gpDeadzone    = extractJsonInt(line, "gpDz", cfg.gpDeadzone);
    saveConfigToNVS();
    CONSOLE.println("{\"ev\":\"cfg_saved\"}");
  } else if (line.indexOf("\"cmd\":\"ota_begin\"") >= 0) {
    NimBLEDevice::getScan()->stop();
    isScanning = false;
    otaTotalSize = (size_t)extractJsonInt(line, "size", 0);
    if (otaTotalSize == 0) otaTotalSize = UPDATE_SIZE_UNKNOWN;
    if (Update.begin(otaTotalSize, U_FLASH)) {
      otaInProgress = true;
      otaWritten = 0;
      CONSOLE.println("{\"ev\":\"ota_ready\"}");
    } else {
      otaInProgress = false;
      CONSOLE.printf("{\"ev\":\"ota_err\",\"msg\":\"%s\"}\n", Update.errorString());
    }
  } else if (line.indexOf("\"cmd\":\"ota_chunk\"") >= 0) {
    if (!otaInProgress) return;
    String b64 = extractJsonStr(line, "d");
    static uint8_t binBuf[800];
    size_t decLen = decodeBase64(b64.c_str(), b64.length(), binBuf);
    if (decLen > 0) {
      size_t w = Update.write(binBuf, decLen);
      if (w != decLen) {
        otaInProgress = false;
        Update.abort();
        CONSOLE.printf("{\"ev\":\"ota_err\",\"msg\":\"%s\"}\n", Update.errorString());
        return;
      }
      otaWritten += w;
    }
    CONSOLE.printf("{\"ev\":\"ota_ack\",\"w\":%u}\n", (unsigned int)otaWritten);
  } else if (line.indexOf("\"cmd\":\"ota_end\"") >= 0) {
    if (!otaInProgress) return;
    otaInProgress = false;
    if (Update.end(true)) {
      CONSOLE.println("{\"ev\":\"ota_done\"}");
      delay(350);
      esp_restart();
    } else {
      CONSOLE.printf("{\"ev\":\"ota_err\",\"msg\":\"%s\"}\n", Update.errorString());
    }
  } else if (line.indexOf("\"cmd\":\"ota_rollback\"") >= 0) {
    if (Update.canRollBack() && Update.rollBack()) {
      CONSOLE.println("{\"ev\":\"ota_done\"}");
      delay(350);
      esp_restart();
    } else {
      CONSOLE.println("{\"ev\":\"ota_err\",\"msg\":\"Нет второго образа для отката в разделе OTA\"}");
    }
  } else if (line.indexOf("\"cmd\":\"reboot\"") >= 0) {
    CONSOLE.println("{\"ev\":\"rebooting\"}");
    delay(200);
    esp_restart();
  } else if (line.indexOf("\"cmd\":\"bootloader\"") >= 0) {
    CONSOLE.println("{\"ev\":\"bootloader\"}");
    delay(200);
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
  }
}

void setup() {
  CONSOLE.begin(115200);
  loadConfigFromNVS();

  if (cfg.ledPin != WS2812_PIN && (cfg.ledType == 0 || cfg.ledType == 2)) {
    pinMode(cfg.ledPin, OUTPUT);
    digitalWrite(cfg.ledPin, LOW);
  }
  writeRgbSafe(WS2812_PIN, 0, 0, 0);

  Keyboard.begin();
  Mouse.begin();
  ConsumerControl.begin();
#if BRIDGE_HAS_USB_GAMEPAD
  Gamepad.begin();
#endif
  USB.begin();

  // Короткий фиолетовый тест подтверждает работу WS2812 на GPIO21.
  writeRgbSafe(WS2812_PIN, 24, 0, 24);
  delay(80);
  writeRgbSafe(WS2812_PIN, 0, 0, 0);

  NimBLEDevice::init("ESP32-S3-HID-Bridge");
  NimBLEDevice::setSecurityAuth(true, false, true);

  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setScanCallbacks(new ScanCallbacks(), true);
  pScan->setActiveScan(true);
  pScan->setInterval(45);
  pScan->setWindow(30);

  startBleScan();
}

void loop() {
  while (CONSOLE.available()) {
    char c = (char)CONSOLE.read();
    if (c == '\n') {
      serialBuffer.trim();
      if (serialBuffer.length() > 0) handleSerialCommand(serialBuffer);
      serialBuffer = "";
    } else {
      serialBuffer += c;
    }
  }

  if (pendingConnect) {
    pendingConnect = false;
    connectToBleDevice(targetConnectMac, targetConnectType, targetConnectName);
  }

  // Не запускаем reconnect во время ввода: старый цикл мог блокировать HID
  // на несколько секунд при выключенном втором устройстве.
  if (cfg.autoReconnect && !NimBLEDevice::getScan()->isScanning() &&
      millis() - lastReconnectTry > 8000 && millis() - lastHidInputMs > 1500) {
    lastReconnectTry = millis();
    static uint8_t reconnectCursor = 0;
    for (int i = 0; i < savedCount; i++) {
      int idx = (reconnectCursor + i) % savedCount;
      NimBLEAddress addr(savedBonds[idx].mac.c_str(), savedBonds[idx].addrType);
      NimBLEClient* pCli = NimBLEDevice::getClientByPeerAddress(addr);
      bool isConn = (pCli && pCli->isConnected());
      savedBonds[idx].connected = isConn;
      if (!isConn) {
        reconnectCursor = (idx + 1) % savedCount;
        connectToBleDevice(savedBonds[idx].mac, savedBonds[idx].addrType, savedBonds[idx].name);
        break;
      }
    }
  }

  updateLedEngine();

  if (millis() - lastStatusPush > 2000) {
    lastStatusPush = millis();
    sendStatusJson();
  }
  delay(5);
}
