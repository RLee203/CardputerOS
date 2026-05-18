#include "launcher.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>
#include <WiFi.h>

// ── Layout (3 cols × 2 rows) ───────────────────────────────────────────────
//  Status bar y=0..12 (STATUS_H=13)
//  Col spacing: 1px left + 78px + 2px gap + 78px + 2px gap + 78px + 1px right = 240
//  Row spacing: 2px top + 57px row0 + 2px gap + 59px row1 + 2px bottom = 122 (+ 13 status = 135)
static constexpr int NCOLS  = 3;
static constexpr int NROWS  = 2;
static constexpr int CELL_X[3] = { 1,  81, 161 };
static constexpr int CELL_Y[2] = { 15,  74 };
static constexpr int CELL_W    = 78;
static constexpr int CELL_H[2] = { 57,  59 };
static constexpr uint32_t LBKG = 0x141414;
static constexpr int APPS_PER_PAGE = NCOLS * NROWS;

struct AppEntry {
    const char* label;
    char hotkey;
    AppScene scene;
    uint32_t color;
};

static const AppEntry APPS[] = {
    { "SSH", 's', AppScene::SSH, 0x1155BB },
    { "MP3", 'm', AppScene::MP3, 0xAA4400 },
    { "Notes", 'n', AppScene::NOTES, 0x116633 },
    { "Games", 'g', AppScene::GAMES, 0xCC2200 },
    { "Settings", 'p', AppScene::SETTINGS, 0x5522AA },
    { "Files", 'f', AppScene::FILES, 0x226677 },
    { "IR Remote", 'r', AppScene::IR_REMOTE, 0x7A002E },
    { "Photos", 'h', AppScene::PHOTOS, 0x3A3A8C },
    { "VoiceMemo", 'v', AppScene::VOICE_MEMOS, 0x6A005A },
    { "HID Keybd", 'b', AppScene::HID_KEYBOARD, 0x0050A8 },
    { "USB Store", 'u', AppScene::USB_STORAGE, 0x7A5A00 },
    { "Timer", 't', AppScene::TIMER, 0x006C4D },
    { "GPS", 'x', AppScene::GPS, 0x005E7A },
    { "LoRa", 'l', AppScene::LORA, 0x7A3E00 },
    { "NFC", 'c', AppScene::NFC, 0x006644 },
    { "Payloads", 'y', AppScene::PAYLOADS, 0x8B0013 },
    { "BLE", 'e', AppScene::BLE, 0x003566 },
    { "Detector", 'd', AppScene::DETECTOR, 0x005533 },
    { "WiFi", 'w', AppScene::WIFI_TOOLS, 0x003377 },
};
static constexpr int APP_COUNT = (int)(sizeof(APPS) / sizeof(APPS[0]));
static const int SD_APP_IDS[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 15 };
static const int RADIO_APP_IDS[] = { 0, 4, 12, 13, 14, 16, 17, 18 };
static constexpr int SD_APP_COUNT = (int)(sizeof(SD_APP_IDS) / sizeof(SD_APP_IDS[0]));
static constexpr int RADIO_APP_COUNT = (int)(sizeof(RADIO_APP_IDS) / sizeof(RADIO_APP_IDS[0]));

static int selRow = 0, selCol = 0, selPage = 0;

static void drawAll();

static const int* activeAppIds() {
    return isSdMode() ? SD_APP_IDS : RADIO_APP_IDS;
}

static int activeAppCount() {
    return isSdMode() ? SD_APP_COUNT : RADIO_APP_COUNT;
}

static const AppEntry* appForVisibleIndex(int idx) {
    int count = activeAppCount();
    if (idx < 0 || idx >= count) return nullptr;
    return &APPS[activeAppIds()[idx]];
}

static int pageCount() {
    return (activeAppCount() + APPS_PER_PAGE - 1) / APPS_PER_PAGE;
}

static int currentIndex(int row, int col) {
    return selPage * APPS_PER_PAGE + row * NCOLS + col;
}

static int currentIndex() {
    return currentIndex(selRow, selCol);
}

static bool pageHasCell(int page, int row, int col) {
    int idx = page * APPS_PER_PAGE + row * NCOLS + col;
    return idx < activeAppCount();
}

static void clampSelectionForPage() {
    if (pageHasCell(selPage, selRow, selCol)) return;
    for (int r = NROWS - 1; r >= 0; --r) {
        for (int c = NCOLS - 1; c >= 0; --c) {
            if (pageHasCell(selPage, r, c)) {
                selRow = r;
                selCol = c;
                return;
            }
        }
    }
    selRow = 0;
    selCol = 0;
}

