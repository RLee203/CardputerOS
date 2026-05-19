#include "app_keyfob.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>

// ── Constants ──────────────────────────────────────────────────────────────
static constexpr int MAX_CAPS   = 6;
static constexpr int MAX_BITS   = 80;   // KeeLoq = 66 bits + margin
static constexpr int MAX_EDGES  = 400;
static constexpr int BIT_PX     = 3;    // pixels per bit in analyze view
static constexpr int BIT_X0     = 3;    // left margin for bit display
static constexpr int CAP_ROW_H  = 9;    // px per capture row

// KeeLoq field boundaries (bits, LSB-first after sync)
static constexpr int KEELOQ_HOP_BITS = 32;  // encrypted rolling code
static constexpr int KEELOQ_SER_BITS = 28;  // serial number (fixed)
static constexpr int KEELOQ_BTN_BITS =  4;  // button code
static constexpr int KEELOQ_TOTAL    = 64;  // standard KeeLoq = 64 bits

// PWM decoding thresholds (in 100µs units)
static constexpr int SYNC_MIN   = 40;   // >4ms low = sync gap
static constexpr int BIT1_THRESH = 8;   // low gap >800µs = bit '1'
static constexpr int FRAME_END  = 30;   // gap >3ms = end of frame

// CC1101 register constants
static constexpr uint8_t REG_IOCFG0   = 0x02;
static constexpr uint8_t REG_PKTCTRL0 = 0x08;
static constexpr uint8_t STR_SIDLE    = 0x36;
static constexpr uint8_t STR_SRX      = 0x34;
static constexpr uint8_t STR_SFRX     = 0x3A;

// ── Capture storage ────────────────────────────────────────────────────────
struct FobCap {
    uint16_t edges[MAX_EDGES];
    int      edgeCount;
    uint8_t  bits[MAX_BITS / 8 + 1];
    int      bitCount;
    bool     decoded;
};

static FobCap   s_caps[MAX_CAPS];
static int      s_capCount  = 0;
static uint8_t  s_fixedMask[(MAX_BITS / 8) + 1]; // 1 bit = fixed across all captures
static int      s_compareBits = 0;

// ── State ──────────────────────────────────────────────────────────────────
enum class KFState { MENU, CAPTURE, ANALYZE };
static KFState  s_state     = KFState::MENU;
static int      s_menuSel   = 0;
static bool     s_dirty     = true;
static bool     s_inited    = false;
static bool     s_initError = false;
static int      s_freqSel   = 0;  // 0=315MHz  1=433MHz

static const float FREQS[]      = { 315.0f, 433.92f };
static const char* FREQ_NAMES[] = { "315MHz", "433MHz" };

// ── Capture state ──────────────────────────────────────────────────────────
static volatile uint16_t s_rawEdges[MAX_EDGES];
static volatile int      s_rawCount = 0;
static volatile uint32_t s_lastEdge = 0;
static bool              s_capturing = false;
static uint32_t          s_capStart  = 0;

// ── CC1101 ─────────────────────────────────────────────────────────────────
static bool initChip() {
    if (s_inited) return true;
    ELECHOUSE_cc1101.setSpiPin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, CC1101_CS_PIN);
    if (!ELECHOUSE_cc1101.getCC1101()) return false;
    ELECHOUSE_cc1101.Init();
    s_inited = true;
    return true;
}

static void idleChip() { ELECHOUSE_cc1101.SpiStrobe(STR_SIDLE); }

static void startRx() {
    idleChip();
    ELECHOUSE_cc1101.setMHZ(FREQS[s_freqSel]);
    ELECHOUSE_cc1101.setModulation(2);   // ASK/OOK
    ELECHOUSE_cc1101.setDRate(10);
    ELECHOUSE_cc1101.SpiWriteReg(REG_PKTCTRL0, 0x32); // async serial, infinite
    ELECHOUSE_cc1101.SpiWriteReg(REG_IOCFG0, 0x0D);   // GDO0 = demod output
    ELECHOUSE_cc1101.SpiStrobe(STR_SFRX);
    ELECHOUSE_cc1101.SpiStrobe(STR_SRX);
}

