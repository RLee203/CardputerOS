#include "app_games.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <stdlib.h>
#include <peanut_gb.h>

// ── Display geometry ───────────────────────────────────────────────────────
static constexpr int GB_W       = 160;
static constexpr int DISP_X     = (SCREEN_W - GB_W) / 2;     // 40
static constexpr int DISP_LINES = SCREEN_H;                   // 135 (crop bottom 9 of 144 GB lines)
static constexpr uint32_t FRAME_MS = 16;                      // ~60 fps

// Grayscale RGB565 palette: black, dark, light, white
static const uint16_t GB_PAL[4] = { 0x0000, 0x4208, 0xA514, 0xFFFF };

// ── State ──────────────────────────────────────────────────────────────────
enum class GBScene { ROM_BROWSE, EMULATING, ERROR };
static GBScene gbScene = GBScene::ROM_BROWSE;

static String   romFiles[64];
static int      romCount = 0;
static int      romSel   = 0;
static bool     sdOk     = false;
static char     errMsg[64];

// ROM is streamed from SD in 16 KB banks — bank 0 stays resident, the
// current switchable bank is loaded on demand.  Total ROM RAM: 32 KB.
static constexpr uint32_t BANK_SIZE = 16384;

static File          romFile;
static uint32_t      romSize      = 0;
static uint8_t*      bank0Cache   = nullptr;   // always loaded
static uint8_t*      bankNCache   = nullptr;   // current switchable bank
static int           cachedBankN  = -1;

static uint8_t*      cartRam      = nullptr;
static uint_fast32_t cartRamSize  = 0;

static struct gb_s   gb;
static uint16_t      lineBuf[GB_W];
static unsigned long lastFrame    = 0;
static unsigned long hintUntil    = 0;

// ── Peanut-GB callbacks ────────────────────────────────────────────────────

static uint8_t gbRomRead(struct gb_s*, const uint_fast32_t addr) {
    if (addr >= romSize) return 0xFF;
    if (addr < BANK_SIZE) return bank0Cache[addr];
    int bankIdx = (int)(addr >> 14);    // addr / 16384
    int offset  = (int)(addr & 0x3FFF); // addr % 16384
    if (bankIdx != cachedBankN) {
        romFile.seek((uint32_t)bankIdx << 14);
        uint32_t toRead = min(BANK_SIZE, romSize - ((uint32_t)bankIdx << 14));
        romFile.read(bankNCache, toRead);
        cachedBankN = bankIdx;
    }
    return bankNCache[offset];
}

static uint8_t gbCartRamRead(struct gb_s*, const uint_fast32_t addr) {
    return (cartRam && addr < cartRamSize) ? cartRam[addr] : 0xFF;
}

static void gbCartRamWrite(struct gb_s*, const uint_fast32_t addr, const uint8_t val) {
    if (cartRam && addr < cartRamSize) cartRam[addr] = val;
}

static void gbError(struct gb_s*, const enum gb_error_e err, const uint16_t) {
    static const char* names[] = { "Unknown", "Opcode", "Read", "Write", "Halt" };
    snprintf(errMsg, sizeof(errMsg), "GB error: %s", (err < GB_INVALID_MAX) ? names[err] : "?");
    gbScene = GBScene::ERROR;
}

static void gbLcdLine(struct gb_s*, const uint8_t* pixels, const uint_fast8_t line) {
    if (line >= (uint_fast8_t)DISP_LINES) return;
    for (int x = 0; x < GB_W; x++)
        lineBuf[x] = GB_PAL[pixels[x] & 0x03];
    M5Cardputer.Display.pushImage(DISP_X, (int)line, GB_W, 1, lineBuf);
}

// ── ROM browser UI ─────────────────────────────────────────────────────────

static void drawBrowseStatus(const char* msg) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_STATUS_FG, C_STATUS_BG);
    d.setCursor(2, 3);
    d.print(msg);
    drawBatteryWidget(C_STATUS_BG);
}

