#include "app_nrf24.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>
#include <RF24.h>

// ── Constants ──────────────────────────────────────────────────────────────
static constexpr int N_CH    = 126;   // 2400..2525 MHz
static constexpr int BAR_W   = 2;     // px per channel
static constexpr int N_SHOW  = (SCREEN_W / BAR_W) < N_CH ? (SCREEN_W / BAR_W) : N_CH;
static constexpr int SWEEP_SAMPLES = 20; // sweeps before clear
static constexpr int SPEC_PAD_X = 6;
static constexpr int BAR_Y0  = STATUS_H + 6;
static constexpr int BAR_H   = SCREEN_H - STATUS_H - 20;
static constexpr int LABEL_Y = SCREEN_H - 9;

// ── State ──────────────────────────────────────────────────────────────────
enum class NRF24State { MENU, SPECTRUM, SNIFF, MOUSEJACK };
static NRF24State   s_state   = NRF24State::MENU;
static int          s_menuSel = 0;
static bool         s_dirty   = true;
static bool         s_inited    = false;
static bool         s_initError = false;

// Spectrum
static uint8_t  s_hits[N_CH]  = {};  // hit count per channel (accumulates, then fades)
static uint8_t  s_sweepN      = 0;
static uint32_t s_lastUpdate  = 0;
static int      s_sniffCh     = 0;

// Sniff display
static constexpr int SNIFF_ROWS = 8;
struct SniffLine { char hex[96]; bool fresh; };
static SniffLine s_sniffBuf[SNIFF_ROWS] = {};
static int       s_sniffHead = 0;
static uint32_t  s_lastSniff = 0;
static bool      s_sniffDirty = false;

// Mousejack
struct MjDevice { uint8_t ch; uint8_t addr[5]; uint8_t len; };
static constexpr int MAX_MJ = 16;
static MjDevice  s_mjDev[MAX_MJ] = {};
static int       s_mjCount = 0;
static int       s_mjScanCh = 0;
static uint32_t  s_mjLastDraw = 0;

static RF24 radio(NRF24_CE_PIN, NRF24_CSN_PIN);

// ── Init-error overlay (non-blocking) ─────────────────────────────────────
static void drawInitError() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, STATUS_H, SCREEN_W, SCREEN_H - STATUS_H, C_BG);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(C_ERROR, C_BG);
    d.setCursor(8, 44); d.print("nRF24 not found!");
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(8, 58); d.print("Check nRF24 module.");
    d.setCursor(8, 80); d.print("Press any key...");
}

// ── Init ───────────────────────────────────────────────────────────────────
static bool initChip() {
    if (s_inited) return true;
#ifndef BOARD_TEMBED
    // On Cardputer SPI.begin() sets up the bus. On T-Embed LovyanGFX already owns
    // SPI2_HOST and the GPIO matrix; passing NRF24_CSN_PIN here would configure
    // GPIO44 as hardware SPI SS and drive it LOW during every transaction →
    // NRF24 contends on MISO while CC1101 is active → CC1101 freezes.
    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, NRF24_CSN_PIN);
#endif
    pinMode(NRF24_CSN_PIN, OUTPUT);
    digitalWrite(NRF24_CSN_PIN, HIGH);
    pinMode(NRF24_CE_PIN, OUTPUT);
    digitalWrite(NRF24_CE_PIN, LOW);
#if LORA_NSS_PIN >= 0
    pinMode(LORA_NSS_PIN, OUTPUT);
    digitalWrite(LORA_NSS_PIN, HIGH);
    pinMode(LORA_RST_PIN, OUTPUT);
    digitalWrite(LORA_RST_PIN, LOW);
#endif
    delay(10);
    if (!radio.begin()) return false;
    delay(5);
    if (!radio.isChipConnected()) return false;
    radio.setAutoAck(false);
    radio.setRetries(0, 0);
    radio.setPALevel(RF24_PA_MAX);
    radio.setDataRate(RF24_2MBPS);
    radio.setPayloadSize(32);
    radio.setCRCLength(RF24_CRC_DISABLED);
    s_inited = true;
    return true;
}

