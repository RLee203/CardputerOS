#include "app_ble.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <SD.h>

static constexpr int MAX_DEVICES = 48;
static constexpr int ROWS_VIS    = 10;   // visible rows in list view
static constexpr int SCAN_SECS   = 1;    // seconds per scan (short so back key is responsive)

struct BLEEntry {
    char mac[18];
    char name[21];
    int  rssi;
};

// ── Global scan state (accessed from callback) ─────────────────────────────
static BLEEntry s_devices[MAX_DEVICES];
static int      s_devCount   = 0;
static bool     s_wardrive   = false;
static File     s_wdFile;
static int      s_wdTotal    = 0;

class BLEScanCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice dev) override {
        if (s_wardrive) {
            if (s_wdFile) {
                char line[96];
                snprintf(line, sizeof(line), "%lu,%s,%d,%s\n",
                    (unsigned long)millis(),
                    dev.getAddress().toString().c_str(),
                    dev.getRSSI(),
                    dev.haveName() ? dev.getName().c_str() : "");
                s_wdFile.print(line);
            }
            s_wdTotal++;
        } else {
            if (s_devCount >= MAX_DEVICES) return;
            auto& e = s_devices[s_devCount++];
            strncpy(e.mac,  dev.getAddress().toString().c_str(), 17); e.mac[17] = '\0';
            strncpy(e.name, dev.haveName() ? dev.getName().c_str() : "(unknown)", 20);
            e.name[20] = '\0';
            e.rssi = dev.getRSSI();
        }
    }
};

namespace {

enum class BleState { MENU, SCANNING, LIST, WARDRIVING };

bool        s_bleInited  = false;
BleState    s_state      = BleState::MENU;
bool        s_dirty      = true;
int         s_scroll     = 0;
int         s_wdScans    = 0;
bool        s_wdSdOk     = false;
char        s_wdFilePath[32] = {};
BLEScan*    s_scan       = nullptr;
BLEScanCallbacks s_callbacks;

void ensureBleInit() {
    if (s_bleInited) return;
    BLEDevice::init("");
    s_scan = BLEDevice::getScan();
    s_scan->setAdvertisedDeviceCallbacks(&s_callbacks);
    s_scan->setActiveScan(true);
    s_scan->setInterval(100);
    s_scan->setWindow(99);
    s_bleInited = true;
}

void drawStatusBar(const char* title) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_STATUS_FG, C_STATUS_BG);
    d.setCursor(2, 3);
    d.print(title);
    drawBatteryWidget(C_STATUS_BG, SCREEN_W - 43);
}

void drawMenu() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("BLE Scanner");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    // SCAN card
    d.fillRoundRect(10, 22, 220, 30, 6, 0x001A40);
    d.drawRoundRect(10, 22, 220, 30, 6, 0x0055CC);
    d.setTextColor(0x6699FF, 0x001A40);
    d.setTextSize(2);
    d.setCursor(20, 29);
    d.print("SCAN");
    d.setTextSize(1);

    // WARDRIVE card
    d.fillRoundRect(10, 62, 220, 30, 6, 0x1A0040);
    d.drawRoundRect(10, 62, 220, 30, 6, 0x6600CC);
    d.setTextColor(0xAA66FF, 0x1A0040);
    d.setTextSize(2);
    d.setCursor(20, 69);
    d.print("WARDRIVE");
    d.setTextSize(1);

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, 106);
    d.print("Enter=scan  W=wardrive  fn+bksp=back");
}

void drawScanning(bool ward, int scanNum, int total) {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar(ward ? "BLE Wardrive" : "BLE Scanner");
    d.setFont(&fonts::Font0);
    d.setTextSize(2);
    d.setTextColor(C_FG, C_BG);
    const char* msg = "Scanning...";
    d.setCursor((SCREEN_W - (int)strlen(msg) * FONT_W * 2) / 2, 36);
    d.print(msg);
    d.setTextSize(1);
    if (ward) {
        char buf[40];
        snprintf(buf, sizeof(buf), "Scan #%d   Logged: %d", scanNum, total);
        d.setTextColor(C_FG, C_BG);
        d.setCursor((SCREEN_W - (int)strlen(buf) * FONT_W) / 2, 64);
        d.print(buf);

        // SD status
        if (s_wdSdOk) {
            d.setTextColor(0x00AA00, C_BG);
            d.setCursor(2, 76); d.print("SD: "); d.print(s_wdFilePath);
        } else {
            d.setTextColor(C_ERROR, C_BG);
            d.setCursor(2, 76); d.print("SD: not found - logging disabled");
        }
    }
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 1);
    // Hold fn+bksp OR press Enter between scans (1-sec window) to stop
    d.print(ward ? "Hold fn+bksp OR Enter to stop" : "Please wait...");
}