static void drawRomList() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, STATUS_H, SCREEN_W, TERM_H, C_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    if (!sdOk) {
        d.setTextColor(C_ERROR, C_BG);
        d.setCursor(4, STATUS_H + 4);  d.print("SD card not found.");
        d.setTextColor(C_DIM,   C_BG);
        d.setCursor(4, STATUS_H + 16); d.print("Fn+Q = back");
        return;
    }
    if (romCount == 0) {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(4, STATUS_H + 4);  d.print("No .gb/.gbc on SD.");
        d.setCursor(4, STATUS_H + 16); d.print("Fn+Q = back");
        return;
    }

    int listBottom = SCREEN_H - (FONT_H + 4); // reserve footer/status area
    int vis = (listBottom - STATUS_H) / (FONT_H + 2);
    int off = (romSel >= vis) ? romSel - vis + 1 : 0;
    for (int i = 0; i < vis; i++) {
        int idx = i + off;
        if (idx >= romCount) break;
        bool sel = (idx == romSel);
        int y = STATUS_H + i * (FONT_H + 2);
        uint32_t bg = sel ? C_HIGHLIGHT : C_BG;
        d.fillRect(0, y, SCREEN_W, FONT_H + 2, bg);
        d.setTextColor(sel ? (uint32_t)C_INPUT : (uint32_t)C_FG, bg);
        d.setCursor(4, y + 1);
        d.print(romFiles[idx].c_str());
    }
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 2);
    d.print("Ent=load  bksp=home  ;/./,//=nav");
}

static void showError() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawBrowseStatus("Game Boy - Error");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_ERROR, C_BG);
    d.setCursor(4, STATUS_H + 10); d.print(errMsg);
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(4, STATUS_H + 26); d.print("Fn+Q = back");
}

// ── ROM loader ─────────────────────────────────────────────────────────────

static void freeRom() {
    if (bank0Cache) { free(bank0Cache); bank0Cache = nullptr; }
    if (bankNCache) { free(bankNCache); bankNCache = nullptr; }
    if (romFile)    { romFile.close(); }
    if (cartRam)    { free(cartRam);   cartRam    = nullptr; }
    romSize = 0; cartRamSize = 0; cachedBankN = -1;
}

static bool loadRom(int idx) {
    freeRom();

    String path = "/" + romFiles[idx];
    romFile = SD.open(path.c_str(), FILE_READ);
    if (!romFile) {
        snprintf(errMsg, sizeof(errMsg), "Can't open: %s", romFiles[idx].c_str());
        return false;
    }

    romSize    = (uint32_t)romFile.size();
    bank0Cache = (uint8_t*)malloc(BANK_SIZE);
    bankNCache = (uint8_t*)malloc(BANK_SIZE);
    if (!bank0Cache || !bankNCache) {
        romFile.close();
        if (bank0Cache) { free(bank0Cache); bank0Cache = nullptr; }
        if (bankNCache) { free(bankNCache); bankNCache = nullptr; }
        snprintf(errMsg, sizeof(errMsg), "Bank cache alloc failed");
        return false;
    }

    // Pre-load bank 0 (always resident)
    romFile.seek(0);
    romFile.read(bank0Cache, min(BANK_SIZE, romSize));
    cachedBankN = -1;

    enum gb_init_error_e ret = gb_init(&gb, gbRomRead, gbCartRamRead, gbCartRamWrite, gbError, nullptr);
    if (ret != GB_INIT_NO_ERROR) {
        freeRom();
        snprintf(errMsg, sizeof(errMsg), "gb_init error: %d", (int)ret);
        return false;
    }

    size_t csz = 0;
    gb_get_save_size_s(&gb, &csz);
    cartRamSize = (uint_fast32_t)csz;
    if (cartRamSize > 0) {
        cartRam = (uint8_t*)calloc(cartRamSize, 1);
        // null cartRam is handled gracefully by the read/write callbacks
    }

    gb_init_lcd(&gb, gbLcdLine);
    gb.direct.interlace  = 0;
    gb.direct.frame_skip = 0;
    gb.direct.joypad     = 0xFF;

    M5Cardputer.Display.fillScreen(0x000000);
    lastFrame = millis();
    hintUntil = millis() + 4000;
    return true;
}

// ── Public API ─────────────────────────────────────────────────────────────

