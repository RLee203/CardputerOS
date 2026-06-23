#include <M5Cardputer.h>
#include <SPI.h>
#include <WiFi.h>
#ifdef BOARD_TEMBED
#  include <Wire.h>
#endif
#include "config.h"
#include "profiles.h"
#include "wifi_mgr.h"
#include "input.h"
#include "nav.h"
#include "launcher.h"
#include "app_mp3.h"
#include "app_notes.h"
#include "app_settings.h"
#include "app_files.h"
#include "app_ir_remote.h"
#include "app_photos.h"
#include "app_placeholder.h"
#include "app_timer.h"
#include "app_gps.h"
#include "app_nfc.h"
#include "app_lock.h"
#include "app_payloads.h"
#include "app_ble.h"
#include "app_detector.h"
#include "app_wifi.h"
#include "app_cc1101.h"
#include "app_espnow.h"
#include "app_sd_health.h"
#include "app_calc.h"
#include "app_firmware.h"
#ifndef BOARD_TEMBED
#  include "terminal.h"
#  include "ssh_session.h"
#  include "app_ssh.h"
#  include "app_games.h"
#  include "app_hid_keyboard.h"
#  include "app_usb_storage.h"
#  include "app_usb_tester.h"
#  include "app_lora.h"
#  include "app_voice_memos.h"
#endif
#include "app_nrf24.h"

#ifndef BOARD_TEMBED
Terminal term;
#endif

// ── App state ──────────────────────────────────────────────────────────────
enum class State {
    BOOT,
    MODE_PICKER,
    MODE_SWITCH_PROMPT,
    LOCK_SCREEN,
    LAUNCHER,
    APP_SSH, APP_MP3, APP_NOTES, APP_SETTINGS, APP_GAMES, APP_FILES,
    APP_IR_REMOTE, APP_PHOTOS, APP_VOICE_MEMOS, APP_HID_KEYBOARD,
    APP_USB_STORAGE, APP_TIMER, APP_GPS, APP_LORA, APP_NFC, APP_PAYLOADS,
    APP_BLE, APP_DETECTOR, APP_WIFI, APP_CC1101, APP_NRF24, APP_ESPNOW,
    APP_SD_HEALTH, APP_CALC, APP_FIRMWARE, APP_PLACEHOLDER
};

static State state             = State::BOOT;
static bool  dirty             = true;
static bool  g_sessionUnlocked = false;
static bool  g_wifiSuspended   = false;
static bool  g_wifiWasConnected = false;
static wifi_mode_t g_wifiPrevMode = WIFI_OFF;
static DeviceMode g_deviceMode = DeviceMode::RADIO;
static DeviceMode g_modePickerSel = DeviceMode::RADIO;
static DeviceMode g_modeSwitchTarget = DeviceMode::RADIO;
static String g_modeSwitchFeature = "";
static uint32_t g_modePickerReadyAt = 0;
static bool g_modePickerPrimed = false;