static bool switchPage(int delta) {
    int next = selPage + delta;
    if (next < 0 || next >= pageCount()) return false;
    selPage = next;
    clampSelectionForPage();
    drawAll();
    return true;
}

static void moveToPageStart() {
    selCol = 0;
    if (!pageHasCell(selPage, selRow, selCol)) {
        selRow = 0;
        clampSelectionForPage();
    }
}

static void moveToPageEnd() {
    selCol = NCOLS - 1;
    if (!pageHasCell(selPage, selRow, selCol)) {
        clampSelectionForPage();
    }
}

// ── Icons ──────────────────────────────────────────────────────────────────

static void iSSH(int cx, int cy) {
    auto& d = M5Cardputer.Display;
    d.drawRoundRect(cx-14, cy-10, 28, 19, 3, 0xFFFFFF);
    d.drawFastHLine(cx-14, cy-4, 28, 0xFFFFFF);
    d.fillTriangle(cx-8, cy+1, cx-8, cy+7, cx-3, cy+4, 0xFFFFFF);
    d.fillRect(cx-1, cy+6, 7, 2, 0xFFFFFF);
}

static void iMP3(int cx, int cy) {
    auto& d = M5Cardputer.Display;
    d.fillCircle(cx-6, cy+7, 5, 0xFFFFFF);
    d.fillCircle(cx+7, cy+3, 5, 0xFFFFFF);
    d.drawFastVLine(cx-1,  cy-8, 16, 0xFFFFFF);
    d.drawFastVLine(cx+12, cy-8, 12, 0xFFFFFF);
    d.drawFastHLine(cx-1,  cy-8, 13, 0xFFFFFF);
}

static void iNotes(int cx, int cy, uint32_t tc) {
    auto& d = M5Cardputer.Display;
    d.fillRoundRect(cx-10, cy-12, 20, 24, 2, 0xFFFFFF);
    d.fillRoundRect(cx-8,  cy-10, 16, 20, 2, tc);
    d.fillRect(cx-5, cy-4, 10, 2, 0xFFFFFF);
    d.fillRect(cx-5, cy+1, 10, 2, 0xFFFFFF);
    d.fillRect(cx-5, cy+6,  7, 2, 0xFFFFFF);
}

static void iGames(int cx, int cy) {
    auto& d = M5Cardputer.Display;
    // Game controller: rounded rect body + 2 bumps on top
    d.drawRoundRect(cx-13, cy-6, 26, 16, 4, 0xFFFFFF);
    d.fillCircle(cx-7, cy-6, 3, 0xFFFFFF);  // left grip bump
    d.fillCircle(cx+7, cy-6, 3, 0xFFFFFF);  // right grip bump
    // D-pad (left side): cross
    d.fillRect(cx-11, cy-1, 6, 2, 0xFFFFFF);
    d.fillRect(cx-9,  cy-3, 2, 6, 0xFFFFFF);
    // Buttons (right side): two dots
    d.fillCircle(cx+7, cy-1, 2, 0xFFFFFF);
    d.fillCircle(cx+11, cy+2, 2, 0xFFFFFF);
}

static void iFiles(int cx, int cy) {
    auto& d = M5Cardputer.Display;
    // Folder shape: tab on top-left, rectangle body
    d.fillRoundRect(cx-12, cy-4, 24, 14, 2, 0xFFFFFF);
    d.fillRoundRect(cx-12, cy-10, 10, 8, 2, 0xFFFFFF);
    d.fillRoundRect(cx-10, cy-2, 10, 2, 0, 0x226677);
    d.fillRoundRect(cx-3,  cy-2, 10, 2, 0, 0x226677);
}

static void iSettings(int cx, int cy, uint32_t tc) {
    auto& d = M5Cardputer.Display;
    d.fillCircle(cx, cy, 10, 0xFFFFFF);
    d.fillCircle(cx, cy,  5, tc);
    d.fillRect(cx-2, cy-13, 4, 5, 0xFFFFFF);
    d.fillRect(cx-2, cy+8,  4, 5, 0xFFFFFF);
    d.fillRect(cx-13, cy-2, 5, 4, 0xFFFFFF);
    d.fillRect(cx+8,  cy-2, 5, 4, 0xFFFFFF);
}

