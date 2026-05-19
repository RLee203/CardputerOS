#include "app_cc1101.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>

// ── Constants ──────────────────────────────────────────────────────────────
static constexpr int  N_BARS   = 40;
static constexpr int  BAR_W    = SCREEN_W / N_BARS;   // 6 px
static constexpr int  BAR_Y0   = STATUS_H;             // 13
static constexpr int  BAR_H    = SCREEN_H - STATUS_H - 10; // 112 px
static constexpr int  LABEL_Y  = SCREEN_H - 9;
static constexpr int  MAX_EDGES = 512;

// ── CC1101 register addresses ──────────────────────────────────────────────
static constexpr uint8_t REG_IOCFG0   = 0x02;
static constexpr uint8_t REG_PKTCTRL0 = 0x08;
static constexpr uint8_t STR_SIDLE    = 0x36;
static constexpr uint8_t STR_STX      = 0x35;
static constexpr uint8_t STR_SRX      = 0x34;
static constexpr uint8_t STR_SFRX     = 0x3A;

// ── Frequency bands ────────────────────────────────────────────────────────
struct Band { const char* name; float minMHz; float maxMHz; float capMHz; };
static const Band BANDS[] = {
    { "315MHz", 312.0f, 318.0f, 315.0f },
    { "433MHz", 430.0f, 436.0f, 433.92f },
    { "868MHz", 863.0f, 870.0f, 868.35f },
    { "915MHz", 908.0f, 925.0f, 915.0f },
    { "Full",   300.0f, 928.0f, 433.92f },
    { "Custom", 0.0f,   0.0f,   0.0f   }, // limits computed from s_customMHz
};
static constexpr int N_BANDS        = 6;
static constexpr int N_BANDS_PRESET = 5; // presets only (not Custom)

// ── State ──────────────────────────────────────────────────────────────────
enum class CC1101State { MENU, SPECTRUM, CAPTURE, REPLAY, FREQ_INPUT, SCANNER };
static CC1101State  s_state    = CC1101State::MENU;
static int          s_menuSel  = 0;
static int          s_bandSel  = 0;
static bool         s_dirty     = true;
static bool         s_inited    = false;
static bool         s_initError = false; // non-blocking "not found" screen

// Spectrum
static int8_t  s_rssi[N_BARS] = {};
static uint32_t s_lastSweep   = 0;

// Scanner
static constexpr int SCAN_N   = 8;
static constexpr int SCAN_ROW = 13;  // px per row
struct ScanEntry { float mhz; int8_t rssi; };
static ScanEntry s_scan[SCAN_N]  = {};
static int       s_scanCur       = 0;
static int8_t    s_squelch       = -70;
static bool      s_dwelling      = false;
static uint32_t  s_dwellEnd      = 0;
static uint32_t  s_scanStep      = 0;
static uint32_t  s_scanRedraw    = 0;

// Custom frequency
static float s_customMHz   = 433.92f;
static char  s_freqInput[12] = "433.920";
static int   s_freqLen       = 7;

// Capture / replay
static volatile uint16_t s_edges[MAX_EDGES]; // 100µs units
static volatile int      s_edgeCount  = 0;
static volatile uint32_t s_lastEdge   = 0;
static bool              s_capturing  = false;
static bool              s_hasCap     = false;
static uint32_t          s_captureStart = 0;

// ── CC1101 init ────────────────────────────────────────────────────────────
static bool initChip() {
    if (s_inited) return true;
    ELECHOUSE_cc1101.setSpiPin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, CC1101_CS_PIN);
    if (!ELECHOUSE_cc1101.getCC1101()) return false;
    ELECHOUSE_cc1101.Init();
    s_inited = true;
    return true;
}

static void idleChip() {
    ELECHOUSE_cc1101.SpiStrobe(STR_SIDLE);
}

// ── Band helpers ───────────────────────────────────────────────────────────
static float      bandMin()  { return (s_bandSel < N_BANDS_PRESET) ? BANDS[s_bandSel].minMHz : s_customMHz - 5.0f; }
static float      bandMax()  { return (s_bandSel < N_BANDS_PRESET) ? BANDS[s_bandSel].maxMHz : s_customMHz + 5.0f; }
static float      capFreq()  { return (s_bandSel < N_BANDS_PRESET) ? BANDS[s_bandSel].capMHz : s_customMHz; }
static const char* bandName() { return BANDS[s_bandSel].name; } // "Custom" already in BANDS[5]

