#include "app_sd_health.h"
#include "config.h"
#include "input.h"
#include "nav.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>

static bool     g_dirty = true;
static bool     g_sdOk = false;
static uint64_t g_total = 0;
static uint64_t g_used = 0;
static uint64_t g_free = 0;
static String   g_status = "Press Enter to test";
static String   g_fsType = "FAT";

static String fmtBytes(uint64_t bytes) {
    char buf[32];
    if (bytes >= (1024ULL * 1024ULL * 1024ULL)) {
        snprintf(buf, sizeof(buf), "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= (1024ULL * 1024ULL)) {
        snprintf(buf, sizeof(buf), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        snprintf(buf, sizeof(buf), "%.1f KB", (double)bytes / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%llu B", bytes);
    }
    return String(buf);
}

static void refreshCardInfo() {
    digitalWrite(LORA_NSS_PIN, HIGH);
    digitalWrite(LORA_RST_PIN, LOW);
    SD.end();
    delay(30);
    g_sdOk = SD.begin(SD_CS_PIN, SPI, 25000000);
    if (!g_sdOk) {
        g_total = g_used = g_free = 0;
        g_status = "SD mount failed";
        return;
    }
    g_total = SD.totalBytes();
    g_used  = SD.usedBytes();
    g_free  = (g_total > g_used) ? (g_total - g_used) : 0;
    g_status = "Card mounted";
}

static void runCardTest() {
    refreshCardInfo();
    if (!g_sdOk) return;

    const char* path = "/sd_health_test.tmp";
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        g_status = "Write test failed";
        return;
    }
    f.print("ghostwire-sd-check");
    f.close();

    f = SD.open(path, FILE_READ);
    if (!f) {
        g_status = "Read test failed";
        SD.remove(path);
        return;
    }
    String content = f.readString();
    f.close();

    if (content != "ghostwire-sd-check") {
        g_status = "Verify failed";
        SD.remove(path);
        return;
    }

    if (!SD.remove(path)) {
        g_status = "Delete test failed";
        return;
    }

    g_status = "Read/write test passed";
    refreshCardInfo();
    if (g_sdOk) g_status = "Read/write test passed";
}

static void drawApp() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.fillRect(0, 0, SCREEN_W, STATUS_H, 0x0A1520);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(0x4FD9FF, 0x0A1520);
    d.setCursor(2, 3);
    d.print("SD Health");

    d.fillRoundRect(8, 20, SCREEN_W - 16, 76, 5, 0x101820);
    d.drawRoundRect(8, 20, SCREEN_W - 16, 76, 5, 0x1E5C7A);

    d.setTextColor(g_sdOk ? 0x44F18A : C_ERROR, 0x101820);
    d.setCursor(16, 30);
    d.print(g_sdOk ? "Status: MOUNTED" : "Status: NOT FOUND");

    d.setTextColor(C_FG, 0x101820);
    d.setCursor(16, 44);
    d.print("FS: ");
    d.print(g_fsType);

    d.setCursor(16, 56);
    d.print("Total: ");
    d.print(fmtBytes(g_total));

    d.setCursor(16, 68);
    d.print("Used:  ");
    d.print(fmtBytes(g_used));

    d.setCursor(16, 80);
    d.print("Free:  ");
    d.print(fmtBytes(g_free));

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(10, 104);
    d.print(g_status);
    d.setCursor(2, SCREEN_H - FONT_H - 2);
    d.print("Enter=test  Del=refresh  Back=home");
}

void appSdHealthEnter() {
    suspendWifiForSd();
    refreshCardInfo();
    g_dirty = true;
}

void appSdHealthLoop() {
    if (g_dirty) {
        drawApp();
        g_dirty = false;
    }

    auto ev = readKeys();
    if (!ev.changed) return;
    if (ev.back) {
        goHome();
        return;
    }
    if (ev.enter) {
        runCardTest();
        g_dirty = true;
        return;
    }
    if (ev.del) {
        refreshCardInfo();
        g_dirty = true;
        return;
    }
}
