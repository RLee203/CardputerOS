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

static constexpr uint32_t TCOL[6] = {
    0x1155BB,  // SSH      blue
    0xAA4400,  // MP3      amber
    0x116633,  // Notes    green
    0xCC2200,  // Games    red
    0x5522AA,  // Settings purple
    0x226677,  // Files    teal
};
static const char*    TLABEL[6]  = { "SSH", "MP3", "Notes", "Games", "Settings", "Files" };
static const char     TKEY[6]    = { 's', 'm', 'n', 'g', 'p', 'f' };
static const AppScene TSCENE[6]  = {
    AppScene::SSH, AppScene::MP3, AppScene::NOTES,
    AppScene::GAMES, AppScene::SETTINGS, AppScene::FILES
};

static int selRow = 0, selCol = 0;

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

// ── Cell ───────────────────────────────────────────────────────────────────

static void drawCell(int row, int col) {
    int idx = row * NCOLS + col;
    int cx0 = CELL_X[col], cy0 = CELL_Y[row];
    int cw  = CELL_W,      ch  = CELL_H[row];
    auto& d = M5Cardputer.Display;

    if (idx >= 6) {
        d.fillRoundRect(cx0, cy0, cw, ch, 6, 0x1E1E1E);
        d.drawRoundRect(cx0, cy0, cw, ch, 6, 0x333333);
        return;
    }

    uint32_t tc  = TCOL[idx];
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
    switch (idx) {
        case 0: iSSH(icx, icy);          break;
        case 1: iMP3(icx, icy);          break;
        case 2: iNotes(icx, icy, tc);    break;
        case 3: iGames(icx, icy);        break;
        case 4: iSettings(icx, icy, tc); break;
        case 5: iFiles(icx, icy);        break;
    }

    // Label
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(0xFFFFFF, tc);
    int lw = strlen(TLABEL[idx]) * FONT_W;
    d.setCursor(cx0 + (cw - lw) / 2, cy0 + ch - 11);
    d.print(TLABEL[idx]);
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
    d.print("Cardputer OS");

    bool    ok   = (WiFi.status() == WL_CONNECTED);
    String  info = ok ? WiFi.localIP().toString() : "no wifi";
    int     iw   = info.length() * FONT_W;
    d.setTextColor(ok ? (uint32_t)0x00BBFF : (uint32_t)0x555555, SBG);
    d.setCursor(SCREEN_W - iw - 2, 3);
    d.print(info.c_str());
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
    selRow = 0; selCol = 0;
    drawAll();
}

void launcherLoop() {
    auto ev = readKeys();
    if (!ev.changed) return;

    int pr = selRow, pc = selCol;
    if (ev.up    && selRow > 0)         selRow--;
    if (ev.down  && selRow < NROWS - 1) selRow++;
    if (ev.left  && selCol > 0)         selCol--;
    if (ev.right && selCol < NCOLS - 1) selCol++;

    if (!ev.fnKey) {
        for (char c : ev.chars) {
            char lo = (char)tolower((unsigned char)c);
            for (int i = 0; i < 5; i++)
                if (TKEY[i] == lo) { launchApp(TSCENE[i]); return; }
        }
    }

    if (ev.enter) {
        int idx = selRow * NCOLS + selCol;
        if (idx < 6) { launchApp(TSCENE[idx]); return; }
    }

    if (selRow != pr || selCol != pc) {
        drawCell(pr, pc);
        drawCell(selRow, selCol);
    }
}
