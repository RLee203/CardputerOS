#include "app_cc1101.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <RCSwitch.h>
#include <SD.h>

// ── Constants ──────────────────────────────────────────────────────────────
static constexpr int BAR_TOP = STATUS_H;
static constexpr int BAR_BOTTOM = SCREEN_H - 10;
static constexpr int SPECTRUM_CENTER_Y = BAR_TOP + ((BAR_BOTTOM - BAR_TOP) / 2);
static constexpr int SPEC_PAD_X = 6;
static constexpr int SPEC_BOX_Y = STATUS_H + 4;
static constexpr int SPEC_BOX_H = SCREEN_H - STATUS_H - 18;
static constexpr int MAX_EDGES = 512;
static constexpr int TIME_DIVIDER = 18;
static constexpr uint8_t REG_IOCFG0 = 0x02;
static constexpr uint8_t REG_PKTCTRL0 = 0x08;
static constexpr uint8_t STR_SIDLE = 0x36;
static constexpr uint8_t STR_STX = 0x35;
static constexpr uint8_t STR_SRX = 0x34;
static constexpr uint8_t STR_SFRX = 0x3A;

// ── Frequency bands ────────────────────────────────────────────────────────
struct Band { const char* name; float minMHz; float maxMHz; float capMHz; };
static const Band BANDS[] = {
    { "315MHz", 312.0f, 318.0f, 315.0f },
    { "433MHz", 430.0f, 436.0f, 433.92f },
    { "868MHz", 863.0f, 870.0f, 868.35f },
    { "915MHz", 908.0f, 925.0f, 915.0f },
    { "Full",   300.0f, 928.0f, 433.92f },
    { "Custom", 0.0f,   0.0f,   0.0f   },
};
static constexpr int N_BANDS = 6;
static constexpr int N_BANDS_PRESET = 5;

// ── State ──────────────────────────────────────────────────────────────────
enum class CC1101State { MENU, SPECTRUM, DETECT, CAPTURE, REPLAY, REPLAY_LIST, FREQ_INPUT };
static CC1101State s_state = CC1101State::MENU;
static int s_menuSel = 0;
static int s_bandSel = 0;
static bool s_dirty = true;
static bool s_inited = false;
static bool s_initError = false;
static bool s_specFrameDirty = true;
static bool s_receiverEnabled = false;
static bool s_spectrumConfigured = false;

// Spectrum
static uint32_t s_lastSweep = 0;
static uint32_t s_lastWaveDraw = 0;
#ifdef BOARD_TEMBED
static uint16_t s_specEdges[MAX_EDGES];  // persistent last-captured waveform for display
static int      s_specEdgeCount = 0;
#endif

// Custom frequency
static float s_customMHz = 433.92f;
static char s_freqInput[12] = "433.920";
static int s_freqLen = 7;

// Capture / replay
static uint16_t s_edges[MAX_EDGES];
static int s_edgeCount = 0;
static bool s_capturing = false;
static bool s_hasCap = false;
static uint32_t s_captureStart = 0;
static uint32_t s_lastSignalCount = 0;
static uint64_t s_lastDecoded = 0;
static uint16_t s_lastBitLength = 0;
static uint16_t s_lastTransitions = 0;
static uint16_t s_lastPulse = 0;
static char s_lastProtocol[20] = "RAW(0)";
static char s_lastSummary[48] = "No code identified";
static char s_lastCrc[32] = "No code identified";
static int s_detectRepeats = 0;
static uint16_t s_detectMinPulse = 0;
static uint16_t s_detectMaxPulse = 0;
static uint32_t s_detectBurstUs = 0;
static char s_detectMod[24] = "OOK / ASK-like";
static String s_lastSaveFile;
static String s_replayFiles[32];
static int s_replayFileCount = 0;
static int s_replayFileSel = 0;
static RCSwitch s_rcswitch;
static String s_toastMsg;
static uint32_t s_toastUntil = 0;

// ── Antenna switch (T-Embed CC1101 Plus: GPIO47=SW1, GPIO48=SW0) ──────────
#ifdef BOARD_TEMBED
static void setAntennaSwitch(float mhz) {
    // Routing per LILYGO official example (BOARD_LORA_SW1=47, BOARD_LORA_SW0=48)
    if (mhz < 400.0f) {          // 315 MHz
        digitalWrite(CC1101_SW1_PIN, HIGH);
        digitalWrite(CC1101_SW0_PIN, LOW);
    } else if (mhz < 600.0f) {   // 434 MHz
        digitalWrite(CC1101_SW1_PIN, HIGH);
        digitalWrite(CC1101_SW0_PIN, HIGH);
    } else {                      // 868 / 915 MHz
        digitalWrite(CC1101_SW1_PIN, LOW);
        digitalWrite(CC1101_SW0_PIN, HIGH);
    }
}
#endif

// ── CC1101 init ────────────────────────────────────────────────────────────
static bool initChip() {
    if (s_inited) return true;
#ifdef BOARD_TEMBED
    pinMode(CC1101_SW0_PIN, OUTPUT);
    pinMode(CC1101_SW1_PIN, OUTPUT);
    digitalWrite(CC1101_SW0_PIN, LOW);
    digitalWrite(CC1101_SW1_PIN, LOW);
#endif
    ELECHOUSE_cc1101.setSpiPin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, CC1101_CS_PIN);
    ELECHOUSE_cc1101.setGDO0(CC1101_GDO0_PIN);
    ELECHOUSE_cc1101.Init();
    delay(20);
    bool found = false;
    for (int attempt = 0; attempt < 5; attempt++) {
        if (ELECHOUSE_cc1101.getCC1101()) { found = true; break; }
        delay(20);
    }
    if (!found) return false;
    s_inited = true;
    return true;
}

static void idleChip() {
    ELECHOUSE_cc1101.SpiStrobe(STR_SIDLE);
}

static void enableReceiver() {
    if (!s_receiverEnabled) {
        s_rcswitch.enableReceive(CC1101_GDO0_PIN);
        s_receiverEnabled = true;
    }
}

static void disableReceiver() {
    if (s_receiverEnabled) {
        s_rcswitch.disableReceive();
        s_receiverEnabled = false;
    }
}

static void showToast(const String& msg, uint32_t ms = 1400) {
    s_toastMsg = msg;
    s_toastUntil = millis() + ms;
}

static void clearCaptureState() {
    s_edgeCount = 0;
    s_lastTransitions = 0;
    s_lastDecoded = 0;
    s_lastBitLength = 0;
    s_lastPulse = 0;
    s_lastSignalCount = 0;
    s_hasCap = false;
    s_capturing = false;
    s_lastSaveFile = "";
    s_detectRepeats = 0;
    s_detectMinPulse = 0;
    s_detectMaxPulse = 0;
    s_detectBurstUs = 0;
    strcpy(s_detectMod, "OOK / ASK-like");
    strcpy(s_lastProtocol, "RAW(0)");
    strcpy(s_lastSummary, "No code identified");
    strcpy(s_lastCrc, "No code identified");
}

static void drawToast() {
    if (!s_toastUntil || millis() >= s_toastUntil) return;
    auto& d = M5Cardputer.Display;
    int w = (int)s_toastMsg.length() * FONT_W + 12;
    if (w > SCREEN_W - 8) w = SCREEN_W - 8;
    d.fillRoundRect(4, SCREEN_H - 24, w, 18, 4, 0x222222);
    d.drawRoundRect(4, SCREEN_H - 24, w, 18, 4, C_ACCENT);
    d.setTextColor(C_INPUT, 0x222222);
    d.setCursor(10, SCREEN_H - 20);
    d.print(s_toastMsg);
}

// ── Band helpers ───────────────────────────────────────────────────────────
static float bandMin() { return (s_bandSel < N_BANDS_PRESET) ? BANDS[s_bandSel].minMHz : s_customMHz - 5.0f; }
static float bandMax() { return (s_bandSel < N_BANDS_PRESET) ? BANDS[s_bandSel].maxMHz : s_customMHz + 5.0f; }
static float capFreq() { return (s_bandSel < N_BANDS_PRESET) ? BANDS[s_bandSel].capMHz : s_customMHz; }
static const char* bandName() { return BANDS[s_bandSel].name; }

