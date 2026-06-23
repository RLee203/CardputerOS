#ifdef BOARD_TEMBED
#include "vkb_tembed.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>

// ── Key layout ─────────────────────────────────────────────────────────────
// 4 rows × 13 cols of regular chars, then 1 row of 4 special keys
// Cell index: 0-51 = regular chars, 52=SPACE, 53=SHIFT, 54=DEL, 55=OK
static const char VKB_CHARS[] =
    "abcdefghijklm"   // row 0: cells 0-12
    "nopqrstuvwxyz"   // row 1: cells 13-25
    "0123456789.-_"   // row 2: cells 26-38
    "@!?#$%&*()+=/"   // row 3: cells 39-51
;
static constexpr int VKB_REG   = 52;
static constexpr int VKB_SPC   = 52;
static constexpr int VKB_SHF   = 53;
static constexpr int VKB_DEL   = 54;
static constexpr int VKB_OK    = 55;
static constexpr int VKB_TOTAL = 56;
static constexpr int VKB_COLS  = 13;
static constexpr int VKB_ROWS  = 4;   // regular rows

// ── Display layout (320×170 landscape) ────────────────────────────────────
// Y=0..12: status bar (prompt)
// Y=14..27: input field
// Y=28: divider
// Y=29..169: 5 key rows (4 regular + 1 special), each 28px tall

static constexpr int VKX  = 4;    // left margin of key grid
static constexpr int KW   = 23;   // key visual width
static constexpr int KH   = 26;   // key visual height
static constexpr int CW   = 24;   // cell width (key + 1px gap)
static constexpr int CH   = 28;   // cell height (key + 2px gap)
static constexpr int KY   = 29;   // y of first key row

static constexpr uint32_t CK_NORM = 0x0D0D1A;
static constexpr uint32_t CK_SEL  = 0x0044CC;
static constexpr uint32_t CK_OK   = 0x003800;
static constexpr uint32_t CT_NORM = 0x6677AA;
static constexpr uint32_t CT_SEL  = 0xFFFFFF;
static constexpr uint32_t CT_OK   = 0x007722;
static constexpr uint32_t CT_OK_S = 0x00EE44;

static void drawKey(int x, int y, int w, const char* label, bool sel, bool okStyle, bool shiftLit) {
    auto& d = M5Cardputer.Display;
    uint32_t bg, fg;
    if (shiftLit)         { bg = 0x005500; fg = 0x00FF44; }
    else if (okStyle)     { bg = sel ? 0x005500 : CK_OK;   fg = sel ? CT_OK_S : CT_OK; }
    else                  { bg = sel ? CK_SEL  : CK_NORM;  fg = sel ? CT_SEL  : CT_NORM; }
    d.fillRect(x, y, w, KH, bg);
    if (label && *label) {
        int lw = (int)strlen(label) * FONT_W;
        d.setTextColor(fg, bg);
        d.setCursor(x + (w - lw) / 2, y + (KH - FONT_H) / 2);
        d.print(label);
    }
}

static void vkbDraw(const char* prompt, const char* text, int textLen, int cursor, bool shift) {
    auto& d = M5Cardputer.Display;
    d.fillScreen(0x000000);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    // Status bar — prompt
    d.fillRect(0, 0, SCREEN_W, STATUS_H, 0x001133);
    d.setTextColor(0x00AAFF, 0x001133);
    d.setCursor(2, 3);
    // Truncate prompt to fit
    char hdr[53];
    snprintf(hdr, sizeof(hdr), "%.*s", (SCREEN_W - 4) / FONT_W, prompt);
    d.print(hdr);

    // Input field
    constexpr int INP_Y = 14, INP_H = 13;
    d.fillRect(0, INP_Y, SCREEN_W, INP_H, 0x050510);
    d.setTextColor(0xCCCCFF, 0x050510);
    constexpr int MAX_VIS = (SCREEN_W - 8) / FONT_W;
    int start = (textLen > MAX_VIS) ? (textLen - MAX_VIS) : 0;
    d.setCursor(4, INP_Y + (INP_H - FONT_H) / 2);
    d.print(text + start);
    int cx = 4 + (textLen - start) * FONT_W;
    if (cx < SCREEN_W - 2) d.fillRect(cx, INP_Y + 2, FONT_W - 1, INP_H - 4, 0x0066DD);

    // Divider
    d.drawFastHLine(0, 28, SCREEN_W, 0x222244);

    // Regular key grid
    char lbl[2] = {};
    for (int idx = 0; idx < VKB_REG; idx++) {
        int row = idx / VKB_COLS, col = idx % VKB_COLS;
        char c = VKB_CHARS[idx];
        if (shift && c >= 'a' && c <= 'z') c -= 32;
        lbl[0] = c;
        drawKey(VKX + col * CW, KY + row * CH, KW, lbl, idx == cursor, false, false);
    }

    // Special row (row 4)
    int y4 = KY + VKB_ROWS * CH;
    drawKey(VKX + 0*CW, y4, 5*CW - 1, "SPC",  cursor == VKB_SPC, false, false);
    drawKey(VKX + 5*CW, y4, 2*CW - 1, shift ? "SHF" : "shf",
            cursor == VKB_SHF, false, shift && cursor != VKB_SHF);
    drawKey(VKX + 7*CW, y4, 3*CW - 1, "DEL",  cursor == VKB_DEL, false, false);
    drawKey(VKX +10*CW, y4, 3*CW - 1, "OK",   cursor == VKB_OK,  true,  false);
}

String vkbInput(const char* prompt, const char* initial, int maxLen, bool* cancelled) {
    char* text = new char[maxLen + 1];
    strncpy(text, initial ? initial : "", maxLen);
    text[maxLen] = '\0';
    int  textLen = (int)strlen(text);
    int  cursor  = 0;
    bool shift   = false;
    bool dirty   = true;
    bool done    = false;
    bool cancel  = false;

    while (!done) {
        if (dirty) {
            vkbDraw(prompt, text, textLen, cursor, shift);
            dirty = false;
        }
        auto ev = readKeys();
        if (!ev.changed) continue;

        if (ev.back) {
            if (textLen > 0) { text[--textLen] = '\0'; dirty = true; }
            else             { cancel = true; done = true; }
            continue;
        }
        if (ev.down) { cursor = (cursor + 1)            % VKB_TOTAL; dirty = true; continue; }
        if (ev.up)   { cursor = (cursor + VKB_TOTAL - 1) % VKB_TOTAL; dirty = true; continue; }

        if (ev.enter) {
            if (cursor < VKB_REG) {
                if (textLen < maxLen) {
                    char c = VKB_CHARS[cursor];
                    if (shift && c >= 'a' && c <= 'z') c -= 32;
                    text[textLen++] = c; text[textLen] = '\0';
                    if (shift) shift = false;
                    dirty = true;
                }
            } else if (cursor == VKB_SPC) {
                if (textLen < maxLen) { text[textLen++] = ' '; text[textLen] = '\0'; dirty = true; }
            } else if (cursor == VKB_SHF) {
                shift = !shift; dirty = true;
            } else if (cursor == VKB_DEL) {
                if (textLen > 0) { text[--textLen] = '\0'; dirty = true; }
            } else if (cursor == VKB_OK) {
                done = true;
            }
        }
    }

    String result = String(text);
    delete[] text;
    if (cancelled) *cancelled = cancel;
    return result;
}

#endif // BOARD_TEMBED