static void iGPS(int cx, int cy) {
    auto& d = M5Cardputer.Display;
    d.drawCircle(cx, cy - 2, 8, 0xFFFFFF);
    d.fillCircle(cx, cy - 2, 2, 0xFFFFFF);
    d.drawLine(cx, cy + 6, cx - 5, cy + 14, 0xFFFFFF);
    d.drawLine(cx, cy + 6, cx + 5, cy + 14, 0xFFFFFF);
}

static void iLora(int cx, int cy) {
    auto& d = M5Cardputer.Display;
    d.drawFastVLine(cx, cy - 10, 16, 0xFFFFFF);
    d.drawCircle(cx - 6, cy - 1, 4, 0xFFFFFF);
    d.drawCircle(cx + 6, cy - 1, 4, 0xFFFFFF);
    d.drawArc(cx, cy - 1, 12, 10, 220, 320, 0xFFFFFF);
}

static void iRemote(int cx, int cy) {
    auto& d = M5Cardputer.Display;
    d.drawRoundRect(cx - 8, cy - 12, 16, 24, 3, 0xFFFFFF);
    d.fillCircle(cx, cy - 7, 2, 0xFFFFFF);
    d.fillCircle(cx - 3, cy - 1, 1, 0xFFFFFF);
    d.fillCircle(cx + 3, cy - 1, 1, 0xFFFFFF);
    d.fillCircle(cx - 3, cy + 5, 1, 0xFFFFFF);
    d.fillCircle(cx + 3, cy + 5, 1, 0xFFFFFF);
}

static void iPhotos(int cx, int cy) {
    auto& d = M5Cardputer.Display;
    d.drawRoundRect(cx - 12, cy - 10, 24, 18, 3, 0xFFFFFF);
    d.drawCircle(cx - 4, cy - 3, 2, 0xFFFFFF);
    d.drawLine(cx - 10, cy + 6, cx - 2, cy, 0xFFFFFF);
    d.drawLine(cx - 2, cy, cx + 2, cy + 3, 0xFFFFFF);
    d.drawLine(cx + 2, cy + 3, cx + 10, cy - 5, 0xFFFFFF);
}

static void iRecorder(int cx, int cy) {
    auto& d = M5Cardputer.Display;
    d.fillRoundRect(cx - 5, cy - 9, 10, 14, 4, 0xFFFFFF);
    d.drawFastVLine(cx, cy + 5, 5, 0xFFFFFF);
    d.drawFastHLine(cx - 6, cy + 10, 12, 0xFFFFFF);
    d.drawArc(cx, cy + 2, 10, 8, 200, 340, 0xFFFFFF);
}

static void iKeyboard(int cx, int cy) {
    auto& d = M5Cardputer.Display;
    d.drawRoundRect(cx - 12, cy - 8, 24, 16, 3, 0xFFFFFF);
    d.drawFastHLine(cx - 8, cy - 2, 16, 0xFFFFFF);
    d.drawFastHLine(cx - 8, cy + 3, 16, 0xFFFFFF);
    d.drawFastVLine(cx - 4, cy - 5, 11, 0xFFFFFF);
    d.drawFastVLine(cx + 4, cy - 5, 11, 0xFFFFFF);
}

static void iUsb(int cx, int cy) {
    auto& d = M5Cardputer.Display;
    d.drawFastVLine(cx, cy - 10, 13, 0xFFFFFF);
    d.drawLine(cx, cy - 10, cx - 4, cy - 5, 0xFFFFFF);
    d.drawLine(cx, cy - 10, cx + 4, cy - 5, 0xFFFFFF);
    d.drawFastHLine(cx - 8, cy + 2, 16, 0xFFFFFF);
    d.drawFastVLine(cx - 8, cy + 2, 5, 0xFFFFFF);
    d.drawFastVLine(cx + 8, cy + 2, 5, 0xFFFFFF);
}

static void iNfc(int cx, int cy) {
    auto& d = M5Cardputer.Display;
    // Three concentric arcs (NFC symbol)
    d.drawArc(cx, cy + 4, 14, 12, 210, 330, 0xFFFFFF);
    d.drawArc(cx, cy + 4,  9,  7, 210, 330, 0xFFFFFF);
    d.drawArc(cx, cy + 4,  4,  2, 210, 330, 0xFFFFFF);
}

static void iTimer(int cx, int cy) {
    auto& d = M5Cardputer.Display;
    d.drawCircle(cx, cy, 10, 0xFFFFFF);
    d.drawFastVLine(cx, cy - 5, 6, 0xFFFFFF);
    d.drawLine(cx, cy, cx + 5, cy + 3, 0xFFFFFF);
    d.drawFastHLine(cx - 3, cy - 13, 6, 0xFFFFFF);
}

