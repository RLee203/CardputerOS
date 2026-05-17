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
    "  [ SETTINGS / WIFI ]",
    "WiFi: Enter=saved networks",
    "fn+D=disconnect current",
    "In networks: Enter=connect",
    "fn+D=forget  +Scan=add new",
    "fn+,//    Brightness - / +",
    "fn+,//    Theme cycle",
};
static constexpr int NUM_HELP     = sizeof(HELP_LINES) / sizeof(HELP_LINES[0]);
static constexpr int HELP_VISIBLE = 12;

// ── State ─────────────────────────────────────────────────────────────────

enum class SettingsState { MENU, WIFI_NETS, WIFI_SCAN, WIFI_PASSWORD, HELP, LOCK_PIN };

static SettingsState settState  = SettingsState::MENU;
static bool          settDirty  = true;
static int           settSel    = 0;
static int           helpScroll = 0;

static constexpr int MENU_ITEMS   = 8;   // WiFi, Brightness, Theme, Help, Sleep, About, Lock, PIN
static constexpr int MENU_ITEM_H  = FONT_H + 4;  // 12px — fits 8 items in available height
static constexpr int LIST_VISIBLE = 11;

// Lock PIN entry state (used within settings)
static String lockPinBuf;
static String lockPinFirst;   // stores first entry during confirmation
static String lockPinMsg;
static int    lockPinPhase = 0;  // 0=enter new  1=confirm

static const char* MENU_FOOTER[] = {
    "Enter=nets  fn+D=disconn",
    "fn+, dim   fn+/ bright",
    "fn+, prev  fn+/ next theme",
    "Enter to open",
    "fn+, prev  fn+/ next timeout",
    "Cardputer OS v1.6",
    "Enter to toggle on/off",
    "Enter to set a new PIN",
};

// WiFi networks list state
static int netSel    = 0;
static int netScroll = 0;

// WiFi scan state
static constexpr int MAX_SCAN = 20;
static String        scanSSID[MAX_SCAN];
static int           scanCount  = 0;
static int           scanSel    = 0;
static int           scanScroll = 0;

// WiFi password state
static String wifiPassBuf;
static int    wifiPassCursor = 0;

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
                bool conn = (WiFi.status() == WL_CONNECTED);
                if (conn) snprintf(buf, sizeof(buf), "WiFi: %s", WiFi.SSID().c_str());
                else      snprintf(buf, sizeof(buf), "WiFi: %d saved", WifiMgr.netCount());
                d.print(buf);
                d.setTextColor(conn ? (uint32_t)0x00CC00 : (uint32_t)C_ERROR, bg);
                d.setCursor(SCREEN_W - 2 * FONT_W - 4, y + 2);
                d.print(conn ? "*" : "x");
                break;
            }
            case 1:
                snprintf(buf, sizeof(buf), "Brightness: %d", brightness);
                d.print(buf);
                break;
            case 2:
                snprintf(buf, sizeof(buf), "Theme: %s", THEMES[themeIdx].name);
                d.print(buf);
                break;
            case 3:
                d.print("Key Reference");
                break;
            case 4:
                snprintf(buf, sizeof(buf), "Sleep: %s", SLEEP_LABELS[sleepIdx]);
                d.print(buf);
                break;
            case 5:
                d.print("About: Cardputer OS v1.8");
                if (sel) {
                    d.setTextColor(C_DIM, bg);
                    d.setCursor(SCREEN_W / 2, y + 2);
                    d.print(WifiMgr.localIP().c_str());
                }
                break;
            case 6:
                snprintf(buf, sizeof(buf), "Lock Screen: %s", lockEnabled ? "ON " : "OFF");
                d.print(buf);
                break;
            case 7:
                snprintf(buf, sizeof(buf), "Lock PIN: %s", lockPin.length() ? "set" : "---");
                d.print(buf);
                break;
        }
    }

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 2);
    if (settSel < MENU_ITEMS) d.print(MENU_FOOTER[settSel]);
}