// ── Spectrum ───────────────────────────────────────────────────────────────
static void sweepSpectrum() {
    if (!s_inited) return;
    for (int ch = 0; ch < N_CH; ch++) {
        radio.setChannel(ch);
        radio.startListening();
        delayMicroseconds(130);
        radio.stopListening();
        if (radio.testRPD()) {
            if (s_hits[ch] < 255) s_hits[ch]++;
        }
    }
    s_sweepN++;
    if (s_sweepN >= SWEEP_SAMPLES) {
        // Fade all hits
        for (int i = 0; i < N_CH; i++) {
            if (s_hits[i] > 0) s_hits[i]--;
        }
        s_sweepN = 0;
    }
}

static void drawSpectrumBars() {
    auto& d = M5Cardputer.Display;
    uint8_t maxHit = 0;
    for (int i = 0; i < N_SHOW; i++) {
        if (s_hits[i] > maxHit) maxHit = s_hits[i];
    }
    if (maxHit < 3) maxHit = 3;
    for (int i = 0; i < N_SHOW; i++) {
        int h = (s_hits[i] * BAR_H) / maxHit;
        if (h > BAR_H) h = BAR_H;
        if (s_hits[i] > 0 && h < 2) h = 2;
        int x = SPEC_PAD_X + i * BAR_W;
        uint32_t col = (h > BAR_H * 2 / 3) ? (uint32_t)0xCDE7CD :
                       (h > BAR_H / 3)      ? (uint32_t)0x8EBA8E : (uint32_t)0x516A51;
        int boty = BAR_Y0 + BAR_H;
        if (h > 0) d.fillRect(x, boty - h, BAR_W, h, col);
        d.fillRect(x, BAR_Y0, BAR_W, BAR_H - h, 0x0A0D0A);
    }
}

static void drawSpectrumHeader() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, SCREEN_W, STATUS_H, 0x0A0F0A);
    d.fillRect(0, STATUS_H, SCREEN_W, SCREEN_H - STATUS_H, 0x040604);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(0xCFE9CF, 0x0A0F0A);
    d.setCursor(2, 3);
    d.print("NRF24 SCOPE  2.4G");
    d.setTextColor(0x6E8B6E, 0x0A0F0A);
    d.setCursor(SCREEN_W - 54, 3);
    d.print("live");

    d.drawRect(SPEC_PAD_X, BAR_Y0 - 2, SCREEN_W - SPEC_PAD_X * 2, BAR_H + 4, 0x4F654F);
    d.fillRect(SPEC_PAD_X + 1, BAR_Y0 - 1, SCREEN_W - SPEC_PAD_X * 2 - 2, BAR_H + 2, 0x0A0D0A);

    for (int gx = 1; gx < 4; ++gx) {
        int x = SPEC_PAD_X + ((SCREEN_W - SPEC_PAD_X * 2) * gx) / 4;
        d.drawFastVLine(x, BAR_Y0 + 1, BAR_H, 0x1A231A);
    }
    d.drawFastHLine(SPEC_PAD_X + 2, BAR_Y0 + BAR_H / 2, SCREEN_W - SPEC_PAD_X * 2 - 4, 0x314231);

    d.setTextColor(0x8EBA8E, 0x040604);
    d.setCursor(SPEC_PAD_X, LABEL_Y); d.print("2400");
    d.setCursor(SCREEN_W - 34, LABEL_Y); d.print("2525");
}

// ── Sniff ──────────────────────────────────────────────────────────────────
static void configSniff() {
    radio.stopListening();
    radio.setDataRate(RF24_2MBPS);
    radio.setChannel(s_sniffCh);
    radio.setAddressWidth(5);
    // Common broadcast address to catch more packets
    uint8_t addr[5] = { 0xE7, 0xE7, 0xE7, 0xE7, 0xE7 };
    radio.openReadingPipe(0, addr);
    radio.openReadingPipe(1, addr);
    radio.setPayloadSize(32);
    radio.startListening();
}