bool requiresSdMode(AppScene scene) {
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

bool requiresRadioMode(AppScene scene) {
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
bool isSdMode()    { return g_deviceMode == DeviceMode::SD; }
bool isRadioMode() { return g_deviceMode == DeviceMode::RADIO; }
void setCurrentDeviceMode(DeviceMode mode) {
    g_deviceMode = mode;
    settingsSetBootMode(mode);
}

void requestModeSwitch(DeviceMode targetMode, const char* feature) {
    g_modeSwitchTarget  = targetMode;
    g_modeSwitchFeature = feature ? feature : "This feature";
    state = State::MODE_SWITCH_PROMPT;
    dirty = true;
}

void suspendWifiForSd() {
    g_wifiPrevMode      = WiFi.getMode();
    g_wifiWasConnected  = (WiFi.status() == WL_CONNECTED);
    g_wifiSuspended     = (g_wifiPrevMode != WIFI_OFF);
    if (!g_wifiSuspended) return;
    WiFi.disconnect(true, true);
    delay(80);
    WiFi.mode(WIFI_OFF);
    delay(220);
}

void resumeWifiAfterSd() {
    g_wifiSuspended    = false;
    g_wifiPrevMode     = WIFI_OFF;
    g_wifiWasConnected = false;
}

static String inputBuf;
static int    inputCursor = 0;

static void setState(State s) { state = s; dirty = true; inputBuf = ""; inputCursor = 0; }

// ── nav.h implementations ──────────────────────────────────────────────────

void goHome() {
    resumeWifiAfterSd();
    bgAudioResume();
    if (settingsLockEnabled() && !g_sessionUnlocked) {
        appLockEnter();
        state = State::LOCK_SCREEN;
        return;
    }
    launcherEnter();
    state = State::LAUNCHER;
}

void launchApp(AppScene scene) {
    if (requiresSdMode(scene) && scene != AppScene::MP3) bgAudioSuspend();
    if (requiresSdMode(scene) && !isSdMode()) {
        requestModeSwitch(DeviceMode::SD, "This app"); return;
    }
    if (requiresRadioMode(scene) && !isRadioMode()) {
        requestModeSwitch(DeviceMode::RADIO, "This app"); return;
    }
    switch (scene) {
#ifndef BOARD_TEMBED
        case AppScene::SSH:        appSshEnter();        state = State::APP_SSH;        break;
        case AppScene::GAMES:      appGamesEnter();      state = State::APP_GAMES;      break;
        case AppScene::VOICE_MEMOS: appVoiceMemosEnter(); state = State::APP_VOICE_MEMOS; break;
        case AppScene::HID_KEYBOARD: appHidKeyboardEnter(); state = State::APP_HID_KEYBOARD; break;
        case AppScene::USB_STORAGE:  appUsbStorageEnter(); state = State::APP_USB_STORAGE; break;
        case AppScene::LORA:       appLoraEnter();       state = State::APP_LORA;       break;
#endif
        case AppScene::NRF24:      appNrf24Enter();      state = State::APP_NRF24;      break;
        case AppScene::MP3:        appMp3Enter();        state = State::APP_MP3;        break;
#ifndef BOARD_TEMBED
        case AppScene::NOTES:      appNotesEnter();      state = State::APP_NOTES;      break;
#endif
        case AppScene::SETTINGS:   appSettingsEnter();   state = State::APP_SETTINGS;   break;
        case AppScene::FILES:      appFilesEnter();      state = State::APP_FILES;      break;
        case AppScene::IR_REMOTE:  appIrRemoteEnter();   state = State::APP_IR_REMOTE;  break;
        case AppScene::PHOTOS:     appPhotosEnter();     state = State::APP_PHOTOS;     break;
        case AppScene::NFC:        appNfcEnter();        state = State::APP_NFC;        break;
#ifndef BOARD_TEMBED
        case AppScene::PAYLOADS:   appPayloadsEnter();   state = State::APP_PAYLOADS;   break;
#endif
        case AppScene::TIMER:      appTimerEnter();      state = State::APP_TIMER;      break;
        case AppScene::GPS:        appGpsEnter();        state = State::APP_GPS;        break;
        case AppScene::BLE:        appBleEnter();        state = State::APP_BLE;        break;
        case AppScene::DETECTOR:   appDetectorEnter();   state = State::APP_DETECTOR;   break;
        case AppScene::WIFI_TOOLS: appWifiEnter();       state = State::APP_WIFI;       break;
        case AppScene::CC1101:     appCc1101Enter();     state = State::APP_CC1101;     break;
        case AppScene::ESPNOW:     appEspnowEnter();     state = State::APP_ESPNOW;     break;
        case AppScene::SD_HEALTH:  appSdHealthEnter();   state = State::APP_SD_HEALTH;  break;
#ifndef BOARD_TEMBED
        case AppScene::CALC:       appCalcEnter();       state = State::APP_CALC;       break;
#endif
        case AppScene::FIRMWARE:   appFirmwareEnter();   state = State::APP_FIRMWARE;   break;
        default: break;
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
#ifdef BOARD_TEMBED
    const char* title = "T-Embed OS 2.4";
#else
    const char* title = "Cardputer OS 2.4";
#endif
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
        int x = 14 + i * (SCREEN_W / 2 - 14);
        int y = 46;
        int w = SCREEN_W / 2 - 28;
        uint32_t box = sel ? (mode == DeviceMode::SD ? 0x7A5A00 : 0x114488) : 0x1A1A1A;
        d.fillRoundRect(x, y, w, 44, 6, box);
        d.drawRoundRect(x, y, w, 44, 6, sel ? 0xFFFFFF : C_DIM);
        d.setTextColor(0xFFFFFF, box);
        const char* t = (mode == DeviceMode::SD) ? "MEDIA" : "RADIO";
        d.setCursor(x + (w - (int)strlen(t) * FONT_W) / 2, y + 8);
        d.print(t);
        d.setTextColor(sel ? 0xFFFFFF : C_DIM, box);
        const char* s = (mode == DeviceMode::SD) ? "Local tools" : "Wireless tools";
        d.setCursor(x + (w - (int)strlen(s) * FONT_W) / 2, y + 24);
        d.print(s);
    }

    d.setTextColor(C_DIM, C_BG);
#ifdef BOARD_TEMBED
    d.setCursor(14, SCREEN_H - 26);
    d.print("Rotate=select  Click=boot");
    d.setCursor(14, SCREEN_H - 14);
    d.print("Side btn = switch mode");
#else
    d.setCursor(14, 102);
    d.print("Left/Right=select  Enter=boot");
    d.setCursor(14, 114);
    d.print("fn+M = switch mode in launcher");
#endif
}

static void handleModePicker() {
    if (dirty) {
        resetSleepTimer();
#ifndef BOARD_TEMBED
        M5Cardputer.update();
        delay(25);
        M5Cardputer.update();
#endif
        g_modePickerReadyAt = millis() + 320;
        g_modePickerPrimed  = true;
        drawModePicker();
        dirty = false;
    }
    if (g_modePickerPrimed) {
#ifndef BOARD_TEMBED
        M5Cardputer.update();
#endif
        if (millis() < g_modePickerReadyAt) return;
        g_modePickerPrimed = false;
    }
    auto ev = readKeys();
    if (!ev.changed) return;
    if (ev.right || ev.down) { g_modePickerSel = DeviceMode::RADIO; dirty = true; return; }
    if (ev.left  || ev.up)   { g_modePickerSel = DeviceMode::SD;    dirty = true; return; }
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
#ifdef BOARD_TEMBED
    d.print("Side btn = cancel");
#else
    d.print("fn+bksp = cancel");
#endif
}

static void handleModeSwitchPrompt() {
    if (dirty) { drawModeSwitchPrompt(); dirty = false; }
    auto ev = readKeys();
    if (!ev.changed) return;
    if (ev.back) { goHome(); return; }
    if (ev.enter) {
        settingsSetBootMode(g_modeSwitchTarget);
        delay(80);
        ESP.restart();
    }
}

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

// ── Boot screen ────────────────────────────────────────────────────────────
static unsigned long bootStart = 0;
static int bootLine = 0;
static const char* bootLines[] = {
    "Mounting LittleFS...",
    "Loading profiles...",
    "Starting WiFi...",
    "Initializing hardware...",
};
static constexpr int BOOT_LINES  = 4;
static constexpr int BOOT_MSG_Y  = 72;
static constexpr int BOOT_LINE_H = 10;

void handleBoot() {
    if (dirty) {
        auto& d = M5Cardputer.Display;
        d.fillScreen(C_BG);
        d.setFont(&fonts::Font0);

#ifdef BOARD_TEMBED
        const char* t1  = "T-Embed";
        const char* ver = "v2.4  --  LILYGO T-Embed CC1101";
#else
        const char* t1  = "Cardputer";
        const char* ver = "v2.4  --  M5Stack Cardputer";
#endif
        d.setTextSize(2);
        d.setTextColor(C_FG, C_BG);
        d.setCursor((SCREEN_W - (int)strlen(t1) * FONT_W * 2) / 2, 8);
        d.print(t1);

        d.setTextSize(3);
        d.setCursor((SCREEN_W - 2 * FONT_W * 3) / 2, 28);
        d.print("OS");

        d.setTextSize(1);
        d.setTextColor(C_DIM, C_BG);
        d.setCursor((SCREEN_W - (int)strlen(ver) * FONT_W) / 2, 56);
        d.print(ver);

        d.drawFastHLine(0, 65, SCREEN_W, C_DIM);

        bootLine  = 0;
        bootStart = millis();
        dirty     = false;
        Profiles.begin();
        WifiMgr.begin();
        settingsLoadFromFS();
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
        readKeys();   // flush any encoder ticks / key state accumulated during boot
        state = State::MODE_PICKER;
        dirty = true;
    }
}

// ── Arduino entry points ───────────────────────────────────────────────────

void setup() {
#ifdef BOARD_TEMBED
    Serial.begin(115200);

    // Enable board power rail before anything else
    pinMode(POWER_EN_PIN, OUTPUT);
    digitalWrite(POWER_EN_PIN, HIGH);
    delay(50);   // give rail time to stabilise

    // Park SPI CS lines before LovyanGFX takes the bus
    pinMode(CC1101_CS_PIN, OUTPUT); digitalWrite(CC1101_CS_PIN, HIGH);
    pinMode(SD_CS_PIN,     OUTPUT); digitalWrite(SD_CS_PIN,     HIGH);
    // GPIO44 floating LOW makes NRF24 drive MISO simultaneously with CC1101 → bus contention
#if NRF24_CSN_PIN >= 0
    pinMode(NRF24_CSN_PIN, OUTPUT); digitalWrite(NRF24_CSN_PIN, HIGH);
    pinMode(NRF24_CE_PIN,  OUTPUT); digitalWrite(NRF24_CE_PIN,  LOW);
#endif

    // Pre-init Arduino SPI BEFORE LovyanGFX. On ESP32-S3, spiStartBus() calls
    // periph_module_reset(SPI2) which wipes the SPI2 peripheral state. If ELECHOUSE
    // or RF24 calls SPI.begin() after LovyanGFX has taken SPI2 via ESP-IDF, that
    // reset destroys LovyanGFX's device handle → next display draw hangs.
    // Pre-init sets _spi non-NULL so any subsequent SPI.begin() call (from ELECHOUSE,
    // RF24, etc.) hits the early-return guard and never resets the peripheral.
    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN);   // no SS → no hardware CS pin

    Serial.println("Display init...");
    M5Cardputer.begin();
    M5Cardputer.Display.setRotation(3);   // landscape 320×170 (panel is physically mounted flipped)
    M5Cardputer.Display.setFont(&fonts::Font0);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.fillScreen(0x001F);   // blue splash — confirms display alive
    Serial.println("Display OK");

    // Deassert PN532 RESET before I2C init (GPIO45 may float LOW as a strapping pin)
#if NFC_RESET_PIN >= 0
    pinMode(NFC_RESET_PIN, OUTPUT);
    digitalWrite(NFC_RESET_PIN, HIGH);
    delay(10);
#endif

    // I2C for NFC + battery gauge (BQ27220)
    Wire.begin(NFC_SDA_PIN, NFC_SCL_PIN);

#else
    // ── Cardputer init ─────────────────────────────────────────────────────
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    initBatteryMonitoring();
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setFont(&fonts::Font0);
    M5Cardputer.Display.setTextSize(1);

    // Park LoRa / GPS cap in safe idle state
    pinMode(LORA_NSS_PIN,  OUTPUT);         digitalWrite(LORA_NSS_PIN,  HIGH);
    pinMode(LORA_RST_PIN,  OUTPUT);         digitalWrite(LORA_RST_PIN,  LOW);
    pinMode(LORA_BUSY_PIN, INPUT_PULLDOWN);
    pinMode(LORA_DIO1_PIN, INPUT_PULLDOWN);
    pinMode(GPS_TX_PIN, OUTPUT);            digitalWrite(GPS_TX_PIN, HIGH);
    pinMode(GPS_RX_PIN, INPUT_PULLUP);

    // Park PINGEQUA RF hat
    pinMode(CC1101_CS_PIN,   OUTPUT);       digitalWrite(CC1101_CS_PIN,   HIGH);
    pinMode(CC1101_GDO0_PIN, INPUT_PULLDOWN);
    pinMode(NRF24_CSN_PIN,   OUTPUT);       digitalWrite(NRF24_CSN_PIN,   HIGH);
    pinMode(NRF24_CE_PIN,    OUTPUT);       digitalWrite(NRF24_CE_PIN,    LOW);

    // SD SPI
    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);
#endif
}