static void configOokRx(float mhz) {
    idleChip();
#ifdef BOARD_TEMBED
    setAntennaSwitch(mhz);
#endif
    ELECHOUSE_cc1101.setMHZ(mhz);   // Clb() called internally — do NOT override with setClb()
    ELECHOUSE_cc1101.setModulation(2);    // OOK/ASK
    ELECHOUSE_cc1101.setRxBW(203);        // 203 kHz: wide enough for crystal drift
    ELECHOUSE_cc1101.setDcFilterOff(1);   // disable DC filter for OOK
    ELECHOUSE_cc1101.SpiWriteReg(REG_PKTCTRL0, 0x32); // async serial, infinite packet
    ELECHOUSE_cc1101.SpiWriteReg(REG_IOCFG0, 0x0D);   // GDO0 = demodulated OOK output
    ELECHOUSE_cc1101.SpiStrobe(STR_SFRX);
    ELECHOUSE_cc1101.SetRx();
    disableReceiver();
    enableReceiver();
    s_rcswitch.resetAvailable();
}

// ── Spectrum ───────────────────────────────────────────────────────────────
static void drawSpectrumFrame() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.fillRect(0, 0, SCREEN_W, STATUS_H, 0x0A0F0A);
    d.fillRect(0, STATUS_H, SCREEN_W, SCREEN_H - STATUS_H, 0x040604);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(0xCFE9CF, 0x0A0F0A);
    d.setCursor(2, 3);
    char hdr[40];
#ifdef BOARD_TEMBED
    snprintf(hdr, sizeof(hdr), "CC1101 RSSI  %s", bandName());
#else
    snprintf(hdr, sizeof(hdr), "CC1101 SCOPE  %s", bandName());
#endif
    d.print(hdr);
    d.setTextColor(0x6E8B6E, 0x0A0F0A);
    d.setCursor(SCREEN_W - 54, 3);
    d.print("live");

    d.drawRect(SPEC_PAD_X, SPEC_BOX_Y, SCREEN_W - SPEC_PAD_X * 2, SPEC_BOX_H, 0x4F654F);
    d.fillRect(SPEC_PAD_X + 1, SPEC_BOX_Y + 1, SCREEN_W - SPEC_PAD_X * 2 - 2, SPEC_BOX_H - 2, 0x0A0D0A);

    int centerY = SPEC_BOX_Y + SPEC_BOX_H / 2;
    d.drawFastHLine(SPEC_PAD_X + 3, centerY, SCREEN_W - SPEC_PAD_X * 2 - 6, 0x314231);
    for (int gx = 1; gx < 5; ++gx) {
        int x = SPEC_PAD_X + ((SCREEN_W - SPEC_PAD_X * 2) * gx) / 5;
        d.drawFastVLine(x, SPEC_BOX_Y + 3, SPEC_BOX_H - 6, 0x1A231A);
    }

    d.setTextColor(0x8EBA8E, 0x040604);
    d.setCursor(SPEC_PAD_X, SCREEN_H - 10);
    d.print(String((int)bandMin()) + "M");
    d.setCursor(SCREEN_W - 42, SCREEN_H - 10);
    d.print(String((int)bandMax()) + "M");
    drawToast();
}

static void drawSpectrumWaveform() {
    bool rawReady = s_rcswitch.RAWavailable();
    bool decodedReady = s_rcswitch.available();
    if (!rawReady && !decodedReady) return;

    auto& d = M5Cardputer.Display;
    unsigned int* raw = decodedReady ? s_rcswitch.getReceivedRawdata()
                                     : s_rcswitch.getRAWReceivedRawdata();
    d.fillRect(SPEC_PAD_X + 1, SPEC_BOX_Y + 1, SCREEN_W - SPEC_PAD_X * 2 - 2, SPEC_BOX_H - 2, 0x0A0D0A);
    int centerY = SPEC_BOX_Y + SPEC_BOX_H / 2;
    d.drawFastHLine(SPEC_PAD_X + 3, centerY, SCREEN_W - SPEC_PAD_X * 2 - 6, 0x314231);
    for (int gx = 1; gx < 5; ++gx) {
        int x = SPEC_PAD_X + ((SCREEN_W - SPEC_PAD_X * 2) * gx) / 5;
        d.drawFastVLine(x, SPEC_BOX_Y + 3, SPEC_BOX_H - 6, 0x1A231A);
    }

    int lineX = SPEC_PAD_X + 6;
    int lineY = centerY - 7;
    for (int i = 0; i < RCSWITCH_RAW_MAX_CHANGES - 1; i += 2) {
        if (raw[i] == 0) break;
        unsigned int hi = raw[i] > 20000 ? 20000 : raw[i];
        unsigned int lo = raw[i + 1] > 20000 ? 20000 : raw[i + 1];
        int hiW = max(1, (int)(hi / TIME_DIVIDER));
        int loW = max(1, (int)(lo / TIME_DIVIDER));
        if (lineX + hiW + loW > SCREEN_W - SPEC_PAD_X - 6) break;

        d.drawFastVLine(lineX, lineY, 6, 0xCDE7CD);
        d.drawFastHLine(lineX, lineY, hiW, 0xCDE7CD);
        d.drawFastVLine(lineX + hiW, lineY, 6, 0xCDE7CD);
        d.drawFastHLine(lineX + hiW, lineY + 6, loW, 0x9FBA9F);
        lineX += hiW + loW;
    }
    s_rcswitch.resetAvailable();
    drawToast();
}

#ifdef BOARD_TEMBED
// ── T-Embed spectrum: persistent last-waveform + live RSSI when no signal ──
static void drawSpectrumBoxTembed() {
    auto& d = M5Cardputer.Display;
    int boxX = SPEC_PAD_X + 1, boxY = SPEC_BOX_Y + 1;
    int boxW = SCREEN_W - SPEC_PAD_X * 2 - 2, boxH = SPEC_BOX_H - 2;
    d.fillRect(boxX, boxY, boxW, boxH, 0x0A0D0A);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    // Green "listening" dot (always active in spectrum mode)
    d.fillCircle(boxX + boxW - 5, boxY + 5, 3, 0x003311);
    d.fillCircle(boxX + boxW - 5, boxY + 5, 2, 0x00EE44);

    if (s_specEdgeCount < 8) {
        int rssi = ELECHOUSE_cc1101.getRssi();
        char buf[32];
        snprintf(buf, sizeof(buf), "Listening...  %d dBm", rssi);
        d.setTextColor(C_DIM, 0x0A0D0A);
        d.setCursor(boxX + 4, boxY + boxH / 2 - 4);
        d.print(buf);
        d.setCursor(boxX + 4, boxY + boxH - 11);
        d.print("up/dn=band  back=exit");
        return;
    }

    // Draw persistent waveform from last good capture
    int lineW = boxW - 4;
    uint32_t totalUs = 0;
    for (int i = 0; i < s_specEdgeCount; i++) totalUs += s_specEdges[i];
    if (totalUs == 0) return;

    int lineX = boxX + 2;
    int centerY = boxY + boxH / 2;
    int lineH = min(boxH / 2 - 4, 16);
    bool level = true;
    for (int i = 0; i < s_specEdgeCount; i++) {
        int w = max(1, (int)((uint32_t)s_specEdges[i] * (uint32_t)lineW / totalUs));
        if (lineX + w > boxX + boxW - 2) break;
        if (level) {
            d.drawFastHLine(lineX, centerY - lineH, w, 0xCDE7CD);
            d.drawFastVLine(lineX, centerY - lineH, lineH, 0xCDE7CD);
            if (lineX + w + 1 < boxX + boxW - 2)
                d.drawFastVLine(lineX + w, centerY - lineH, lineH, 0xCDE7CD);
        } else {
            d.drawFastHLine(lineX, centerY, w, 0x9FBA9F);
        }
        lineX += w;
        level = !level;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "%d edges  %ldus", s_specEdgeCount, (long)totalUs);
    d.setTextColor(C_DIM, 0x0A0D0A);
    d.setCursor(boxX + 4, boxY + boxH - 11);
    d.print(buf);
}
#endif

