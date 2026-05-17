#include "app_nfc.h"

#include <M5Cardputer.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <SD.h>
#include "config.h"
#include "input.h"
#include "nav.h"

namespace {

// ── Hardware ────────────────────────────────────────────────────────────────
Adafruit_PN532 nfc(-1, -1, &Wire1);
bool     g_nfcReady = false;
uint32_t g_fwVer    = 0;

// ── Modes ────────────────────────────────────────────────────────────────────
enum class NfcMode { MENU, SCAN, READ, CLONE, WRITE_NDEF, EMULATE, LOAD };
NfcMode g_mode  = NfcMode::MENU;
bool    g_dirty = true;

// ── Shared tag info ──────────────────────────────────────────────────────────
struct TagInfo {
    String  uid;
    String  type;
    String  ndef;
    uint8_t sak        = 0;
    uint8_t atqa[2]    = {};   // SENS_RES bytes (LSB first, per PN532 frame)
    uint8_t uidLen     = 0;
    uint8_t uidBytes[7] = {};
    bool    valid      = false;
    void clear() { *this = TagInfo{}; }
} g_tag;

// ── Dump buffer (NTAG: 4 bytes/slot, MIFARE: 16 bytes/slot) ─────────────────
static constexpr int MAX_SLOTS = 64;
struct DumpData {
    uint8_t data[MAX_SLOTS][16];
    int     count    = 0;
    int     slotSize = 4;   // 4 = NTAG page, 16 = MIFARE block
    bool    valid    = false;
    void clear() { *this = DumpData{}; slotSize = 4; }
} g_dump;

// ── Scan state ───────────────────────────────────────────────────────────────
bool     g_scanning   = false;
int      g_diagAck    = -1;
uint8_t  g_diagRaw7   = 0;
uint32_t g_diagCycles = 0;

// ── Clone state ──────────────────────────────────────────────────────────────
enum class ClonePhase { READ_SRC, WRITE_DST };
ClonePhase g_clonePhase  = ClonePhase::READ_SRC;
uint8_t    g_cloneSrcSak = 0;   // sak of the source card
String     g_cloneStatus;

// ── Write NDEF state ─────────────────────────────────────────────────────────
String g_ndefUrl;
String g_ndefStatus;

// ── Emulate state (non-blocking state machine) ───────────────────────────────
bool     g_emulating      = false;
String   g_emulStatus;
uint32_t g_emulWaitStart  = 0;
enum class EmulPhase { IDLE, WAIT_ACK, WAIT_RESP, WAIT_REL_ACK, WAIT_REL_RESP };
EmulPhase g_emulPhase = EmulPhase::IDLE;

// ── Load state ───────────────────────────────────────────────────────────────
static constexpr int MAX_SD_FILES = 20;
String g_sdFiles[MAX_SD_FILES];
int    g_sdFileCount  = 0;
int    g_sdFileSel    = 0;
int    g_sdFileScroll = 0;
String g_loadStatus;

// ── Menu ─────────────────────────────────────────────────────────────────────
static constexpr int MENU_COUNT = 6;
static const char* MENU_LABELS[] = { "Scan", "Read", "Clone", "Write NDEF", "Emulate", "Load" };
static const char* MENU_DESC[]   = {
    "Scan+show tag info",
    "Full dump -> SD card",
    "Copy tag to blank",
    "Write URL to NTAG",
    "Spoof UID to reader",
    "Browse SD dumps"
};
int g_menuSel = 0;

// ── Layout constants ─────────────────────────────────────────────────────────
static constexpr int HDR_H    = STATUS_H;
static constexpr int BODY_Y   = HDR_H + 2;
static constexpr int FTR_Y    = SCREEN_H - FONT_H - 2;
static constexpr int BODY_H   = FTR_Y - BODY_Y - 2;

// ── URI prefix table ─────────────────────────────────────────────────────────
static const char* URI_PREFIX[] = {
    "", "http://www.", "https://www.", "http://", "https://",
    "tel:", "mailto:", "ftp://", "ftps://", "sftp://",
};
static constexpr int URI_PREFIX_COUNT = sizeof(URI_PREFIX)/sizeof(URI_PREFIX[0]);

static const uint8_t PASSIVE_SCAN_CMD[] = {
    PN532_COMMAND_INLISTPASSIVETARGET, 0x01, PN532_MIFARE_ISO14443A
};

// ── Draw helpers ─────────────────────────────────────────────────────────────

void drawHeader(const char* title) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, SCREEN_W, HDR_H, C_STATUS_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_STATUS_FG, C_STATUS_BG);
    d.setCursor(2, 3);
    d.print(title);
    drawBatteryWidget(C_STATUS_BG, SCREEN_W - 43);
}

void drawFooter(const char* text) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, FTR_Y - 1, SCREEN_W, SCREEN_H - FTR_Y + 1, C_BG);
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, FTR_Y);
    d.print(text);
}

String tagTypeName(uint8_t uidLen, uint8_t sak) {
    if (sak == 0x08 || sak == 0x38) return "MIFARE 1K";
    if (sak == 0x18)                 return "MIFARE 4K";
    if (sak == 0x09)                 return "MIFARE Mini";
    if (sak == 0x20)                 return "MIFARE Plus";
    if (sak == 0x00 && uidLen == 7)  return "NTAG/UL";
    if (sak == 0x00 && uidLen == 4)  return "MIFARE UL";
    return "Unknown";
}

