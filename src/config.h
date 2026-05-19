#pragma once
#include <cstdint>

// ── Display ────────────────────────────────────────────────────────────────
constexpr int SCREEN_W  = 240;
constexpr int SCREEN_H  = 135;
constexpr int STATUS_H  = 13;
constexpr int TERM_Y    = STATUS_H;
constexpr int TERM_H    = SCREEN_H - STATUS_H;

// Font0 in LovyanGFX: 6 wide × 8 tall per glyph
constexpr int FONT_W    = 6;
constexpr int FONT_H    = 8;
constexpr int TERM_COLS = SCREEN_W / FONT_W;        // 40
constexpr int TERM_ROWS = TERM_H   / FONT_H;        // 15

// ── Colours (RGB888) ───────────────────────────────────────────────────────
// Fixed – never change
constexpr uint32_t C_BG    = 0x000000;
constexpr uint32_t C_INPUT = 0xFFFFFF;
constexpr uint32_t C_ERROR = 0xFF3333;
constexpr uint32_t C_ACCENT= 0x00AAFF;

// Theme-able – settings can modify these at runtime (C++17 inline globals)
inline uint32_t C_FG        = 0x00EE00;
inline uint32_t C_DIM       = 0x006600;
inline uint32_t C_CURSOR    = 0x00EE00;
inline uint32_t C_STATUS_BG = 0x002200;
inline uint32_t C_STATUS_FG = 0x00EE00;
inline uint32_t C_HIGHLIGHT = 0x003300;

// ── Timing ─────────────────────────────────────────────────────────────────
constexpr uint32_t CURSOR_BLINK_MS = 500;
constexpr uint32_t WIFI_TIMEOUT_MS = 15000;
constexpr uint32_t SSH_RECV_MS     = 20;

// Sleep timeout in ms; 0 = disabled. Changed at runtime by Settings.
inline uint32_t sleepTimeoutMs = 0;

// ── Storage ────────────────────────────────────────────────────────────────
constexpr const char* PROFILES_PATH = "/profiles.json";
constexpr const char* WIFI_PATH     = "/wifi.json";
constexpr int MAX_PROFILES          = 10;
constexpr const char* NOTES_DIR     = "/notes";
constexpr const char* VOICE_MEMOS_DIR = "/voice";

// ── SSH ────────────────────────────────────────────────────────────────────
constexpr int SSH_DEFAULT_PORT = 22;
constexpr int SSH_RECV_BUF     = 512;

// ── IR ─────────────────────────────────────────────────────────────────────
constexpr int IR_TX_PIN = 44;
constexpr int IR_RX_PIN = 2;
constexpr const char* IR_CODES_PATH = "/ir_codes.json";

// ── I2S / Audio ────────────────────────────────────────────────────────────
constexpr int I2S_BCLK_PIN  = 41;
constexpr int I2S_LRCLK_PIN = 43;
constexpr int I2S_DOUT_PIN  = 42;

// ── SD card SPI ────────────────────────────────────────────────────────────
constexpr int SD_SCK_PIN  = 40;
constexpr int SD_MISO_PIN = 39;
constexpr int SD_MOSI_PIN = 14;
constexpr int SD_CS_PIN   = 12;

// ── NFC / PN532 (Grove on LoRa cap) ───────────────────────────────────────
constexpr int NFC_SDA_PIN     = 8;
constexpr int NFC_SCL_PIN     = 9;
constexpr const char* NFC_DIR = "/nfc";

// ── GPS / LoRa Cap ────────────────────────────────────────────────────────
constexpr int GPS_TX_PIN      = 13;   // Cardputer G13 -> cap RX
constexpr int GPS_RX_PIN      = 15;   // Cardputer G15 <- cap TX
constexpr uint32_t GPS_BAUD   = 115200;
constexpr const char* GPS_DIR = "/gps";
constexpr int LORA_NSS_PIN    = 5;
constexpr int LORA_RST_PIN    = 3;
constexpr int LORA_BUSY_PIN   = 6;
constexpr int LORA_DIO1_PIN   = 4;

// ── PINGEQUA NRF24&CC1101 RF 2in1 Module ──────────────────────────────────
// Shares SPI bus (SCK=40, MISO=39, MOSI=14) with SD/LoRa cap slot.
// Hardware MUX on module selects CC1101 vs nRF24 (can't use both at once).
// G6/G4 conflict with LORA_BUSY/DIO1 — LoRa cap not present with PINGEQUA.
constexpr int CC1101_CS_PIN   = 13;   // Hydra/PINGEQUA hat manual: CC1101 CS
constexpr int CC1101_GDO0_PIN = 5;    // Hydra/PINGEQUA hat manual: CC1101 IO0/GDO0
constexpr int NRF24_CSN_PIN   = 6;    // Hydra/PINGEQUA hat manual: nRF24 CS
constexpr int NRF24_CE_PIN    = 4;    // Hydra/PINGEQUA hat manual: nRF24 IO0/CE

// ── BLE logging ────────────────────────────────────────────────────────────
constexpr const char* BLE_DIR = "/ble";
constexpr const char* RF_DIR  = "/rf";
