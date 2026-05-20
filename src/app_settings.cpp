#include "app_settings.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include "wifi_mgr.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

static constexpr const char* SETTINGS_FILE = "/settings.json";

// ── Themes ────────────────────────────────────────────────────────────────

struct Theme {
    const char* name;
    uint32_t fg, dim, statusFg, statusBg, highlight, cursor;
};

static const Theme THEMES[] = {
    { "Green",  0x00EE00, 0x006600, 0x00EE00, 0x002200, 0x003300, 0x00EE00 },
    { "White",  0xCCCCCC, 0x666666, 0xCCCCCC, 0x1A1A1A, 0x333333, 0xCCCCCC },
    { "Cyan",   0x00CCCC, 0x005555, 0x00CCCC, 0x001111, 0x002222, 0x00CCCC },
    { "Amber",  0xFFAA00, 0x664400, 0xFFAA00, 0x221A00, 0x443300, 0xFFAA00 },
};
static constexpr int NUM_THEMES = 4;

static int themeIdx   = 0;
static int brightness = 128;

static const uint32_t SLEEP_OPTIONS[] = { 0, 10000, 20000, 30000, 60000 };
static const char*    SLEEP_LABELS[]  = { "Off", "10s", "20s", "30s", "60s" };
static constexpr int  NUM_SLEEP = 5;
static int    sleepIdx    = 0;
static bool   lockEnabled = false;
static String lockPin     = "1234";
static DeviceMode bootMode = DeviceMode::RADIO;

static void applyTheme(int idx) {
    const Theme& t = THEMES[idx];
    C_FG        = t.fg;
    C_DIM       = t.dim;
    C_STATUS_FG = t.statusFg;
    C_STATUS_BG = t.statusBg;
    C_HIGHLIGHT = t.highlight;
    C_CURSOR    = t.cursor;
}

// ── Persist ───────────────────────────────────────────────────────────────

static void saveSettings() {
    File f = LittleFS.open(SETTINGS_FILE, FILE_WRITE);
    if (!f) return;
    JsonDocument doc;
    doc["brightness"]   = brightness;
    doc["theme"]        = themeIdx;
    doc["sleep"]        = sleepIdx;
    doc["lockEnabled"]  = lockEnabled;
    doc["lockPin"]      = lockPin;
    doc["bootMode"]     = (bootMode == DeviceMode::SD) ? "sd" : "radio";
    serializeJson(doc, f);
    f.close();
}

void settingsLoadFromFS() {
    LittleFS.begin(false);
    File f = LittleFS.open(SETTINGS_FILE, FILE_READ);
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f) == DeserializationError::Ok) {
        brightness  = doc["brightness"]  | 128;
        themeIdx    = doc["theme"]       | 0;
        sleepIdx    = doc["sleep"]       | 0;
        lockEnabled = doc["lockEnabled"] | false;
        lockPin     = doc["lockPin"]     | String("1234");
        String mode = doc["bootMode"]    | String("radio");
        bootMode = (mode == "sd" || mode == "SD") ? DeviceMode::SD : DeviceMode::RADIO;
        if (themeIdx < 0 || themeIdx >= NUM_THEMES) themeIdx = 0;
        if (sleepIdx < 0 || sleepIdx >= NUM_SLEEP)  sleepIdx = 0;
        if (lockPin.length() == 0) lockPin = "1234";
        M5Cardputer.Display.setBrightness(brightness);
        applyTheme(themeIdx);
        sleepTimeoutMs = SLEEP_OPTIONS[sleepIdx];
    }
    f.close();
}

// ── Help lines ────────────────────────────────────────────────────────────