#ifdef BOARD_TEMBED
// ── T-Embed RF capture: bounded ISR, no RCSwitch flood ───────────────────
static volatile uint16_t s_gdo0Buf[MAX_EDGES];
static volatile int32_t  s_gdo0BufCnt = 0;
static volatile uint32_t s_gdo0LastUs  = 0;
static volatile uint32_t s_gdo0StartUs = 0;
static volatile uint32_t s_gdo0IdleSinceUs = 0;
static volatile uint16_t s_gdo0MinUs = 0xFFFF;
static volatile uint16_t s_gdo0MaxUs = 0;
static constexpr uint32_t T_EMBED_MIN_EDGE_US = 80;
static constexpr uint32_t T_EMBED_MAX_EDGE_US = 20000;
static constexpr uint32_t T_EMBED_BURST_GAP_US = 18000;
static constexpr int T_EMBED_MIN_WAVE_EDGES = 12;
static constexpr int T_EMBED_MAX_WAVE_EDGES = 128;
static constexpr int T_EMBED_MIN_SHARED_EDGES = 12;
static constexpr int T_EMBED_MAX_SHARED_EDGES = 32;
static constexpr uint32_t T_EMBED_MIN_SHARED_ACTIVE_US = 7000;
static constexpr uint32_t T_EMBED_MAX_SHARED_ACTIVE_US = 30000;
static constexpr uint16_t T_EMBED_MIN_SHARED_MAX_PULSE_US = 1400;
static constexpr uint16_t T_EMBED_MAX_SHARED_MAX_PULSE_US = 7000;
static constexpr bool T_EMBED_RX_DEBUG = true;

static bool isAmbientFalseBurst(int edges, uint32_t activeUs, uint16_t maxUs) {
    return edges >= 18 &&
           edges <= 21 &&
           activeUs >= 8500 &&
           activeUs <= 9500 &&
           maxUs >= 1800 &&
           maxUs <= 4500;
}

static bool isValidTembedBurst(int edges, uint32_t activeUs, uint16_t maxUs) {
    return edges >= T_EMBED_MIN_SHARED_EDGES &&
           edges <= T_EMBED_MAX_SHARED_EDGES &&
           activeUs >= T_EMBED_MIN_SHARED_ACTIVE_US &&
           activeUs <= T_EMBED_MAX_SHARED_ACTIVE_US &&
           maxUs >= T_EMBED_MIN_SHARED_MAX_PULSE_US &&
           maxUs <= T_EMBED_MAX_SHARED_MAX_PULSE_US &&
           !isAmbientFalseBurst(edges, activeUs, maxUs);
}


static void logTembedRx(const char* tag, int32_t cnt, uint32_t burstUs, uint32_t gapUs,
                        uint16_t minUs, uint16_t maxUs) {
    if (!T_EMBED_RX_DEBUG) return;
    Serial.printf("[tembed-rx] %s cnt=%ld burst=%lu lastGap=%lu min=%u max=%u avg=%lu\n",
                  tag,
                  (long)cnt,
                  (unsigned long)burstUs,
                  (unsigned long)gapUs,
                  (unsigned)(minUs == 0xFFFF ? 0 : minUs),
                  (unsigned)maxUs,
                  (unsigned long)(cnt > 0 ? (burstUs / (uint32_t)cnt) : 0));
}

static void resetTembedRawBuffer() {
    noInterrupts();
    s_gdo0BufCnt = 0;
    s_gdo0LastUs = 0;
    s_gdo0StartUs = 0;
    s_gdo0IdleSinceUs = 0;
    s_gdo0MinUs = 0xFFFF;
    s_gdo0MaxUs = 0;
    interrupts();
}

static uint32_t snapshotTembedActiveUs(int32_t cnt) {
    uint32_t activeUs = 0;
    if (cnt <= 0) return 0;
    noInterrupts();
    int32_t limit = min<int32_t>(cnt, MAX_EDGES);
    for (int32_t i = 0; i < limit; ++i) activeUs += s_gdo0Buf[i];
    interrupts();
    return activeUs;
}

static void IRAM_ATTR onGdo0Edge() {
    uint32_t now = micros();
    uint32_t dur = now - s_gdo0LastUs;
    if (!s_gdo0StartUs) s_gdo0StartUs = now;
    if (s_gdo0LastUs && s_gdo0BufCnt < MAX_EDGES && dur >= T_EMBED_MIN_EDGE_US && dur <= T_EMBED_MAX_EDGE_US) {
        s_gdo0Buf[s_gdo0BufCnt++] = (uint16_t)dur;
        if (dur < s_gdo0MinUs) s_gdo0MinUs = (uint16_t)dur;
        if (dur > s_gdo0MaxUs) s_gdo0MaxUs = (uint16_t)dur;
    }
    s_gdo0LastUs = now;
    s_gdo0IdleSinceUs = 0;
}

static void startCaptureTembed() {
    idleChip();
    setAntennaSwitch(capFreq());
    ELECHOUSE_cc1101.setMHZ(capFreq());
    ELECHOUSE_cc1101.setModulation(2);
    ELECHOUSE_cc1101.setRxBW(203);
    ELECHOUSE_cc1101.setDcFilterOff(1);
    ELECHOUSE_cc1101.SpiWriteReg(REG_PKTCTRL0, 0x32);
    ELECHOUSE_cc1101.SpiWriteReg(REG_IOCFG0, 0x0D);
    ELECHOUSE_cc1101.SpiStrobe(STR_SFRX);
    ELECHOUSE_cc1101.SetRx();
    resetTembedRawBuffer();
    clearCaptureState();
    attachInterrupt(digitalPinToInterrupt(CC1101_GDO0_PIN), onGdo0Edge, CHANGE);
    s_hasCap      = false;
    s_capturing   = true;
    s_captureStart = millis();
}

static bool pollCaptureTembed(bool relaxed = false) {
    noInterrupts();
    int32_t cnt = s_gdo0BufCnt;
    uint32_t last = s_gdo0LastUs;
    uint32_t started = s_gdo0StartUs;
    uint32_t idleSince = s_gdo0IdleSinceUs;
    uint16_t minUs = s_gdo0MinUs;
    uint16_t maxUs = s_gdo0MaxUs;
    interrupts();
    if (cnt >= MAX_EDGES - 1) return true;
    if (!last || !started) return false;

    uint32_t now = micros();
    if (!idleSince && (uint32_t)(now - last) > T_EMBED_BURST_GAP_US) {
        noInterrupts();
        if (!s_gdo0IdleSinceUs && s_gdo0LastUs == last) s_gdo0IdleSinceUs = now;
        idleSince = s_gdo0IdleSinceUs;
        interrupts();
    }
    if (!idleSince) return false;

    uint32_t activeUs = snapshotTembedActiveUs(cnt);
    bool valid = isValidTembedBurst((int)cnt, activeUs, maxUs);
    if (!valid) {
        logTembedRx(relaxed ? "reject copy" : "reject detect", cnt, activeUs, (now - last), minUs, maxUs);
        resetTembedRawBuffer();
        return false;
    }
    logTembedRx(relaxed ? "accept copy" : "accept detect", cnt, activeUs, (now - last), minUs, maxUs);
    return true;
}

