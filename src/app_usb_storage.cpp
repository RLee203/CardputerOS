#include "app_usb_storage.h"
#include "nav.h"
#include "input.h"
#include "config.h"

#include <M5Cardputer.h>
#include <SD.h>
#include "USB.h"
#include "USBMSC.h"

namespace {
USBMSC usbMsc;

bool usbStorageDirty = true;
bool usbStorageActive = false;
bool usbStorageSdReady = false;
bool usbStorageCableSeen = false;
bool usbStorageShouldExit = false;
String usbStorageStatus;
uint32_t usbStorageStatusUntilMs = 0;

constexpr uint32_t USB_STORAGE_STATUS_MS = 1800;

void showStatus(const String& text) {
    usbStorageStatus = text;
    usbStorageStatusUntilMs = millis() + USB_STORAGE_STATUS_MS;
    usbStorageDirty = true;
}

void clearExpiredStatus() {
    if (usbStorageStatus.length() > 0 && millis() >= usbStorageStatusUntilMs) {
        usbStorageStatus = "";
        usbStorageDirty = true;
    }
}

bool mountSdForStorage() {
    if (usbStorageSdReady) return true;
    SD.end();
    usbStorageSdReady = SD.begin(SD_CS_PIN, SPI, 25000000);
    return usbStorageSdReady;
}

void unmountSdFromStorage() {
    if (!usbStorageSdReady) return;
    SD.end();
    usbStorageSdReady = false;
}

int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    const uint32_t sectorSize = SD.sectorSize();
    if (sectorSize == 0 || offset != 0 || (bufsize % sectorSize) != 0) return -1;

    const uint32_t blocks = bufsize / sectorSize;
    auto* out = static_cast<uint8_t*>(buffer);
    for (uint32_t i = 0; i < blocks; ++i) {
        if (!SD.readRAW(out + (i * sectorSize), lba + i)) return -1;
    }
    return static_cast<int32_t>(bufsize);
}

int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    const uint32_t sectorSize = SD.sectorSize();
    if (sectorSize == 0 || offset != 0 || (bufsize % sectorSize) != 0) return -1;

    const uint32_t blocks = bufsize / sectorSize;
    for (uint32_t i = 0; i < blocks; ++i) {
        if (!SD.writeRAW(buffer + (i * sectorSize), lba + i)) return -1;
    }
    return static_cast<int32_t>(bufsize);
}

bool onStartStop(uint8_t powerCondition, bool start, bool loadEject) {
    (void)powerCondition;
    if (!start && loadEject) {
        usbStorageShouldExit = true;
        return true;
    }
    return true;
}

void usbEventCallback(void* arg, esp_event_base_t eventBase, int32_t eventId, void* eventData) {
    (void)arg;
    (void)eventData;
    if (eventBase != ARDUINO_USB_EVENTS) return;

    switch (eventId) {
        case ARDUINO_USB_STARTED_EVENT:
            usbStorageCableSeen = true;
            usbStorageDirty = true;
            break;
        case ARDUINO_USB_STOPPED_EVENT:
            usbStorageCableSeen = false;
            usbStorageDirty = true;
            break;
        case ARDUINO_USB_SUSPEND_EVENT:
        case ARDUINO_USB_RESUME_EVENT:
        default:
            break;
    }
}

void drawThumbDrive(bool active) {
    auto& d = M5Cardputer.Display;

    const int iconX = 58;
    const int iconY = 33;
    const int iconW = 124;
    const int iconH = 40;
    const int bodyW = 84;
    const int portW = 34;
    const int ledW = 8;
    const int ledH = 20;

    uint32_t bodyColor = 0x0A4E63;
    uint32_t portColor = 0xD6D6D6;
    uint32_t detailColor = 0x707070;
    uint32_t ledColor = active ? 0x22FF66 : 0xFF4040;

    d.fillRoundRect(iconX, iconY, bodyW, iconH, 7, bodyColor);
    d.fillRoundRect(iconX + bodyW - 3, iconY + 4, portW, iconH - 8, 5, portColor);
    d.fillRoundRect(iconX + bodyW + 9, iconY + 10, 12, 5, 2, detailColor);
    d.fillRoundRect(iconX + bodyW + 9, iconY + 25, 12, 5, 2, detailColor);
    d.fillRoundRect(iconX + 15, iconY + 10, ledW, ledH, 3, ledColor);
}