static const char* HELP_LINES[] = {
    "  [ GLOBAL ]",
    "fn+bksp   Back / Home",
    "fn+Q      Home (backup)",
    "  [ NAVIGATION ]",
    "fn+;  Up      fn+.  Down",
    "fn+,  Left    fn+/  Right",
    "  [ SSH TERMINAL ]",
    "fn+C/D/Z  Ctrl+C / D / Z",
    "Tab       Tab complete",
    "fn+;/.    History up / dn",
    "fn+,//    Cursor left / right",
    "  [ GAME BOY ]",
    "WASD      D-Pad",
    "\\ / spc   A / B buttons",
    "Enter     Start",
    "Tab       Select",
    "  [ MP3 PLAYER ]",
    "Enter     Play / Pause",
    "fn+; / .  Prev / Next song",
    "+ / -     Volume up / down",
    "  [ MODES ]",
    "Multimedia: MP3 Files Photos",
    "Radio: WiFi BLE GPS LoRa",
    "Use WiFi app to connect",
    "fn+,//    Brightness - / +",
    "fn+,//    Theme cycle",
};
static constexpr int NUM_HELP     = sizeof(HELP_LINES) / sizeof(HELP_LINES[0]);
static constexpr int HELP_VISIBLE = 12;

// ── State ─────────────────────────────────────────────────────────────────

enum class SettingsState { MENU, HELP, LOCK_PIN };

static SettingsState settState  = SettingsState::MENU;
static bool          settDirty  = true;
static int           settSel    = 0;
static int           helpScroll = 0;

static constexpr int MENU_ITEMS   = 8;   // Brightness, Theme, Help, Sleep, About, Lock, PIN, Mode
static constexpr int MENU_ITEM_H  = FONT_H + 4;  // 12px — fits 8 items in available height
static constexpr int LIST_VISIBLE = 11;

// Lock PIN entry state (used within settings)
static String lockPinBuf;
static String lockPinFirst;   // stores first entry during confirmation
static String lockPinMsg;
static int    lockPinPhase = 0;  // 0=enter new  1=confirm

static const char* MENU_FOOTER[] = {
    "fn+, dim   fn+/ bright",
    "fn+, prev  fn+/ next theme",
    "Enter to open",
    "fn+, prev  fn+/ next timeout",
    "Cardputer OS v2.2",
    "Enter to toggle on/off",
    "Enter to set a new PIN",
    "Enter to switch and restart",
};

// ── Draw ──────────────────────────────────────────────────────────────────

static void drawStatusBar(const char* title) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_STATUS_FG, C_STATUS_BG);
    d.setCursor(2, 3);
    d.print(title);
    drawBatteryWidget(C_STATUS_BG);
}

static void drawMenu() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("Settings");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    for (int i = 0; i < MENU_ITEMS; i++) {
        bool sel = (i == settSel);
        int  y   = STATUS_H + 2 + i * MENU_ITEM_H;
        uint32_t bg = sel ? C_HIGHLIGHT : (uint32_t)C_BG;
        d.fillRect(0, y, SCREEN_W, MENU_ITEM_H, bg);
        d.setTextColor(sel ? (uint32_t)C_INPUT : (uint32_t)C_FG, bg);
        d.setCursor(4, y + 2);

        char buf[48];
        switch (i) {
            case 0: {
                snprintf(buf, sizeof(buf), "Brightness: %d", brightness);
                d.print(buf);
                break;
            }
            case 1:
                snprintf(buf, sizeof(buf), "Theme: %s", THEMES[themeIdx].name);
                d.print(buf);
                break;
            case 2:
                d.print("Key Reference");
                break;
            case 3:
                snprintf(buf, sizeof(buf), "Sleep: %s", SLEEP_LABELS[sleepIdx]);
                d.print(buf);
                break;
            case 4:
                d.print("About: Cardputer OS v2.2");
                if (sel) {
                    d.setTextColor(C_DIM, bg);
                    d.setCursor(SCREEN_W / 2, y + 2);
                    d.print(WifiMgr.localIP().c_str());
                }
                break;
            case 5:
                snprintf(buf, sizeof(buf), "Lock Screen: %s", lockEnabled ? "ON " : "OFF");
                d.print(buf);
                break;
            case 6:
                snprintf(buf, sizeof(buf), "Lock PIN: %s", lockPin.length() ? "set" : "---");
                d.print(buf);
                break;
            case 7:
                snprintf(buf, sizeof(buf), "Mode: %s", isSdMode() ? "Multimedia" : "Radio");
                d.print(buf);
                break;
        }
    }

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 2);
    if (settSel < MENU_ITEMS) d.print(MENU_FOOTER[settSel]);
}