String readNdefUri() {
    uint8_t buf[4];
    if (!nfc.ntag2xx_ReadPage(4, buf)) return "";
    if (buf[0] != 0x03) return "";
    uint8_t ndefLen = buf[1];
    if (ndefLen < 5) return "";
    if (!nfc.ntag2xx_ReadPage(5, buf)) return "";
    if (buf[3] != 0x55) return "";
    uint8_t payloadLen = buf[2];
    if (payloadLen < 2) return "";
    if (!nfc.ntag2xx_ReadPage(6, buf)) return "";
    uint8_t prefix = buf[0];
    String uri = (prefix < URI_PREFIX_COUNT) ? String(URI_PREFIX[prefix]) : "";
    uint8_t remaining = payloadLen - 1;
    int startByte = 1, page = 6;
    while (remaining > 0 && page < 60) {
        for (uint8_t i = startByte; i < 4 && remaining > 0; ++i, --remaining)
            uri += (char)buf[i];
        startByte = 0;
        if (remaining > 0 && !nfc.ntag2xx_ReadPage(++page, buf)) break;
    }
    return uri;
}

// Scan once; fills g_tag on success. Returns true if NEW tag found (different uid).
bool doScanOnce(uint16_t timeoutMs) {
    ++g_diagCycles;
    bool ackOk = nfc.sendCommandCheckAck((uint8_t*)PASSIVE_SCAN_CMD, sizeof(PASSIVE_SCAN_CMD), timeoutMs);
    g_diagAck = ackOk ? 1 : 0;
    if (!ackOk) return false;
    uint8_t raw[20] = {};
    nfc.readdata(raw, sizeof(raw));
    g_diagRaw7 = raw[7];
    if (raw[7] != 1) return false;
    uint8_t sak    = raw[11];
    uint8_t uidLen = raw[12];
    if (uidLen == 0 || uidLen > 7) return false;
    String uid;
    for (uint8_t i = 0; i < uidLen; ++i) {
        if (i) uid += ":";
        if (raw[13 + i] < 0x10) uid += "0";
        uid += String(raw[13 + i], HEX);
    }
    uid.toUpperCase();
    if (uid == g_tag.uid && g_tag.valid) return false;  // same tag, skip
    g_tag.uid     = uid;
    g_tag.sak     = sak;
    g_tag.atqa[0] = raw[9];   // SENS_RES byte 1 (LSB)
    g_tag.atqa[1] = raw[10];  // SENS_RES byte 2 (MSB)
    g_tag.uidLen  = uidLen;
    memcpy(g_tag.uidBytes, raw + 13, uidLen);
    g_tag.type  = tagTypeName(uidLen, sak);
    g_tag.ndef  = (uidLen == 7) ? readNdefUri() : "";
    g_tag.valid = true;
    return true;
}

// Blocking scan: tries up to maxTries × timeoutMs before giving up.
bool scanBlocking(int maxTries = 20, uint16_t eachMs = 500) {
    for (int i = 0; i < maxTries; i++) {
        if (doScanOnce(eachMs)) return true;
    }
    return false;
}

bool initSD() {
    digitalWrite(LORA_NSS_PIN, HIGH);
    digitalWrite(LORA_RST_PIN, LOW);
    return SD.begin(SD_CS_PIN, SPI, 25000000);
}

// ════════════════════════════════════════════════════════════════════════════
// MENU
// ════════════════════════════════════════════════════════════════════════════

void drawMenu() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawHeader("NFC");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    if (!g_nfcReady) {
        d.setTextColor(C_ERROR, C_BG);
        d.setCursor(8, BODY_Y + 4);  d.print("PN532 not found");
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, BODY_Y + 16); d.print("G8=SDA  G9=SCL  3.3V  GND");
        drawFooter("Bksp: home");
        g_dirty = false;
        return;
    }

    for (int i = 0; i < MENU_COUNT; i++) {
        bool sel = (i == g_menuSel);
        int  y   = BODY_Y + i * 14;
        uint32_t bg = sel ? C_HIGHLIGHT : (uint32_t)C_BG;
        d.fillRect(0, y, SCREEN_W, 13, bg);
        d.setTextColor(sel ? (uint32_t)C_INPUT : (uint32_t)C_FG, bg);
        d.setCursor(6, y + 3);
        d.print(MENU_LABELS[i]);
        if (sel) {
            d.setTextColor(C_DIM, bg);
            d.setCursor(80, y + 3);
            d.print(MENU_DESC[i]);
        }
    }
    drawFooter("fn+;/.=nav  Enter=open  Bksp=home");
    g_dirty = false;
}