// ── Edge interrupt ──────────────────────────────────────────────────────────
static void IRAM_ATTR onEdge() {
    if (s_rawCount < MAX_EDGES) {
        uint32_t now = micros();
        uint32_t dur = now - s_lastEdge;
        uint16_t units = (dur > 6553500) ? 65535 : (uint16_t)(dur / 100);
        if (units == 0) units = 1;
        s_rawEdges[s_rawCount++] = units;
        s_lastEdge = now;
    }
}

// ── PWM decoder ─────────────────────────────────────────────────────────────
// Edges alternate: high dur, low dur, high dur, low dur ...
// Preamble: many equal-length pulses. Sync: very long low gap.
// Data bits: constant-width high pulse + short low (=0) or long low (=1).
static int decodePWM(FobCap* cap) {
    const uint16_t* e = cap->edges;
    int n = cap->edgeCount;
    cap->bitCount = 0;
    memset(cap->bits, 0, sizeof(cap->bits));

    // Find sync gap: a low period > SYNC_MIN that follows at least 6 edges
    int syncIdx = -1;
    for (int i = 1; i < n; i += 2) { // odd indices = low periods
        if (e[i] >= SYNC_MIN && i >= 6) { syncIdx = i; break; }
    }
    if (syncIdx < 0) return 0;

    // Decode data bits after sync (each bit = high + low pair)
    for (int i = syncIdx + 1; i + 1 < n && cap->bitCount < MAX_BITS; i += 2) {
        uint16_t lowT = e[i + 1];
        if (lowT >= FRAME_END) break; // end of frame

        int bit = (lowT >= BIT1_THRESH) ? 1 : 0;
        if (bit) {
            int byteIdx = cap->bitCount / 8;
            int bitIdx  = cap->bitCount % 8;
            cap->bits[byteIdx] |= (1 << bitIdx);
        }
        cap->bitCount++;
    }
    cap->decoded = (cap->bitCount >= 32);
    return cap->bitCount;
}

// ── Fixed-bit analysis ─────────────────────────────────────────────────────
static void computeFixed() {
    if (s_capCount < 2) return;
    s_compareBits = s_caps[0].bitCount;
    for (int c = 1; c < s_capCount; c++)
        if (s_caps[c].bitCount < s_compareBits) s_compareBits = s_caps[c].bitCount;

    memset(s_fixedMask, 0xFF, sizeof(s_fixedMask)); // start all fixed
    for (int b = 0; b < s_compareBits; b++) {
        int byteIdx = b / 8;
        int bitIdx  = b % 8;
        int refBit  = (s_caps[0].bits[byteIdx] >> bitIdx) & 1;
        for (int c = 1; c < s_capCount; c++) {
            int cb = (s_caps[c].bits[byteIdx] >> bitIdx) & 1;
            if (cb != refBit) {
                s_fixedMask[byteIdx] &= ~(1 << bitIdx); // mark rolling
                break;
            }
        }
    }
}

// ── Draw helpers ────────────────────────────────────────────────────────────
static void drawBar(const char* title) {
    auto& d = M5Cardputer.Display;
    constexpr uint32_t BAR_BG = 0x1A0C00;
    constexpr uint32_t BAR_FG = 0xFF9A2E;
    constexpr uint32_t BAR_SUB = 0xC87A2A;
    d.fillRect(0, 0, SCREEN_W, STATUS_H, BAR_BG);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(BAR_FG, BAR_BG);
    d.setCursor(2, 3); d.print(title);
    d.setTextColor(BAR_SUB, BAR_BG);
    d.setCursor(SCREEN_W - 6 * FONT_W - 2, 3);
    d.print(FREQ_NAMES[s_freqSel]);
}

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

