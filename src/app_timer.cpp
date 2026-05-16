#include "app_timer.h"
#include "nav.h"
#include "input.h"
#include "config.h"

#include <M5Cardputer.h>

namespace {
enum class TimerView {
    TIMER,
    STOPWATCH,
};

enum class TimerMode {
    SETUP,
    RUNNING,
    PAUSED,
    DONE,
};

enum class StopwatchMode {
    READY,
    RUNNING,
    PAUSED,
};

struct TimerPreset {
    const char* label;
    uint32_t seconds;
};

constexpr TimerPreset kPresets[] = {
    {"1 min", 60},
    {"3 min", 180},
    {"5 min", 300},
    {"10 min", 600},
    {"15 min", 900},
    {"25 min", 1500},
};
constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

bool timerDirty = true;
TimerView timerView = TimerView::TIMER;

TimerMode timerMode = TimerMode::SETUP;
int timerSelected = 2;
uint32_t timerDurationSec = kPresets[2].seconds;
uint32_t timerRemainingSec = kPresets[2].seconds;
uint32_t timerLastTickMs = 0;
String manualDigits;
bool timerFlash = false;
uint32_t timerLastFlashMs = 0;
bool alarmToneActive = false;
uint32_t lastAlarmToneMs = 0;
bool toneFlip = false;
char lastTimerTimeText[16] = "";
char lastStopwatchText[16] = "";
StopwatchMode lastDrawnStopwatchMode = StopwatchMode::READY;
TimerMode lastDrawnTimerMode = TimerMode::SETUP;

StopwatchMode stopwatchMode = StopwatchMode::READY;
uint32_t stopwatchElapsedMs = 0;
uint32_t stopwatchLastTickMs = 0;
uint32_t stopwatchLastDisplayMs = 0;

void ensureSpeakerReady() {
    M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.setVolume(200);
}

void startDoneTone() {
    alarmToneActive = true;
    lastAlarmToneMs = 0;
    toneFlip = false;
    ensureSpeakerReady();
}

void stopDoneTone() {
    if (!alarmToneActive) return;
    alarmToneActive = false;
    while (M5Cardputer.Speaker.isPlaying()) delay(1);
    M5Cardputer.Speaker.end();
}

void serviceDoneTone() {
    if (!alarmToneActive) return;
    uint32_t now = millis();
    if (M5Cardputer.Speaker.isPlaying()) return;
    if (now - lastAlarmToneMs < 180) return;
    lastAlarmToneMs = now;
    toneFlip = !toneFlip;
    float freq = toneFlip ? 1396.0f : 1046.0f;
    M5Cardputer.Speaker.tone(freq, 120);
}

void formatTime(uint32_t totalSeconds, char* out, size_t outSize) {
    uint32_t mins = totalSeconds / 60;
    uint32_t secs = totalSeconds % 60;
    snprintf(out, outSize, "%02lu:%02lu", static_cast<unsigned long>(mins), static_cast<unsigned long>(secs));
}

void formatStopwatch(uint32_t totalMs, char* out, size_t outSize) {
    uint32_t totalSeconds = totalMs / 1000;
    uint32_t mins = totalSeconds / 60;
    uint32_t secs = totalSeconds % 60;
    uint32_t centis = (totalMs % 1000) / 10;
    snprintf(out, outSize, "%02lu:%02lu.%02lu",
             static_cast<unsigned long>(mins),
             static_cast<unsigned long>(secs),
             static_cast<unsigned long>(centis));
}

bool hasManualEntry() {
    return manualDigits.length() > 0;
}

uint32_t parseManualSeconds(const String& digits) {
    if (digits.length() == 0) return timerDurationSec;
    uint32_t value = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') continue;
        value = value * 10 + static_cast<uint32_t>(c - '0');
    }
    uint32_t mins = value / 100;
    uint32_t secs = value % 100;
    if (secs > 59) secs = 59;
    uint32_t total = mins * 60 + secs;
    if (total == 0) total = 1;
    if (total > 99 * 60 + 59) total = 99 * 60 + 59;
    return total;
}

void applyManualEntry() {
    if (!hasManualEntry()) return;
    timerDurationSec = parseManualSeconds(manualDigits);
    timerRemainingSec = timerDurationSec;
}

void syncPresetSelection() {
    timerSelected = -1;
    if (hasManualEntry()) return;
    for (int i = 0; i < kPresetCount; ++i) {
        if (kPresets[i].seconds == timerDurationSec) {
            timerSelected = i;
            return;
        }
    }
}