static void drawWifiNets() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("WiFi Networks");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    int total = WifiMgr.netCount() + 1;  // saved networks + "+ Scan for new"
    for (int i = 0; i < LIST_VISIBLE; i++) {
        int idx = i + netScroll;
        if (idx >= total) break;
        bool sel = (idx == netSel);
        int y = STATUS_H + i * (FONT_H + 2);
        uint32_t bg = sel ? C_HIGHLIGHT : (uint32_t)C_BG;
        d.fillRect(0, y, SCREEN_W, FONT_H + 2, bg);
        d.setTextColor(sel ? (uint32_t)C_INPUT : (uint32_t)C_FG, bg);
        d.setCursor(4, y + 1);

        if (idx < WifiMgr.netCount()) {
            d.print(WifiMgr.net(idx).ssid.c_str());
            // Mark currently connected network
            if (WiFi.status() == WL_CONNECTED &&
                WiFi.SSID() == WifiMgr.net(idx).ssid) {
                d.setTextColor((uint32_t)0x00CC00, bg);
                d.setCursor(SCREEN_W - 2 * FONT_W - 4, y + 1);
                d.print("*");
            }
        } else {
            d.setTextColor(sel ? (uint32_t)C_INPUT : (uint32_t)C_DIM, bg);
            d.print("+ Scan for new network");
        }
    }

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 2);
    if (netSel < WifiMgr.netCount())
        d.print("Enter=conn  fn+D=forget  bksp=back");
    else
        d.print("Enter=scan  bksp=back");
}

static void drawWifiScan() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("WiFi Scan");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    if (scanCount == 0) {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(4, STATUS_H + 10);
        d.print("No networks found.");
    } else {
        for (int i = 0; i < LIST_VISIBLE; i++) {
            int idx = i + scanScroll;
            if (idx >= scanCount) break;
            bool sel = (idx == scanSel);
            int y = STATUS_H + i * (FONT_H + 2);
            uint32_t bg = sel ? C_HIGHLIGHT : (uint32_t)C_BG;
            d.fillRect(0, y, SCREEN_W, FONT_H + 2, bg);
            d.setTextColor(sel ? (uint32_t)C_INPUT : (uint32_t)C_FG, bg);
            d.setCursor(4, y + 1);
            d.print(scanSSID[idx].c_str());
        }
    }
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 2);
    d.print("fn+;/.=nav  Enter=sel  bksp=back");
}

static void drawWifiPassword() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("WiFi Setup");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    d.setTextColor(C_FG, C_BG);
    char buf[64];
    snprintf(buf, sizeof(buf), "SSID: %.32s", scanSSID[scanSel].c_str());
    d.setCursor(4, STATUS_H + 6);
    d.print(buf);

    constexpr int passX = 4 + 6 * FONT_W;   // x after "Pass: "
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(4, STATUS_H + 22);
    d.print("Pass: ");
    d.setTextColor(C_FG, C_BG);
    for (char ch : wifiPassBuf) d.print('*');
    d.fillRect(passX + wifiPassCursor * FONT_W, STATUS_H + 22, FONT_W, FONT_H, C_INPUT);

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(4, STATUS_H + 40);
    d.print("Enter=connect  bksp=back");
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

// ── Scan helper (shared by menu WiFi Enter and WIFI_NETS scan option) ─────

