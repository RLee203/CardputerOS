#include "app_firmware.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>

// ── State ──────────────────────────────────────────────────────────────────
enum class FwScene { LIST, CONFIRM, FLASHING, DONE, ERROR };

static FwScene  fwScene  = FwScene::LIST;
static bool     fwDirty  = true;
static bool     sdOk     = false;
static String   binFiles[32];
static size_t   binSizes[32];
static int      binCount = 0;
static int      binSel   = 0;
static int      fwSel    = 0;
static String   fwError;

// ── Helpers ────────────────────────────────────────────────────────────────

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

static void drawList() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("Firmware Installer");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    if (!sdOk) {
        d.setTextColor(C_ERROR, C_BG);
        d.setCursor(4, STATUS_H + 8);  d.print("SD card not found.");
        d.setTextColor(C_DIM,   C_BG);
        d.setCursor(4, STATUS_H + 20); d.print("fn+bksp = back");
        return;
    }
    if (binCount == 0) {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(4, STATUS_H + 8);  d.print("No .bin files on SD.");
        d.setCursor(4, STATUS_H + 20); d.print("fn+bksp = back");
        return;
    }

    int vis = (SCREEN_H - STATUS_H - FONT_H - 4) / (FONT_H + 2);
    int off = (fwSel >= vis) ? fwSel - vis + 1 : 0;
    for (int i = 0; i < vis; i++) {
        int idx = i + off;
        if (idx >= binCount) break;
        bool sel = (idx == fwSel);
        int y = STATUS_H + i * (FONT_H + 2);
        uint32_t bg = sel ? C_HIGHLIGHT : C_BG;
        d.fillRect(0, y, SCREEN_W, FONT_H + 2, bg);
        d.setTextColor(sel ? (uint32_t)C_INPUT : (uint32_t)C_FG, bg);
        d.setCursor(4, y + 1);
        char line[40];
        size_t sz = binSizes[idx];
        int kb = (int)(sz / 1024);
        snprintf(line, sizeof(line), "%-28s%4dK", binFiles[idx].c_str(), kb);
        d.print(line);
    }
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 2);
    d.print("Enter=flash  fn+bksp=back");
}

static void drawConfirm() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("Confirm Flash");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_FG, C_BG);
    d.setCursor(4, STATUS_H + 8);
    d.print("Flash firmware?");
    d.setTextColor(C_ERROR, C_BG);
    d.setCursor(4, STATUS_H + 22);
    char buf[36];
    snprintf(buf, sizeof(buf), "%.34s", binFiles[fwSel].c_str());
    d.print(buf);
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(4, STATUS_H + 36);
    char sz[20];
    snprintf(sz, sizeof(sz), "Size: %d KB", (int)(binSizes[fwSel] / 1024));
    d.print(sz);
    d.setTextColor(0xFFAA00, C_BG);
    d.setCursor(4, STATUS_H + 52);
    d.print("Device will restart after flash.");
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(4, SCREEN_H - FONT_H * 2 - 4);
    d.print("Enter = YES, flash now");
    d.setCursor(4, SCREEN_H - FONT_H - 2);
    d.print("fn+bksp = cancel");
}

static void drawProgress(size_t written, size_t total) {
    auto& d = M5Cardputer.Display;
    int pct = (total > 0) ? (int)((uint64_t)written * 100 / total) : 0;
    // Progress bar
    constexpr int BAR_X = 8, BAR_Y = 70, BAR_W = 224, BAR_H = 12;
    d.fillRect(BAR_X, BAR_Y, BAR_W, BAR_H, C_HIGHLIGHT);
    d.fillRect(BAR_X, BAR_Y, (int)((uint64_t)(BAR_W) * written / (total ? total : 1)), BAR_H, 0x0099FF);
    d.drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, C_DIM);
    d.setFont(&fonts::Font0);
    d.setTextSize(2);
    d.setTextColor(C_FG, C_BG);
    d.fillRect(0, 88, SCREEN_W, 16, C_BG);
    char buf[12];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    int tw = strlen(buf) * FONT_W * 2;
    d.setCursor((SCREEN_W - tw) / 2, 88);
    d.print(buf);
}

// Detect the offset of the actual application image inside the file.
// Returns 0 for an app-only binary, 0x10000 for a full merged binary,
// or UINT32_MAX if the file doesn't look like a valid ESP32 image.
static uint32_t detectAppOffset(File& f) {
    uint8_t magic;
    // Check offset 0x10000 first (merged binary app section)
    if (f.size() > 0x12000) {
        f.seek(0x10000);
        if (f.read(&magic, 1) == 1 && magic == 0xE9) {
            // Confirm offset 0 is also 0xE9 (bootloader) or padding
            f.seek(0);
            uint8_t m0;
            f.read(&m0, 1);
            if (m0 == 0xE9 || m0 == 0xFF) return 0x10000;
        }
    }
    // Check offset 0 for an app-only binary
    f.seek(0);
    if (f.read(&magic, 1) == 1 && magic == 0xE9) return 0;
    return UINT32_MAX;  // unknown format
}

