// T-Embed encoder + button input — compiled only for BOARD_TEMBED.
// Excluded from Cardputer build via platformio.ini src_filter.
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>   // resolves to T-Embed shim
#include <Arduino.h>
#include <esp_sleep.h>

static unsigned long lastActivityMs = 0;
static bool          screenSleeping = false;
static uint8_t       preSleepBright = 128;
static bool          g_justWoke     = false;
static bool          g_initialized  = false;

// ── Encoder (quadrature, interrupt-driven) ─────────────────────────────────
static volatile int32_t g_encTicks = 0;
static int32_t          g_encAccum = 0;   // persistent accumulator — one event per detent
static uint32_t         g_lastEncEventMs = 0;  // dead-time: suppress re-fire within 50ms

// EC11 encoder: 4 quadrature transitions per physical detent (click).
// Require ±4 ticks before firing so one click = one scroll event.
static constexpr int32_t ENC_DETENT_TICKS = 4;

static void IRAM_ATTR onEncChange() {
    static uint8_t old_AB = 3;
    uint8_t AB = ((uint8_t)digitalRead(ENC_A_PIN) << 1) | (uint8_t)digitalRead(ENC_B_PIN);
    // Standard quadrature decode table: positive = CW = down, negative = CCW = up
    static const int8_t tbl[16] = { 0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0 };
    g_encTicks += tbl[(old_AB << 2) | AB];
    old_AB = AB;
}

static void initInput() {
    pinMode(ENC_A_PIN,    INPUT_PULLUP);
    pinMode(ENC_B_PIN,    INPUT_PULLUP);
    pinMode(ENC_KEY_PIN,  INPUT_PULLUP);
    pinMode(USER_KEY_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(ENC_A_PIN), onEncChange, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENC_B_PIN), onEncChange, CHANGE);
    lastActivityMs = millis();
    g_initialized  = true;
}

bool screenJustWoke() {
    bool w = g_justWoke;
    g_justWoke = false;
    return w;
}

void resetSleepTimer() {
    lastActivityMs = millis();
    if (screenSleeping) {
        screenSleeping = false;
        M5Cardputer.Display.setBrightness(preSleepBright);
    }
}

KeyEvent readKeys() {
    KeyEvent ev = {};

    if (!g_initialized) initInput();

    // ── Screen sleep ───────────────────────────────────────────────────────
    if (sleepTimeoutMs > 0 && !screenSleeping) {
        if (millis() - lastActivityMs >= sleepTimeoutMs) {
            preSleepBright = (uint8_t)M5Cardputer.Display.getBrightness();
            M5Cardputer.Display.setBrightness(0);
            screenSleeping = true;
        }
    }

    // ── Encoder ticks (atomic read) ────────────────────────────────────────
    int32_t ticks;
    noInterrupts();
    ticks      = g_encTicks;
    g_encTicks = 0;
    interrupts();

    g_encAccum += ticks;

    // Fire once per detent (ENC_DETENT_TICKS ticks), with 50ms dead-time.
    // Accumulator is always cleared on threshold cross so stale ticks never re-trigger.
    if (g_encAccum >= ENC_DETENT_TICKS || g_encAccum <= -ENC_DETENT_TICKS) {
        bool canFire = (millis() - g_lastEncEventMs >= 50);
        if (canFire) {
            lastActivityMs = millis();
            if (screenSleeping) {
                screenSleeping = false;
                M5Cardputer.Display.setBrightness(preSleepBright);
                g_justWoke = true;
                g_encAccum = 0;
                g_lastEncEventMs = millis();
                return ev;
            }
            ev.changed = true;
            if (g_encAccum > 0) { ev.down = true; }
            else                 { ev.up   = true; }
            g_lastEncEventMs = millis();
        }
        g_encAccum = 0;  // always discard — stale ticks must not re-trigger
    }

    // ── Encoder click (GPIO0, active-low with pull-up) ─────────────────────
    static bool     lastKey   = HIGH;
    static uint32_t lastKeyMs = 0;
    bool key = (bool)digitalRead(ENC_KEY_PIN);
    if (!key && lastKey && millis() - lastKeyMs > 50) {
        lastKeyMs = millis();
        lastActivityMs = millis();
        if (!screenSleeping) {
            ev.changed = true;
            ev.enter   = true;
        } else {
            screenSleeping = false;
            M5Cardputer.Display.setBrightness(preSleepBright);
            g_justWoke = true;
        }
    }
    lastKey = key;

    // ── Side button (GPIO6, active-low) ───────────────────────────────────
    // Short press (<2 s): back/home.  Long hold (>=2 s): deep-sleep power off.
    static bool     lastSide      = HIGH;
    static uint32_t lastSideMs    = 0;
    static uint32_t sidePressAt   = 0;
    static bool     sidePowerFired = false;

    bool side = (bool)digitalRead(USER_KEY_PIN);
    uint32_t sideNow = millis();
    if (!side && lastSide && sideNow - lastSideMs > 50) {
        lastSideMs    = sideNow;
        sidePressAt   = sideNow;
        sidePowerFired = false;
        lastActivityMs = sideNow;
        if (!screenSleeping) {
            ev.changed = true;
            ev.back    = true;
        } else {
            screenSleeping = false;
            M5Cardputer.Display.setBrightness(preSleepBright);
            g_justWoke = true;
        }
    }
    if (!side && !sidePowerFired && sidePressAt && sideNow - sidePressAt >= 2000) {
        sidePowerFired = true;
        M5Cardputer.Display.setBrightness(0);
        M5Cardputer.Display.fillScreen(0);
        // POWER_HOLD (GPIO15) is driven HIGH in setup to keep the 3.3V rail on.
        // Pulling it LOW cuts the supply — true power off, not deep sleep.
        // Deep sleep + EXT0 wakeup on the same pin fired immediately because
        // the button is still held LOW when esp_deep_sleep_start() runs.
        digitalWrite(POWER_EN_PIN, LOW);
    }
    if (side) { sidePressAt = 0; sidePowerFired = false; }
    lastSide = side;

    return ev;
}