static bool pollSpectrumCaptureTembed() {
    noInterrupts();
    int32_t cnt = s_gdo0BufCnt;
    uint32_t last = s_gdo0LastUs;
    uint32_t started = s_gdo0StartUs;
    uint32_t idleSince = s_gdo0IdleSinceUs;
    uint16_t minUs = s_gdo0MinUs;
    uint16_t maxUs = s_gdo0MaxUs;
    interrupts();
    if (cnt >= MAX_EDGES - 1) return true;
    if (!last || !started) return false;

    uint32_t now = micros();
    if (!idleSince && (uint32_t)(now - last) > T_EMBED_BURST_GAP_US) {
        noInterrupts();
        if (!s_gdo0IdleSinceUs && s_gdo0LastUs == last) s_gdo0IdleSinceUs = now;
        idleSince = s_gdo0IdleSinceUs;
        interrupts();
    }
    if (!idleSince) return false;

    uint32_t activeUs = snapshotTembedActiveUs(cnt);
    if (!isValidTembedBurst((int)cnt, activeUs, maxUs)) {
        logTembedRx("reject spectrum", cnt, activeUs, (now - last), minUs, maxUs);
        resetTembedRawBuffer();
        return false;
    }
    if (cnt > T_EMBED_MAX_WAVE_EDGES) logTembedRx("trim spectrum", cnt, activeUs, (now - last), minUs, maxUs);
    logTembedRx("accept spectrum", cnt, activeUs, (now - last), minUs, maxUs);
    return true;
}
static void stopCaptureTembed(bool keepCapture = true, bool allowRawKeep = false) {
    detachInterrupt(digitalPinToInterrupt(CC1101_GDO0_PIN));
    idleChip();
    s_capturing = false;
    noInterrupts();
    int cnt = (int)s_gdo0BufCnt;
    uint32_t started = s_gdo0StartUs;
    uint32_t idleSince = s_gdo0IdleSinceUs;
    uint32_t last = s_gdo0LastUs;
    uint16_t minUs = s_gdo0MinUs;
    uint16_t maxUs = s_gdo0MaxUs;
    interrupts();
    if (!idleSince && last) {
        uint32_t now = micros();
        if ((uint32_t)(now - last) > T_EMBED_BURST_GAP_US) idleSince = now;
    }
    s_edgeCount = cnt < MAX_EDGES ? cnt : MAX_EDGES;
    for (int i = 0; i < s_edgeCount; i++) s_edges[i] = s_gdo0Buf[i];
    s_lastTransitions = s_edgeCount;
    uint32_t activeUs = 0;
    for (int i = 0; i < s_edgeCount; i++) activeUs += s_edges[i];
    bool fullCapture = (s_edgeCount >= MAX_EDGES - 1);
    bool detectCapture = isValidTembedBurst(s_edgeCount, activeUs, maxUs);
    bool copyCapture = detectCapture;
    s_hasCap = fullCapture || detectCapture || (allowRawKeep && copyCapture);
    if (!keepCapture) s_hasCap = false;
    if (T_EMBED_RX_DEBUG) {
        Serial.printf("[tembed-rx] stop keep=%d edges=%d burst=%lu full=%d detectCap=%d copyCap=%d hasCap=%d rawKeep=%d\n",
                      keepCapture ? 1 : 0,
                      s_edgeCount,
                      (unsigned long)activeUs,
                      fullCapture ? 1 : 0,
                      detectCapture ? 1 : 0,
                      copyCapture ? 1 : 0,
                      s_hasCap ? 1 : 0,
                      allowRawKeep ? 1 : 0);
    }
    if (!s_hasCap) { clearCaptureState(); return; }
    s_lastSignalCount++;
    uint16_t minP = 0xFFFF, maxP = 0;
    for (int i = 0; i < s_edgeCount; i++) {
        if (s_edges[i] < minP) minP = s_edges[i];
        if (s_edges[i] > maxP) maxP = s_edges[i];
    }
    s_detectMinPulse = (minP == 0xFFFF) ? 0 : minP;
    s_detectMaxPulse = maxP;
    s_detectBurstUs  = activeUs;
    s_detectRepeats  = 1;
    strcpy(s_detectMod, "OOK/ASK raw");
    snprintf(s_lastProtocol, sizeof(s_lastProtocol), "RAW(%d)", s_edgeCount);
    snprintf(s_lastSummary, sizeof(s_lastSummary), "%d edges", s_edgeCount);
    strcpy(s_lastCrc, "raw capture");
    s_lastDecoded   = 0;
    s_lastBitLength = 0;
    s_lastPulse = s_edgeCount ? (uint16_t)(activeUs / (uint32_t)s_edgeCount) : 0;
}
#endif

static String rfCaptureFilename() {
    char fallback[32];
    snprintf(fallback, sizeof(fallback), "/rf_%08lu.txt", millis());
    return String(RF_DIR) + fallback;
}

static bool beginSdAccess() {
    disableReceiver();
    idleChip();
    delay(20);
    pinMode(CC1101_CS_PIN, OUTPUT);
    digitalWrite(CC1101_CS_PIN, HIGH);
#if NRF24_CSN_PIN >= 0
    pinMode(NRF24_CSN_PIN, OUTPUT);
    digitalWrite(NRF24_CSN_PIN, HIGH);
#endif
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);
    SD.end();
#ifndef BOARD_TEMBED
    SPI.end();
    delay(5);
    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
#else
    delay(5);
    // LovyanGFX owns SPI2_HOST — don't teardown; CC1101 init already called SPI.begin()
#endif
    return SD.begin(SD_CS_PIN, SPI, 25000000);
}

static void endSdAccessAndRestoreRf() {
    SD.end();
#ifndef BOARD_TEMBED
    SPI.end();
    delay(5);
    configOokRx(capFreq());
#else
    delay(5);
    s_inited = false;  // force CC1101 re-init; caller must call initChip() to restore
#endif
}

static bool saveCaptureToSd() {
    if (!s_hasCap || s_edgeCount < 2) return false;
    if (!beginSdAccess()) return false;
    if (!SD.exists(RF_DIR) && !SD.mkdir(RF_DIR)) {
        endSdAccessAndRestoreRf();
        return false;
    }

    s_lastSaveFile = rfCaptureFilename();
    File file = SD.open(s_lastSaveFile, FILE_WRITE);
    if (!file) {
        endSdAccessAndRestoreRf();
        return false;
    }

    file.printf("freq_mhz=%.3f\n", capFreq());
    file.printf("band=%s\n", bandName());
    file.printf("protocol=%s\n", s_lastProtocol);
    file.printf("summary=%s\n", s_lastSummary);
    file.printf("crc=%s\n", s_lastCrc);
    file.printf("decoded_value=%llu\n", s_lastDecoded);
    file.printf("bit_length=%u\n", (unsigned)s_lastBitLength);
    file.printf("pulse_length=%u\n", (unsigned)s_lastPulse);
    file.printf("transitions=%u\n", (unsigned)s_lastTransitions);
    file.print("raw_us=");
    for (int i = 0; i < s_edgeCount; ++i) {
        if (i) file.print(',');
        file.print((unsigned)s_edges[i]);
    }
    file.println();
    file.close();
    endSdAccessAndRestoreRf();
    return true;
}

static bool loadCaptureFromFile(const String& path) {
    if (path.isEmpty()) return false;
    if (!beginSdAccess()) return false;

    File file = SD.open(path, FILE_READ);
    if (!file) {
        endSdAccessAndRestoreRf();
        return false;
    }

    clearCaptureState();
    s_lastSaveFile = path;

    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.startsWith("freq_mhz=")) {
            float f = line.substring(9).toFloat();
            if (f >= 300.0f && f <= 928.0f) {
                s_customMHz = f;
                s_bandSel = N_BANDS - 1;
            }
        } else if (line.startsWith("protocol=")) {
            String v = line.substring(9);
            v.toCharArray(s_lastProtocol, sizeof(s_lastProtocol));
        } else if (line.startsWith("summary=")) {
            String v = line.substring(8);
            v.toCharArray(s_lastSummary, sizeof(s_lastSummary));
        } else if (line.startsWith("crc=")) {
            String v = line.substring(4);
            v.toCharArray(s_lastCrc, sizeof(s_lastCrc));
        } else if (line.startsWith("decoded_value=")) {
            s_lastDecoded = strtoull(line.substring(14).c_str(), nullptr, 10);
        } else if (line.startsWith("bit_length=")) {
            s_lastBitLength = (uint16_t)line.substring(11).toInt();
        } else if (line.startsWith("pulse_length=")) {
            s_lastPulse = (uint16_t)line.substring(13).toInt();
        } else if (line.startsWith("transitions=")) {
            s_lastTransitions = (uint16_t)line.substring(12).toInt();
        } else if (line.startsWith("raw_us=")) {
            String rawLine = line.substring(7);
            int start = 0;
            while (start < (int)rawLine.length() && s_edgeCount < MAX_EDGES) {
                int comma = rawLine.indexOf(',', start);
                String token = (comma < 0) ? rawLine.substring(start) : rawLine.substring(start, comma);
                token.trim();
                if (token.length()) {
                    s_edges[s_edgeCount++] = (uint16_t)min(65535L, token.toInt());
                }
                if (comma < 0) break;
                start = comma + 1;
            }
        }
    }
    file.close();
    endSdAccessAndRestoreRf();

    if (s_edgeCount > 1) {
        s_hasCap = true;
        if (s_lastTransitions == 0) s_lastTransitions = s_edgeCount;
        return true;
    }
    return false;
}