static void drawSniffHeader() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, SCREEN_W, STATUS_H, 0x000A1A);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(0x00AAFF, 0x000A1A);
    d.setCursor(2, 3);
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "nRF24 Sniff  ch%d (2%03dMHz)", s_sniffCh, 400 + s_sniffCh);
    d.print(hdr);
}

static void drawSniffFull() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawSniffHeader();
    d.setFont(&fonts::Font0); d.setTextSize(1);
    for (int i = 0; i < SNIFF_ROWS; i++) {
        int idx = (s_sniffHead - SNIFF_ROWS + i + SNIFF_ROWS) % SNIFF_ROWS;
        int y = STATUS_H + 2 + i * 14;
        d.fillRect(0, y, SCREEN_W, 13, C_BG);
        if (s_sniffBuf[idx].hex[0]) {
            uint32_t col = s_sniffBuf[idx].fresh ? (uint32_t)0x00EE44 : C_DIM;
            d.setTextColor(col, C_BG);
            d.setCursor(0, y + 2);
            d.print(s_sniffBuf[idx].hex);
            s_sniffBuf[idx].fresh = false;
        }
    }
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(0, SCREEN_H - 9);
    d.print("L/R=ch  Back=menu");
}

static void pushSniffLine(const uint8_t* buf, uint8_t len) {
    char* dst = s_sniffBuf[s_sniffHead].hex;
    int pos = 0;
    for (int i = 0; i < len && pos < (int)sizeof(s_sniffBuf[0].hex) - 3; i++) {
        snprintf(dst + pos, 3, "%02X", buf[i]);
        pos += 2;
        if ((i & 7) == 7 && i < len - 1) { dst[pos++] = '|'; }
        else if (i < len - 1) dst[pos++] = ' ';
    }
    dst[pos] = '\0';
    s_sniffBuf[s_sniffHead].fresh = true;
    s_sniffHead = (s_sniffHead + 1) % SNIFF_ROWS;
    s_sniffDirty = true;
}

// ── Mousejack ──────────────────────────────────────────────────────────────
static void configMousejack() {
    radio.stopListening();
    radio.setDataRate(RF24_2MBPS);
    radio.setAddressWidth(2);              // ESB promiscuous uses 2-byte width
    radio.setCRCLength(RF24_CRC_DISABLED); // no CRC check
    radio.setAutoAck(false);
    radio.setPayloadSize(32);
    uint64_t addr = 0x0000000055LL;
    radio.openReadingPipe(0, addr);
    addr = 0x00000000AALL;
    radio.openReadingPipe(1, addr);
    radio.startListening();
}

static bool isHidLike(const uint8_t* buf) {
    // ESB HID packets often start with 0x00 0x00 or device-type byte
    // Very basic heuristic: check for typical HID frame length indicators
    return (buf[1] == 0x00 || buf[1] == 0x01 || buf[1] == 0x11);
}

static void drawMousejack() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.fillRect(0, 0, SCREEN_W, STATUS_H, 0x1A000A);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(0xFF44AA, 0x1A000A);
    d.setCursor(2, 3);
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "Mousejack  scanning ch%d", s_mjScanCh);
    d.print(hdr);

    if (s_mjCount == 0) {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, 40);
        d.print("Scanning for unencrypted");
        d.setCursor(8, 54);
        d.print("HID/ESB devices...");
        d.setCursor(8, 80);
        d.print("Use wireless mouse/kbd");
        d.setCursor(8, 94);
        d.print("near device.");
    } else {
        d.setTextColor(0xFF44AA, C_BG);
        d.setCursor(8, 20);
        char buf[32];
        snprintf(buf, sizeof(buf), "%d device(s) found:", s_mjCount);
        d.print(buf);
        for (int i = 0; i < s_mjCount && i < 5; i++) {
            d.setTextColor(C_FG, C_BG);
            snprintf(buf, sizeof(buf), "ch%d  %02X:%02X:%02X:%02X:%02X  [%dB]",
                s_mjDev[i].ch,
                s_mjDev[i].addr[0], s_mjDev[i].addr[1], s_mjDev[i].addr[2],
                s_mjDev[i].addr[3], s_mjDev[i].addr[4],
                s_mjDev[i].len);
            d.setCursor(4, 32 + i * 14);
            d.print(buf);
        }
    }
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(0, SCREEN_H - 9);
    d.print("Enter=clear  Back=menu");
}