void drawList() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("BLE Scanner");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    if (s_devCount == 0) {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(14, 54); d.print("No devices found");
    } else {
        for (int i = 0; i < ROWS_VIS && (s_scroll + i) < s_devCount; i++) {
            const auto& dev = s_devices[s_scroll + i];
            int y = STATUS_H + 2 + i * 11;

            uint32_t rssiCol = dev.rssi >= -70 ? (uint32_t)0x00CC00
                             : dev.rssi >= -80 ? (uint32_t)0xCCAA00
                                              : (uint32_t)0xFF5555;
            // RSSI (4 chars)
            d.setTextColor(rssiCol, C_BG);
            char rb[5]; snprintf(rb, sizeof(rb), "%4d", dev.rssi);
            d.setCursor(0, y); d.print(rb);

            // Last 3 octets of MAC (short form)
            d.setTextColor(0x888888, C_BG);
            d.setCursor(26, y);
            d.print(dev.mac + 9);   // "DD:EE:FF"

            // Name (truncated)
            d.setTextColor(C_FG, C_BG);
            d.setCursor(82, y);
            char nm[21]; strncpy(nm, dev.name, 20); nm[20] = '\0';
            d.print(nm);
        }
    }

    d.fillRect(0, SCREEN_H - FONT_H - 2, SCREEN_W, FONT_H + 2, C_BG);
    d.setTextColor(C_DIM, C_BG);
    char footer[48];
    snprintf(footer, sizeof(footer), "%d found  up/dn=scroll  Enter=rescan", s_devCount);
    d.setCursor(2, SCREEN_H - FONT_H - 1);
    d.print(footer);
}

bool openWardiveFile() {
    digitalWrite(LORA_NSS_PIN, HIGH);
    digitalWrite(LORA_RST_PIN, LOW);
    if (!SD.begin(SD_CS_PIN, SPI, 25000000)) return false;
    if (!SD.exists("/ble")) SD.mkdir("/ble");
    int n = 1;
    do { snprintf(s_wdFilePath, sizeof(s_wdFilePath), "/ble/scan_%04d.csv", n++); }
    while (SD.exists(s_wdFilePath) && n < 9999);
    s_wdFile = SD.open(s_wdFilePath, FILE_WRITE);
    if (!s_wdFile) return false;
    s_wdFile.println("millis,mac,rssi,name");
    return true;
}

void doScan() {
    s_scan->clearResults();
    s_scan->start(SCAN_SECS, false);
    if (s_wdFile) s_wdFile.flush();
}

} // namespace

void appBleEnter() {
    s_state    = BleState::MENU;
    s_dirty    = true;
    s_devCount = 0;
    s_scroll   = 0;
    ensureBleInit();
}

void appBleLoop() {
    // Wardriving: run 1-second scans, check keys between each scan
    if (s_state == BleState::WARDRIVING) {
        drawScanning(true, s_wdScans + 1, s_wdTotal);
        doScan();
        s_wdScans++;
        // 1-second scan window: user can hold fn+bksp or press Enter to stop
        auto ev = readKeys();
        if (ev.back || ev.enter) {
            if (s_wdFile) { s_wdFile.close(); }
            s_state   = BleState::MENU;
            s_dirty   = true;
            s_wdScans = 0;
            s_wdTotal = 0;
            s_wdSdOk  = false;
            s_wardrive = false;
        }
        return;
    }

    if (s_dirty) {
        switch (s_state) {
            case BleState::MENU:     drawMenu();  break;
            case BleState::LIST:     drawList();  break;
            default: break;
        }
        s_dirty = false;
    }

    auto ev = readKeys();
    if (!ev.changed) return;

    if (ev.back) {
        if (s_state == BleState::MENU) { goHome(); return; }
        s_state = BleState::MENU;
        s_dirty = true;
        return;
    }

    switch (s_state) {
        case BleState::MENU: {
            bool ward = false;
            if (!ev.fnKey) {
                for (char c : ev.chars) {
                    if (c == 'w' || c == 'W') { ward = true; break; }
                }
            }
            if (ev.enter || ward) {
                if (ward) {
                    s_wdTotal   = 0;
                    s_wdScans   = 0;
                    s_wardrive  = true;
                    s_wdSdOk    = openWardiveFile();
                    if (!s_wdSdOk) {
                        // SD failed — stay on menu, show brief message
                        auto& d = M5Cardputer.Display;
                        d.fillRect(0, SCREEN_H - FONT_H - 2, SCREEN_W, FONT_H + 2, C_BG);
                        d.setTextColor(C_ERROR, C_BG);
                        d.setCursor(2, SCREEN_H - FONT_H - 1);
                        d.print("SD card not found!");
                        delay(1500);
                        s_wardrive  = false;
                        s_dirty     = true;
                        break;
                    }
                    s_state = BleState::WARDRIVING;
                } else {
                    s_devCount = 0;
                    s_scroll   = 0;
                    s_wardrive = false;
                    drawScanning(false, 0, 0);
                    doScan();
                    s_state = BleState::LIST;
                    s_dirty = true;
                }
            }
            break;
        }

        case BleState::LIST:
            if (ev.up   && s_scroll > 0)              { s_scroll--; s_dirty = true; }
            if (ev.down && s_scroll < s_devCount - 1) { s_scroll++; s_dirty = true; }
            if (ev.enter) {
                // Rescan
                s_devCount = 0;
                s_scroll   = 0;
                drawScanning(false, 0, 0);
                doScan();
                s_dirty = true;
            }
            break;

        default:
            break;
    }
}