static bool loadMostRecentCapture() {
    if (!beginSdAccess()) return false;
    if (!SD.exists(RF_DIR)) {
        endSdAccessAndRestoreRf();
        return false;
    }
    File dir = SD.open(RF_DIR);
    if (!dir) {
        endSdAccessAndRestoreRf();
        return false;
    }

    String newestPath;
    uint32_t newestTime = 0;
    while (true) {
        File entry = dir.openNextFile();
        if (!entry) break;
        if (!entry.isDirectory()) {
            String name = String(entry.name());
            if (name.endsWith(".txt")) {
                uint32_t t = entry.getLastWrite();
                if (newestPath.isEmpty() || t >= newestTime) {
                    newestTime = t;
                    newestPath = name;
                }
            }
        }
        entry.close();
    }
    dir.close();
    endSdAccessAndRestoreRf();

    if (newestPath.isEmpty()) return false;
    if (!newestPath.startsWith("/")) newestPath = String(RF_DIR) + "/" + newestPath;
    return loadCaptureFromFile(newestPath);
}

static void loadReplayFileList() {
    s_replayFileCount = 0;
    s_replayFileSel = 0;
    if (!beginSdAccess()) return;
    if (!SD.exists(RF_DIR)) {
        endSdAccessAndRestoreRf();
        return;
    }
    File dir = SD.open(RF_DIR);
    if (!dir) {
        endSdAccessAndRestoreRf();
        return;
    }
    while (s_replayFileCount < 32) {
        File entry = dir.openNextFile();
        if (!entry) break;
        if (!entry.isDirectory()) {
            String name = String(entry.name());
            if (name.endsWith(".txt")) {
                if (!name.startsWith("/")) name = String(RF_DIR) + "/" + name;
                s_replayFiles[s_replayFileCount++] = name;
            }
        }
        entry.close();
    }
    dir.close();
    endSdAccessAndRestoreRf();
}

// ── Capture / Scan Copy ───────────────────────────────────────────────────
static void summarizeCapture(bool decoded, uint64_t value, unsigned int* raw) {
    s_lastDecoded = value;
    s_lastBitLength = decoded ? s_rcswitch.getReceivedBitlength() : 0;
    s_lastPulse = decoded ? s_rcswitch.getReceivedDelay() : 0;

    int transitions = 0;
    for (; transitions < RCSWITCH_RAW_MAX_CHANGES; transitions++) {
        if (raw[transitions] == 0) break;
    }
    s_lastTransitions = transitions;
    s_edgeCount = min(transitions, MAX_EDGES);
    s_detectMinPulse = 0xFFFF;
    s_detectMaxPulse = 0;
    s_detectBurstUs = 0;
    for (int i = 0; i < s_edgeCount; ++i) {
        s_edges[i] = raw[i] > 65535 ? 65535 : raw[i];
        if (s_edges[i] > 0) {
            if (s_edges[i] < s_detectMinPulse) s_detectMinPulse = s_edges[i];
            if (s_edges[i] > s_detectMaxPulse) s_detectMaxPulse = s_edges[i];
            s_detectBurstUs += s_edges[i];
        }
    }
    if (s_detectMinPulse == 0xFFFF) s_detectMinPulse = 0;
    s_detectRepeats = (s_lastPulse > 0 && s_detectBurstUs > 0)
        ? max(1, (int)(s_detectBurstUs / max<uint32_t>(1, s_lastPulse * max<uint16_t>(1, s_lastBitLength))))
        : 1;
    strcpy(s_detectMod, decoded ? "OOK / ASK decoded" : "OOK / ASK-like");

    if (decoded) {
        snprintf(s_lastProtocol, sizeof(s_lastProtocol), "RcSwitch(%d)", s_rcswitch.getReceivedProtocol());
        snprintf(s_lastSummary, sizeof(s_lastSummary), "%u bits", s_lastBitLength);
        snprintf(s_lastCrc, sizeof(s_lastCrc), "%llX", value);
    } else {
        snprintf(s_lastProtocol, sizeof(s_lastProtocol), "RAW(0)");
        snprintf(s_lastSummary, sizeof(s_lastSummary), "No code identified");
        snprintf(s_lastCrc, sizeof(s_lastCrc), "No code identified");
    }
}

static void startCapture() {
    configOokRx(capFreq());
    delay(10);
    s_edgeCount = 0;
    s_lastTransitions = 0;
    s_lastDecoded = 0;
    s_lastBitLength = 0;
    s_lastPulse = 0;
    strcpy(s_lastProtocol, "RAW(0)");
    strcpy(s_lastSummary, "No code identified");
    strcpy(s_lastCrc, "No code identified");
    s_hasCap = false;
    s_capturing = true;
    s_captureStart = millis();
    s_rcswitch.resetAvailable();
}


static bool pollCapture() {
    if (s_rcswitch.available()) {
        delay(120);
        summarizeCapture(true, s_rcswitch.getReceivedValue(), s_rcswitch.getReceivedRawdata());
        s_rcswitch.resetAvailable();
        return true;
    }
    if (s_rcswitch.RAWavailable()) {
        delay(180);
        unsigned int* raw = s_rcswitch.getRAWReceivedRawdata();
        int transitions = 0;
        for (; transitions < RCSWITCH_RAW_MAX_CHANGES; transitions++) {
            if (raw[transitions] == 0) break;
        }
        if (transitions >= 20) {
            summarizeCapture(false, 0, raw);
            s_rcswitch.resetAvailable();
            return true;
        }
        s_rcswitch.resetAvailable();
    }
    return false;
}

static void stopCapture() {
    idleChip();
    s_capturing = false;
    s_hasCap = (s_lastTransitions >= 20 || s_lastDecoded != 0);
    if (s_hasCap) s_lastSignalCount++;
    s_rcswitch.resetAvailable();
}

static void drawDetect() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.fillRect(0, 0, SCREEN_W, STATUS_H, 0x112000);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(0xD8F0A8, 0x112000);
    d.setCursor(2, 3);
    char hdr[48];
#ifdef BOARD_TEMBED
    snprintf(hdr, sizeof(hdr), "RF Detect  %s  %.3fMHz", bandName(), capFreq());
#else
    snprintf(hdr, sizeof(hdr), "RF Detect  %.3fMHz", capFreq());
#endif
    d.print(hdr);

    if (s_capturing) {
        char buf[64];
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, 26); d.print("Listening for RF burst...");
        snprintf(buf, sizeof(buf), "Band: %s", bandName());
        d.setCursor(8, 40); d.print(buf);
        snprintf(buf, sizeof(buf), "Elapsed: %lus", (millis() - s_captureStart) / 1000);
        d.setCursor(8, 54); d.print(buf);
        d.setCursor(8, 118); d.print("Enter=stop  Back=cancel");
    } else if (s_hasCap) {
        char buf[72];
        d.setTextColor(C_FG, C_BG);
        snprintf(buf, sizeof(buf), "Type: %s", s_detectMod);
        d.setCursor(8, 20); d.print(buf);
        snprintf(buf, sizeof(buf), "Protocol: %s", s_lastProtocol);
        d.setCursor(8, 32); d.print(buf);
        snprintf(buf, sizeof(buf), "Transitions: %u", (unsigned)s_lastTransitions);
        d.setCursor(8, 44); d.print(buf);
        snprintf(buf, sizeof(buf), "Pulse range: %u-%u us", (unsigned)s_detectMinPulse, (unsigned)s_detectMaxPulse);
        d.setCursor(8, 56); d.print(buf);
        snprintf(buf, sizeof(buf), "Burst len: %lu us", (unsigned long)s_detectBurstUs);
        d.setCursor(8, 68); d.print(buf);
        snprintf(buf, sizeof(buf), "Repeats: %d", s_detectRepeats);
        d.setCursor(8, 80); d.print(buf);
        snprintf(buf, sizeof(buf), "Summary: %s", s_lastSummary);
        d.setCursor(8, 94); d.print(buf);
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, 118); d.print("Ent=again  Del=clear");
    } else {
        char buf[64];
        d.setTextColor(C_DIM, C_BG);
        snprintf(buf, sizeof(buf), "Freq: %.3f MHz", capFreq());
        d.setCursor(8, 34); d.print(buf);
        d.setCursor(8, 48); d.print("Reports burst timing and type.");
        d.setTextColor(C_FG, C_BG);
        d.setCursor(8, 84); d.print("Press Enter to start.");
        d.setTextColor(C_DIM, C_BG);
#ifdef BOARD_TEMBED
        d.setCursor(8, 118); d.print("up/dn=band  Back=return");
#else
        d.setCursor(8, 118); d.print("Back=return");
#endif
    }
    drawToast();
}

