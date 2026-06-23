#pragma once
#include <cstdint>

// ── Board selection ────────────────────────────────────────────────────────
#if defined(BOARD_TEMBED)
#  include "boards/tembed.h"
#else
#  include "boards/cardputer.h"
#endif

// ── Shared layout constants ────────────────────────────────────────────────
constexpr int STATUS_H  = 13;
constexpr int TERM_Y    = STATUS_H;
constexpr int TERM_H    = SCREEN_H - STATUS_H;

// Font0 in LovyanGFX: 6 wide × 8 tall per glyph
constexpr int FONT_W    = 6;
constexpr int FONT_H    = 8;
constexpr int TERM_COLS = SCREEN_W / FONT_W;
constexpr int TERM_ROWS = TERM_H   / FONT_H;

// ── Colours (RGB888) ───────────────────────────────────────────────────────
// Fixed
constexpr uint32_t C_BG    = 0x000000;
constexpr uint32_t C_INPUT = 0xFFFFFF;
constexpr uint32_t C_ERROR = 0xFF3333;
constexpr uint32_t C_ACCENT= 0x00AAFF;

// Theme-able (modified at runtime by Settings)
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
constexpr const char* PROFILES_PATH   = "/profiles.json";
constexpr const char* WIFI_PATH       = "/wifi.json";
constexpr int         MAX_PROFILES    = 10;
constexpr const char* NOTES_DIR       = "/notes";
constexpr const char* VOICE_MEMOS_DIR = "/voice";

// ── SSH ────────────────────────────────────────────────────────────────────
constexpr int SSH_DEFAULT_PORT = 22;
constexpr int SSH_RECV_BUF     = 512;

// ── IR codes ───────────────────────────────────────────────────────────────
constexpr const char* IR_CODES_PATH = "/ir_codes.json";

// ── Logging dirs ───────────────────────────────────────────────────────────
constexpr const char* NFC_DIR = "/nfc";
constexpr const char* GPS_DIR = "/gps";
constexpr const char* BLE_DIR = "/ble";
constexpr const char* RF_DIR  = "/rf";