// ── Spectrum ───────────────────────────────────────────────────────────────
static void scanSpectrum() {
    float minF = bandMin();
    float maxF = bandMax();
    float step = (maxF - minF) / N_BARS;
    ELECHOUSE_cc1101.setModulation(0); // 2-FSK for RSSI
    for (int i = 0; i < N_BARS; i++) {
        ELECHOUSE_cc1101.setMHZ(minF + i * step);
        ELECHOUSE_cc1101.SetRx();
        delayMicroseconds(300);
        s_rssi[i] = ELECHOUSE_cc1101.getRssi();
    }
}

static void drawSpectrum() {
    auto& d = M5Cardputer.Display;

    // Title
    d.fillRect(0, 0, SCREEN_W, STATUS_H, 0x001133);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(0x00AAFF, 0x001133);
    d.setCursor(2, 3);
    char hdr[40];
    snprintf(hdr, sizeof(hdr), "CC1101 Spectrum  %s", bandName());
    d.print(hdr);

    // Labels
    float minF = bandMin();
    float maxF = bandMax();
    char lmin[10], lmax[10];
    snprintf(lmin, sizeof(lmin), "%.0f", minF);
    snprintf(lmax, sizeof(lmax), "%.0fMHz", maxF);
    d.setTextColor(C_DIM, C_BG);
    d.fillRect(0, LABEL_Y, SCREEN_W, SCREEN_H - LABEL_Y, C_BG);
    d.setCursor(0, LABEL_Y);
    d.print(lmin);
    int rw = strlen(lmax) * FONT_W;
    d.setCursor(SCREEN_W - rw, LABEL_Y);
    d.print(lmax);

    // Bars
    for (int i = 0; i < N_BARS; i++) {
        int8_t r = s_rssi[i];
        int h = (r > -100) ? (int)((r + 100) * BAR_H / 70) : 0;
        if (h < 0) h = 0;
        if (h > BAR_H) h = BAR_H;
        int x = i * BAR_W;
        uint32_t col = (h > BAR_H * 2 / 3) ? (uint32_t)0xFF3333 :
                       (h > BAR_H / 3)      ? (uint32_t)0xFFAA00 : (uint32_t)0x0066AA;
        int boty = BAR_Y0 + BAR_H;
        if (h > 0) d.fillRect(x, boty - h, BAR_W - 1, h, col);
        d.fillRect(x, BAR_Y0, BAR_W - 1, BAR_H - h, 0x060A0F);
    }
}

// ── Capture (interrupt-based) ──────────────────────────────────────────────
static void IRAM_ATTR onEdge() {
    if (s_edgeCount < MAX_EDGES) {
        uint32_t now = micros();
        uint32_t dur = now - s_lastEdge;
        uint16_t units = (dur > 6553500) ? 65535 : (uint16_t)(dur / 100);
        if (units == 0) units = 1;
        s_edges[s_edgeCount++] = units;
        s_lastEdge = now;
    }
}

static void startCapture() {
    idleChip();
    ELECHOUSE_cc1101.setMHZ(capFreq());
    ELECHOUSE_cc1101.setModulation(2);                    // ASK/OOK
    ELECHOUSE_cc1101.setDRate(10);                        // 10 kbps ≈ 100µs/bit
    // Async serial mode: PKTCTRL0 = 0x32 (async, infinite length, no CRC)
    ELECHOUSE_cc1101.SpiWriteReg(REG_PKTCTRL0, 0x32);
    // GDO0 = async serial data output (demodulated OOK)
    ELECHOUSE_cc1101.SpiWriteReg(REG_IOCFG0, 0x0D);
    ELECHOUSE_cc1101.SpiStrobe(STR_SFRX);
    ELECHOUSE_cc1101.SpiStrobe(STR_SRX);

    s_edgeCount = 0;
    s_lastEdge  = micros();
    s_capturing = true;
    s_captureStart = millis();
    pinMode(CC1101_GDO0_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(CC1101_GDO0_PIN), onEdge, CHANGE);
}