static void drawCapture() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.fillRect(0, 0, SCREEN_W, STATUS_H, 0x001133);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(0x00AAFF, 0x001133);
    d.setCursor(2, 3);
    char hdr[40];
    snprintf(hdr, sizeof(hdr), "RF Scan Copy  %.3fMHz", capFreq());
    d.print(hdr);

    if (s_capturing) {
        char buf[64];
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, 30);
        d.print("Recording: Any RAW signal.");
        snprintf(buf, sizeof(buf), "Scanning: %.2f MHz", capFreq());
        d.setCursor(8, 44);
        d.print(buf);
        snprintf(buf, sizeof(buf), "Total signals found: %lu", (unsigned long)s_lastSignalCount);
        d.setCursor(8, 58);
        d.print(buf);
        uint32_t elapsed = (millis() - s_captureStart) / 1000;
        snprintf(buf, sizeof(buf), "Time: %lus", elapsed);
        d.setCursor(8, 78);
        d.print(buf);
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, 118);
        d.print("Enter=stop  Back=cancel");
    } else if (s_hasCap) {
        char buf[64];
        d.setTextColor(C_FG, C_BG);
        snprintf(buf, sizeof(buf), "Protocol: %s", s_lastProtocol);
        d.setCursor(8, 24);
        d.print(buf);
        snprintf(buf, sizeof(buf), "Length: %s", s_lastSummary);
        d.setCursor(8, 36);
        d.print(buf);
        snprintf(buf, sizeof(buf), "Record length: %u transitions", (unsigned)s_lastTransitions);
        d.setCursor(8, 48);
        d.print(buf);
        snprintf(buf, sizeof(buf), "CRC: %s", s_lastCrc);
        d.setCursor(8, 60);
        d.print(buf);
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, 78);
        d.print("Recording: Any RAW signal.");
        snprintf(buf, sizeof(buf), "Scanning: %.2f MHz", capFreq());
        d.setCursor(8, 90);
        d.print(buf);
        snprintf(buf, sizeof(buf), "Total signals found: %lu", (unsigned long)s_lastSignalCount);
        d.setCursor(8, 102);
        d.print(buf);
#ifdef BOARD_TEMBED
        d.setTextColor(C_FG, C_BG);
        d.setCursor(8, 118);
        d.print("Ent=save  Del=clear");
#else
        d.setTextColor(C_FG, C_BG);
        d.setCursor(8, 118);
        d.print("Ent=again fn+S=save");
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(128, 118);
        d.print("Del=clr");
#endif
    } else {
        char buf[64];
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, 34);
        d.print("Recording: Any RAW signal.");
        snprintf(buf, sizeof(buf), "Scanning: %.2f MHz", capFreq());
        d.setCursor(8, 46);
        d.print(buf);
        snprintf(buf, sizeof(buf), "Total signals found: %lu", (unsigned long)s_lastSignalCount);
        d.setCursor(8, 58);
        d.print(buf);
        d.setTextColor(C_FG, C_BG);
        d.setCursor(8, 86);
        d.print("Press Enter to start.");
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, 118);
        d.print("Back=return");
    }
    drawToast();
}

// ── Replay (async OOK TX via GDO0 pin) ────────────────────────────────────
static void doReplay() {
    if (!s_hasCap || s_edgeCount < 2) return;

    idleChip();
#ifdef BOARD_TEMBED
    setAntennaSwitch(capFreq());
#endif
    ELECHOUSE_cc1101.setMHZ(capFreq());
    ELECHOUSE_cc1101.setModulation(2);
    ELECHOUSE_cc1101.setDRate(50);
    ELECHOUSE_cc1101.setPA(10);
    ELECHOUSE_cc1101.SpiWriteReg(REG_PKTCTRL0, 0x32);
    ELECHOUSE_cc1101.SpiWriteReg(REG_IOCFG0, 0x2E);
    ELECHOUSE_cc1101.SpiStrobe(STR_STX);

    pinMode(CC1101_GDO0_PIN, OUTPUT);
    digitalWrite(CC1101_GDO0_PIN, LOW);
    delayMicroseconds(500);

    for (int rep = 0; rep < 3; rep++) {
        bool level = false;
        for (int i = 0; i < s_edgeCount; i++) {
            level = !level;
            digitalWrite(CC1101_GDO0_PIN, level ? HIGH : LOW);
            delayMicroseconds((uint32_t)s_edges[i]);
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
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(0x00AAFF, 0x001133);
    d.setCursor(2, 3);
    d.print("CC1101 Replay");

    if (!s_hasCap) {
        d.setTextColor(C_ERROR, C_BG);
        d.setCursor(8, 48);
        d.print("No capture data.");
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, 64);
        d.print("Go to Scan Copy first.");
        d.setCursor(8, 104);
        d.print("Back=return");
    } else {
        d.setTextColor(C_FG, C_BG);
        char buf[48];
        snprintf(buf, sizeof(buf), "%d edges  %.3fMHz", s_edgeCount, capFreq());
        d.setCursor(8, 36);
        d.print(buf);
        d.setTextColor(0x00EE44, C_BG);
        d.setCursor(8, 56);
        d.print("Enter=transmit (x3)");
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, 72);
#ifdef BOARD_TEMBED
        d.print("Up/Dn=change band");
#else
        d.print("L/R=change band");
#endif
        d.setCursor(8, 104);
        d.print("Back=return");
    }
    drawToast();
}

static void drawReplayList() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.fillRect(0, 0, SCREEN_W, STATUS_H, 0x001133);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(0x00AAFF, 0x001133);
    d.setCursor(2, 3);
    d.print("CC1101 Replay Files");

    if (s_replayFileCount <= 0) {
        d.setTextColor(C_ERROR, C_BG);
        d.setCursor(8, 44);
        d.print("No saved RF files.");
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, 60);
        d.print("Save from Scan Copy first.");
        d.setCursor(8, 118);
        d.print("Back=return");
        drawToast();
        return;
    }

    static constexpr int VIS_ROWS = 10;
    int off = (s_replayFileSel >= VIS_ROWS) ? s_replayFileSel - VIS_ROWS + 1 : 0;
    for (int i = 0; i < VIS_ROWS; ++i) {
        int idx = i + off;
        if (idx >= s_replayFileCount) break;
        int y = STATUS_H + 2 + i * (FONT_H + 2);
        bool sel = (idx == s_replayFileSel);
        uint32_t bg = sel ? C_HIGHLIGHT : C_BG;
        d.fillRect(0, y, SCREEN_W, FONT_H + 2, bg);
        d.setTextColor(sel ? (uint32_t)C_INPUT : (uint32_t)C_FG, bg);
        const char* path = s_replayFiles[idx].c_str();
        const char* slash = strrchr(path, '/');
        const char* name = slash ? slash + 1 : path;
        d.setCursor(4, y + 1);
        d.print(name);
    }
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 2);
    d.print("Enter=load  Back=menu");
    drawToast();
}

// ── Init-error overlay ────────────────────────────────────────────────────
static void drawInitError() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, STATUS_H, SCREEN_W, SCREEN_H - STATUS_H, C_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_ERROR, C_BG);
    d.setCursor(8, 44);
    d.print("CC1101 not found!");
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(8, 58);
    d.print("Check CC1101 module.");
    d.setCursor(8, 80);
    d.print("Press any key...");
}