// ── MENU ────────────────────────────────────────────────────────────────────
static const char* MENU_LABELS[] = { "Capture",       "Analyze",          "Clear All" };
static const char* MENU_DESC[]   = { "Record fob press","Compare captures","Reset captures" };
static const uint32_t MENU_COLS[]= { 0x006622,         0x004488,           0x662200 };
static constexpr int N_MENU  = 3;
static constexpr int CARD_H  = 35;

static void drawMenu() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawBar("Key Fob Analyzer");

    // Cap count + freq selector hint
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(0x7A7A7A, C_BG);
    char info[40];
    snprintf(info, sizeof(info), "Captures: %d/%d   L/R=freq", s_capCount, MAX_CAPS);
    d.setCursor(4, STATUS_H + 3);
    d.print(info);

    for (int i = 0; i < N_MENU; i++) {
        int y = STATUS_H + 14 + i * (CARD_H + 2);
        bool sel = (i == s_menuSel);
        bool dim = (i == 1 && s_capCount < 2) || (i == 2 && s_capCount == 0);
        uint32_t col = dim ? (uint32_t)0x2A2A2A : MENU_COLS[i];
        uint32_t cardBg = sel ? col : (uint32_t)0x101010;
        uint32_t border = sel ? (uint32_t)0xFFD27A : (dim ? (uint32_t)0x2A2A2A : col);
        uint32_t textFg = sel ? (uint32_t)0xFFF7E8 : (dim ? (uint32_t)0x4A4A4A : col);
        uint32_t descFg = sel ? (uint32_t)0xFFE1B8 : (dim ? (uint32_t)0x444444 : (uint32_t)0x9A9A9A);

        d.fillRoundRect(0, y, SCREEN_W, CARD_H, 3, cardBg);
        d.drawRoundRect(0, y, SCREEN_W, CARD_H, 3, border);
        d.fillRect(0, y, 5, CARD_H, col);
        d.fillRect(6, y + 4, 2, CARD_H - 8, sel ? border : col);
        d.setTextColor(textFg, cardBg);
        d.setCursor(8, y + 5);
        d.print(MENU_LABELS[i]);
        if (i == 0 && s_capCount > 0) {
            // Show last capture bit count
            d.setTextColor(sel ? (uint32_t)0xFFF7B8 : (uint32_t)0xC08A2A, cardBg);
            char bc[16];
            snprintf(bc, sizeof(bc), "  last:%dbits", s_caps[s_capCount - 1].bitCount);
            d.print(bc);
        }
        d.setTextColor(descFg, cardBg);
        d.setCursor(8, y + 18);
        d.print(MENU_DESC[i]);
    }
}