static void drawHelp() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("Key Reference");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    for (int i = 0; i < HELP_VISIBLE; i++) {
        int li = i + helpScroll;
        if (li >= NUM_HELP) break;
        int  y        = STATUS_H + 2 + i * (FONT_H + 1);
        bool isHeader = (HELP_LINES[li][0] == ' ' && HELP_LINES[li][1] == ' ');
        d.setTextColor(isHeader ? (uint32_t)C_DIM : (uint32_t)C_FG, C_BG);
        d.setCursor(2, y);
        d.print(HELP_LINES[li]);
    }
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 2);
    d.print("fn+;/.=scroll   fn+bksp=back");
}

// ── Handlers ──────────────────────────────────────────────────────────────

static void handleMenu() {
    if (settDirty) { drawMenu(); settDirty = false; }
    auto ev = readKeys();
    if (!ev.changed) return;

    if (ev.back) { goHome(); return; }
    if (ev.up   && settSel > 0)              { settSel--; settDirty = true; }
    if (ev.down && settSel < MENU_ITEMS - 1) { settSel++; settDirty = true; }

    if (ev.fnKey) {
        for (char c : ev.chars) {
            if (c == 'q' || c == 'Q') { goHome(); return; }
        }
    }

    // Brightness: fn+, = dim, fn+/ = bright
    if (settSel == 0) {
        if (ev.left)  { brightness = max(10,  brightness - 10); M5Cardputer.Display.setBrightness(brightness); saveSettings(); settDirty = true; }
        if (ev.right) { brightness = min(255, brightness + 10); M5Cardputer.Display.setBrightness(brightness); saveSettings(); settDirty = true; }
    }

    // Text Color: fn+, / fn+/ cycles themes
    if (settSel == 1) {
        if (ev.left)  { themeIdx = (themeIdx + NUM_THEMES - 1) % NUM_THEMES; applyTheme(themeIdx); saveSettings(); settDirty = true; }
        if (ev.right) { themeIdx = (themeIdx + 1) % NUM_THEMES;              applyTheme(themeIdx); saveSettings(); settDirty = true; }
    }

    // Screen Sleep: fn+, / fn+/ cycles timeout options
    if (settSel == 3) {
        if (ev.left) {
            sleepIdx = (sleepIdx + NUM_SLEEP - 1) % NUM_SLEEP;
            sleepTimeoutMs = SLEEP_OPTIONS[sleepIdx];
            resetSleepTimer();
            saveSettings();
            settDirty = true;
        }
        if (ev.right) {
            sleepIdx = (sleepIdx + 1) % NUM_SLEEP;
            sleepTimeoutMs = SLEEP_OPTIONS[sleepIdx];
            resetSleepTimer();
            saveSettings();
            settDirty = true;
        }
    }

    if (ev.enter) {
        if (settSel == 2) {
            helpScroll = 0;
            settState  = SettingsState::HELP;
            settDirty  = true;
        } else if (settSel == 5) {
            lockEnabled = !lockEnabled;
            saveSettings();
            settDirty = true;
        } else if (settSel == 6) {
            lockPinBuf   = "";
            lockPinFirst = "";
            lockPinMsg   = "";
            lockPinPhase = 0;
            settState    = SettingsState::LOCK_PIN;
            settDirty    = true;
        } else if (settSel == 7) {
            requestModeSwitch(isSdMode() ? DeviceMode::RADIO : DeviceMode::SD, "Switch Mode");
        }
    }
}

static void handleHelp() {
    if (settDirty) { drawHelp(); settDirty = false; }
    auto ev = readKeys();
    if (!ev.changed) return;
    if (ev.back) { settState = SettingsState::MENU; settDirty = true; return; }
    if (ev.up   && helpScroll > 0)                          { helpScroll--; settDirty = true; }
    if (ev.down && helpScroll < NUM_HELP - HELP_VISIBLE + 1){ helpScroll++; settDirty = true; }
}