void handleMenu() {
    if (g_dirty) drawMenu();
    auto ev = readKeys();
    if (!ev.changed) return;
    if (ev.back || ev.del) { goHome(); return; }
    if (!g_nfcReady) return;
    if (ev.up   && g_menuSel > 0)              { g_menuSel--; g_dirty = true; }
    if (ev.down && g_menuSel < MENU_COUNT - 1) { g_menuSel++; g_dirty = true; }
    if (ev.enter) {
        g_dirty = true;
        switch (g_menuSel) {
            case 0:
                g_tag.clear();
                g_mode = NfcMode::SCAN;
                g_scanning = false; g_diagAck = -1; g_diagRaw7 = 0; g_diagCycles = 0;
                break;
            case 1:
                g_tag.clear();
                g_mode = NfcMode::READ;
                g_dump.clear();
                break;
            case 2:
                g_tag.clear();
                g_mode = NfcMode::CLONE;
                g_clonePhase = ClonePhase::READ_SRC; g_cloneStatus = ""; g_dump.clear();
                break;
            case 3:
                g_tag.clear();
                g_mode = NfcMode::WRITE_NDEF;
                g_ndefUrl = ""; g_ndefStatus = "";
                break;
            case 4:
                // Keep g_tag — Emulate uses whatever was scanned or loaded
                g_mode = NfcMode::EMULATE;
                g_emulating = false; g_emulStatus = ""; g_emulPhase = EmulPhase::IDLE;
                break;
            case 5:
                // Keep g_tag — Load will overwrite it when a file is selected
                g_mode = NfcMode::LOAD;
                g_loadStatus = ""; g_sdFileCount = 0; g_sdFileSel = 0; g_sdFileScroll = 0;
                break;
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// SCAN
// ════════════════════════════════════════════════════════════════════════════

void drawScan() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawHeader("NFC - Scan");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    int y = BODY_Y + 2;

    if (g_tag.valid) {
        d.setTextColor(0x33CC66, C_BG);
        d.setCursor(8, y); d.print("TAG FOUND"); y += 13;

        d.setTextColor(C_DIM, C_BG); d.setCursor(8, y);   d.print("Type:");
        d.setTextColor(C_FG,  C_BG); d.setCursor(44, y);  d.print(g_tag.type.c_str()); y += 11;

        d.setTextColor(C_DIM, C_BG); d.setCursor(8, y);   d.print("UID: ");
        d.setTextColor(C_FG,  C_BG); d.setCursor(44, y);  d.print(g_tag.uid.c_str()); y += 11;

        char sakBuf[8];
        snprintf(sakBuf, sizeof(sakBuf), "0x%02X", g_tag.sak);
        d.setTextColor(C_DIM, C_BG); d.setCursor(8, y);   d.print("SAK: ");
        d.setTextColor(C_FG,  C_BG); d.setCursor(44, y);  d.print(sakBuf); y += 11;

        if (g_tag.ndef.length()) {
            d.setTextColor(C_DIM, C_BG); d.setCursor(8, y); d.print("NDEF:"); y += 10;
            d.setTextColor(C_FG,  C_BG); d.setCursor(8, y);
            String nd = g_tag.ndef;
            if (nd.length() > 36) nd = nd.substring(0, 33) + "...";
            d.print(nd.c_str());
        }
        drawFooter("Enter:scan again  Bksp:menu");
    } else {
        d.setTextColor(g_scanning ? (uint32_t)0xCCAA00 : (uint32_t)C_FG, C_BG);
        d.setCursor(8, y); d.print(g_scanning ? "SCANNING..." : "READY"); y += 14;
        if (g_scanning) {
            d.setTextColor(C_DIM, C_BG);
            d.setCursor(8, y); d.print("Hold tag near reader"); y += 11;
            char buf[48];
            const char* ackStr = g_diagAck < 0 ? "---" : g_diagAck ? "OK " : "NO ";
            snprintf(buf, sizeof(buf), "FW:%05lX ack:%s tgt:%u cyc:%lu",
                     g_fwVer, ackStr, g_diagRaw7, g_diagCycles);
            d.setCursor(8, y); d.print(buf);
            drawFooter("Enter:stop  Bksp:menu");
        } else {
            d.setTextColor(C_DIM, C_BG);
            d.setCursor(8, y); d.print("Press Enter to scan");
            drawFooter("Enter:scan  Bksp:menu");
        }
    }
    g_dirty = false;
}

void handleScan() {
    auto ev = readKeys();
    if (ev.changed) {
        if (ev.back || ev.del) { g_mode = NfcMode::MENU; g_scanning = false; g_dirty = true; return; }
        if (ev.enter) {
            g_scanning = !g_scanning;
            g_tag.clear();
            g_diagAck = -1; g_diagCycles = 0;
            g_dirty = true;
        }
    }
    if (g_dirty) drawScan();

    if (g_scanning && g_nfcReady) {
        bool found = doScanOnce(500);
        g_dirty = true;
        if (found) g_scanning = false;  // auto-stop on find
    }
}

// ════════════════════════════════════════════════════════════════════════════
// READ (full dump)
// ════════════════════════════════════════════════════════════════════════════

bool readFullDump() {
    g_dump.clear();
    if (!g_tag.valid) return false;
    bool isNtag = (g_tag.sak == 0x00);
    if (isNtag) {
        g_dump.slotSize = 4;
        for (int pg = 0; pg < MAX_SLOTS; pg++) {
            uint8_t buf[4];
            if (!nfc.ntag2xx_ReadPage(pg, buf)) { g_dump.count = pg; break; }
            memcpy(g_dump.data[pg], buf, 4);
            g_dump.count = pg + 1;
        }
    } else {
        g_dump.slotSize = 16;
        uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        int blocks = (g_tag.sak == 0x18) ? MAX_SLOTS : min(64, MAX_SLOTS);
        for (int blk = 0; blk < blocks; blk++) {
            if (blk % 4 == 0) {
                if (!nfc.mifareclassic_AuthenticateBlock(
                        g_tag.uidBytes, g_tag.uidLen, blk, 0, keyA)) {
                    g_dump.count = blk; break;
                }
            }
            uint8_t data[16];
            if (!nfc.mifareclassic_ReadDataBlock(blk, data)) { g_dump.count = blk; break; }
            memcpy(g_dump.data[blk], data, 16);
            g_dump.count = blk + 1;
        }
    }
    return g_dump.count > 0;
}

void saveDump() {
    if (!g_dump.count || !g_tag.valid) return;
    if (!initSD()) return;
    if (!SD.exists(NFC_DIR)) SD.mkdir(NFC_DIR);
    char fname[48];
    snprintf(fname, sizeof(fname), "%s/dump_%08lu.txt", NFC_DIR, millis());
    File f = SD.open(fname, FILE_WRITE);
    if (!f) return;
    f.println("CardputerOS NFC Dump");
    f.println("UID:  " + g_tag.uid);
    f.println("Type: " + g_tag.type);
    char sakBuf[8];
    snprintf(sakBuf, sizeof(sakBuf), "%02X", g_tag.sak);
    f.println("SAK:  " + String(sakBuf));
    char atqaBuf[8];
    snprintf(atqaBuf, sizeof(atqaBuf), "%02X%02X", g_tag.atqa[0], g_tag.atqa[1]);
    f.println("ATQA: " + String(atqaBuf));
    if (g_tag.ndef.length()) f.println("NDEF: " + g_tag.ndef);
    f.println("SLOT: " + String(g_dump.slotSize));
    f.println("---");
    for (int i = 0; i < g_dump.count; i++) {
        char line[64] = {};
        if (g_dump.slotSize == 4) {
            snprintf(line, sizeof(line), "P%03d: %02X %02X %02X %02X",
                     i, g_dump.data[i][0], g_dump.data[i][1],
                     g_dump.data[i][2], g_dump.data[i][3]);
        } else {
            snprintf(line, sizeof(line),
                     "P%03d: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                     i, g_dump.data[i][0],  g_dump.data[i][1],  g_dump.data[i][2],  g_dump.data[i][3],
                        g_dump.data[i][4],  g_dump.data[i][5],  g_dump.data[i][6],  g_dump.data[i][7],
                        g_dump.data[i][8],  g_dump.data[i][9],  g_dump.data[i][10], g_dump.data[i][11],
                        g_dump.data[i][12], g_dump.data[i][13], g_dump.data[i][14], g_dump.data[i][15]);
        }
        f.println(line);
    }
    f.close();
    g_dump.valid = true;
}

void drawRead() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawHeader("NFC - Read");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    int y = BODY_Y + 2;

    if (g_dump.valid) {
        d.setTextColor(0x33CC66, C_BG);
        d.setCursor(8, y); d.print("Saved to SD!"); y += 13;
        d.setTextColor(C_FG, C_BG);
        d.setCursor(8, y); d.print(g_tag.uid.c_str()); y += 11;
        d.setCursor(8, y); d.print(g_tag.type.c_str()); y += 11;
        char buf[32];
        snprintf(buf, sizeof(buf), "%d slots  %d bytes/slot", g_dump.count, g_dump.slotSize);
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, y); d.print(buf);
        drawFooter("Enter:scan again  Bksp:menu");
    } else {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, y); d.print("Hold tag to reader");
        y += 11;
        d.setCursor(8, y); d.print("Press Enter to scan+dump");
        drawFooter("Enter:scan  Bksp:menu");
    }
    g_dirty = false;
}