void setPreset(int index) {
    if (index < 0) index = 0;
    if (index >= kPresetCount) index = kPresetCount - 1;
    timerSelected = index;
    manualDigits = "";
    timerDurationSec = kPresets[index].seconds;
    timerRemainingSec = timerDurationSec;
    timerDirty = true;
}

void resetTimerToDuration() {
    timerRemainingSec = timerDurationSec;
    timerMode = TimerMode::SETUP;
    timerLastTickMs = 0;
    timerFlash = false;
    stopDoneTone();
    timerDirty = true;
}

void resetStopwatch() {
    stopwatchMode = StopwatchMode::READY;
    stopwatchElapsedMs = 0;
    stopwatchLastTickMs = 0;
    stopwatchLastDisplayMs = 0;
    timerDirty = true;
}

void drawStatusBar() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS_BG);
    d.setTextColor(C_STATUS_FG, C_STATUS_BG);
    d.setCursor(2, 3);
    d.print(timerView == TimerView::TIMER ? "Timer" : "Stopwatch");
    drawBatteryWidget(C_STATUS_BG, SCREEN_W - 43);
}

void drawPresetList() {
    auto& d = M5Cardputer.Display;
    for (int i = 0; i < kPresetCount; ++i) {
        int y = 36 + (i * 13);
        uint32_t bg = (i == timerSelected && !hasManualEntry()) ? C_HIGHLIGHT : C_BG;
        uint32_t fg = (i == timerSelected && !hasManualEntry()) ? C_INPUT : C_FG;
        d.fillRoundRect(12, y - 1, 92, 11, 3, bg);
        d.setTextColor(fg, bg);
        d.setCursor(18, y + 1);
        d.print(kPresets[i].label);
    }
}

void drawTimerSetupScreen() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar();

    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_FG, C_BG);
    d.setCursor(14, 22);
    d.print("Countdown presets");
    drawPresetList();

    d.fillRoundRect(112, 26, 116, 34, 6, 0x001821);
    d.drawRoundRect(112, 26, 116, 34, 6, C_ACCENT);
    d.setTextColor(C_ACCENT, 0x001821);
    d.setTextSize(3);
    char timeBuf[8];
    formatTime(timerRemainingSec, timeBuf, sizeof(timeBuf));
    int tw = strlen(timeBuf) * FONT_W * 3;
    d.setCursor(170 - (tw / 2), 35);
    d.print(timeBuf);

    d.setTextSize(1);
    d.setTextColor(C_FG, C_BG);
    d.setCursor(116, 70);
    d.print("Manual keypad:");
    d.fillRoundRect(112, 80, 116, 18, 4, 0x101010);
    d.drawRoundRect(112, 80, 116, 18, 4, hasManualEntry() ? 0xFFE36C : C_DIM);
    d.setTextColor(hasManualEntry() ? 0xFFE36C : C_DIM, 0x101010);
    d.setCursor(118, 86);
    if (hasManualEntry()) {
        d.print(manualDigits.c_str());
    } else {
        d.print("type mmss");
    }

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(116, 106);
    d.print("Tab=stopwatch");

    d.fillRect(0, SCREEN_H - 10, SCREEN_W, 10, C_BG);
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 1);
    d.print("Enter=start Del=clear fn+bksp=home");
}

void drawTimerBody(const char* subtitle, uint32_t accent, uint32_t cardBg) {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar();

    d.fillRoundRect(14, 22, 212, 72, 10, cardBg);
    d.drawRoundRect(14, 22, 212, 72, 10, accent);
    d.drawRoundRect(15, 23, 210, 70, 9, accent);

    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_FG, cardBg);
    int sw = strlen(subtitle) * FONT_W;
    d.setCursor((SCREEN_W - sw) / 2, 31);
    d.print(subtitle);

    char timeBuf[8];
    formatTime(timerRemainingSec, timeBuf, sizeof(timeBuf));
    d.setTextColor(accent, cardBg);
    d.setTextSize(4);
    int tw = strlen(timeBuf) * FONT_W * 4;
    d.setCursor((SCREEN_W - tw) / 2, 48);
    d.print(timeBuf);
    strncpy(lastTimerTimeText, timeBuf, sizeof(lastTimerTimeText) - 1);
    lastTimerTimeText[sizeof(lastTimerTimeText) - 1] = '\0';

    d.setTextSize(1);
}