static void iPayloads(int cx, int cy) {
    auto& d = M5Cardputer.Display;
    // Script/inject icon: document with right-arrow
    d.drawRoundRect(cx - 9, cy - 11, 14, 18, 2, 0xFFFFFF);
    d.fillRect(cx - 6, cy - 7, 8, 2, 0xFFFFFF);
    d.fillRect(cx - 6, cy - 2, 8, 2, 0xFFFFFF);
    d.fillRect(cx - 6, cy + 3, 5, 2, 0xFFFFFF);
    // Arrow pointing right
    d.fillTriangle(cx + 7, cy, cx + 3, cy - 4, cx + 3, cy + 4, 0xFFFFFF);
    d.fillRect(cx - 1, cy - 1, 5, 2, 0xFFFFFF);
}

static void iWifi(int cx, int cy) {
    auto& d = M5Cardputer.Display;
    // WiFi arcs
    d.drawArc(cx, cy + 4, 14, 12, 210, 330, 0x55AAFF);
    d.drawArc(cx, cy + 4,  9,  7, 210, 330, 0x55AAFF);
    d.drawArc(cx, cy + 4,  4,  2, 210, 330, 0x55AAFF);
    d.fillCircle(cx, cy + 4, 2, 0x55AAFF);
    // Lightning bolt (attack/tools indicator)
    d.drawLine(cx + 6, cy - 10, cx + 2, cy - 3, 0xFF8800);
    d.drawLine(cx + 2, cy - 3,  cx + 6, cy - 3, 0xFF8800);
    d.drawLine(cx + 6, cy - 3,  cx + 2, cy + 4, 0xFF8800);
}

static void iDetector(int cx, int cy) {
    auto& d = M5Cardputer.Display;
    // Radar sweep: outer arc, mid arc, centre dot
    d.drawArc(cx, cy, 13, 11, 270, 90, 0xFFFFFF);
    d.drawArc(cx, cy,  8,  6, 270, 90, 0xFFFFFF);
    d.fillCircle(cx, cy, 2, 0xFFFFFF);
    // Sweep line
    d.drawLine(cx, cy, cx + 11, cy - 7, 0x00FF88);
    // Signal dot on sweep
    d.fillCircle(cx + 9, cy - 6, 2, 0xFF3333);
}

static void iBle(int cx, int cy) {
    auto& d = M5Cardputer.Display;
    // Bluetooth "B-rune" symbol: vertical bar + two right-facing triangles
    d.drawFastVLine(cx, cy - 10, 20, 0xFFFFFF);
    // Upper right lobe
    d.drawLine(cx, cy - 10, cx + 7, cy - 4, 0xFFFFFF);
    d.drawLine(cx + 7, cy - 4, cx, cy + 2, 0xFFFFFF);
    // Lower right lobe
    d.drawLine(cx, cy + 2, cx + 7, cy + 8, 0xFFFFFF);
    d.drawLine(cx + 7, cy + 8, cx, cy + 14, 0xFFFFFF);
    // Upper left arm
    d.drawLine(cx, cy - 4, cx - 5, cy - 8, 0xFFFFFF);
    // Lower left arm
    d.drawLine(cx, cy + 8, cx - 5, cy + 12, 0xFFFFFF);
}

// ── Cell ───────────────────────────────────────────────────────────────────

static void drawCell(int row, int col) {
    int idx = selPage * APPS_PER_PAGE + row * NCOLS + col;
    int cx0 = CELL_X[col], cy0 = CELL_Y[row];
    int cw  = CELL_W,      ch  = CELL_H[row];
    auto& d = M5Cardputer.Display;

    const auto* app = appForVisibleIndex(idx);
    if (!app) {
        d.fillRect(cx0, cy0, cw, ch, LBKG);
        return;
    }

    uint32_t tc  = app->color;
    bool     sel = (row == selRow && col == selCol);

    d.fillRoundRect(cx0, cy0, cw, ch, 6, tc);
    if (sel) {
        d.drawRoundRect(cx0+2, cy0+2, cw-4, ch-4, 5, 0xFFFFFF);
        d.drawRoundRect(cx0+3, cy0+3, cw-6, ch-6, 4, 0xFFFFFF);
    }

    int icx = cx0 + cw / 2;
    int icy = cy0 + (ch - 14) / 2 - 2;
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(0xFFFFFF, tc);
    int appId = activeAppIds()[idx];
    switch (appId) {
        case 0: iSSH(icx, icy);          break;
        case 1: iMP3(icx, icy);          break;
        case 2: iNotes(icx, icy, tc);    break;
        case 3: iGames(icx, icy);        break;
        case 4: iSettings(icx, icy, tc); break;
        case 5: iFiles(icx, icy);        break;
        case 6: iRemote(icx, icy);       break;
        case 7: iPhotos(icx, icy);       break;
        case 8: iRecorder(icx, icy);     break;
        case 9: iKeyboard(icx, icy);     break;
        case 10: iUsb(icx, icy);         break;
        case 11: iTimer(icx, icy);       break;
        case 12: iGPS(icx, icy);         break;
        case 13: iLora(icx, icy);        break;
        case 14: iNfc(icx, icy);          break;
        case 15: iPayloads(icx, icy);    break;
        case 16: iBle(icx, icy);         break;
        case 17: iDetector(icx, icy);    break;
        case 18: iWifi(icx, icy);       break;
    }

    // Label
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(0xFFFFFF, tc);
    int lw = strlen(app->label) * FONT_W;
    d.setCursor(cx0 + (cw - lw) / 2, cy0 + ch - 11);
    d.print(app->label);
}