void appGamesEnter() {
    gbScene = GBScene::ROM_BROWSE;
    romSel = 0; romCount = 0;
    digitalWrite(LORA_NSS_PIN, HIGH);
    digitalWrite(LORA_RST_PIN, LOW);
    SD.end();
    delay(40);
    sdOk = SD.begin(SD_CS_PIN, SPI, 25000000);
    if (sdOk) {
        File root = SD.open("/");
        while (true) {
            File f = root.openNextFile();
            if (!f) break;
            if (!f.isDirectory()) {
                String name = f.name();
                String lo = name; lo.toLowerCase();
                if ((lo.endsWith(".gb") || lo.endsWith(".gbc")) && romCount < 64)
                    romFiles[romCount++] = name;
            }
            f.close();
        }
        root.close();
    }
    drawBrowseStatus("Game Boy");
    drawRomList();
}

void appGamesLoop() {
    auto ev = readKeys();

    // fn+backspace or fn+Q exits from any sub-state
    if (ev.back) { freeRom(); goHome(); return; }
    if (ev.fnKey)
        for (char c : ev.chars)
            if (c == 'q' || c == 'Q') { freeRom(); goHome(); return; }

    if (gbScene == GBScene::ROM_BROWSE) {
        if (!ev.changed) return;
        if (ev.up   && romSel > 0)           { romSel--; drawRomList(); }
        if (ev.down && romSel < romCount - 1) { romSel++; drawRomList(); }
        if (ev.enter && romCount > 0) {
            drawBrowseStatus("Loading...");
            if (loadRom(romSel)) {
                gbScene = GBScene::EMULATING;
            } else {
                gbScene = GBScene::ERROR;
                showError();
            }
        }
        return;
    }

    if (gbScene == GBScene::ERROR) return;  // only back above

    // ── EMULATING ──────────────────────────────────────────────────────────
    // Read CURRENT key state directly (not change-based) so held keys work.
    // readKeys() above already called M5Cardputer.update(), so state is fresh.
    {
        auto s = M5Cardputer.Keyboard.keysState();
        uint8_t jp = 0xFF;

        for (char c : s.word) {
            char lo = (char)tolower((unsigned char)c);
            // D-pad: WASD (primary — no fn needed)
            if (lo == 'w') jp &= ~JOYPAD_UP;
            if (lo == 's') jp &= ~JOYPAD_DOWN;
            if (lo == 'a') jp &= ~JOYPAD_LEFT;
            if (lo == 'd') jp &= ~JOYPAD_RIGHT;
            // Action buttons
            if (c == '\\') jp &= ~JOYPAD_A;
            if (c == ' ')  jp &= ~JOYPAD_B;
        }
        // D-pad: fn+;/./,// (physical arrow keys)
        if (s.fn) {
            for (char c : s.word) {
                if (c == ';') jp &= ~JOYPAD_UP;
                if (c == '.') jp &= ~JOYPAD_DOWN;
                if (c == ',') jp &= ~JOYPAD_LEFT;
                if (c == '/') jp &= ~JOYPAD_RIGHT;
            }
        }
        if (s.enter) jp &= ~JOYPAD_START;
        if (s.tab)   jp &= ~JOYPAD_SELECT;

        gb.direct.joypad = jp;
        if (jp != 0xFF) resetSleepTimer();  // held button = active gameplay
    }

    // Control hint overlay — shown for 4 s after ROM loads
    if (hintUntil && millis() < hintUntil) {
        auto& d = M5Cardputer.Display;
        d.setFont(&fonts::Font0);
        d.setTextSize(1);
        // Left strip (40px wide)
        d.fillRect(0, 0, DISP_X, SCREEN_H, 0x000000);
        d.setTextColor(0x4208, 0x000000);   // dim green
        int y = 4;
        auto pr = [&](const char* s){ d.setCursor(0, y); d.print(s); y += FONT_H + 1; };
        pr("WASD");
        pr("=dpad");
        pr("\\=A");
        pr("sp=B");
        pr("Ent=");
        pr("Start");
        pr("Tab=");
        pr("Sel");
        y += 4;
        d.setTextColor(0x2104, 0x000000);
        pr("bksp");
        pr("=exit");
    } else if (hintUntil && millis() >= hintUntil) {
        // Erase the hint strip once it expires
        M5Cardputer.Display.fillRect(0, 0, DISP_X, SCREEN_H, 0x000000);
        hintUntil = 0;
    }

    unsigned long now = millis();
    if (now - lastFrame >= FRAME_MS) {
        lastFrame = now;
        gb_run_frame(&gb);
    }
}