static void doScan() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("WiFi Scan");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_FG, C_BG);
    d.setCursor(4, STATUS_H + 20);
    d.print("Scanning...");
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks();
    scanCount = 0;
    if (n > 0) {
        scanCount = (n > MAX_SCAN) ? MAX_SCAN : n;
        for (int i = 0; i < scanCount; i++) scanSSID[i] = WiFi.SSID(i);
        WiFi.scanDelete();
    }
    scanSel    = 0;
    scanScroll = 0;
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
            if ((c == 'd' || c == 'D') && settSel == 0) {
                WifiMgr.disconnect();
                settDirty = true;
                return;
            }
        }
    }

    // Brightness: fn+, = dim, fn+/ = bright
    if (settSel == 1) {
        if (ev.left)  { brightness = max(10,  brightness - 10); M5Cardputer.Display.setBrightness(brightness); saveSettings(); settDirty = true; }
        if (ev.right) { brightness = min(255, brightness + 10); M5Cardputer.Display.setBrightness(brightness); saveSettings(); settDirty = true; }
    }

    // Text Color: fn+, / fn+/ cycles themes
    if (settSel == 2) {
        if (ev.left)  { themeIdx = (themeIdx + NUM_THEMES - 1) % NUM_THEMES; applyTheme(themeIdx); saveSettings(); settDirty = true; }
        if (ev.right) { themeIdx = (themeIdx + 1) % NUM_THEMES;              applyTheme(themeIdx); saveSettings(); settDirty = true; }
    }

    // Screen Sleep: fn+, / fn+/ cycles timeout options
    if (settSel == 4) {
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
        if (settSel == 0) {
            netSel    = 0;
            netScroll = 0;
            settState = SettingsState::WIFI_NETS;
            settDirty = true;
        } else if (settSel == 3) {
            helpScroll = 0;
            settState  = SettingsState::HELP;
            settDirty  = true;
        } else if (settSel == 6) {
            lockEnabled = !lockEnabled;
            saveSettings();
            settDirty = true;
        } else if (settSel == 7) {
            lockPinBuf   = "";
            lockPinFirst = "";
            lockPinMsg   = "";
            lockPinPhase = 0;
            settState    = SettingsState::LOCK_PIN;
            settDirty    = true;
        }
    }
}

static void handleWifiNets() {
    if (settDirty) { drawWifiNets(); settDirty = false; }

    auto ev = readKeys();
    if (!ev.changed) return;

    if (ev.back) { settState = SettingsState::MENU; settDirty = true; return; }

    int total = WifiMgr.netCount() + 1;

    // fn+D on a saved network = forget it
    if (ev.fnKey) {
        for (char c : ev.chars) {
            if ((c == 'd' || c == 'D') && netSel < WifiMgr.netCount()) {
                WifiMgr.removeNet(netSel);
                int newTotal = WifiMgr.netCount() + 1;
                if (netSel >= newTotal) netSel = newTotal - 1;
                if (netSel < netScroll) netScroll = netSel;
                drawWifiNets();
                return;
            }
        }
    }

    bool nav = false;
    if (ev.up && netSel > 0) {
        netSel--;
        if (netSel < netScroll) netScroll = netSel;
        nav = true;
    }
    if (ev.down && netSel < total - 1) {
        netSel++;
        if (netSel >= netScroll + LIST_VISIBLE) netScroll = netSel - LIST_VISIBLE + 1;
        nav = true;
    }
    if (nav) { drawWifiNets(); return; }

    if (ev.enter) {
        if (netSel < WifiMgr.netCount()) {
            // Connect to saved network (no password needed)
            auto& d = M5Cardputer.Display;
            d.fillScreen(C_BG);
            drawStatusBar("Connecting...");
            d.setFont(&fonts::Font0);
            d.setTextSize(1);
            d.setTextColor(C_FG, C_BG);
            char buf[64];
            snprintf(buf, sizeof(buf), "Connecting to %s", WifiMgr.net(netSel).ssid.c_str());
            d.setCursor(4, STATUS_H + 20);
            d.print(buf);

            WifiState r = WifiMgr.connect(netSel);
            d.setCursor(4, STATUS_H + 36);
            if (r == WifiState::CONNECTED) {
                d.setTextColor((uint32_t)0x00CC00, C_BG);
                snprintf(buf, sizeof(buf), "Connected! %s", WifiMgr.localIP().c_str());
            } else {
                d.setTextColor(C_ERROR, C_BG);
                snprintf(buf, sizeof(buf), "Failed to connect.");
            }
            d.print(buf);
            delay(2000);
            settDirty = true;
        } else {
            // "+ Scan for new network"
            doScan();
            settState = SettingsState::WIFI_SCAN;
            settDirty = true;
        }
    }
}