static void stopCapture() {
    detachInterrupt(digitalPinToInterrupt(CC1101_GDO0_PIN));
    idleChip();
    s_capturing = false;
    s_hasCap    = (s_edgeCount > 4);
    ELECHOUSE_cc1101.Init(); // restore normal config
}

static void drawCapture() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.fillRect(0, 0, SCREEN_W, STATUS_H, 0x001133);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(0x00AAFF, 0x001133);
    d.setCursor(2, 3);
    char hdr[40];
    snprintf(hdr, sizeof(hdr), "CC1101 Capture  %.3fMHz", capFreq());
    d.print(hdr);

    if (s_capturing) {
        d.setTextSize(2);
        d.setTextColor(0xFF2222, C_BG);
        d.setCursor(16, 36);
        d.print("RECORDING");
        d.setTextSize(1);
        char buf[32];
        snprintf(buf, sizeof(buf), "Edges: %d / %d", (int)s_edgeCount, MAX_EDGES);
        d.setTextColor(C_FG, C_BG);
        d.setCursor(8, 76);
        d.print(buf);
        uint32_t elapsed = (millis() - s_captureStart) / 1000;
        snprintf(buf, sizeof(buf), "Time: %lus", elapsed);
        d.setCursor(8, 90);
        d.print(buf);
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, 112);
        d.print("Enter=stop  Back=cancel");
    } else if (s_hasCap) {
        d.setTextColor(0x00EE44, C_BG);
        d.setCursor(8, 36);
        d.print("Capture complete!");
        char buf[40];
        snprintf(buf, sizeof(buf), "%d edges recorded", (int)s_edgeCount);
        d.setTextColor(C_FG, C_BG);
        d.setCursor(8, 52);
        d.print(buf);
        snprintf(buf, sizeof(buf), "At %.2f MHz", BANDS[s_bandSel].capMHz);
        d.setCursor(8, 64);
        d.print(buf);
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, 90);
        d.print("Enter=record again");
        d.setCursor(8, 104);
        d.print("Back=return");
    } else {
        d.setTextColor(C_FG, C_BG);
        d.setCursor(8, 36);
        d.print("Ready to capture OOK.");
        d.setCursor(8, 52);
        d.print("Point remote at device,");
        d.setCursor(8, 64);
        d.print("then press Enter.");
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, 88);
        d.print("GDO0 pin detects signal");
        d.setCursor(8, 104);
        d.print("Back=return");
    }
}

// ── Replay (async OOK TX via GDO0 pin) ────────────────────────────────────
static void doReplay() {
    if (!s_hasCap || s_edgeCount < 2) return;

    idleChip();
    ELECHOUSE_cc1101.setMHZ(capFreq());
    ELECHOUSE_cc1101.setModulation(2);
    ELECHOUSE_cc1101.setDRate(10);
    ELECHOUSE_cc1101.setPA(10);
    // Async TX: GDO0 = data INPUT to CC1101
    ELECHOUSE_cc1101.SpiWriteReg(REG_PKTCTRL0, 0x32);
    ELECHOUSE_cc1101.SpiWriteReg(REG_IOCFG0, 0x2E); // high-Z in async TX, data on GDO0
    ELECHOUSE_cc1101.SpiStrobe(STR_STX);

    // Set pin as output, start with carrier off
    pinMode(CC1101_GDO0_PIN, OUTPUT);
    digitalWrite(CC1101_GDO0_PIN, LOW);
    delayMicroseconds(500);

    // Replay 3x
    for (int rep = 0; rep < 3; rep++) {
        bool level = false; // captured starts from first falling/rising edge
        for (int i = 0; i < s_edgeCount; i++) {
            level = !level;
            digitalWrite(CC1101_GDO0_PIN, level ? HIGH : LOW);
            delayMicroseconds((uint32_t)s_edges[i] * 100);
        }
        digitalWrite(CC1101_GDO0_PIN, LOW);
        delay(20);
    }

    idleChip();
    pinMode(CC1101_GDO0_PIN, INPUT_PULLDOWN);
    ELECHOUSE_cc1101.Init();
}