void drawRunningScreen() {
    drawTimerBody("Counting down", 0x6CFFB2, 0x062F1D);
    auto& d = M5Cardputer.Display;
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 1);
    d.print("Enter=pause Del=reset fn+bksp=home");
}

void drawPausedScreen() {
    drawTimerBody("Paused", 0xFFE36C, 0x2A2400);
    auto& d = M5Cardputer.Display;
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 1);
    d.print("Enter=resume Del=reset fn+bksp=home");
}

void drawDoneScreen() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar();

    uint32_t flashColor = timerFlash ? 0x1E3A0A : 0x0B2006;
    uint32_t accent = timerFlash ? 0xB7FF5A : 0x6CFFB2;
    d.fillRoundRect(16, 26, 208, 66, 10, flashColor);
    d.drawRoundRect(16, 26, 208, 66, 10, accent);
    d.drawRoundRect(17, 27, 206, 64, 9, accent);

    d.setFont(&fonts::Font0);
    d.setTextSize(2);
    d.setTextColor(accent, flashColor);
    const char* msg = "TIME UP";
    int mw = strlen(msg) * FONT_W * 2;
    d.setCursor((SCREEN_W - mw) / 2, 40);
    d.print(msg);

    d.setTextSize(1);
    d.setTextColor(C_FG, flashColor);
    d.setCursor(76, 68);
    d.print("Press Enter");
    d.setCursor(70, 80);
    d.print("or Del to reset");

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 1);
    d.print("Enter=again Del=reset fn+bksp=home");
}

void drawStopwatchScreen() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar();

    uint32_t cardBg = stopwatchMode == StopwatchMode::RUNNING ? 0x062F1D
                      : stopwatchMode == StopwatchMode::PAUSED ? 0x2A2400
                      : 0x001821;
    uint32_t accent = stopwatchMode == StopwatchMode::RUNNING ? 0x6CFFB2
                      : stopwatchMode == StopwatchMode::PAUSED ? 0xFFE36C
                      : C_ACCENT;

    d.fillRoundRect(14, 22, 212, 72, 10, cardBg);
    d.drawRoundRect(14, 22, 212, 72, 10, accent);
    d.drawRoundRect(15, 23, 210, 70, 9, accent);

    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_FG, cardBg);
    const char* subtitle = stopwatchMode == StopwatchMode::RUNNING ? "Running"
                         : stopwatchMode == StopwatchMode::PAUSED ? "Paused"
                         : "Ready";
    int sw = strlen(subtitle) * FONT_W;
    d.setCursor((SCREEN_W - sw) / 2, 31);
    d.print(subtitle);

    char timeBuf[12];
    formatStopwatch(stopwatchElapsedMs, timeBuf, sizeof(timeBuf));
    d.setTextColor(accent, cardBg);
    d.setTextSize(3);
    int tw = strlen(timeBuf) * FONT_W * 3;
    d.setCursor((SCREEN_W - tw) / 2, 48);
    d.print(timeBuf);
    strncpy(lastStopwatchText, timeBuf, sizeof(lastStopwatchText) - 1);
    lastStopwatchText[sizeof(lastStopwatchText) - 1] = '\0';
    lastDrawnStopwatchMode = stopwatchMode;

    d.setTextSize(1);
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(14, 104);
    d.print("Tab=timer");

    d.fillRect(0, SCREEN_H - 10, SCREEN_W, 10, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 1);
    d.print("Enter=start/pause Del=reset fn+bksp=home");
}

void drawScreen() {
    if (timerView == TimerView::STOPWATCH) {
        drawStopwatchScreen();
        return;
    }

    switch (timerMode) {
        case TimerMode::SETUP: drawTimerSetupScreen(); break;
        case TimerMode::RUNNING: drawRunningScreen(); break;
        case TimerMode::PAUSED: drawPausedScreen(); break;
        case TimerMode::DONE: drawDoneScreen(); break;
    }
    lastDrawnTimerMode = timerMode;
}

