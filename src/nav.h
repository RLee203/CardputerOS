#pragma once
#include <M5Cardputer.h>
#include "config.h"
#ifdef BOARD_TEMBED
#  include <Wire.h>
#endif

#ifndef BOARD_TEMBED
#  include <M5Unified.h>
#  include <utility/power/IP5306_Class.hpp>
   inline m5::IP5306_Class g_cardputerBatteryPmic{};
   inline bool             g_cardputerBatteryPmicReady = false;
#endif

enum class AppScene {
    SSH, MP3, NOTES, SETTINGS, GAMES, FILES, IR_REMOTE, PHOTOS,
    VOICE_MEMOS, HID_KEYBOARD, USB_STORAGE, TIMER, GPS, LORA, NFC,
    PAYLOADS, BLE, DETECTOR, WIFI_TOOLS, CC1101, NRF24, ESPNOW,
    SD_HEALTH, CALC, FIRMWARE
};
enum class DeviceMode : uint8_t { SD = 0, RADIO = 1 };

void goHome();
void launchApp(AppScene scene);
void suspendWifiForSd();
void resumeWifiAfterSd();
DeviceMode currentDeviceMode();
void setCurrentDeviceMode(DeviceMode mode);
void requestModeSwitch(DeviceMode targetMode, const char* feature);
bool isSdMode();
bool isRadioMode();
bool requiresSdMode(AppScene scene);
bool requiresRadioMode(AppScene scene);

inline void initBatteryMonitoring() {
#ifndef BOARD_TEMBED
    g_cardputerBatteryPmicReady = g_cardputerBatteryPmic.begin();
#endif
}

inline int getBatteryPercent() {
#ifdef BOARD_TEMBED
    // BQ27220 fuel gauge at I2C 0x55, command 0x1C = StateOfCharge (uint16 LE, in %)
    Wire.beginTransmission(0x55);
    Wire.write(0x1C);
    if (Wire.endTransmission(false) != 0) return -1;
    if (Wire.requestFrom((uint8_t)0x55, (uint8_t)2) != 2) return -1;
    uint16_t soc = (uint16_t)Wire.read() | ((uint16_t)Wire.read() << 8);
    return (int)constrain((int)soc, 0, 100);
#else
    if (g_cardputerBatteryPmicReady) {
        int pct = g_cardputerBatteryPmic.getBatteryLevel();
        if (pct >= 0) return pct;
    }
    return M5Cardputer.Power.getBatteryLevel();
#endif
}

// ── Battery widget ──────────────────────────────────────────────────────────
// Draws a small battery icon + % centred in the status bar.
// Default bx centres the widget on whatever SCREEN_W this build uses.
inline void drawBatteryWidget(uint32_t bg, int bx = (SCREEN_W - 43) / 2) {
    auto& d   = M5Cardputer.Display;
    int   pct = getBatteryPercent();
    uint32_t col = pct > 50 ? (uint32_t)0x00CC00
                 : pct > 20 ? (uint32_t)0xCCAA00
                            : (uint32_t)0xFF3333;
    d.drawRect(bx, 3, 14, 8, col);
    d.fillRect(bx + 14, 5, 2, 4, col);
    int fw = (pct >= 0) ? (pct * 12) / 100 : 0;
    if (fw > 0) d.fillRect(bx + 1, 4, fw, 6, col);
    char buf[5];
    snprintf(buf, sizeof(buf), pct >= 0 ? "%d%%" : "---", pct);
    d.setTextColor(col, bg);
    d.setCursor(bx + 17, 3);
    d.print(buf);
}