// ── CAPTURE ─────────────────────────────────────────────────────────────────
static void drawCapture() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawBar("Capturing...");
    d.setFont(&fonts::Font0); d.setTextSize(1);

    if (s_capturing) {
        d.setTextSize(2);
        d.setTextColor(0xFF6A3A, C_BG);
        d.setCursor(20, 34); d.print("LISTENING");
        d.setTextSize(1);
        d.setTextColor(C_FG, C_BG);
        char buf[32];
        snprintf(buf, sizeof(buf), "Edges: %d", (int)s_rawCount);
        d.setCursor(8, 72); d.print(buf);
        snprintf(buf, sizeof(buf), "Time: %lus", (millis() - s_capStart) / 1000);
        d.setCursor(8, 86); d.print(buf);
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, 110); d.print("Press fob button now");
        d.setCursor(8, 122); d.print("Enter=stop  Back=cancel");
    } else {
        // Show decode result
        FobCap* cap = &s_caps[s_capCount - 1];
        if (cap->decoded) {
            d.setTextColor(0x33DD77, C_BG);
            d.setCursor(8, 30); d.print("Decoded OK!");
            d.setTextColor(C_FG, C_BG);
            char buf[40];
            snprintf(buf, sizeof(buf), "Bits: %d", cap->bitCount);
            d.setCursor(8, 46); d.print(buf);

            // Show raw hex of decoded bits
            d.setTextColor(C_DIM, C_BG);
            d.setCursor(8, 60); d.print("Data (hex):");
            d.setTextColor(0x33CCFF, C_BG);
            char hex[32] = {};
            int bytes = (cap->bitCount + 7) / 8;
            int pos = 0;
            for (int b = 0; b < bytes && pos < 28; b++) {
                snprintf(hex + pos, 3, "%02X", cap->bits[b]);
                pos += 2;
                if (b < bytes - 1) hex[pos++] = ' ';
            }
            d.setCursor(8, 72); d.print(hex);

            // KeeLoq field info if 64 bits
            if (cap->bitCount >= KEELOQ_TOTAL) {
                uint32_t hop    = 0, serial = 0;
                uint8_t  button = 0;
                for (int b = 0; b < 32; b++)
                    if ((cap->bits[b / 8] >> (b % 8)) & 1) hop |= (1u << b);
                for (int b = 0; b < 28; b++)
                    if ((cap->bits[(32 + b) / 8] >> ((32 + b) % 8)) & 1) serial |= (1u << b);
                for (int b = 0; b < 4; b++)
                    if ((cap->bits[(60 + b) / 8] >> ((60 + b) % 8)) & 1) button |= (1u << b);

                d.setTextColor(0xFFD166, C_BG);
                char kl[48];
                snprintf(kl, sizeof(kl), "Serial: 0x%07lX", (unsigned long)serial);
                d.setCursor(8, 88); d.print(kl);
                const char* btns[] = { "Lock", "Unlock", "Trunk", "Aux", "?" };
                snprintf(kl, sizeof(kl), "Button: %s  Hop: %08lX",
                    button < 4 ? btns[button] : btns[4], (unsigned long)hop);
                d.setCursor(8, 100); d.print(kl);
            }
            d.setTextColor(C_DIM, C_BG);
            d.setCursor(8, 116); d.print("Enter=another  Back=menu");
        } else {
            d.setTextColor(0xFF6A3A, C_BG);
            d.setCursor(8, 40); d.print("Decode failed.");
            d.setTextColor(C_DIM, C_BG);
            d.setCursor(8, 56); d.print("Try again: hold fob close,");
            d.setCursor(8, 68); d.print("press button clearly.");
            d.setCursor(8, 96); d.print("Enter=retry  Back=menu");
            // Remove the failed cap
            if (s_capCount > 0) s_capCount--;
        }
    }
}

