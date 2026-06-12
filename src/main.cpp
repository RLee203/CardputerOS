#include <M5Cardputer.h>
#include <SPI.h>
#include <WiFi.h>
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
#include "app_ir_remote.h"
#include "app_photos.h"
#include "app_voice_memos.h"
#include "app_placeholder.h"
#include "app_hid_keyboard.h"
#include "app_usb_storage.h"
#include "app_timer.h"
#include "app_gps.h"
#include "app_lora.h"
#include "app_nfc.h"
#include "app_lock.h"
#include "app_payloads.h"
#include "app_ble.h"
#include "app_detector.h"
#include "app_wifi.h"
#include "app_cc1101.h"
#include "app_nrf24.h"
#include "app_espnow.h"
#include "app_sd_health.h"
#include "app_calc.h"
#include "app_firmware.h"

Terminal term;

// ── App state ──────────────────────────────────────────────────────────────
enum class State {
    BOOT,
    MODE_PICKER,
    MODE_SWITCH_PROMPT,
    LOCK_SCREEN,
    LAUNCHER, APP_SSH, APP_MP3, APP_NOTES, APP_SETTINGS, APP_GAMES, APP_FILES, APP_IR_REMOTE, APP_PHOTOS, APP_VOICE_MEMOS, APP_HID_KEYBOARD, APP_USB_STORAGE, APP_TIMER, APP_GPS, APP_LORA, APP_NFC, APP_PAYLOADS, APP_BLE, APP_DETECTOR, APP_WIFI, APP_CC1101, APP_NRF24, APP_ESPNOW, APP_SD_HEALTH, APP_CALC, APP_FIRMWARE, APP_PLACEHOLDER
};

static State state             = State::BOOT;
static bool  dirty             = true;
static bool  g_sessionUnlocked = false;
static bool  g_wifiSuspended   = false;  // true when an SD app suspended WiFi
static bool  g_wifiWasConnected = false;
static wifi_mode_t g_wifiPrevMode = WIFI_OFF;
static DeviceMode g_deviceMode = DeviceMode::RADIO;
static DeviceMode g_modePickerSel = DeviceMode::RADIO;
static DeviceMode g_modeSwitchTarget = DeviceMode::RADIO;
static String g_modeSwitchFeature = "";
static uint32_t g_modePickerReadyAt = 0;
static bool g_modePickerPrimed = false;

static bool requiresSdMode(AppScene scene) {
    switch (scene) {
        case AppScene::MP3:
        case AppScene::GAMES:
        case AppScene::FILES:
        case AppScene::PHOTOS:
        case AppScene::VOICE_MEMOS:
        case AppScene::USB_STORAGE:
        case AppScene::PAYLOADS:
        case AppScene::SD_HEALTH:
        case AppScene::FIRMWARE:
            return true;
        default:
            return false;
    }
}

static bool requiresRadioMode(AppScene scene) {
    switch (scene) {
        case AppScene::SSH:
        case AppScene::GPS:
        case AppScene::LORA:
        case AppScene::BLE:
        case AppScene::DETECTOR:
        case AppScene::WIFI_TOOLS:
        case AppScene::CC1101:
        case AppScene::NRF24:
        case AppScene::ESPNOW:
            return true;
        default:
            return false;
    }
}

DeviceMode currentDeviceMode() { return g_deviceMode; }
bool isSdMode() { return g_deviceMode == DeviceMode::SD; }
bool isRadioMode() { return g_deviceMode == DeviceMode::RADIO; }
void setCurrentDeviceMode(DeviceMode mode) {
    g_deviceMode = mode;
    settingsSetBootMode(mode);
}

void requestModeSwitch(DeviceMode targetMode, const char* feature) {
    g_modeSwitchTarget = targetMode;
    g_modeSwitchFeature = feature ? feature : "This feature";
    state = State::MODE_SWITCH_PROMPT;
    dirty = true;
}

void suspendWifiForSd() {
    g_wifiPrevMode = WiFi.getMode();
    g_wifiWasConnected = (WiFi.status() == WL_CONNECTED);
    g_wifiSuspended = (g_wifiPrevMode != WIFI_OFF);
    if (!g_wifiSuspended) return;

    // SD/audio apps are more stable if WiFi is fully dropped before they open.
    // We intentionally do not auto-resume later; the user can reconnect from
    // Settings / WiFi Tools when they actually need radio again.
    WiFi.disconnect(true, true);
    delay(80);
    WiFi.mode(WIFI_OFF);
    delay(220);
}