void handleRead() {
    auto ev = readKeys();
    if (ev.changed) {
        if (ev.back || ev.del) { g_mode = NfcMode::MENU; g_tag.clear(); g_dump.clear(); g_dirty = true; return; }
        if (ev.enter) {
            if (g_dump.valid) {
                g_tag.clear(); g_dump.clear(); g_dirty = true; return;
            }
            // Scan → read → save
            g_diagCycles = 0;
            g_dirty = true;
            drawRead();
            if (scanBlocking()) {
                auto& d = M5Cardputer.Display;
                d.setTextColor(0xCCAA00, C_BG);
                d.setCursor(8, BODY_Y + 2); d.print("Reading dump...");
                readFullDump();
                saveDump();
                g_dirty = true;
            }
        }
    }
    if (g_dirty) drawRead();
}

// ════════════════════════════════════════════════════════════════════════════
// CLONE
// ════════════════════════════════════════════════════════════════════════════

bool writeNtagDump() {
    // Skip pages 0-3 (manufacturer + OTP + config)
    for (int pg = 4; pg < g_dump.count; pg++) {
        if (!nfc.ntag2xx_WritePage(pg, g_dump.data[pg])) return false;
    }
    return true;
}

bool writeMifareDump() {
    uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    // Auth sector 0 before writing blocks 1-3 (blk=1 doesn't trigger the per-sector auth)
    if (!nfc.mifareclassic_AuthenticateBlock(g_tag.uidBytes, g_tag.uidLen, 0, 0, keyA)) return false;
    for (int blk = 1; blk < g_dump.count; blk++) {   // skip block 0 (manufacturer)
        if (blk % 4 == 0) {
            if (!nfc.mifareclassic_AuthenticateBlock(
                    g_tag.uidBytes, g_tag.uidLen, blk, 0, keyA)) return false;
        }
        if (!nfc.mifareclassic_WriteDataBlock(blk, g_dump.data[blk])) return false;
    }
    return true;
}