// ── ANALYZE ─────────────────────────────────────────────────────────────────
static void drawAnalyze() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawBar("Analyze  Fixed=grn  Roll=org");
    d.setFont(&fonts::Font0); d.setTextSize(1);

    if (s_capCount < 2) {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, 48); d.print("Need 2+ captures.");
        d.setCursor(8, 64); d.print("Go to Capture first.");
        return;
    }

    computeFixed();
    int showBits = (s_compareBits > 66) ? 66 : s_compareBits;

    // KeeLoq field dividers (at bit 32 and 60)
    int divX32 = BIT_X0 + 32 * BIT_PX;
    int divX60 = BIT_X0 + 60 * BIT_PX;

    // Draw captures
    int baseY = STATUS_H + 2;
    for (int c = 0; c < s_capCount; c++) {
        int y = baseY + c * CAP_ROW_H;
        FobCap* cap = &s_caps[c];

        // Cap label
        d.setTextColor(0x555555, C_BG);
        char lbl[4]; snprintf(lbl, sizeof(lbl), "C%d", c + 1);
        // No room for label, use left border colour instead
        d.fillRect(0, y, 2, CAP_ROW_H - 1, 0x334433);

        // Bits
        for (int b = 0; b < showBits; b++) {
            int x   = BIT_X0 + b * BIT_PX;
            int byteIdx = b / 8;
            int bitIdx  = b % 8;
            int bit     = (cap->bits[byteIdx] >> bitIdx) & 1;
            bool fixed  = (s_fixedMask[byteIdx] >> bitIdx) & 1;

            uint32_t col;
            if (fixed) {
                col = bit ? (uint32_t)0x2DD96F : (uint32_t)0x0E311D; // fixed: bright/dim green
            } else {
                col = bit ? (uint32_t)0xFF8A1F : (uint32_t)0x351708; // rolling: bright/dim orange
            }
            d.fillRect(x, y, BIT_PX - 1, CAP_ROW_H - 2, col);
        }
    }

    // Field divider lines
    int fieldY = baseY;
    int fieldH = s_capCount * CAP_ROW_H;
    d.drawFastVLine(divX32, fieldY, fieldH, 0x555555);
    if (showBits >= 60) d.drawFastVLine(divX60, fieldY, fieldH, 0x555555);

    // Field labels below captures
    int labelY = baseY + s_capCount * CAP_ROW_H + 2;
    d.setTextColor(0x2DD96F, C_BG);
    d.setCursor(BIT_X0, labelY);
    d.print("HOP(32)");
    d.setTextColor(0x2DD96F, C_BG);
    d.setCursor(divX32 + 2, labelY);
    d.print("SERIAL(28)");
    if (showBits >= 60) {
        d.setTextColor(0x2DD96F, C_BG);
        d.setCursor(divX60 + 2, labelY);
        d.print("BTN");
    }

    // Serial number + counter delta
    int infoY = labelY + 11;
    if (s_capCount >= 2 && s_caps[0].bitCount >= KEELOQ_TOTAL) {
        auto getSerial = [](int ci) {
            uint32_t serial = 0;
            for (int b = 0; b < 28; b++)
                if ((s_caps[ci].bits[(32 + b) / 8] >> ((32 + b) % 8)) & 1)
                    serial |= (1u << b);
            return serial;
        };
        auto getHop = [](int ci) {
            uint32_t hop = 0;
            for (int b = 0; b < 32; b++)
                if ((s_caps[ci].bits[b / 8] >> (b % 8)) & 1)
                    hop |= (1u << b);
            return hop;
        };

        uint32_t serial = getSerial(0);
        char buf[48];
        d.setTextColor(0xFFD166, C_BG);
        snprintf(buf, sizeof(buf), "Ser:0x%07lX  Hops:%d",
            (unsigned long)serial, s_capCount);
        d.setCursor(2, infoY); d.print(buf);

        // Show hop code differences between first and last capture
        if (s_capCount >= 2) {
            uint32_t hop0 = getHop(0);
            uint32_t hopN = getHop(s_capCount - 1);
            infoY += 11;
            snprintf(buf, sizeof(buf), "Hop[0]:%08lX", (unsigned long)hop0);
            d.setTextColor(0x2DD96F, C_BG);
            d.setCursor(2, infoY); d.print(buf);
            infoY += 11;
            snprintf(buf, sizeof(buf), "Hop[%d]:%08lX", s_capCount - 1, (unsigned long)hopN);
            d.setTextColor(0xFF8A1F, C_BG);
            d.setCursor(2, infoY); d.print(buf);
        }
    }

    // Footer
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - 9);
    d.print("Back=menu");
}

// ── Capture control ──────────────────────────────────────────────────────────
static void beginCapture() {
    if (s_capCount >= MAX_CAPS) s_capCount = MAX_CAPS - 1; // overwrite oldest by shifting
    // Shift captures to make room if at max
    if (s_capCount == MAX_CAPS) {
        for (int i = 0; i < MAX_CAPS - 1; i++) s_caps[i] = s_caps[i + 1];
        s_capCount = MAX_CAPS - 1;
    }

    s_rawCount = 0;
    s_lastEdge = micros();
    startRx();
    pinMode(CC1101_GDO0_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(CC1101_GDO0_PIN), onEdge, CHANGE);
    s_capturing  = true;
    s_capStart   = millis();
}

