#include "app_hid_keyboard.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>
#include "USB.h"
#ifdef KEY_BACKSPACE
#undef KEY_BACKSPACE
#endif
#ifdef KEY_TAB
#undef KEY_TAB
#endif
#include "USBHIDKeyboard.h"

namespace {
USBHIDKeyboard usbKeyboard;
bool hidDirty = true;
bool hidEnabled = false;
String hidStatus;
uint32_t hidStatusUntilMs = 0;
static constexpr uint32_t HID_STATUS_MS = 1600;

void showStatus(const String& text) {
    hidStatus = text;
    hidStatusUntilMs = millis() + HID_STATUS_MS;
    hidDirty = true;
}

void clearExpiredStatus() {
    if (hidStatus.length() > 0 && millis() >= hidStatusUntilMs) {
        hidStatus = "";
        hidDirty = true;
    }
}

void ensureUsbState(bool enabled) {
    if (enabled == hidEnabled) return;
    hidEnabled = enabled;
    if (hidEnabled) {
        usbKeyboard.begin();
        USB.begin();
        showStatus("USB keyboard enabled");
    } else {
        usbKeyboard.releaseAll();
        usbKeyboard.end();
        showStatus("USB keyboard disabled");
    }
}

void drawScreen() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    d.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS_BG);
    d.setTextColor(C_STATUS_FG, C_STATUS_BG);
    d.setCursor(2, 3);
    d.print("HID Keyboard");
    drawBatteryWidget(C_STATUS_BG, SCREEN_W - 43);

    uint32_t cardBg = hidEnabled ? 0x003B22 : 0x2A2400;
    uint32_t cardBorder = hidEnabled ? 0x00D27A : 0xD8B000;
    uint32_t stateColor = hidEnabled ? 0x6CFFB2 : 0xFFE36C;
    d.fillRoundRect(10, 22, 220, 56, 8, cardBg);
    d.drawRoundRect(10, 22, 220, 56, 8, cardBorder);
    d.drawRoundRect(11, 23, 218, 54, 7, cardBorder);

    d.setTextColor(C_FG, cardBg);
    d.setCursor(20, 30);
    d.print("USB-C keyboard mode");

    d.setTextColor(stateColor, cardBg);
    d.setTextSize(2);
    const char* stateText = hidEnabled ? "ACTIVE" : "READY";
    int sw = strlen(stateText) * FONT_W * 2;
    d.setCursor((SCREEN_W - sw) / 2, 44);
    d.print(stateText);
    d.setTextSize(1);

    d.setTextColor(C_FG, C_BG);
    d.setCursor(14, 88);
    if (hidEnabled) {
        d.print("Typing to host over USB");
    } else {
        d.print("Press Enter to enable");
    }

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(14, 102);
    d.print("Keys: Tab  Enter  Del  Arrows");
    d.setCursor(14, 114);
    d.print("fn+bksp exits + disables");

    d.fillRect(0, SCREEN_H - 10, SCREEN_W, 10, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 1);
    if (hidStatus.length() > 0) {
        d.setTextColor(C_ACCENT, C_BG);
        d.print(hidStatus.c_str());
    } else {
        d.setTextColor(C_DIM, C_BG);
        d.print(hidEnabled ? "Keyboard active" : "Enter=enable");
    }
}

void sendSpecialKeys(const KeyEvent& ev) {
    if (ev.enter) {
        usbKeyboard.press(KEY_RETURN);
        usbKeyboard.release(KEY_RETURN);
    }
    if (ev.tab) {
        usbKeyboard.press(KEY_TAB);
        usbKeyboard.release(KEY_TAB);
    }
    if (ev.del) {
        usbKeyboard.press(KEY_BACKSPACE);
        usbKeyboard.release(KEY_BACKSPACE);
    }
    if (ev.up) {
        usbKeyboard.press(KEY_UP_ARROW);
        usbKeyboard.release(KEY_UP_ARROW);
    }
    if (ev.down) {
        usbKeyboard.press(KEY_DOWN_ARROW);
        usbKeyboard.release(KEY_DOWN_ARROW);
    }
    if (ev.left) {
        usbKeyboard.press(KEY_LEFT_ARROW);
        usbKeyboard.release(KEY_LEFT_ARROW);
    }
    if (ev.right) {
        usbKeyboard.press(KEY_RIGHT_ARROW);
        usbKeyboard.release(KEY_RIGHT_ARROW);
    }
}

void sendChars(const String& chars) {
    for (char c : chars) {
        usbKeyboard.write(static_cast<uint8_t>(c));
    }
}
}

void appHidKeyboardEnter() {
    hidDirty = true;
    hidStatus = "";
    hidStatusUntilMs = 0;
}

void appHidKeyboardLoop() {
    clearExpiredStatus();

    if (hidDirty) {
        drawScreen();
        hidDirty = false;
    }

    auto ev = readKeys();
    if (!ev.changed) return;

    if (ev.back) {
        ensureUsbState(false);
        // USB peripheral stays in HID mode through a soft reset; a full restart
        // is the only reliable way to re-enumerate as CDC.
        auto& d = M5Cardputer.Display;
        d.setFont(&fonts::Font0);
        d.setTextSize(1);
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(14, SCREEN_H - 20);
        d.print("Restarting to restore USB...");
        delay(1200);
        ESP.restart();
        return;
    }

    if (ev.enter && !hidEnabled) {
        ensureUsbState(true);
        return;
    }

    if (!hidEnabled) return;

    if (ev.fnKey) {
        for (char c : ev.chars) {
            if (c == 'q' || c == 'Q') {
                ensureUsbState(false);
                goHome();
                return;
            }
        }
    }

    sendSpecialKeys(ev);
    if (ev.chars.length() > 0) {
        sendChars(ev.chars);
    }
}
