#include <M5Cardputer.h>
#include <SPI.h>
#include "config.h"
#include "terminal.h"
#include "profiles.h"
#include "wifi_mgr.h"
#include "ssh_session.h"
#include "input.h"
#include "nav.h"
#include "launcher.h"
#include "app_ssh.h"
#include "app_mp3.h"
#include "app_notes.h"
#include "app_settings.h"
#include "app_games.h"
#include "app_files.h"

Terminal term;

// ── App state ──────────────────────────────────────────────────────────────
enum class State {
    BOOT,
    LAUNCHER, APP_SSH, APP_MP3, APP_NOTES, APP_SETTINGS, APP_GAMES, APP_FILES
};

static State state = State::BOOT;
static bool  dirty = true;

static String inputBuf;
static int    inputCursor = 0;

static void setState(State s) { state = s; dirty = true; inputBuf = ""; inputCursor = 0; }

// ── nav.h implementations ──────────────────────────────────────────────────

void goHome() {
    launcherEnter();
    state = State::LAUNCHER;
}

void launchApp(AppScene scene) {
    switch (scene) {
        case AppScene::SSH:
            appSshEnter();
            state = State::APP_SSH;
            break;
        case AppScene::MP3:
            appMp3Enter();
            state = State::APP_MP3;
            break;
        case AppScene::NOTES:
            appNotesEnter();
            state = State::APP_NOTES;
            break;
        case AppScene::SETTINGS:
            appSettingsEnter();
            state = State::APP_SETTINGS;
            break;
        case AppScene::GAMES:
            appGamesEnter();
            state = State::APP_GAMES;
            break;
        case AppScene::FILES:
            appFilesEnter();
            state = State::APP_FILES;
            break;
    }
}


// ── Helpers ────────────────────────────────────────────────────────────────

static void drawBanner(const char* title) {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.setTextColor(C_FG, C_BG);
    d.drawRect(0, 0, SCREEN_W, SCREEN_H, C_FG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    int tw = strlen(title) * FONT_W;
    d.setCursor((SCREEN_W - tw) / 2, 6);
    d.print(title);
    d.drawFastHLine(0, 14, SCREEN_W, C_FG);
}

// ── BOOT ───────────────────────────────────────────────────────────────────
static unsigned long bootStart = 0;
static int bootLine = 0;
static const char* bootLines[] = {
    "Mounting LittleFS...",
    "Loading profiles...",
    "Starting WiFi...",
    "Initializing hardware...",
};
static constexpr int BOOT_LINES = 4;
static constexpr int BOOT_MSG_Y = 72;   // y where messages start
static constexpr int BOOT_LINE_H = 10;

void handleBoot() {
    if (dirty) {
        auto& d = M5Cardputer.Display;
        d.fillScreen(C_BG);
        d.setFont(&fonts::Font0);

        // ── Logo area ─────────────────────────────────────────────────────
        // Big title: "Cardputer" line 1, "OS" line 2
        d.setTextSize(2);
        d.setTextColor(C_FG, C_BG);
        const char* t1 = "Cardputer";
        int w1 = strlen(t1) * FONT_W * 2;
        d.setCursor((SCREEN_W - w1) / 2, 8);
        d.print(t1);

        d.setTextSize(3);
        d.setTextColor(C_FG, C_BG);
        const char* t2 = "OS";
        int w2 = strlen(t2) * FONT_W * 3;
        d.setCursor((SCREEN_W - w2) / 2, 28);
        d.print(t2);

        // Version / tagline
        d.setTextSize(1);
        d.setTextColor(C_DIM, C_BG);
        const char* ver = "v1.1  --  M5Stack Cardputer";
        int vw = strlen(ver) * FONT_W;
        d.setCursor((SCREEN_W - vw) / 2, 56);
        d.print(ver);

        // Separator
        d.drawFastHLine(0, 65, SCREEN_W, C_DIM);

        bootLine  = 0;
        bootStart = millis();
        dirty     = false;
        Profiles.begin();
        WifiMgr.begin();
        settingsLoadFromFS();   // restore brightness + theme from last session
    }

    if (bootLine < BOOT_LINES && millis() - bootStart > (unsigned long)bootLine * 300) {
        auto& d = M5Cardputer.Display;
        d.setFont(&fonts::Font0);
        d.setTextSize(1);
        d.setTextColor(C_FG, C_BG);
        char buf[64];
        snprintf(buf, sizeof(buf), "[ OK ] %s", bootLines[bootLine]);
        d.setCursor(4, BOOT_MSG_Y + bootLine * BOOT_LINE_H);
        d.print(buf);
        bootLine++;
    }

    if (bootLine >= BOOT_LINES && millis() - bootStart > 1500) {
        goHome();   // always launch offline — connect via Settings > WiFi
    }
}


// ── Arduino entry points ────────────────────────────────────────────────────

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setFont(&fonts::Font0);
    M5Cardputer.Display.setTextSize(1);
    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
}

void loop() {
    switch (state) {
        case State::BOOT:         handleBoot();        break;
        case State::LAUNCHER:     launcherLoop();      break;
        case State::APP_SSH:      appSshLoop();        break;
        case State::APP_MP3:      appMp3Loop();        break;
        case State::APP_NOTES:    appNotesLoop();      break;
        case State::APP_SETTINGS: appSettingsLoop();   break;
        case State::APP_GAMES:    appGamesLoop();      break;
        case State::APP_FILES:    appFilesLoop();      break;
    }
}