static void doFlash() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("Flashing...");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_FG, C_BG);
    d.setCursor(4, STATUS_H + 12);
    char buf[40];
    snprintf(buf, sizeof(buf), "%.38s", binFiles[fwSel].c_str());
    d.print(buf);
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(4, STATUS_H + 26);
    d.print("Do not power off...");

    // Confirm an OTA destination partition exists before touching the file
    if (!esp_ota_get_next_update_partition(NULL)) {
        fwError = "No OTA slot — need dual-app partition table";
        fwScene = FwScene::ERROR;
        fwDirty = true;
        return;
    }

    String path = binFiles[fwSel];
    if (!path.startsWith("/")) path = "/" + path;
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) {
        fwError = "Cannot open file";
        fwScene = FwScene::ERROR;
        fwDirty = true;
        return;
    }

    uint32_t appOffset = detectAppOffset(f);
    if (appOffset == UINT32_MAX) {
        f.close();
        fwError = "Not a valid ESP32 .bin";
        fwScene = FwScene::ERROR;
        fwDirty = true;
        return;
    }

    f.seek(appOffset);
    size_t appSize = f.size() - appOffset;

    esp_task_wdt_reset();  // feed WDT before partition erase (can take seconds)
    if (!Update.begin(appSize)) {
        f.close();
        fwError = String("Flash init: ") + Update.errorString();
        fwScene = FwScene::ERROR;
        fwDirty = true;
        return;
    }

    static uint8_t chunk[4096];
    size_t written = 0;
    bool ok = true;
    while (f.available() && ok) {
        int n = f.read(chunk, sizeof(chunk));
        if (n <= 0) break;
        if (Update.write(chunk, n) != (size_t)n) {
            ok = false;
            break;
        }
        written += n;
        drawProgress(written, appSize);
        esp_task_wdt_reset();
        yield();
    }
    f.close();

    if (ok && Update.end(true)) {
        fwScene = FwScene::DONE;
        fwDirty = true;
    } else {
        fwError = Update.hasError()
                    ? (String("Write error: ") + Update.errorString())
                    : "Write incomplete";
        Update.abort();
        fwScene = FwScene::ERROR;
        fwDirty = true;
    }
}

static void drawDone() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("Flash Complete");
    d.setFont(&fonts::Font0);
    d.setTextSize(2);
    d.setTextColor(0x00CC66, C_BG);
    const char* msg = "Success!";
    d.setCursor((SCREEN_W - (int)strlen(msg) * FONT_W * 2) / 2, STATUS_H + 20);
    d.print(msg);
    d.setTextSize(1);
    d.setTextColor(C_FG, C_BG);
    d.setCursor(4, STATUS_H + 52);
    d.print("Restarting in 3 seconds...");
}

static void drawError() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("Flash Error");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_ERROR, C_BG);
    d.setCursor(4, STATUS_H + 12);
    d.print(fwError.c_str());
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(4, STATUS_H + 28);
    d.print("fn+bksp = back");
}

// ── Public API ─────────────────────────────────────────────────────────────

void appFirmwareEnter() {
    fwScene = FwScene::LIST;
    fwDirty = true;
    binCount = 0;
    fwSel    = 0;

    digitalWrite(LORA_NSS_PIN, HIGH);
    digitalWrite(LORA_RST_PIN, LOW);
    SD.end();
    delay(40);
    sdOk = SD.begin(SD_CS_PIN, SPI, 25000000);
    if (sdOk) {
        File root = SD.open("/");
        while (binCount < 32) {
            File f = root.openNextFile();
            if (!f) break;
            if (!f.isDirectory()) {
                String name = f.name();
                String lo   = name; lo.toLowerCase();
                if (lo.endsWith(".bin")) {
                    binFiles[binCount] = name;
                    binSizes[binCount] = f.size();
                    binCount++;
                }
            }
            f.close();
        }
        root.close();
    }
}

void appFirmwareLoop() {
    if (fwDirty) {
        switch (fwScene) {
            case FwScene::LIST:     drawList();    break;
            case FwScene::CONFIRM:  drawConfirm(); break;
            case FwScene::ERROR:    drawError();   break;
            case FwScene::DONE:     drawDone();    break;
            default: break;
        }
        fwDirty = false;
    }

    if (fwScene == FwScene::DONE) {
        delay(3000);
        ESP.restart();
        return;
    }

    if (fwScene == FwScene::FLASHING) return;  // handled in doFlash()

    auto ev = readKeys();
    if (!ev.changed) return;

    if (ev.back) {
        if (fwScene == FwScene::CONFIRM) {
            fwScene = FwScene::LIST;
            fwDirty = true;
        } else {
            goHome();
        }
        return;
    }

    if (fwScene == FwScene::ERROR) return;

    if (fwScene == FwScene::LIST) {
        if (ev.up   && fwSel > 0)            { fwSel--; fwDirty = true; }
        if (ev.down && fwSel < binCount - 1) { fwSel++; fwDirty = true; }
        if (ev.enter && binCount > 0) {
            fwScene = FwScene::CONFIRM;
            fwDirty = true;
        }
        return;
    }

    if (fwScene == FwScene::CONFIRM && ev.enter) {
        fwScene = FwScene::FLASHING;
        doFlash();
        return;
    }
}