void drawClone() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawHeader("NFC - Clone");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    int y = BODY_Y + 2;

    if (g_cloneStatus.length()) {
        bool ok = g_cloneStatus.startsWith("OK");
        d.setTextColor(ok ? (uint32_t)0x33CC66 : (uint32_t)C_ERROR, C_BG);
        d.setCursor(8, y); d.print(g_cloneStatus.c_str());
        drawFooter("Enter:again  Bksp:menu");
        g_dirty = false;
        return;
    }

    if (g_clonePhase == ClonePhase::READ_SRC) {
        d.setTextColor(C_FG, C_BG);
        d.setCursor(8, y); d.print("Phase 1: Read source tag"); y += 13;
        if (g_dump.count > 0) {
            d.setTextColor(0x33CC66, C_BG);
            d.setCursor(8, y); d.print("Source scanned!"); y += 11;
            d.setTextColor(C_DIM, C_BG);
            d.setCursor(8, y); d.print(g_tag.uid.c_str()); y += 10;
            char buf[32];
            snprintf(buf, sizeof(buf), "%d slots read (%s)", g_dump.count, g_tag.type.c_str());
            d.setCursor(8, y); d.print(buf);
            drawFooter("Enter:go to write  Bksp:menu");
        } else {
            d.setTextColor(C_DIM, C_BG);
            d.setCursor(8, y); d.print("Hold SOURCE tag to reader");
            drawFooter("Enter:scan source  Bksp:menu");
        }
    } else {
        d.setTextColor(0xCCAA00, C_BG);
        d.setCursor(8, y); d.print("Phase 2: Write to blank"); y += 13;
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, y); d.print("Hold BLANK tag to reader"); y += 11;
        char buf[40];
        snprintf(buf, sizeof(buf), "Will write %d slots", g_dump.count);
        d.setCursor(8, y); d.print(buf);
        drawFooter("Enter:write  Bksp:menu");
    }
    g_dirty = false;
}

void handleClone() {
    auto ev = readKeys();
    if (ev.changed) {
        if (ev.back || ev.del) {
            if (g_cloneStatus.length()) {
                g_cloneStatus = ""; g_clonePhase = ClonePhase::READ_SRC;
                g_tag.clear(); g_dump.clear(); g_dirty = true;
            } else {
                g_mode = NfcMode::MENU; g_tag.clear(); g_dump.clear(); g_dirty = true;
            }
            return;
        }
        if (ev.enter) {
            if (g_cloneStatus.length()) {
                g_cloneStatus = ""; g_clonePhase = ClonePhase::READ_SRC;
                g_tag.clear(); g_dump.clear(); g_dirty = true;
                return;
            }
            if (g_clonePhase == ClonePhase::READ_SRC) {
                if (g_dump.count == 0) {
                    // Scan source
                    g_dirty = true;
                    if (scanBlocking()) {
                        auto& d = M5Cardputer.Display;
                        d.setTextColor(0xCCAA00, C_BG);
                        d.setCursor(8, BODY_Y + 14); d.print("Reading source dump...");
                        readFullDump();
                        g_dirty = true;
                    }
                } else {
                    // Source ready → move to write phase
                    g_cloneSrcSak = g_tag.sak;
                    g_tag.clear();
                    g_clonePhase = ClonePhase::WRITE_DST;
                    g_dirty = true;
                }
            } else {
                // Write to destination
                g_dirty = true;
                if (!scanBlocking()) {
                    g_cloneStatus = "ERR: No tag found";
                } else {
                    bool ok = (g_cloneSrcSak == 0x00) ? writeNtagDump() : writeMifareDump();
                    g_cloneStatus = ok ? "OK! Clone complete" : "ERR: Write failed";
                }
                g_dirty = true;
            }
        }
    }
    if (g_dirty) drawClone();
}

// ════════════════════════════════════════════════════════════════════════════
// WRITE NDEF
// ════════════════════════════════════════════════════════════════════════════

uint8_t parseUriPrefix(const String& url, String& remainder) {
    struct { const char* scheme; uint8_t id; } prefixes[] = {
        {"https://www.", 2}, {"http://www.", 1},
        {"https://",    4}, {"http://",    3},
        {"tel:",        5}, {"mailto:",    6},
    };
    for (auto& p : prefixes) {
        if (url.startsWith(p.scheme)) {
            remainder = url.substring(strlen(p.scheme));
            return p.id;
        }
    }
    remainder = url;
    return 0;
}

void drawWriteNdef() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawHeader("NFC - Write NDEF");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    int y = BODY_Y + 2;

    if (g_ndefStatus.length()) {
        bool ok = g_ndefStatus.startsWith("OK");
        d.setTextColor(ok ? (uint32_t)0x33CC66 : (uint32_t)C_ERROR, C_BG);
        d.setCursor(8, y); d.print(g_ndefStatus.c_str());
        drawFooter("Enter:again  Bksp:menu");
        g_dirty = false;
        return;
    }

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(8, y); d.print("URL to write:"); y += 12;

    // Input box
    d.fillRect(4, y, SCREEN_W - 8, 12, C_HIGHLIGHT);
    d.setTextColor(C_INPUT, C_HIGHLIGHT);
    String disp = g_ndefUrl.length() > 36 ? g_ndefUrl.substring(g_ndefUrl.length() - 36) : g_ndefUrl;
    d.setCursor(6, y + 2);
    d.print(disp.c_str());
    int cx = 6 + disp.length() * FONT_W;
    if (cx < SCREEN_W - 8) d.fillRect(cx, y + 2, FONT_W, FONT_H - 1, C_CURSOR);
    y += 16;

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(8, y); d.print("Then hold NTAG to reader,"); y += 10;
    d.setCursor(8, y); d.print("press Enter to write.");

    drawFooter("Type URL  Bksp=del  Enter=write");
    g_dirty = false;
}