void drawScreen() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    d.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS_BG);
    d.setTextColor(C_STATUS_FG, C_STATUS_BG);
    d.setCursor(2, 3);
    d.print("USB Storage");
    drawBatteryWidget(C_STATUS_BG, SCREEN_W - 43);

    uint32_t cardBg = usbStorageActive ? 0x062F1D : 0x2B120E;
    uint32_t cardBorder = usbStorageActive ? 0x00D27A : 0xFF5B5B;
    uint32_t stateColor = usbStorageActive ? 0x6CFFB2 : 0xFF8686;
    d.fillRoundRect(10, 20, 220, 58, 8, cardBg);
    d.drawRoundRect(10, 20, 220, 58, 8, cardBorder);
    d.drawRoundRect(11, 21, 218, 56, 7, cardBorder);

    d.setTextColor(C_FG, cardBg);
    d.setCursor(17, 27);
    d.print("Share SD card over USB-C");

    drawThumbDrive(usbStorageActive);

    d.setTextColor(stateColor, cardBg);
    d.setTextSize(2);
    const char* stateText = usbStorageActive ? "ON" : "OFF";
    int sw = strlen(stateText) * FONT_W * 2;
    d.setCursor(191 - sw, 45);
    d.print(stateText);
    d.setTextSize(1);

    d.setTextColor(C_FG, C_BG);
    d.setCursor(14, 88);
    if (usbStorageActive) {
        d.print("PC can read/write SD card");
        d.setCursor(14, 98);
        d.print("Exit before opening files here");
    } else {
        d.print("Enter toggles storage mode");
        d.setCursor(14, 98);
        d.print("Card stays local until enabled");
    }

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(14, 112);
    d.print("fn+bksp exits");

    d.fillRect(0, SCREEN_H - 10, SCREEN_W, 10, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 1);
    if (usbStorageStatus.length() > 0) {
        d.setTextColor(C_ACCENT, C_BG);
        d.print(usbStorageStatus.c_str());
    } else if (usbStorageActive) {
        d.setTextColor(C_DIM, C_BG);
        d.print(usbStorageCableSeen ? "Storage shared over USB" : "Storage ON");
    } else {
        d.setTextColor(C_DIM, C_BG);
        d.print("Enter=toggle  fn+bksp=home");
    }
}

bool beginMassStorage() {
    if (!mountSdForStorage()) {
        showStatus("SD mount failed");
        return false;
    }

    usbMsc.vendorID("M5Stack");
    usbMsc.productID("Cardputer SD");
    usbMsc.productRevision("1.4");
    usbMsc.onRead(onRead);
    usbMsc.onWrite(onWrite);
    usbMsc.onStartStop(onStartStop);
    usbMsc.mediaPresent(true);
    if (!usbMsc.begin(static_cast<uint32_t>(SD.numSectors()), static_cast<uint16_t>(SD.sectorSize()))) {
        showStatus("USB storage start failed");
        return false;
    }

    USB.begin();
    usbStorageActive = true;
    usbStorageCableSeen = true;
    showStatus("USB storage enabled");
    return true;
}

void endMassStorage() {
    if (usbStorageActive) {
        usbMsc.mediaPresent(false);
        usbMsc.end();
    }
    usbStorageActive = false;
    usbStorageCableSeen = false;
    unmountSdFromStorage();
    if (mountSdForStorage()) {
        showStatus("USB storage disabled");
    } else {
        showStatus("Remount failed");
    }
}
}

void appUsbStorageEnter() {
    usbStorageDirty = true;
    usbStorageStatus = "";
    usbStorageStatusUntilMs = 0;
    usbStorageShouldExit = false;
    mountSdForStorage();
    USB.onEvent(usbEventCallback);
}

void appUsbStorageLoop() {
    clearExpiredStatus();

    if (usbStorageShouldExit) {
        endMassStorage();
        usbStorageShouldExit = false;
        goHome();
        return;
    }

    if (usbStorageDirty) {
        drawScreen();
        usbStorageDirty = false;
    }

    auto ev = readKeys();
    if (!ev.changed) return;

    if (ev.back) {
        endMassStorage();
        goHome();
        return;
    }

    if (ev.enter) {
        if (usbStorageActive) {
            endMassStorage();
        } else {
            beginMassStorage();
        }
        usbStorageDirty = true;
        return;
    }
}