// ── Status bar ─────────────────────────────────────────────────────────────

static void drawStatusBar() {
    auto& d = M5Cardputer.Display;
    constexpr uint32_t SBG = 0x1A1A1A;
    d.fillRect(0, 0, SCREEN_W, STATUS_H, SBG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(0xDDDDDD, SBG);
    d.setCursor(2, 3);
    d.print(isSdMode() ? "SD Mode" : "Radio Mode");

    String  info;
    if (isSdMode()) info = "sd mode";
    else {
        bool ok = (WiFi.status() == WL_CONNECTED);
        info = ok ? WiFi.localIP().toString() : "radio";
    }
    int     iw   = info.length() * FONT_W;
    uint32_t infoCol = isSdMode() ? (uint32_t)0xFFAA00 : ((WiFi.status() == WL_CONNECTED) ? (uint32_t)0x00BBFF : (uint32_t)0x555555);
    d.setTextColor(infoCol, SBG);
    d.setCursor(SCREEN_W - iw - 2, 3);
    d.print(info.c_str());
    char pageBuf[8];
    snprintf(pageBuf, sizeof(pageBuf), "%d/%d", selPage + 1, pageCount());
    d.setTextColor(0xDDDDDD, SBG);
    d.setCursor((SCREEN_W - (int)strlen(pageBuf) * FONT_W) / 2, 3);
    d.print(pageBuf);
    drawBatteryWidget(SBG);
}

// ── Public ─────────────────────────────────────────────────────────────────

static void drawAll() {
    M5Cardputer.Display.fillScreen(LBKG);
    drawStatusBar();
    for (int r = 0; r < NROWS; r++)
        for (int c = 0; c < NCOLS; c++)
            drawCell(r, c);
}

void launcherEnter() {
    selRow = 0; selCol = 0; selPage = 0;
    drawAll();
}

void launcherLoop() {
    auto ev = readKeys();
    if (!ev.changed) return;

    if (ev.tab) {
        if (!switchPage(1) && pageCount() > 1) {
            selPage = 0;
            clampSelectionForPage();
            drawAll();
        }
        return;
    }

    int pr = selRow, pc = selCol, pp = selPage;
    if (ev.up && selRow > 0) {
        selRow--;
    }
    if (ev.down && selRow < NROWS - 1 && pageHasCell(selPage, selRow + 1, selCol)) {
        selRow++;
    }
    if (ev.left) {
        if (selCol > 0) selCol--;
        else if (switchPage(-1)) {
            moveToPageEnd();
            drawAll();
        }
    }
    if (ev.right) {
        if (selCol < NCOLS - 1 && pageHasCell(selPage, selRow, selCol + 1)) selCol++;
        else if (switchPage(1)) {
            moveToPageStart();
            drawAll();
        }
    }

    if (!ev.fnKey) {
        for (char c : ev.chars) {
            char lo = (char)tolower((unsigned char)c);
            int count = activeAppCount();
            for (int i = 0; i < count; i++) {
                const auto* app = appForVisibleIndex(i);
                if (app && app->hotkey == lo) { launchApp(app->scene); return; }
            }
        }
    }

    if (ev.enter) {
        int idx = currentIndex();
        const auto* app = appForVisibleIndex(idx);
        if (app) { launchApp(app->scene); return; }
    }

    if (selPage != pp) {
        return;
    }
    if (selRow != pr || selCol != pc) {
        drawCell(pr, pc);
        drawCell(selRow, selCol);
    }
}
