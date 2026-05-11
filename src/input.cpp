#include "input.h"
#include "config.h"
#include <M5Cardputer.h>

static unsigned long lastActivityMs = 0;
static bool          screenSleeping = false;
static uint8_t       preSleepBright = 128;

void resetSleepTimer() {
    lastActivityMs = millis();
    if (screenSleeping) {
        screenSleeping = false;
        M5Cardputer.Display.setBrightness(preSleepBright);
    }
}

KeyEvent readKeys() {
    KeyEvent ev = {};
    M5Cardputer.update();

    // Lazily initialise on first call so the timer doesn't fire immediately.
    if (lastActivityMs == 0) lastActivityMs = millis();

    // Check whether we should put the screen to sleep.
    if (sleepTimeoutMs > 0 && !screenSleeping) {
        if (millis() - lastActivityMs >= sleepTimeoutMs) {
            preSleepBright = (uint8_t)M5Cardputer.Display.getBrightness();
            M5Cardputer.Display.setBrightness(0);
            screenSleeping = true;
        }
    }

    if (!M5Cardputer.Keyboard.isChange()) return ev;

    if (M5Cardputer.Keyboard.isPressed()) {
        lastActivityMs = millis();
        if (screenSleeping) {
            // Wake on any key press; swallow the key so it isn't acted on.
            screenSleeping = false;
            M5Cardputer.Display.setBrightness(preSleepBright);
            return ev;
        }
    }

    if (!M5Cardputer.Keyboard.isPressed()) return ev;

    ev.changed = true;
    auto s = M5Cardputer.Keyboard.keysState();
    ev.enter = s.enter;
    ev.del   = s.del;
    ev.tab   = s.tab;
    ev.fnKey = s.fn;
    for (char c : s.word) ev.chars += c;

    if (s.fn) {
        if (s.del) {
            ev.back = true;
            ev.del  = false;
        }
        for (char c : s.word) {
            if (c == ';') { ev.up    = true; ev.chars = ""; }
            if (c == '.') { ev.down  = true; ev.chars = ""; }
            if (c == ',') { ev.left  = true; ev.chars = ""; }
            if (c == '/') { ev.right = true; ev.chars = ""; }
        }
    }
    return ev;
}