void handleWriteNdef() {
    auto ev = readKeys();
    if (ev.changed) {
        if (g_ndefStatus.length()) {
            if (ev.enter) { g_ndefUrl = ""; g_ndefStatus = ""; g_dirty = true; }
            else if (ev.back || ev.del) { g_mode = NfcMode::MENU; g_dirty = true; }
            return;
        }
        if (ev.back) { g_mode = NfcMode::MENU; g_dirty = true; return; }
        if (ev.del && g_ndefUrl.length() > 0) {
            g_ndefUrl.remove(g_ndefUrl.length() - 1);
            g_dirty = true;
            return;
        }
        if (ev.enter && g_ndefUrl.length() > 0) {
            // Scan and write
            g_dirty = true;
            drawWriteNdef();
            g_tag.clear();
            if (!scanBlocking()) {
                g_ndefStatus = "ERR: No NTAG found";
            } else if (g_tag.uidLen != 7) {
                g_ndefStatus = "ERR: Need NTAG (7-byte UID)";
            } else {
                String rest;
                uint8_t id = parseUriPrefix(g_ndefUrl, rest);
                uint8_t res = nfc.ntag2xx_WriteNDEFURI(id, (char*)rest.c_str(), rest.length());
                g_ndefStatus = res ? "OK! NDEF written" : "ERR: Write failed";
            }
            g_dirty = true;
            return;
        }
        if (!ev.fnKey) {
            for (char c : ev.chars) {
                if (g_ndefUrl.length() < 60) { g_ndefUrl += c; g_dirty = true; }
            }
        }
    }
    if (g_dirty) drawWriteNdef();
}

// ════════════════════════════════════════════════════════════════════════════
// EMULATE  (non-blocking state machine)
// ════════════════════════════════════════════════════════════════════════════

// Build TgInitAsTarget command using stored tag's UID and SAK
static uint8_t s_emulCmd[38];
void buildEmulCmd() {
    memset(s_emulCmd, 0, sizeof(s_emulCmd));
    s_emulCmd[0] = PN532_COMMAND_TGINITASTARGET;
    s_emulCmd[1] = 0x01;   // ISO14443A PICC passive only — prevents FeliCa polling response
    // SENS_RES (ATQA): use captured bytes if available, else default to MIFARE 1K (0x04, 0x00)
    if (g_tag.atqa[0] || g_tag.atqa[1]) {
        s_emulCmd[2] = g_tag.atqa[0];
        s_emulCmd[3] = g_tag.atqa[1];
    } else {
        s_emulCmd[2] = 0x04; s_emulCmd[3] = 0x00;
    }
    if (g_tag.uidLen >= 3) {
        s_emulCmd[4] = g_tag.uidBytes[0];
        s_emulCmd[5] = g_tag.uidBytes[1];
        s_emulCmd[6] = g_tag.uidBytes[2];
    }
    s_emulCmd[7] = g_tag.sak;   // SEL_RES
    // FeliCa params [8-25] = 0, NFCID3 [26-35] = 0, Ln=0, Lt=0 — already zeroed
}

void drawEmulate() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawHeader("NFC - Emulate");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    int y = BODY_Y + 2;

    if (!g_tag.valid) {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, y); d.print("No tag loaded."); y += 12;
        d.setCursor(8, y); d.print("Use Scan or Load first.");
        drawFooter("Bksp:menu");
    } else {
        d.setTextColor(C_DIM, C_BG); d.setCursor(8, y); d.print("Emulating:"); y += 11;
        d.setTextColor(C_FG,  C_BG); d.setCursor(8, y); d.print(g_tag.uid.c_str()); y += 10;
        d.setCursor(8, y); d.print(g_tag.type.c_str()); y += 13;

        if (g_emulating) {
            d.setTextColor(0x33CC66, C_BG);
            d.setCursor(8, y); d.print("ACTIVE - tap phone/reader"); y += 11;
            d.setTextColor(C_DIM, C_BG);
            d.setCursor(8, y); d.print("UID-only (auth=normal)"); y += 10;
            if (g_emulStatus.length()) {
                d.setTextColor(C_ACCENT, C_BG);
                d.setCursor(8, y); d.print(g_emulStatus.c_str());
            }
            drawFooter("Enter:stop  Bksp:menu");
        } else {
            d.setTextColor(C_DIM, C_BG);
            d.setCursor(8, y); d.print("Press Enter to start");
            drawFooter("Enter:start  Bksp:menu");
        }
    }
    g_dirty = false;
}