void resumeWifiAfterSd() {
    // Do not auto-restore WiFi when leaving SD-heavy apps. That transition has
    // been the source of driver failures and SD breakage; reconnect manually
    // through the WiFi settings/tool flows instead.
    g_wifiSuspended = false;
    g_wifiPrevMode = WIFI_OFF;
    g_wifiWasConnected = false;
}

static String inputBuf;
static int    inputCursor = 0;

static void setState(State s) { state = s; dirty = true; inputBuf = ""; inputCursor = 0; }

// ── nav.h implementations ──────────────────────────────────────────────────

void goHome() {
    resumeWifiAfterSd();
    bgAudioResume();   // restart audio if it was suspended for an SD app
    if (settingsLockEnabled() && !g_sessionUnlocked) {
        appLockEnter();
        state = State::LOCK_SCREEN;
        return;
    }
    launcherEnter();
    state = State::LAUNCHER;
}

void launchApp(AppScene scene) {
    // Suspend background audio before any SD app that would reinit the SD bus.
    // MP3 app handles its own audio lifecycle, so it's excluded here.
    if (requiresSdMode(scene) && scene != AppScene::MP3) {
        bgAudioSuspend();
    }
    if (requiresSdMode(scene) && !isSdMode()) {
        requestModeSwitch(DeviceMode::SD, "This app");
        return;
    }
    if (requiresRadioMode(scene) && !isRadioMode()) {
        requestModeSwitch(DeviceMode::RADIO, "This app");
        return;
    }
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
        case AppScene::IR_REMOTE:
            appIrRemoteEnter();
            state = State::APP_IR_REMOTE;
            break;
        case AppScene::PHOTOS:
            appPhotosEnter();
            state = State::APP_PHOTOS;
            break;
        case AppScene::VOICE_MEMOS:
            appVoiceMemosEnter();
            state = State::APP_VOICE_MEMOS;
            break;
        case AppScene::HID_KEYBOARD:
            appHidKeyboardEnter();
            state = State::APP_HID_KEYBOARD;
            break;
        case AppScene::USB_STORAGE:
            appUsbStorageEnter();
            state = State::APP_USB_STORAGE;
            break;
        case AppScene::TIMER:
            appTimerEnter();
            state = State::APP_TIMER;
            break;
        case AppScene::GPS:
            appGpsEnter();
            state = State::APP_GPS;
            break;
        case AppScene::LORA:
            appLoraEnter();
            state = State::APP_LORA;
            break;
        case AppScene::NFC:
            appNfcEnter();
            state = State::APP_NFC;
            break;
        case AppScene::PAYLOADS:
            appPayloadsEnter();
            state = State::APP_PAYLOADS;
            break;
        case AppScene::BLE:
            appBleEnter();
            state = State::APP_BLE;
            break;
        case AppScene::DETECTOR:
            appDetectorEnter();
            state = State::APP_DETECTOR;
            break;
        case AppScene::WIFI_TOOLS:
            appWifiEnter();
            state = State::APP_WIFI;
            break;
        case AppScene::CC1101:
            appCc1101Enter();
            state = State::APP_CC1101;
            break;
        case AppScene::NRF24:
            appNrf24Enter();
            state = State::APP_NRF24;
            break;
        case AppScene::ESPNOW:
            appEspnowEnter();
            state = State::APP_ESPNOW;
            break;
        case AppScene::SD_HEALTH:
            appSdHealthEnter();
            state = State::APP_SD_HEALTH;
            break;
        case AppScene::CALC:
            appCalcEnter();
            state = State::APP_CALC;
            break;
        case AppScene::FIRMWARE:
            appFirmwareEnter();
            state = State::APP_FIRMWARE;
            break;
    }
}

static const char* modeName(DeviceMode mode) {
    return mode == DeviceMode::SD ? "Multimedia" : "Radio";
}