// ── Freq input ─────────────────────────────────────────────────────────────
static void drawFreqInput() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.fillRect(0, 0, SCREEN_W, STATUS_H, 0x001133);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(0x00AAFF, 0x001133);
    d.setCursor(2, 3);
    d.print("CC1101 Custom Frequency");

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(8, 24);
    d.print("Custom Frequency  (300-928 MHz)");

    d.drawRoundRect(6, 38, SCREEN_W - 12, 20, 3, C_ACCENT);
    d.setTextColor(C_INPUT, C_BG);
    d.setTextSize(2);
    char buf[14];
#ifdef BOARD_TEMBED
    snprintf(buf, sizeof(buf), "%.3f MHz", s_customMHz);
#else
    snprintf(buf, sizeof(buf), "%s_", s_freqInput);
#endif
    d.setCursor(12, 41);
    d.print(buf);

    d.setTextSize(1);
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(8, 72);
#ifdef BOARD_TEMBED
    d.print("Up/Dn = tune +/-1 MHz");
#else
    d.print("Digits + '.'   Bksp=delete");
#endif
    d.setCursor(8, 86);
    d.print("Enter=confirm  Back=cancel");

#ifdef BOARD_TEMBED
    d.setTextColor(0x00EE44, C_BG);
    snprintf(buf, sizeof(buf), "=> %.3f MHz", s_customMHz);
    d.setCursor(8, 108);
    d.print(buf);
#else
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
#endif
    drawToast();
}

// ── Menu ───────────────────────────────────────────────────────────────────
static const char* MENU_LABELS[] = { "Spectrum", "Detect", "Scan Copy", "Replay", "Band", "Custom Freq" };
static const char* MENU_DESC[] = { "Live raw waveform", "Analyze RF burst", "Capture raw RF", "Retransmit capture", "Select freq band", "Type exact MHz" };
static const uint32_t MENU_COLS[] = { 0x0066AA, 0x667700, 0x007722, 0xAA6600, 0x660077, 0x556600 };
static constexpr int N_MENU = 6;
static constexpr int CARD_H = 18;
static constexpr int CARD_GAP = 1;

static void drawMenu() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.fillRect(0, 0, SCREEN_W, STATUS_H, 0x001133);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(0x00AAFF, 0x001133);
    d.setCursor(2, 3);
    d.print("CC1101 RF  ");
    d.setTextColor(0x888888, 0x001133);
    d.print(BANDS[s_bandSel].name);
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
        if (i == 3) {
            d.setTextColor(sel ? (uint32_t)0xEEEEEE : (uint32_t)0x888888, sel ? col : (uint32_t)0x111111);
            d.setCursor(8 + strlen(MENU_LABELS[i]) * FONT_W + 6, y + 2);
            d.print(bandName());
        }
        if (i == 4) {
            d.setTextColor(sel ? (uint32_t)0xEEEEEE : (uint32_t)0x888888, sel ? col : (uint32_t)0x111111);
            d.setCursor(8 + strlen(MENU_LABELS[i]) * FONT_W + 6, y + 2);
            char fbuf[16];
            snprintf(fbuf, sizeof(fbuf), "%.3fMHz", s_customMHz);
            d.print(fbuf);
        }
        d.setTextColor(sel ? (uint32_t)0xDDDDDD : (uint32_t)0x555555, sel ? col : (uint32_t)0x111111);
        d.setCursor(8, y + 10);
        d.print(MENU_DESC[i]);
    }
    drawToast();
}

// ── Public entry points ────────────────────────────────────────────────────
void appCc1101Enter() {
    s_state = CC1101State::MENU;
    s_menuSel = 0;
    s_dirty = true;
    s_initError = false;
    s_lastSweep = 0;
    s_lastWaveDraw = 0;
    s_specFrameDirty = true;
    s_lastSignalCount = 0;
    disableReceiver();
    s_spectrumConfigured = false;
}