static void drawReplay() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.fillRect(0, 0, SCREEN_W, STATUS_H, 0x001133);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(0x00AAFF, 0x001133);
    d.setCursor(2, 3);
    d.print("CC1101 Replay");

    if (!s_hasCap) {
        d.setTextColor(C_ERROR, C_BG);
        d.setCursor(8, 48);
        d.print("No capture data.");
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, 64);
        d.print("Go to Capture first.");
        d.setCursor(8, 104);
        d.print("Back=return");
    } else {
        d.setTextColor(C_FG, C_BG);
        char buf[48];
        snprintf(buf, sizeof(buf), "%d edges  %.3fMHz", (int)s_edgeCount, capFreq());
        d.setCursor(8, 36);
        d.print(buf);
        d.setTextColor(0x00EE44, C_BG);
        d.setCursor(8, 56);
        d.print("Enter=transmit (x3)");
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, 72);
        d.print("L/R=change band");
        d.setCursor(8, 104);
        d.print("Back=return");
    }
}

// ── Scanner ────────────────────────────────────────────────────────────────
static void buildScanList() {
    float minF = bandMin();
    float maxF = bandMax();
    float step = (maxF - minF) / SCAN_N;
    for (int i = 0; i < SCAN_N; i++) {
        s_scan[i].mhz  = minF + i * step;
        s_scan[i].rssi = -100;
    }
    s_scanCur  = 0;
    s_dwelling = false;
}

static void drawScanner() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, SCREEN_W, STATUS_H, 0x001133);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(0x00AAFF, 0x001133);
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "CC1101 Scanner  %s  Sq:%d", bandName(), (int)s_squelch);
    d.setCursor(2, 3);
    d.print(hdr);

    static constexpr int BAR_X   = 54;
    static constexpr int BAR_MAX = 108;

    for (int i = 0; i < SCAN_N; i++) {
        int     y   = STATUS_H + i * SCAN_ROW;
        bool    cur = (i == s_scanCur);
        bool    act = (s_scan[i].rssi > s_squelch);
        uint32_t bg = act ? (uint32_t)0x1A0000 : C_BG;
        d.fillRect(0, y, SCREEN_W, SCAN_ROW, bg);
        // Left cursor bar
        d.fillRect(0, y, 3, SCAN_ROW, cur ? (uint32_t)0x00AAFF : (uint32_t)0x111111);

        // Frequency label
        char fbuf[10];
        snprintf(fbuf, sizeof(fbuf), "%.3f", s_scan[i].mhz);
        d.setTextColor(act ? (uint32_t)0xFF4444 : (cur ? (uint32_t)0x00AAFF : C_FG), bg);
        d.setCursor(5, y + 3);
        d.print(fbuf);

        // RSSI bar
        int norm = s_scan[i].rssi + 100; // 0-70
        if (norm < 0) norm = 0;
        int bw = (norm * BAR_MAX) / 70;
        if (bw > BAR_MAX) bw = BAR_MAX;
        uint32_t bcol = act ? (uint32_t)0xFF3333 : (uint32_t)0x004488;
        d.fillRect(BAR_X, y + 3, bw, 6, bcol);
        d.fillRect(BAR_X + bw, y + 3, BAR_MAX - bw, 6, 0x111111);

        // dBm + active tag
        d.setTextColor(act ? (uint32_t)0xFF8800 : C_DIM, bg);
        char rbuf[16];
        snprintf(rbuf, sizeof(rbuf), act ? "%4d <<<" : "%4d", (int)s_scan[i].rssi);
        d.setCursor(BAR_X + BAR_MAX + 2, y + 3);
        d.print(rbuf);
    }

    // Footer
    int fy = STATUS_H + SCAN_N * SCAN_ROW + 2;
    d.fillRect(0, fy, SCREEN_W, SCREEN_H - fy, C_BG);
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(0, fy + 1);
    d.print("L/R=squelch  Back=menu");
}

// ── Init-error overlay (non-blocking) ─────────────────────────────────────
static void drawInitError() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, STATUS_H, SCREEN_W, SCREEN_H - STATUS_H, C_BG);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(C_ERROR, C_BG);
    d.setCursor(8, 44); d.print("CC1101 not found!");
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(8, 58); d.print("Check PINGEQUA wiring.");
    d.setCursor(8, 80); d.print("Press any key...");
}