static void drawModePicker() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(2);
    d.setTextColor(C_FG, C_BG);
    const char* title = "Cardputer OS 2.4";
    d.setCursor((SCREEN_W - (int)strlen(title) * FONT_W * 2) / 2, 8);
    d.print(title);
    d.setTextSize(1);
    d.setTextColor(C_DIM, C_BG);
    const char* sub = "Choose startup view";
    d.setCursor((SCREEN_W - (int)strlen(sub) * FONT_W) / 2, 30);
    d.print(sub);

    for (int i = 0; i < 2; ++i) {
        DeviceMode mode = (i == 0) ? DeviceMode::SD : DeviceMode::RADIO;
        bool sel = (g_modePickerSel == mode);
        int x = 14 + i * 112;
        int y = 46;
        uint32_t box = sel ? (mode == DeviceMode::SD ? 0x7A5A00 : 0x114488) : 0x1A1A1A;
        d.fillRoundRect(x, y, 100, 44, 6, box);
        d.drawRoundRect(x, y, 100, 44, 6, sel ? 0xFFFFFF : C_DIM);
        d.setTextColor(0xFFFFFF, box);
        {
            const char* t = (mode == DeviceMode::SD) ? "MEDIA" : "RADIO";
            d.setCursor(x + (100 - (int)strlen(t) * FONT_W) / 2, y + 8);
            d.print(t);
        }
        d.setTextColor(sel ? 0xFFFFFF : C_DIM, box);
        {
            const char* s = (mode == DeviceMode::SD) ? "Local tools" : "Wireless tools";
            d.setCursor(x + (100 - (int)strlen(s) * FONT_W) / 2, y + 24);
            d.print(s);
        }
    }

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(14, 102);
    d.print("Left/Right=select  Enter=boot");
    d.setCursor(14, 114);
    d.print("fn+M = switch mode in launcher");
}

static void handleModePicker() {
    if (dirty) {
        resetSleepTimer();
        M5Cardputer.update();  // first flush of boot-time keyboard state
        delay(25);
        M5Cardputer.update();  // second pass helps on warm boots before the picker becomes interactive
        g_modePickerReadyAt = millis() + 320;
        g_modePickerPrimed = true;
        drawModePicker();
        dirty = false;
    }
    if (g_modePickerPrimed) {
        M5Cardputer.update();
        if (millis() < g_modePickerReadyAt) return;
        g_modePickerPrimed = false;
    }
    auto ev = readKeys();
    if (!ev.changed) return;
    if (ev.left || ev.right) {
        g_modePickerSel = (g_modePickerSel == DeviceMode::SD) ? DeviceMode::RADIO : DeviceMode::SD;
        dirty = true;
        return;
    }
    if (ev.enter) {
        setCurrentDeviceMode(g_modePickerSel);
        goHome();
        return;
    }
}

static void drawModeSwitchPrompt() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(2);
    d.setTextColor(C_ERROR, C_BG);
    const char* title = "Mode Switch";
    d.setCursor((SCREEN_W - (int)strlen(title) * FONT_W * 2) / 2, 10);
    d.print(title);
    d.setTextSize(1);
    d.setTextColor(C_FG, C_BG);
    d.setCursor(14, 42);
    d.print(g_modeSwitchFeature.c_str());
    d.setCursor(14, 54);
    d.print("needs ");
    d.print(modeName(g_modeSwitchTarget));
    d.print(".");
    d.setCursor(14, 72);
    d.print("Switch mode and restart?");
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(14, 104);
    d.print("Enter = restart");
    d.setCursor(14, 116);
    d.print("fn+bksp = cancel");
}

static void handleModeSwitchPrompt() {
    if (dirty) {
        drawModeSwitchPrompt();
        dirty = false;
    }
    auto ev = readKeys();
    if (!ev.changed) return;
    if (ev.back) {
        goHome();
        return;
    }
    if (ev.enter) {
        settingsSetBootMode(g_modeSwitchTarget);
        delay(80);
        ESP.restart();
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
        const char* ver = "v2.4  --  M5Stack Cardputer";
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
        g_modePickerSel = settingsBootMode();
        resetSleepTimer();
        delay(80);
        M5Cardputer.update();
        state = State::MODE_PICKER;
        dirty = true;
    }
}