// ── Menu ───────────────────────────────────────────────────────────────────
static const char* MENU_LABELS[] = { "Spectrum",   "Sniff",           "Mousejack" };
static const char* MENU_DESC[]   = { "2.4GHz scan","Raw ESB capture", "HID device scan" };
static const uint32_t MENU_COLS[]= { 0x0044AA,     0x006633,          0xAA0044 };
static constexpr int N_MENU = 3;
static constexpr int CARD_H = 36;

static void drawMenu() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.fillRect(0, 0, SCREEN_W, STATUS_H, 0x000A1A);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(0x00AAFF, 0x000A1A);
    d.setCursor(2, 3);
    d.print("nRF24 2.4GHz");

    // Module status shown in title bar (right side)
    const char* status = s_inited ? "ready" : "?";
    int sw = strlen(status) * FONT_W;
    d.setTextColor(s_inited ? (uint32_t)0x00AA44 : (uint32_t)0x888888, 0x000A1A);
    d.setCursor(SCREEN_W - sw - 2, 3);
    d.print(status);

    for (int i = 0; i < N_MENU; i++) {
        int y = STATUS_H + 2 + i * (CARD_H + 2);
        bool sel = (i == s_menuSel);
        uint32_t col = MENU_COLS[i];
        d.fillRoundRect(0, y, SCREEN_W, CARD_H, 3, sel ? col : (uint32_t)0x111111);
        d.fillRect(0, y, 4, CARD_H, col);
        d.setTextColor(sel ? (uint32_t)0xFFFFFF : col, sel ? col : (uint32_t)0x111111);
        d.setCursor(8, y + 5);
        d.print(MENU_LABELS[i]);
        d.setTextColor(sel ? (uint32_t)0xDDDDDD : (uint32_t)0x555555, sel ? col : (uint32_t)0x111111);
        d.setCursor(8, y + 16);
        d.print(MENU_DESC[i]);
        if (i == 1) {
            char cha[12]; snprintf(cha, sizeof(cha), "Ch%d", s_sniffCh);
            d.setTextColor(sel ? (uint32_t)0xCCCCCC : (uint32_t)0x444444, sel ? col : (uint32_t)0x111111);
            d.setCursor(8, y + 26);
            d.print(cha);
        }
    }
}

// ── Public entry points ────────────────────────────────────────────────────
void appNrf24Enter() {
    readKeys();   // flush encoder-click carry-over from launcher
    s_state   = NRF24State::MENU;
    s_menuSel = 0;
    s_dirty   = true;
    s_inited    = false;   // do NOT call initChip() here — radio.begin() can crash if no module
    s_initError = false;
    memset(s_hits, 0, sizeof(s_hits));
    memset(s_sniffBuf, 0, sizeof(s_sniffBuf));
    s_sniffHead = 0;
    s_mjCount   = 0;
    s_mjScanCh  = 0;
    s_sniffDirty = false;
}