// ── Freq input ─────────────────────────────────────────────────────────────
static void drawFreqInput() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.fillRect(0, 0, SCREEN_W, STATUS_H, 0x001133);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(0x00AAFF, 0x001133);
    d.setCursor(2, 3);
    d.print("CC1101 Custom Frequency");

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(8, 24);
    d.print("Enter MHz  (300.000 - 928.000)");

    // Input box
    d.drawRoundRect(6, 38, SCREEN_W - 12, 20, 3, C_ACCENT);
    d.setTextColor(C_INPUT, C_BG);
    d.setTextSize(2);
    char buf[14];
    snprintf(buf, sizeof(buf), "%s_", s_freqInput);
    d.setCursor(12, 41);
    d.print(buf);

    d.setTextSize(1);
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(8, 72);
    d.print("Digits + '.'   Bksp=delete");
    d.setCursor(8, 86);
    d.print("Enter=confirm  Back=cancel");

    // Show what band this maps to
    float f = atof(s_freqInput);
    if (f >= 300.0f && f <= 928.0f) {
        d.setTextColor(0x00EE44, C_BG);
        snprintf(buf, sizeof(buf), "=> %.3f MHz", f);
        d.setCursor(8, 108);
        d.print(buf);
    } else if (s_freqLen > 0) {
        d.setTextColor(C_ERROR, C_BG);
        d.setCursor(8, 108);
        d.print("Out of range!");
    }
}

// ── Menu ───────────────────────────────────────────────────────────────────
static const char* MENU_LABELS[] = { "Spectrum",  "Capture",       "Replay",           "Band",             "Custom Freq",   "Scanner" };
static const char* MENU_DESC[]   = { "RSSI sweep","Record OOK sig","Retransmit capture","Select freq band", "Type exact MHz","Squelch scan" };
static const uint32_t MENU_COLS[]= { 0x0066AA,    0x007722,        0xAA6600,           0x660077,           0x556600,        0x883300 };
static constexpr int N_MENU   = 6;
static constexpr int CARD_H   = 18;
static constexpr int CARD_GAP = 1;

static void drawMenu() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.fillRect(0, 0, SCREEN_W, STATUS_H, 0x001133);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(0x00AAFF, 0x001133);
    d.setCursor(2, 3);
    d.print("CC1101 RF  ");
    d.setTextColor(0x888888, 0x001133);
    d.print(BANDS[s_bandSel].name);
    // Module status (right of title bar)
    const char* st = s_inited ? "ready" : "?";
    d.setTextColor(s_inited ? (uint32_t)0x00AA44 : (uint32_t)0x888888, 0x001133);
    d.setCursor(SCREEN_W - (int)strlen(st) * FONT_W - 2, 3);
    d.print(st);

    for (int i = 0; i < N_MENU; i++) {
        int y = STATUS_H + CARD_GAP + i * (CARD_H + CARD_GAP);
        bool sel = (i == s_menuSel);
        uint32_t col = MENU_COLS[i];
        d.fillRoundRect(0, y, SCREEN_W, CARD_H, 3, sel ? col : (uint32_t)0x111111);
        d.fillRect(0, y, 4, CARD_H, col);
        d.setTextColor(sel ? (uint32_t)0xFFFFFF : col, sel ? col : (uint32_t)0x111111);
        d.setCursor(8, y + 2);
        d.print(MENU_LABELS[i]);
        if (i == 3) { // Band: show current preset name
            d.setTextColor(sel ? (uint32_t)0xEEEEEE : (uint32_t)0x888888, sel ? col : (uint32_t)0x111111);
            d.setCursor(8 + strlen(MENU_LABELS[i]) * FONT_W + 6, y + 2);
            d.print(bandName());
        }
        if (i == 4) { // Custom Freq: show stored value
            d.setTextColor(sel ? (uint32_t)0xEEEEEE : (uint32_t)0x888888, sel ? col : (uint32_t)0x111111);
            d.setCursor(8 + strlen(MENU_LABELS[i]) * FONT_W + 6, y + 2);
            char fbuf[16]; snprintf(fbuf, sizeof(fbuf), "%.3fMHz", s_customMHz);
            d.print(fbuf);
        }
        d.setTextColor(sel ? (uint32_t)0xDDDDDD : (uint32_t)0x555555, sel ? col : (uint32_t)0x111111);
        d.setCursor(8, y + 10);
        d.print(MENU_DESC[i]);
    }
}

