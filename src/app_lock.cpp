#include "app_lock.h"

#include <M5Cardputer.h>
#include "config.h"
#include "input.h"
#include "nav.h"
#include "app_settings.h"

namespace {

static constexpr int MAX_PIN_LEN = 8;

String   g_entered;
bool     g_unlocked   = false;
bool     g_wrongPin   = false;
uint32_t g_wrongUntil = 0;
bool     g_dirty      = true;

void drawLock() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.setFont(&fonts::Font0);

    // Title
    d.setTextSize(2);
    d.setTextColor(C_FG, C_BG);
    const char* t = "Cardputer OS";
    d.setCursor((SCREEN_W - (int)strlen(t) * FONT_W * 2) / 2, 8);
    d.print(t);
    d.setTextSize(1);

    // Padlock icon
    int cx = SCREEN_W / 2;
    int by = 36;
    d.drawArc(cx, by + 4, 12, 9, 200, 340, C_FG);   // shackle
    d.fillRoundRect(cx - 13, by + 4, 26, 20, 3, C_DIM);
    d.drawRoundRect(cx - 13, by + 4, 26, 20, 3, C_FG);
    d.fillCircle(cx, by + 13, 4, C_BG);               // keyhole circle
    d.fillRect(cx - 2, by + 14, 4, 6, C_BG);          // keyhole slot

    // PIN dots — show as many slots as the stored PIN length (min 4)
    const String& pin = settingsLockPin();
    int pinLen = max((int)pin.length(), 4);
    pinLen = min(pinLen, MAX_PIN_LEN);

    static constexpr int DOT_R   = 5;
    static constexpr int DOT_GAP = 14;
    int totalW = (pinLen - 1) * DOT_GAP + DOT_R * 2;
    int startX = (SCREEN_W - totalW) / 2 + DOT_R;
    int dotY   = 76;

    for (int i = 0; i < pinLen; i++) {
        int dx = startX + i * DOT_GAP;
        if (i < (int)g_entered.length()) {
            d.fillCircle(dx, dotY, DOT_R, C_FG);
        } else {
            d.drawCircle(dx, dotY, DOT_R, C_DIM);
        }
    }

    // Status message
    bool wrong = g_wrongPin && (millis() < g_wrongUntil);
    d.setTextColor(wrong ? (uint32_t)C_ERROR : (uint32_t)C_DIM, C_BG);
    const char* msg = wrong ? "Incorrect PIN" : "Enter PIN";
    d.setCursor((SCREEN_W - (int)strlen(msg) * FONT_W) / 2, 92);
    d.print(msg);

    // Footer
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 2);
    d.print("0-9=digit  Bksp=del  Enter=unlock");

    g_dirty = false;
}

void tryUnlock() {
    const String& pin = settingsLockPin();
    if (g_entered == pin) {
        g_unlocked = true;
    } else {
        g_wrongPin   = true;
        g_wrongUntil = millis() + 1500;
        g_dirty      = true;
    }
}

} // namespace

bool appLockIsUnlocked() { return g_unlocked; }

void appLockEnter() {
    g_entered    = "";
    g_unlocked   = false;
    g_wrongPin   = false;
    g_wrongUntil = 0;
    g_dirty      = true;
}

void appLockLoop() {
    if (g_wrongPin && millis() >= g_wrongUntil) {
        g_wrongPin = false;
        g_entered  = "";
        g_dirty    = true;
    }
    if (g_dirty) drawLock();

    auto ev = readKeys();
    if (!ev.changed) return;

    if (ev.del && g_entered.length() > 0) {
        g_entered.remove(g_entered.length() - 1);
        g_dirty = true;
        return;
    }

    if (ev.enter && g_entered.length() > 0) {
        tryUnlock();
        return;
    }

    if (!ev.fnKey) {
        for (char c : ev.chars) {
            if (c >= '0' && c <= '9' && (int)g_entered.length() < MAX_PIN_LEN) {
                g_entered += c;
                g_dirty = true;
                // Auto-confirm when entered length matches stored PIN
                const String& pin = settingsLockPin();
                if ((int)g_entered.length() == (int)pin.length()) {
                    tryUnlock();
                    return;
                }
            }
        }
    }
}