void refreshTimerValueIfNeeded() {
    if (timerView != TimerView::TIMER) return;
    if (timerMode != TimerMode::RUNNING && timerMode != TimerMode::PAUSED) return;
    if (timerMode != lastDrawnTimerMode) return;

    char timeBuf[8];
    formatTime(timerRemainingSec, timeBuf, sizeof(timeBuf));
    if (strcmp(timeBuf, lastTimerTimeText) == 0) return;

    auto& d = M5Cardputer.Display;
    uint32_t cardBg = (timerMode == TimerMode::RUNNING) ? 0x062F1D : 0x2A2400;
    uint32_t accent = (timerMode == TimerMode::RUNNING) ? 0x6CFFB2 : 0xFFE36C;
    d.fillRect(26, 46, 188, 34, cardBg);
    d.setFont(&fonts::Font0);
    d.setTextSize(4);
    d.setTextColor(accent, cardBg);
    int tw = strlen(timeBuf) * FONT_W * 4;
    d.setCursor((SCREEN_W - tw) / 2, 48);
    d.print(timeBuf);
    strncpy(lastTimerTimeText, timeBuf, sizeof(lastTimerTimeText) - 1);
    lastTimerTimeText[sizeof(lastTimerTimeText) - 1] = '\0';
    d.setTextSize(1);
}

void refreshStopwatchValueIfNeeded() {
    if (timerView != TimerView::STOPWATCH) return;

    uint32_t shownMs = stopwatchElapsedMs;
    if (stopwatchMode == StopwatchMode::RUNNING) {
        shownMs = (shownMs / 100) * 100;
    }

    char timeBuf[12];
    formatStopwatch(shownMs, timeBuf, sizeof(timeBuf));
    if (strcmp(timeBuf, lastStopwatchText) == 0 && stopwatchMode == lastDrawnStopwatchMode) return;

    auto& d = M5Cardputer.Display;
    uint32_t cardBg = stopwatchMode == StopwatchMode::RUNNING ? 0x062F1D
                      : stopwatchMode == StopwatchMode::PAUSED ? 0x2A2400
                      : 0x001821;
    uint32_t accent = stopwatchMode == StopwatchMode::RUNNING ? 0x6CFFB2
                      : stopwatchMode == StopwatchMode::PAUSED ? 0xFFE36C
                      : C_ACCENT;
    const char* subtitle = stopwatchMode == StopwatchMode::RUNNING ? "Running"
                         : stopwatchMode == StopwatchMode::PAUSED ? "Paused"
                         : "Ready";

    d.fillRect(24, 29, 192, 46, cardBg);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_FG, cardBg);
    int sw = strlen(subtitle) * FONT_W;
    d.setCursor((SCREEN_W - sw) / 2, 31);
    d.print(subtitle);

    d.setTextColor(accent, cardBg);
    d.setTextSize(3);
    int tw = strlen(timeBuf) * FONT_W * 3;
    d.setCursor((SCREEN_W - tw) / 2, 48);
    d.print(timeBuf);
    d.setTextSize(1);

    strncpy(lastStopwatchText, timeBuf, sizeof(lastStopwatchText) - 1);
    lastStopwatchText[sizeof(lastStopwatchText) - 1] = '\0';
    lastDrawnStopwatchMode = stopwatchMode;
    stopwatchLastDisplayMs = shownMs;
}

void tickRunningTimer() {
    uint32_t now = millis();
    if (timerLastTickMs == 0) timerLastTickMs = now;

    while (timerMode == TimerMode::RUNNING && now - timerLastTickMs >= 1000) {
        timerLastTickMs += 1000;
        if (timerRemainingSec > 0) {
            --timerRemainingSec;
            timerDirty = true;
        }
        if (timerRemainingSec == 0) {
            timerMode = TimerMode::DONE;
            timerLastFlashMs = now;
            timerFlash = true;
            startDoneTone();
            timerDirty = true;
            break;
        }
    }
}

void tickDoneFlash() {
    uint32_t now = millis();
    if (now - timerLastFlashMs >= 350) {
        timerLastFlashMs = now;
        timerFlash = !timerFlash;
        timerDirty = true;
    }
}

void tickStopwatch() {
    if (stopwatchMode != StopwatchMode::RUNNING) return;
    uint32_t now = millis();
    if (stopwatchLastTickMs == 0) {
        stopwatchLastTickMs = now;
        return;
    }
    uint32_t delta = now - stopwatchLastTickMs;
    if (delta == 0) return;
    stopwatchLastTickMs = now;
    stopwatchElapsedMs += delta;
}
}

void appTimerEnter() {
    timerView = TimerView::TIMER;
    timerMode = TimerMode::SETUP;
    syncPresetSelection();
    timerRemainingSec = timerDurationSec;
    timerLastTickMs = 0;
    timerFlash = false;
    timerLastFlashMs = 0;
    stopDoneTone();
    resetStopwatch();
    timerDirty = true;
}