void appCc1101Loop() {
    auto& d = M5Cardputer.Display;
    auto ev = readKeys();

    if (s_toastUntil && millis() >= s_toastUntil) {
        s_toastUntil = 0;
        s_dirty = true;
    }

    switch (s_state) {
        case CC1101State::MENU:
            if (s_dirty) {
                drawMenu();
                s_dirty = false;
            }
            if (s_initError) {
                if (ev.changed) {
                    s_initError = false;
                    s_dirty = true;
                }
                return;
            }
            if (!ev.changed) return;
            if (ev.back) {
                disableReceiver();
                goHome();
                return;
            }
            if (ev.up && s_menuSel > 0) { s_menuSel--; s_dirty = true; }
            if (ev.down && s_menuSel < N_MENU - 1) { s_menuSel++; s_dirty = true; }
            if (ev.enter) {
                bool needsHw = (s_menuSel == 0 || s_menuSel == 1 ||
                               s_menuSel == 2 || s_menuSel == 3);
                if (needsHw && !initChip()) {
                    s_inited = false;
                    drawInitError();
                    s_initError = true;
                    break;
                }
                switch (s_menuSel) {
                    case 0: s_state = CC1101State::SPECTRUM; s_dirty = true; s_specFrameDirty = true; break;
                    case 1:
                        s_state = CC1101State::DETECT;
                        s_dirty = true;
#ifdef BOARD_TEMBED
                        clearCaptureState();
                        if (s_capturing) stopCaptureTembed(false);
#endif
                        break;
                    case 2:
                        s_state = CC1101State::CAPTURE;
                        s_dirty = true;
#ifdef BOARD_TEMBED
                        clearCaptureState();
                        if (s_capturing) stopCaptureTembed(false);
#endif
                        break;
                    case 3: loadReplayFileList(); s_state = CC1101State::REPLAY_LIST; s_dirty = true; break;
                    case 4: s_bandSel = (s_bandSel >= N_BANDS_PRESET - 1) ? 0 : s_bandSel + 1; s_dirty = true; break;
#ifdef BOARD_TEMBED
                    case 5: s_customMHz = capFreq(); s_state = CC1101State::FREQ_INPUT; s_dirty = true; break;
#else
                    case 5: s_state = CC1101State::FREQ_INPUT; s_dirty = true; break;
#endif
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
                if (s_specFrameDirty) {
                    drawSpectrumFrame();
                    s_specFrameDirty = false;
                }
                s_dirty = false;
            }
            if (!s_spectrumConfigured) {
#ifdef BOARD_TEMBED
                s_specEdgeCount = 0;
                startCaptureTembed();
                s_spectrumConfigured = true;
                s_lastWaveDraw = millis();
                drawSpectrumBoxTembed();
#else
                configOokRx(capFreq());
                s_spectrumConfigured = true;
                s_lastSweep = millis();
#endif
            }
#ifdef BOARD_TEMBED
            if (s_capturing) {
                bool timeout = millis() - s_captureStart > 6000;
                bool gotSignal = pollSpectrumCaptureTembed();
                if (gotSignal || timeout) {
                    stopCaptureTembed(gotSignal, true);
                    if (gotSignal && s_hasCap) {
                        s_specEdgeCount = s_edgeCount;
                        for (int i = 0; i < s_specEdgeCount; i++) s_specEdges[i] = s_edges[i];
                        drawSpectrumBoxTembed();
                        s_lastWaveDraw = millis();
                    }
                    startCaptureTembed();
                } else if (millis() - s_lastWaveDraw > 1000) {
                    drawSpectrumBoxTembed();
                    s_lastWaveDraw = millis();
                }
            }
#else
            if (millis() - s_lastWaveDraw > 50) {
                drawSpectrumWaveform();
                s_lastWaveDraw = millis();
            }
#endif
            if (!ev.changed) return;
            if (ev.back) {
                disableReceiver();
                idleChip();
                s_spectrumConfigured = false;
                s_state = CC1101State::MENU;
                s_dirty = true;
                return;
            }
#ifdef BOARD_TEMBED
            if (ev.up || ev.down) {
                if (s_capturing) {
                    detachInterrupt(digitalPinToInterrupt(CC1101_GDO0_PIN));
                    s_capturing = false;
                }
                s_bandSel = (s_bandSel + N_BANDS_PRESET + (ev.down ? 1 : -1)) % N_BANDS_PRESET;
                s_spectrumConfigured = false;
                s_specFrameDirty = true;
                s_dirty = true;
            }
#else
            if (ev.left || ev.right) {
                s_bandSel = (s_bandSel + N_BANDS_PRESET + (ev.right ? 1 : -1)) % N_BANDS_PRESET;
                s_spectrumConfigured = false;
                s_specFrameDirty = true;
                s_dirty = true;
            }
#endif
            break;

        case CC1101State::DETECT:
            if (s_dirty) {
                drawDetect();
                s_dirty = false;
            }
            if (s_capturing) {
                bool timeout = (millis() - s_captureStart > 10000);
#ifdef BOARD_TEMBED
                bool gotSignal = pollCaptureTembed(false);
                if (gotSignal || timeout) {
                    stopCaptureTembed(gotSignal, true);
                    if (gotSignal && s_hasCap) {
                        s_dirty = true;
                    } else {
                        startCaptureTembed();
                    }
                    return;
                }
#else
                bool gotSignal = pollCapture();
                if (gotSignal || timeout) { stopCapture(); s_dirty = true; return; }
#endif
                static uint32_t lastDetectRefresh = 0;
                if (millis() - lastDetectRefresh > 200) {
                    lastDetectRefresh = millis();
                    drawDetect();
                }
            }
            if (!ev.changed) return;
            if (ev.back) {
#ifdef BOARD_TEMBED
                if (s_capturing) stopCaptureTembed();
#else
                if (s_capturing) stopCapture();
#endif
                clearCaptureState();
                disableReceiver();
                idleChip();
                s_state = CC1101State::MENU;
                s_dirty = true;
                return;
            }
            if (ev.del && !s_capturing) {
                clearCaptureState();
                s_dirty = true;
                return;
            }
            if (ev.enter) {
                if (s_capturing) {
#ifdef BOARD_TEMBED
                    stopCaptureTembed();
#else
                    stopCapture();
#endif
                    s_dirty = true;
                } else {
#ifdef BOARD_TEMBED
                    clearCaptureState();
                    startCaptureTembed();
#else
                    startCapture();
#endif
                    s_dirty = true;
                }
            }
#ifdef BOARD_TEMBED
            if ((ev.up || ev.down) && !s_capturing) {
                s_bandSel = (s_bandSel + N_BANDS_PRESET + (ev.down ? 1 : -1)) % N_BANDS_PRESET;
                s_dirty = true;
            }
#endif
            break;

        case CC1101State::CAPTURE:
            if (s_dirty) {
                drawCapture();
                s_dirty = false;
            }
            if (s_capturing) {
                bool timeout = (millis() - s_captureStart > 10000);
#ifdef BOARD_TEMBED
                bool gotSignal = pollCaptureTembed(true);
                if (gotSignal || timeout) {
                    stopCaptureTembed(gotSignal, true);
                    s_dirty = true;
                    return;
                }
#else
                bool gotSignal = pollCapture();
                if (gotSignal || timeout) {
                    stopCapture();
                    s_dirty = true;
                    return;
                }
#endif
                static uint32_t lastRefresh = 0;
                if (millis() - lastRefresh > 200) {
                    lastRefresh = millis();
                    drawCapture();
                }
            }
            if (!ev.changed) return;
            if (ev.back) {
#ifdef BOARD_TEMBED
                if (s_capturing) stopCaptureTembed();
#else
                if (s_capturing) stopCapture();
                disableReceiver();
#endif
                clearCaptureState();
                idleChip();
                s_state = CC1101State::MENU;
                s_dirty = true;
                return;
            }
            if (ev.del && !s_capturing) {
                clearCaptureState();
                s_dirty = true;
                return;
            }
#ifndef BOARD_TEMBED
            if (ev.fnKey && s_hasCap) {
                for (char c : ev.chars) {
                    if (c == 's' || c == 'S') {
                        if (saveCaptureToSd()) {
                            showToast("Saved: " + s_lastSaveFile.substring(s_lastSaveFile.lastIndexOf('/') + 1));
                            initChip();
                        } else {
                            showToast("Save failed");
                        }
                        s_dirty = true;
                        return;
                    }
                }
            }
#endif
            if (ev.enter) {
                if (s_capturing) {
#ifdef BOARD_TEMBED
                    stopCaptureTembed();
#else
                    stopCapture();
#endif
                    s_dirty = true;
                } else {
#ifdef BOARD_TEMBED
                    if (s_hasCap && s_edgeCount >= 2) {
                        if (saveCaptureToSd()) {
                            showToast("Saved: " + s_lastSaveFile.substring(s_lastSaveFile.lastIndexOf('/') + 1));
                            initChip();
                        } else {
                            showToast("Save failed");
                        }
                    } else {
                        clearCaptureState();
                        startCaptureTembed();
                    }
#else
                    startCapture();
#endif
                    s_dirty = true;
                }
            }
            break;

        case CC1101State::REPLAY:
            if (s_dirty) {
                drawReplay();
                s_dirty = false;
            }
            if (!ev.changed) return;
            if (ev.back) {
                disableReceiver();
                s_state = CC1101State::MENU;
                s_dirty = true;
                return;
            }
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
#ifdef BOARD_TEMBED
            if (ev.up || ev.down) {
                s_bandSel = (s_bandSel + N_BANDS_PRESET + (ev.down ? 1 : -1)) % N_BANDS_PRESET;
                s_dirty = true;
            }
#else
            if (ev.left || ev.right) {
                s_bandSel = (s_bandSel + N_BANDS_PRESET + (ev.right ? 1 : -1)) % N_BANDS_PRESET;
                s_dirty = true;
            }
#endif
            break;

        case CC1101State::REPLAY_LIST:
            if (s_dirty) {
                drawReplayList();
                s_dirty = false;
            }
            if (!ev.changed) return;
            if (ev.back) {
                s_state = CC1101State::MENU;
                s_dirty = true;
                return;
            }
            if (ev.up && s_replayFileSel > 0) { s_replayFileSel--; s_dirty = true; return; }
            if (ev.down && s_replayFileSel < s_replayFileCount - 1) { s_replayFileSel++; s_dirty = true; return; }
            if (ev.enter && s_replayFileCount > 0) {
                if (loadCaptureFromFile(s_replayFiles[s_replayFileSel])) {
#ifdef BOARD_TEMBED
                    initChip();   // re-init CC1101 after SD access before replay
#endif
                    s_state = CC1101State::REPLAY;
                    s_dirty = true;
                } else {
                    showToast("Load failed");
                    s_dirty = true;
                }
                return;
            }
            break;

        case CC1101State::FREQ_INPUT:
            if (s_dirty) {
                drawFreqInput();
                s_dirty = false;
            }
            if (!ev.changed) return;
#ifdef BOARD_TEMBED
            if (ev.back) {
                s_state = CC1101State::MENU;
                s_dirty = true;
                return;
            }
            if (ev.enter) {
                s_bandSel = N_BANDS - 1;
                s_state = CC1101State::MENU;
                s_dirty = true;
                return;
            }
            if (ev.up || ev.down) {
                float step = ev.up ? 1.0f : -1.0f;
                s_customMHz = constrain(s_customMHz + step, 300.0f, 928.0f);
                s_dirty = true;
            }
#else
            if (ev.back) {
                if (s_freqLen > 0) {
                    s_freqInput[--s_freqLen] = '\0';
                    s_dirty = true;
                } else {
                    s_state = CC1101State::MENU;
                    s_dirty = true;
                }
                return;
            }
            if (ev.enter) {
                float f = atof(s_freqInput);
                if (f >= 300.0f && f <= 928.0f && s_freqLen > 0) {
                    s_customMHz = f;
                    s_bandSel = N_BANDS - 1;
                }
                s_state = CC1101State::MENU;
                s_dirty = true;
                return;
            }
            for (char c : ev.chars) {
                if (c == '\b' || c == 127) {
                    if (s_freqLen > 0) {
                        s_freqInput[--s_freqLen] = '\0';
                        s_dirty = true;
                    }
                } else if ((isdigit((unsigned char)c) || (c == '.' && !strchr(s_freqInput, '.'))) &&
                           s_freqLen < (int)sizeof(s_freqInput) - 2) {
                    s_freqInput[s_freqLen++] = c;
                    s_freqInput[s_freqLen] = '\0';
                    s_dirty = true;
                }
            }
#endif
            break;
    }
}