void handleEmulate() {
    auto ev = readKeys();
    if (ev.changed) {
        if (ev.back || ev.del) {
            g_emulating     = false;
            g_emulPhase     = EmulPhase::IDLE;
            g_emulWaitStart = 0;
            g_mode          = NfcMode::MENU;
            g_dirty         = true;
            return;
        }
        if (ev.enter && g_tag.valid) {
            g_emulating      = !g_emulating;
            g_emulPhase      = EmulPhase::IDLE;
            g_emulWaitStart  = 0;
            g_emulStatus     = "";
            g_dirty          = true;
        }
    }
    if (g_dirty) drawEmulate();

    if (!g_emulating || !g_nfcReady || !g_tag.valid) return;

    // Non-blocking state machine:
    //   IDLE → TgInitAsTarget → WAIT_ACK → WAIT_RESP
    //   (reader detected) → TgRelease → WAIT_REL_ACK → WAIT_REL_RESP → IDLE
    // TgRelease (0x52) cleanly ends the RF session before re-arming.
    // Without it, re-sending TgInitAsTarget while the PN532 is still in a reader
    // session causes I2C confusion that freezes the device.
    switch (g_emulPhase) {
        case EmulPhase::IDLE:
            buildEmulCmd();
            nfc.writecommand(s_emulCmd, sizeof(s_emulCmd));
            g_emulWaitStart = millis();
            g_emulPhase = EmulPhase::WAIT_ACK;
            break;

        case EmulPhase::WAIT_ACK:
            if (millis() - g_emulWaitStart > 2000) { g_emulPhase = EmulPhase::IDLE; break; }
            if (nfc.isready()) {
                if (nfc.readack()) {
                    g_emulWaitStart = millis();
                    g_emulPhase = EmulPhase::WAIT_RESP;
                } else {
                    g_emulPhase = EmulPhase::IDLE;
                }
            }
            break;

        case EmulPhase::WAIT_RESP:
            if (millis() - g_emulWaitStart > 2000) { g_emulPhase = EmulPhase::IDLE; break; }
            if (nfc.isready()) {
                uint8_t raw[20] = {};
                nfc.readdata(raw, sizeof(raw));
                if (raw[7] == 0x00) {
                    g_emulStatus = "Reader connected!";
                    g_dirty = true;
                }
                // Send TgRelease to end the session before re-arming TgInitAsTarget
                static const uint8_t REL[] = { 0x52, 0x01 };  // TgRelease target 1
                nfc.writecommand((uint8_t*)REL, sizeof(REL));
                g_emulWaitStart = millis();
                g_emulPhase = EmulPhase::WAIT_REL_ACK;
            }
            break;

        case EmulPhase::WAIT_REL_ACK:
            if (millis() - g_emulWaitStart > 500) { g_emulPhase = EmulPhase::IDLE; break; }
            if (nfc.isready()) {
                nfc.readack();
                g_emulWaitStart = millis();
                g_emulPhase = EmulPhase::WAIT_REL_RESP;
            }
            break;

        case EmulPhase::WAIT_REL_RESP:
            if (millis() - g_emulWaitStart > 500) { g_emulPhase = EmulPhase::IDLE; break; }
            if (nfc.isready()) {
                uint8_t resp[8] = {};
                nfc.readdata(resp, sizeof(resp));  // consume TgRelease response
                g_emulPhase = EmulPhase::IDLE;
            }
            break;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// LOAD from SD
// ════════════════════════════════════════════════════════════════════════════

void loadSdFileList() {
    g_sdFileCount = 0;
    if (!initSD()) return;
    if (!SD.exists(NFC_DIR)) return;
    File dir = SD.open(NFC_DIR);
    if (!dir) return;
    while (g_sdFileCount < MAX_SD_FILES) {
        File f = dir.openNextFile();
        if (!f) break;
        if (!f.isDirectory()) {
            String name = String(f.name());
            if (name.endsWith(".txt") || name.endsWith(".rfid") || name.endsWith(".nfc")) {
                g_sdFiles[g_sdFileCount++] = name;
            }
        }
        f.close();
    }
    dir.close();
}

bool parseDumpFile(const String& fname) {
    String path = String(NFC_DIR) + "/" + fname;
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) return false;
    g_tag.clear(); g_dump.clear();
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if      (line.startsWith("UID:"))  { g_tag.uid   = line.substring(5); g_tag.uid.trim(); g_tag.valid = true; }
        else if (line.startsWith("Type:")) { g_tag.type  = line.substring(6); g_tag.type.trim(); }
        else if (line.startsWith("SAK:"))  { g_tag.sak   = strtol(line.substring(5).c_str(), nullptr, 16); }
        else if (line.startsWith("ATQA:")) {
            String hex = line.substring(6); hex.trim();
            g_tag.atqa[0] = strtol(hex.substring(0, 2).c_str(), nullptr, 16);
            g_tag.atqa[1] = strtol(hex.substring(2, 4).c_str(), nullptr, 16);
        }
        else if (line.startsWith("NDEF:")) { g_tag.ndef  = line.substring(6); g_tag.ndef.trim(); }
        else if (line.startsWith("SLOT:")) { g_dump.slotSize = line.substring(6).toInt(); }
        else if (line.length() > 5 && line[0] == 'P') {
            int pgIdx = line.substring(1, 4).toInt();
            if (pgIdx >= 0 && pgIdx < MAX_SLOTS) {
                String hex = line.substring(6);
                hex.replace(" ", "");
                int bytes = min(g_dump.slotSize, 16);
                for (int i = 0; i < bytes && i * 2 + 1 < (int)hex.length(); i++) {
                    g_dump.data[pgIdx][i] = strtol(hex.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
                }
                if (pgIdx + 1 > g_dump.count) g_dump.count = pgIdx + 1;
            }
        }
    }
    f.close();
    g_dump.valid = (g_dump.count > 0);
    return g_tag.valid;
}

void drawLoad() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawHeader("NFC - Load from SD");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    int y = BODY_Y + 2;

    if (g_loadStatus.length()) {
        bool ok = g_loadStatus.startsWith("OK");
        d.setTextColor(ok ? (uint32_t)0x33CC66 : (uint32_t)C_ERROR, C_BG);
        d.setCursor(8, y); d.print(g_loadStatus.c_str()); y += 13;
        if (g_tag.valid) {
            d.setTextColor(C_FG, C_BG);
            d.setCursor(8, y); d.print(g_tag.uid.c_str()); y += 10;
            char buf[32];
            snprintf(buf, sizeof(buf), "%d slots  %s", g_dump.count, g_tag.type.c_str());
            d.setTextColor(C_DIM, C_BG);
            d.setCursor(8, y); d.print(buf);
        }
        drawFooter("Tag loaded: Clone/Emulate rdy  Bksp");
        g_dirty = false;
        return;
    }

    if (g_sdFileCount == 0) {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8, y); d.print("No dumps in /nfc/");
        drawFooter("Enter:refresh  Bksp:menu");
    } else {
        static constexpr int VISIBLE = 8;
        for (int i = 0; i < VISIBLE; i++) {
            int idx = i + g_sdFileScroll;
            if (idx >= g_sdFileCount) break;
            bool sel = (idx == g_sdFileSel);
            int fy = y + i * 11;
            uint32_t bg = sel ? C_HIGHLIGHT : (uint32_t)C_BG;
            d.fillRect(0, fy, SCREEN_W, 10, bg);
            d.setTextColor(sel ? (uint32_t)C_INPUT : (uint32_t)C_FG, bg);
            d.setCursor(4, fy + 1);
            d.print(g_sdFiles[idx].c_str());
        }
        drawFooter("fn+;/.=nav  Enter=load  Bksp:menu");
    }
    g_dirty = false;
}