// ── Arduino entry points ────────────────────────────────────────────────────

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    initBatteryMonitoring();
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setFont(&fonts::Font0);
    M5Cardputer.Display.setTextSize(1);
    // Park shared expansion hats in safe idle states before any app touches them.
    // This avoids warm-boot lockups when a hat is attached but its app has not opened yet.
    //
    // LoRa / GPS cap:
    pinMode(LORA_NSS_PIN,  OUTPUT);          digitalWrite(LORA_NSS_PIN,  HIGH); // deselect SPI
    pinMode(LORA_RST_PIN,  OUTPUT);          digitalWrite(LORA_RST_PIN,  LOW);  // hold in reset until LoRa app opens
    pinMode(LORA_BUSY_PIN, INPUT_PULLDOWN);  // LoRa output — pulldown prevents float
    pinMode(LORA_DIO1_PIN, INPUT_PULLDOWN);  // LoRa IRQ output — pulldown prevents float
    pinMode(GPS_TX_PIN, OUTPUT);             digitalWrite(GPS_TX_PIN, HIGH);     // UART idle high
    pinMode(GPS_RX_PIN, INPUT_PULLUP);                                       // keep RX from floating

    // PINGEQUA / Hydra RF hat:
    pinMode(CC1101_CS_PIN, OUTPUT);         digitalWrite(CC1101_CS_PIN, HIGH);  // deselect CC1101
    pinMode(CC1101_GDO0_PIN, INPUT_PULLDOWN);                                   // raw RX line idle low
    pinMode(NRF24_CSN_PIN, OUTPUT);         digitalWrite(NRF24_CSN_PIN, HIGH);  // deselect nRF24
    pinMode(NRF24_CE_PIN, OUTPUT);          digitalWrite(NRF24_CE_PIN, LOW);    // radio disabled until app opens

    // Shared SD SPI bus comes up last after all chip-selects are parked.
    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);
}

void loop() {
    // Service background audio on every tick (no-op when inactive or suspended).
    // Skip when MP3 app is in foreground — it services audio->loop() itself.
    if (state != State::APP_MP3) bgAudioLoop();

    switch (state) {
        case State::BOOT:         handleBoot();        break;
        case State::MODE_PICKER:  handleModePicker();  break;
        case State::MODE_SWITCH_PROMPT: handleModeSwitchPrompt(); break;
        case State::LOCK_SCREEN:
            appLockLoop();
            if (appLockIsUnlocked()) {
                g_sessionUnlocked = true;
                launcherEnter();
                state = State::LAUNCHER;
            }
            break;

        case State::LAUNCHER:     launcherLoop();      break;
        case State::APP_SSH:      appSshLoop();        break;
        case State::APP_MP3:      appMp3Loop();        break;
        case State::APP_NOTES:    appNotesLoop();      break;
        case State::APP_SETTINGS: appSettingsLoop();   break;
        case State::APP_GAMES:    appGamesLoop();      break;
        case State::APP_FILES:    appFilesLoop();      break;
        case State::APP_IR_REMOTE: appIrRemoteLoop();  break;
        case State::APP_PHOTOS: appPhotosLoop(); break;
        case State::APP_VOICE_MEMOS: appVoiceMemosLoop(); break;
        case State::APP_HID_KEYBOARD: appHidKeyboardLoop(); break;
        case State::APP_USB_STORAGE: appUsbStorageLoop(); break;
        case State::APP_TIMER: appTimerLoop(); break;
        case State::APP_GPS: appGpsLoop(); break;
        case State::APP_LORA: appLoraLoop(); break;
        case State::APP_NFC:       appNfcLoop();       break;
        case State::APP_PAYLOADS:  appPayloadsLoop();  break;
        case State::APP_BLE:       appBleLoop();       break;
        case State::APP_DETECTOR:  appDetectorLoop();  break;
        case State::APP_WIFI:       appWifiLoop();      break;
        case State::APP_CC1101:     appCc1101Loop();    break;
        case State::APP_NRF24:      appNrf24Loop();     break;
        case State::APP_ESPNOW:     appEspnowLoop();    break;
        case State::APP_SD_HEALTH:  appSdHealthLoop();   break;
        case State::APP_CALC:       appCalcLoop();       break;
        case State::APP_FIRMWARE:   appFirmwareLoop();   break;
        case State::APP_PLACEHOLDER: appPlaceholderLoop(); break;
    }

    // After every app tick: if lock is enabled and the screen just woke from
    // sleep, re-lock the session and show the PIN screen.
    if (screenJustWoke() && settingsLockEnabled() && state != State::LOCK_SCREEN && state != State::BOOT) {
        g_sessionUnlocked = false;
        appLockEnter();
        state = State::LOCK_SCREEN;
    }
}