static void finishCapture(bool cancel) {
    detachInterrupt(digitalPinToInterrupt(CC1101_GDO0_PIN));
    idleChip();
    ELECHOUSE_cc1101.Init();
    s_capturing = false;

    if (cancel || s_rawCount < 10) return;

    // Copy raw edges into new capture slot
    FobCap* cap = &s_caps[s_capCount];
    memset(cap, 0, sizeof(FobCap));
    cap->edgeCount = (s_rawCount < MAX_EDGES) ? s_rawCount : MAX_EDGES;
    for (int i = 0; i < cap->edgeCount; i++)
        cap->edges[i] = s_rawEdges[i];

    decodePWM(cap);
    s_capCount++;
}

// ── Public ──────────────────────────────────────────────────────────────────
void appKeyfobEnter() {
    s_state     = KFState::MENU;
    s_menuSel   = 0;
    s_dirty     = true;
    s_inited    = false;
    s_initError = false;
    s_capturing = false;
}

void appKeyfobLoop() {
    auto ev = readKeys();

    switch (s_state) {

    // ── MENU ────────────────────────────────────────────────────────────────
    case KFState::MENU:
        if (s_dirty) { drawMenu(); s_dirty = false; }
        if (s_initError) {
            if (ev.changed) { s_initError = false; s_dirty = true; }
            return;
        }
        if (!ev.changed) return;
        if (ev.back) { goHome(); return; }
        if (ev.up   && s_menuSel > 0)          { s_menuSel--; s_dirty = true; }
        if (ev.down && s_menuSel < N_MENU - 1) { s_menuSel++; s_dirty = true; }
        if (ev.left || ev.right) {
            s_freqSel = 1 - s_freqSel;
            s_dirty = true;
        }
        if (ev.enter) {
            if (!s_inited && !initChip()) {
                drawInitError();
                s_initError = true;
                break;
            }
            switch (s_menuSel) {
                case 0: // Capture
                    if (s_capCount < MAX_CAPS) {
                        beginCapture();
                        s_state = KFState::CAPTURE;
                        s_dirty = true;
                    }
                    break;
                case 1: // Analyze
                    if (s_capCount >= 2) { s_state = KFState::ANALYZE; s_dirty = true; }
                    break;
                case 2: // Clear
                    s_capCount = 0;
                    memset(s_caps, 0, sizeof(s_caps));
                    s_dirty = true;
                    break;
            }
        }
        break;

    // ── CAPTURE ─────────────────────────────────────────────────────────────
    case KFState::CAPTURE:
        if (s_dirty) { drawCapture(); s_dirty = false; }

        if (s_capturing) {
            // Auto-stop: got enough edges, or 8s timeout
            bool edgeFull = (s_rawCount >= MAX_EDGES);
            bool timeout  = (millis() - s_capStart > 8000);
            // Also auto-stop if we detect end-of-frame (long silence after getting data)
            bool gotData  = (s_rawCount > 20);
            bool silence  = gotData && (micros() - s_lastEdge > 200000); // 200ms silence

            if (edgeFull || timeout || silence) {
                finishCapture(false);
                s_dirty = true;
                return;
            }
            // Periodic display refresh
            static uint32_t s_lastDraw = 0;
            if (millis() - s_lastDraw > 200) { drawCapture(); s_lastDraw = millis(); }
        }

        if (!ev.changed) return;
        if (ev.back) {
            if (s_capturing) finishCapture(true);
            s_state = KFState::MENU; s_dirty = true; return;
        }
        if (ev.enter) {
            if (s_capturing) {
                finishCapture(false);
                s_dirty = true;
            } else {
                // Another capture or retry
                if (s_capCount < MAX_CAPS) {
                    beginCapture();
                    s_dirty = true;
                } else {
                    s_state = KFState::MENU; s_dirty = true;
                }
            }
        }
        break;

    // ── ANALYZE ─────────────────────────────────────────────────────────────
    case KFState::ANALYZE:
        if (s_dirty) { drawAnalyze(); s_dirty = false; }
        if (!ev.changed) return;
        if (ev.back) { s_state = KFState::MENU; s_dirty = true; }
        break;
    }
}