static void handleWifiScan() {
    if (settDirty) { drawWifiScan(); settDirty = false; }

    auto ev = readKeys();
    if (!ev.changed) return;

    if (ev.back) { settState = SettingsState::WIFI_NETS; settDirty = true; return; }
    if (ev.enter && scanCount > 0) {
        wifiPassBuf    = "";
        wifiPassCursor = 0;
        settState      = SettingsState::WIFI_PASSWORD;
        settDirty      = true;
        return;
    }

    bool nav = false;
    if (ev.up && scanSel > 0) {
        scanSel--;
        if (scanSel < scanScroll) scanScroll = scanSel;
        nav = true;
    }
    if (ev.down && scanSel < scanCount - 1) {
        scanSel++;
        if (scanSel >= scanScroll + LIST_VISIBLE) scanScroll = scanSel - LIST_VISIBLE + 1;
        nav = true;
    }
    if (nav) drawWifiScan();
}

static void handleWifiPassword() {
    if (settDirty) { drawWifiPassword(); settDirty = false; }

    auto ev = readKeys();
    if (!ev.changed) return;

    if (ev.back) { settState = SettingsState::WIFI_SCAN; settDirty = true; return; }

    if (ev.del && wifiPassCursor > 0) {
        wifiPassBuf.remove(wifiPassCursor - 1, 1);
        wifiPassCursor--;
        drawWifiPassword();
        return;
    }

    if (!ev.fnKey) {
        for (char c : ev.chars) {
            wifiPassBuf = wifiPassBuf.substring(0, wifiPassCursor) + c + wifiPassBuf.substring(wifiPassCursor);
            wifiPassCursor++;
        }
        if (ev.chars.length() > 0) { drawWifiPassword(); return; }
    }

    if (ev.enter) {
        auto& d = M5Cardputer.Display;
        d.fillScreen(C_BG);
        drawStatusBar("Connecting...");
        d.setFont(&fonts::Font0);
        d.setTextSize(1);
        d.setTextColor(C_FG, C_BG);
        char buf[64];
        snprintf(buf, sizeof(buf), "Connecting to %s", scanSSID[scanSel].c_str());
        d.setCursor(4, STATUS_H + 20);
        d.print(buf);

        WifiState r = WifiMgr.connect(scanSSID[scanSel], wifiPassBuf);

        d.setCursor(4, STATUS_H + 36);
        if (r == WifiState::CONNECTED) {
            d.setTextColor((uint32_t)0x00CC00, C_BG);
            WifiMgr.addNet(scanSSID[scanSel], wifiPassBuf);  // save on success only
            snprintf(buf, sizeof(buf), "Saved! IP: %s", WifiMgr.localIP().c_str());
        } else {
            d.setTextColor(C_ERROR, C_BG);
            snprintf(buf, sizeof(buf), "Failed to connect.");
        }
        d.print(buf);
        delay(2000);
        settState = SettingsState::WIFI_NETS;
        settDirty = true;
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
        case SettingsState::WIFI_NETS:     handleWifiNets();      break;
        case SettingsState::WIFI_SCAN:     handleWifiScan();      break;
        case SettingsState::WIFI_PASSWORD: handleWifiPassword();  break;
        case SettingsState::HELP:          handleHelp();          break;
        case SettingsState::LOCK_PIN:      handleLockPin();       break;
    }
}

bool settingsLockEnabled() { return lockEnabled; }
const String& settingsLockPin() { return lockPin; }