void handleLoad() {
    if (g_sdFileCount == 0 && g_loadStatus.length() == 0) {
        loadSdFileList();
        g_dirty = true;
    }
    auto ev = readKeys();
    if (ev.changed) {
        if (ev.back || ev.del) {
            if (g_loadStatus.length()) { g_loadStatus = ""; g_dirty = true; }
            else { g_mode = NfcMode::MENU; g_dirty = true; }
            return;
        }
        if (ev.enter) {
            if (g_loadStatus.length()) { g_mode = NfcMode::MENU; g_dirty = true; return; }
            if (g_sdFileCount == 0) { loadSdFileList(); g_dirty = true; return; }
            bool ok = parseDumpFile(g_sdFiles[g_sdFileSel]);
            g_loadStatus = ok ? "OK! Loaded" : "ERR: Parse failed";
            g_dirty = true;
            return;
        }
        if (ev.up && g_sdFileSel > 0) {
            g_sdFileSel--;
            if (g_sdFileSel < g_sdFileScroll) g_sdFileScroll = g_sdFileSel;
            g_dirty = true;
        }
        if (ev.down && g_sdFileSel < g_sdFileCount - 1) {
            g_sdFileSel++;
            if (g_sdFileSel >= g_sdFileScroll + 8) g_sdFileScroll = g_sdFileSel - 7;
            g_dirty = true;
        }
    }
    if (g_dirty) drawLoad();
}

} // namespace

// ── Public entry points ─────────────────────────────────────────────────────

void appNfcEnter() {
    // Wire1.end() deadlocks the ESP32 I2C peripheral when no slave responded
    // during init — so we never call it. Initialize once and leave Wire1 up;
    // G8/G9 are NFC-only and idle between sessions.
    static bool s_wire1Up = false;
    if (!s_wire1Up) {
        Wire1.begin(NFC_SDA_PIN, NFC_SCL_PIN);
        Wire1.setClock(100000);
        Wire1.setTimeOut(80);
        s_wire1Up = true;
    }
    // Drain any stale response left over from a previous session (e.g. TgInitAsTarget)
    if (nfc.isready()) {
        uint8_t drain[20] = {};
        nfc.readdata(drain, sizeof(drain));
    }
    nfc.begin();
    g_fwVer    = nfc.getFirmwareVersion();
    g_nfcReady = (g_fwVer != 0);
    if (g_nfcReady) {
        nfc.SAMConfig();
        nfc.setPassiveActivationRetries(0xFF);
        uint8_t flush[8] = {};
        nfc.readdata(flush, sizeof(flush));  // drain stale RFConfig response
    }
    g_mode       = NfcMode::MENU;
    g_menuSel    = 0;
    g_scanning   = false;
    g_emulating  = false;
    g_emulPhase  = EmulPhase::IDLE;
    g_tag.clear();
    g_dump.clear();
    g_diagAck    = -1;
    g_diagRaw7   = 0;
    g_diagCycles = 0;
    g_dirty      = true;
}

void appNfcLoop() {
    switch (g_mode) {
        case NfcMode::MENU:       handleMenu();       break;
        case NfcMode::SCAN:       handleScan();       break;
        case NfcMode::READ:       handleRead();       break;
        case NfcMode::CLONE:      handleClone();      break;
        case NfcMode::WRITE_NDEF: handleWriteNdef();  break;
        case NfcMode::EMULATE:    handleEmulate();    break;
        case NfcMode::LOAD:       handleLoad();       break;
    }
}