static void drawLockPin() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar(lockPinPhase == 0 ? "Set Lock PIN" : "Confirm PIN");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    int y = STATUS_H + 8;

    d.setTextColor(C_FG, C_BG);
    d.setCursor(4, y);
    d.print(lockPinPhase == 0 ? "Enter new PIN (digits):" : "Re-enter PIN to confirm:");
    y += 14;

    // Input box (shows asterisks)
    d.fillRect(4, y, SCREEN_W - 8, 12, C_HIGHLIGHT);
    d.setTextColor(C_INPUT, C_HIGHLIGHT);
    String dots;
    for (size_t i = 0; i < lockPinBuf.length(); i++) dots += '*';
    d.setCursor(6, y + 2);
    d.print(dots.c_str());
    int cx = 6 + lockPinBuf.length() * FONT_W;
    if (cx < SCREEN_W - 8) d.fillRect(cx, y + 2, FONT_W, FONT_H - 1, C_CURSOR);
    y += 16;

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(4, y);
    d.print(lockPinPhase == 0 ? "4-8 digits recommended" : "Must match first entry");
    y += 14;

    if (lockPinMsg.length()) {
        bool ok = lockPinMsg.startsWith("OK");
        d.setTextColor(ok ? (uint32_t)0x33CC66 : (uint32_t)C_ERROR, C_BG);
        d.setCursor(4, y); d.print(lockPinMsg.c_str());
    }

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 2);
    d.print("0-9=digit  Bksp=del  Enter=next");
    settDirty = false;
}

static void handleLockPin() {
    if (settDirty) drawLockPin();
    auto ev = readKeys();
    if (!ev.changed) return;

    if (ev.back) {
        if (lockPinPhase == 1) {
            // Go back to first entry step instead of leaving
            lockPinPhase = 0;
            lockPinBuf   = "";
            lockPinMsg   = "";
            settDirty    = true;
        } else {
            settState = SettingsState::MENU;
            settDirty = true;
        }
        return;
    }

    if (ev.del && lockPinBuf.length() > 0) {
        lockPinBuf.remove(lockPinBuf.length() - 1);
        lockPinMsg = "";
        settDirty  = true;
        return;
    }

    if (ev.enter) {
        if (lockPinBuf.length() < 1) {
            lockPinMsg = "ERR: Enter at least 1 digit";
            settDirty  = true;
        } else if (lockPinPhase == 0) {
            // First entry done — move to confirmation
            lockPinFirst = lockPinBuf;
            lockPinBuf   = "";
            lockPinMsg   = "";
            lockPinPhase = 1;
            settDirty    = true;
        } else {
            // Confirmation step
            if (lockPinBuf == lockPinFirst) {
                lockPin  = lockPinBuf;
                saveSettings();
                lockPinMsg = "OK! PIN saved";
                settDirty  = true;
                // Return to menu after short display
                settState    = SettingsState::MENU;
                lockPinPhase = 0;
            } else {
                lockPinMsg   = "ERR: PINs don't match";
                lockPinPhase = 0;
                lockPinBuf   = "";
                lockPinFirst = "";
                settDirty    = true;
            }
        }
        return;
    }

    if (!ev.fnKey) {
        for (char c : ev.chars) {
            if (c >= '0' && c <= '9' && (int)lockPinBuf.length() < 8) {
                lockPinBuf += c;
                lockPinMsg  = "";
                settDirty   = true;
            }
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────

void appSettingsEnter() {
    settState  = SettingsState::MENU;
    settSel    = 0;
    settDirty  = true;
    brightness = M5Cardputer.Display.getBrightness();
}

void appSettingsLoop() {
    switch (settState) {
        case SettingsState::MENU:          handleMenu();          break;
        case SettingsState::HELP:          handleHelp();          break;
        case SettingsState::LOCK_PIN:      handleLockPin();       break;
    }
}

bool settingsLockEnabled() { return lockEnabled; }
const String& settingsLockPin() { return lockPin; }
DeviceMode settingsBootMode() { return bootMode; }
void settingsSetBootMode(DeviceMode mode) {
    bootMode = mode;
    saveSettings();
}