void appTimerService() {
    serviceDoneTone();

    if (timerView == TimerView::TIMER && timerMode == TimerMode::RUNNING) {
        tickRunningTimer();
    } else if (timerView == TimerView::TIMER && timerMode == TimerMode::DONE) {
        tickDoneFlash();
    } else if (timerView == TimerView::STOPWATCH) {
        tickStopwatch();
    }
}

bool appTimerTakeoverRequested() {
    return false;
}

void appTimerConsumeTakeover() {}

void appTimerLoop() {
    appTimerService();

    if (timerDirty) {
        drawScreen();
        timerDirty = false;
    } else {
        refreshTimerValueIfNeeded();
        refreshStopwatchValueIfNeeded();
    }

    auto ev = readKeys();
    if (!ev.changed) return;

    if (ev.back) {
        stopDoneTone();
        if (timerView == TimerView::TIMER && (timerMode == TimerMode::RUNNING || timerMode == TimerMode::PAUSED || timerMode == TimerMode::DONE)) {
            resetTimerToDuration();
        }
        if (timerView == TimerView::STOPWATCH) {
            resetStopwatch();
        }
        goHome();
        return;
    }

    if (ev.tab) {
        stopDoneTone();
        timerView = (timerView == TimerView::TIMER) ? TimerView::STOPWATCH : TimerView::TIMER;
        timerDirty = true;
        return;
    }

    if (timerView == TimerView::STOPWATCH) {
        if (ev.del) {
            resetStopwatch();
            return;
        }
        if (ev.enter) {
            if (stopwatchMode == StopwatchMode::READY) {
                stopwatchMode = StopwatchMode::RUNNING;
                stopwatchLastTickMs = millis();
            } else if (stopwatchMode == StopwatchMode::RUNNING) {
                stopwatchMode = StopwatchMode::PAUSED;
            } else {
                stopwatchMode = StopwatchMode::RUNNING;
                stopwatchLastTickMs = millis();
            }
            timerDirty = true;
            return;
        }
        return;
    }

    if (ev.del) {
        if (timerMode == TimerMode::SETUP && hasManualEntry()) {
            manualDigits.remove(manualDigits.length() - 1);
            if (hasManualEntry()) {
                applyManualEntry();
            } else {
                timerDurationSec = kPresets[timerSelected >= 0 ? timerSelected : 2].seconds;
                timerRemainingSec = timerDurationSec;
                syncPresetSelection();
            }
            timerDirty = true;
            return;
        }
        resetTimerToDuration();
        return;
    }

    switch (timerMode) {
        case TimerMode::SETUP:
            if (ev.up) {
                setPreset((timerSelected < 0 ? 0 : timerSelected) - 1);
                return;
            }
            if (ev.down) {
                setPreset((timerSelected < 0 ? 0 : timerSelected) + 1);
                return;
            }
            if (ev.left) {
                manualDigits = "";
                if (timerDurationSec > 60) timerDurationSec -= 60;
                timerRemainingSec = timerDurationSec;
                syncPresetSelection();
                timerDirty = true;
                return;
            }
            if (ev.right) {
                manualDigits = "";
                timerDurationSec += 60;
                if (timerDurationSec > 99 * 60 + 59) timerDurationSec = 99 * 60 + 59;
                timerRemainingSec = timerDurationSec;
                syncPresetSelection();
                timerDirty = true;
                return;
            }
            for (char c : ev.chars) {
                if (c >= '0' && c <= '9') {
                    if (manualDigits.length() >= 4) {
                        manualDigits.remove(0, 1);
                    }
                    manualDigits += c;
                    applyManualEntry();
                    syncPresetSelection();
                    timerDirty = true;
                }
            }
            if (ev.enter) {
                timerMode = TimerMode::RUNNING;
                timerLastTickMs = millis();
                timerDirty = true;
                return;
            }
            break;

        case TimerMode::RUNNING:
            if (ev.enter) {
                timerMode = TimerMode::PAUSED;
                timerDirty = true;
                return;
            }
            break;

        case TimerMode::PAUSED:
            if (ev.enter) {
                timerMode = TimerMode::RUNNING;
                timerLastTickMs = millis();
                timerDirty = true;
                return;
            }
            break;

        case TimerMode::DONE:
            if (ev.enter) {
                stopDoneTone();
                timerRemainingSec = timerDurationSec;
                timerMode = TimerMode::RUNNING;
                timerLastTickMs = millis();
                timerFlash = true;
                timerDirty = true;
                return;
            }
            break;
    }
}