void appNrf24Loop() {
    auto& d = M5Cardputer.Display;
    auto ev = readKeys();

    switch (s_state) {

    case NRF24State::MENU:
        if (s_dirty) { drawMenu(); s_dirty = false; }
        // Non-blocking error screen: any key dismisses it
        if (s_initError) {
            if (ev.changed) { s_initError = false; s_dirty = true; }
            return;
        }
        if (!ev.changed) return;
        if (ev.back) { if (s_inited) radio.stopListening(); goHome(); return; }
        if (ev.up   && s_menuSel > 0)          { s_menuSel--; s_dirty = true; }
        if (ev.down && s_menuSel < N_MENU - 1) { s_menuSel++; s_dirty = true; }
        if (ev.enter) {
            if (!s_inited && !initChip()) {
                s_inited = false;
                drawInitError();
                s_initError = true;
                break;
            }
            switch (s_menuSel) {
                case 0:
                    memset(s_hits, 0, sizeof(s_hits));
                    s_sweepN = 0;
                    radio.stopListening();
                    s_state = NRF24State::SPECTRUM;
                    d.fillScreen(C_BG);
                    drawSpectrumHeader();
                    s_dirty = false;
                    break;
                case 1:
                    configSniff();
                    s_state = NRF24State::SNIFF;
                    s_dirty = true;
                    break;
                case 2:
                    s_mjCount  = 0;
                    s_mjScanCh = 0;
                    configMousejack();
                    s_state = NRF24State::MOUSEJACK;
                    s_dirty = true;
                    break;
            }
        }
        break;

    case NRF24State::SPECTRUM:
        sweepSpectrum();
        if (millis() - s_lastUpdate > 100) {
            drawSpectrumBars();
            s_lastUpdate = millis();
        }
        if (!ev.changed) return;
        if (ev.back) {
            if (s_inited) radio.stopListening();
            s_state = NRF24State::MENU; s_dirty = true;
        }
        break;

    case NRF24State::SNIFF:
        if (s_dirty) { drawSniffFull(); s_dirty = false; }
        // Poll for packets
        {
            uint8_t buf[32];
            if (radio.available()) {
                radio.read(buf, 32);
                pushSniffLine(buf, 32);
            }
        }
        if (s_sniffDirty && millis() - s_lastSniff > 150) {
            drawSniffFull();
            s_sniffDirty = false;
            s_lastSniff  = millis();
        }
        if (!ev.changed) return;
        if (ev.back) { if (s_inited) radio.stopListening(); s_state = NRF24State::MENU; s_dirty = true; return; }
#ifdef BOARD_TEMBED
        if (ev.up || ev.down) {
            if (s_inited) radio.stopListening();
            s_sniffCh = (s_sniffCh + N_CH + (ev.down ? 1 : -1)) % N_CH;
            configSniff(); drawSniffHeader();
        }
#else
        if (ev.left || ev.right) {
            if (s_inited) radio.stopListening();
            s_sniffCh = (s_sniffCh + N_CH + (ev.right ? 1 : -1)) % N_CH;
            configSniff();
            drawSniffHeader();
        }
#endif
        break;

    case NRF24State::MOUSEJACK: {
        if (s_dirty) { drawMousejack(); s_dirty = false; }

        // Hop channels and listen for HID-like ESB packets
        static uint32_t s_mjHopTime = 0;
        if (millis() - s_mjHopTime > 3) {
            radio.stopListening();
            s_mjScanCh = (s_mjScanCh + 1) % N_CH;
            radio.setChannel(s_mjScanCh);
            radio.startListening();
            s_mjHopTime = millis();
        }

        uint8_t buf[32];
        if (radio.available()) {
            radio.read(buf, 32);
            if (isHidLike(buf) && s_mjCount < MAX_MJ) {
                // Check if we already have this "address" (first 5 bytes)
                bool seen = false;
                for (int k = 0; k < s_mjCount; k++) {
                    if (s_mjDev[k].ch == s_mjScanCh &&
                        memcmp(s_mjDev[k].addr, buf, 5) == 0) { seen = true; break; }
                }
                if (!seen) {
                    s_mjDev[s_mjCount].ch  = s_mjScanCh;
                    s_mjDev[s_mjCount].len = 32;
                    memcpy(s_mjDev[s_mjCount].addr, buf, 5);
                    s_mjCount++;
                }
            }
        }

        if (millis() - s_mjLastDraw > 500) {
            drawMousejack();
            s_mjLastDraw = millis();
        }

        if (!ev.changed) return;
        if (ev.back) { if (s_inited) radio.stopListening(); s_state = NRF24State::MENU; s_dirty = true; return; }
        if (ev.enter) { s_mjCount = 0; s_dirty = true; }
        break;
    }
    }
}

