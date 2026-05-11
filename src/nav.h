#pragma once
#include <M5Cardputer.h>
#include "config.h"

enum class AppScene { SSH, MP3, NOTES, SETTINGS, GAMES, FILES };

void goHome();
void launchApp(AppScene scene);

// ── Battery widget ──────────────────────────────────────────────────────────
// Draws a small battery icon + % centred in the status bar.
// Call after setting Font0 size 1. bg = status bar background colour.
// bx = left edge of battery icon. Default 97 centres icon+text on 240px screen.
// Pass SCREEN_W-43 (=197) to right-align within the status bar.
inline void drawBatteryWidget(uint32_t bg, int bx = 97) {
    auto& d   = M5Cardputer.Display;
    int   pct = M5Cardputer.Power.getBatteryLevel();   // 0-100, or -1 if unknown
    // isCharging() and getBatteryVoltage() return unreliable values on M5Cardputer
    // (StampS3 charge IC not accessible via M5Unified), so colour by level only.
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