void loop() {
    if (state != State::APP_MP3) bgAudioLoop();

    switch (state) {
        case State::BOOT:               handleBoot();               break;
        case State::MODE_PICKER:        handleModePicker();         break;
        case State::MODE_SWITCH_PROMPT: handleModeSwitchPrompt();   break;
        case State::LOCK_SCREEN:
            appLockLoop();
            if (appLockIsUnlocked()) {
                g_sessionUnlocked = true;
                launcherEnter();
                state = State::LAUNCHER;
            }
            break;

        case State::LAUNCHER:
            if (isRadioMode()) espnowProcessBackground();
            launcherLoop();
            break;
        case State::APP_MP3:         appMp3Loop();         break;
#ifndef BOARD_TEMBED
        case State::APP_NOTES:       appNotesLoop();       break;
#endif
        case State::APP_SETTINGS:    appSettingsLoop();    break;
        case State::APP_FILES:       appFilesLoop();       break;
        case State::APP_IR_REMOTE:   appIrRemoteLoop();    break;
        case State::APP_PHOTOS:      appPhotosLoop();      break;
        case State::APP_TIMER:       appTimerLoop();       break;
        case State::APP_GPS:         appGpsLoop();         break;
        case State::APP_NFC:         appNfcLoop();         break;
#ifndef BOARD_TEMBED
        case State::APP_PAYLOADS:    appPayloadsLoop();    break;
#endif
        case State::APP_BLE:         appBleLoop();         break;
        case State::APP_DETECTOR:    appDetectorLoop();    break;
        case State::APP_WIFI:        appWifiLoop();        break;
        case State::APP_CC1101:      appCc1101Loop();      break;
        case State::APP_ESPNOW:      appEspnowLoop();      break;
        case State::APP_SD_HEALTH:   appSdHealthLoop();    break;
#ifndef BOARD_TEMBED
        case State::APP_CALC:        appCalcLoop();        break;
#endif
        case State::APP_FIRMWARE:    appFirmwareLoop();    break;
        case State::APP_PLACEHOLDER: appPlaceholderLoop(); break;

#ifndef BOARD_TEMBED
        case State::APP_SSH:          appSshLoop();          break;
        case State::APP_GAMES:        appGamesLoop();        break;
        case State::APP_VOICE_MEMOS:  appVoiceMemosLoop();   break;
        case State::APP_HID_KEYBOARD: appHidKeyboardLoop();  break;
        case State::APP_USB_STORAGE:  appUsbStorageLoop();   break;
        case State::APP_LORA:         appLoraLoop();         break;
#endif
        case State::APP_NRF24:        appNrf24Loop();        break;
        default: break;
    }

    if (screenJustWoke() && settingsLockEnabled()
        && state != State::LOCK_SCREEN && state != State::BOOT) {
        g_sessionUnlocked = false;
        appLockEnter();
        state = State::LOCK_SCREEN;
    }
}