// ── Public entry points ────────────────────────────────────────────────────
void appCc1101Enter() {
    s_state     = CC1101State::MENU;
    s_menuSel   = 0;
    s_dirty     = true;
    s_inited    = false;   // do NOT call initChip() here — same reason as nRF24
    s_initError = false;
}

void appCc1101Loop() {
    auto& d = M5Cardputer.Display;
    auto ev = readKeys();

    switch (s_state) {

    case CC1101State::MENU:
        if (s_dirty) { drawMenu(); s_dirty = false; }
        // Non-blocking error screen: any key dismisses it
        if (s_initError) {
            if (ev.changed) { s_initError = false; s_dirty = true; }
            return;
        }
        if (!ev.changed) return;
        if (ev.back) { goHome(); return; }
        if (ev.up   && s_menuSel > 0)           { s_menuSel--; s_dirty = true; }
        if (ev.down && s_menuSel < N_MENU - 1)  { s_menuSel++; s_dirty = true; }
        if (ev.enter) {
            // Band and Custom Freq don't need hardware — skip init check
            bool needsHw = (s_menuSel == 0 || s_menuSel == 1 || s_menuSel == 2 || s_menuSel == 5);
            if (needsHw && !s_inited && !initChip()) {
                drawInitError();
                s_initError = true;
                break;
            }
            switch (s_menuSel) {
                case 0: s_state = CC1101State::SPECTRUM;  s_dirty = true; break;
                case 1: s_state = CC1101State::CAPTURE;   s_dirty = true; break;
                case 2: s_state = CC1101State::REPLAY;    s_dirty = true; break;
                case 3:
                    s_bandSel = (s_bandSel >= N_BANDS_PRESET - 1) ? 0 : s_bandSel + 1;
                    s_dirty = true; break;
                case 4: s_state = CC1101State::FREQ_INPUT; s_dirty = true; break;
                case 5:
                    buildScanList();
                    s_state = CC1101State::SCANNER; s_dirty = true; break;
            }
        }
        if (ev.left || ev.right) {
            int dir = ev.right ? 1 : -1;
            s_bandSel = (s_bandSel + N_BANDS_PRESET + dir) % N_BANDS_PRESET;
            s_dirty = true;
        }
        break;

    case CC1101State::SPECTRUM:
        if (s_dirty) {
            d.fillRect(0, 0, SCREEN_W, STATUS_H, 0x001133);
            d.setFont(&fonts::Font0); d.setTextSize(1);
            d.setTextColor(0x00AAFF, 0x001133);
            d.setCursor(2, 3);
            char hdr[40];
            snprintf(hdr, sizeof(hdr), "CC1101 Spectrum  %s", BANDS[s_bandSel].name);
            d.print(hdr);
            s_dirty = false;
        }
        // Sweep every 80ms
        if (millis() - s_lastSweep > 80) {
            scanSpectrum();
            drawSpectrum();
            s_lastSweep = millis();
        }
        if (!ev.changed) return;
        if (ev.back) { idleChip(); s_state = CC1101State::MENU; s_dirty = true; return; }
        if (ev.left || ev.right) {
            s_bandSel = (s_bandSel + N_BANDS_PRESET + (ev.right ? 1 : -1)) % N_BANDS_PRESET;
            s_lastSweep = 0;
        }
        break;

    case CC1101State::CAPTURE:
        if (s_dirty) { drawCapture(); s_dirty = false; }
        if (s_capturing) {
            // Auto-stop on buffer full or 10s timeout
            if (s_edgeCount >= MAX_EDGES || millis() - s_captureStart > 10000) {
                stopCapture();
                s_dirty = true;
                return;
            }
            // Periodic display refresh
            static uint32_t lastRefresh = 0;
            if (millis() - lastRefresh > 200) { lastRefresh = millis(); drawCapture(); }
        }
        if (!ev.changed) return;
        if (ev.back) {
            if (s_capturing) stopCapture();
            idleChip();
            s_state = CC1101State::MENU; s_dirty = true; return;
        }
        if (ev.enter) {
            if (s_capturing) {
                stopCapture(); s_dirty = true;
            } else {
                startCapture(); s_dirty = true;
            }
        }
        break;

    case CC1101State::REPLAY:
        if (s_dirty) { drawReplay(); s_dirty = false; }
        if (!ev.changed) return;
        if (ev.back) { s_state = CC1101State::MENU; s_dirty = true; return; }
        if (ev.enter && s_hasCap) {
            d.fillRect(0, STATUS_H, SCREEN_W, SCREEN_H - STATUS_H, C_BG);
            d.setTextColor(0xFF8800, C_BG);
            d.setTextSize(2);
            d.setCursor(10, 56);
            d.print("Transmitting");
            d.setTextSize(1);
            doReplay();
            s_dirty = true;
        }
        if (ev.left || ev.right) {
            s_bandSel = (s_bandSel + N_BANDS_PRESET + (ev.right ? 1 : -1)) % N_BANDS_PRESET;
            s_dirty = true;
        }
        break;

    case CC1101State::FREQ_INPUT:
        if (s_dirty) { drawFreqInput(); s_dirty = false; }
        if (!ev.changed) return;
        if (ev.back) {
            if (s_freqLen > 0) {
                // First back press: clear a char; if already empty, cancel
                s_freqInput[--s_freqLen] = '\0';
                s_dirty = true;
            } else {
                s_state = CC1101State::MENU; s_dirty = true;
            }
            return;
        }
        if (ev.enter) {
            float f = atof(s_freqInput);
            if (f >= 300.0f && f <= 928.0f && s_freqLen > 0) {
                s_customMHz = f;
                s_bandSel   = N_BANDS - 1; // point to Custom slot
            }
            s_state = CC1101State::MENU; s_dirty = true;
            return;
        }
        // Collect digits and decimal point
        for (char c : ev.chars) {
            if (c == '\b' || c == 127) {
                if (s_freqLen > 0) { s_freqInput[--s_freqLen] = '\0'; s_dirty = true; }
            } else if ((isdigit((unsigned char)c) || (c == '.' && !strchr(s_freqInput, '.')))
                       && s_freqLen < (int)sizeof(s_freqInput) - 2) {
                s_freqInput[s_freqLen++] = c;
                s_freqInput[s_freqLen]   = '\0';
                s_dirty = true;
            }
        }
        break;

    case CC1101State::SCANNER: {
        uint32_t now = millis();

        if (s_dwelling) {
            // Re-measure active channel every 300ms to update RSSI live
            if (now - s_scanRedraw > 300) {
                ELECHOUSE_cc1101.setMHZ(s_scan[s_scanCur].mhz);
                ELECHOUSE_cc1101.SetRx();
                delayMicroseconds(300);
                s_scan[s_scanCur].rssi = ELECHOUSE_cc1101.getRssi();
                drawScanner();
                s_scanRedraw = now;
            }
            if (now >= s_dwellEnd) {
                s_dwelling = false;
                s_scanCur  = (s_scanCur + 1) % SCAN_N;
            }
        } else {
            // Advance one frequency every 60ms
            if (now - s_scanStep > 60) {
                ELECHOUSE_cc1101.setMHZ(s_scan[s_scanCur].mhz);
                ELECHOUSE_cc1101.SetRx();
                delayMicroseconds(300);
                s_scan[s_scanCur].rssi = ELECHOUSE_cc1101.getRssi();
                if (s_scan[s_scanCur].rssi > s_squelch) {
                    s_dwelling = true;
                    s_dwellEnd = now + 3000; // 3s dwell on active signal
                } else {
                    s_scanCur = (s_scanCur + 1) % SCAN_N;
                }
                drawScanner();
                s_scanStep = now;
            }
        }

        if (!ev.changed) return;
        if (ev.back) { idleChip(); s_state = CC1101State::MENU; s_dirty = true; return; }
        if (ev.left  && s_squelch > -100) { s_squelch -= 5; drawScanner(); }
        if (ev.right && s_squelch < -30)  { s_squelch += 5; drawScanner(); }
        if (ev.enter) { s_dwelling = false; } // skip current dwell
        break;
    }
    }
}
