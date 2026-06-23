#pragma once

// ── Display (ST7789V3 170×320 portrait → 320×170 landscape at rotation=1) ─
constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 170;
constexpr int DISP_CS   = 41;
constexpr int DISP_MOSI = 9;
constexpr int DISP_MISO = 10;
constexpr int DISP_SCK  = 11;
constexpr int DISP_DC   = 16;
constexpr int DISP_BL   = 21;

// ── Rotary encoder + side button ───────────────────────────────────────────
constexpr int ENC_A_PIN    = 4;
constexpr int ENC_B_PIN    = 5;
constexpr int ENC_KEY_PIN  = 0;   // encoder click (enter)
constexpr int USER_KEY_PIN = 6;   // side button (back)

// ── I2S / Audio (MAX98357A) ───────────────────────────────────────────────
constexpr int I2S_BCLK_PIN  = 46;
constexpr int I2S_LRCLK_PIN = 40;
constexpr int I2S_DOUT_PIN  = 7;

// ── Shared SPI bus: display + SD + CC1101 all on same pins ────────────────
constexpr int SD_SCK_PIN  = 11;
constexpr int SD_MISO_PIN = 10;
constexpr int SD_MOSI_PIN = 9;
constexpr int SD_CS_PIN   = 13;

// ── CC1101 (built-in sub-GHz) ─────────────────────────────────────────────
constexpr int CC1101_CS_PIN   = 12;
constexpr int CC1101_GDO0_PIN = 3;
constexpr int CC1101_GDO2_PIN = 38;
constexpr int CC1101_SW0_PIN  = 48;   // antenna switch
constexpr int CC1101_SW1_PIN  = 47;

// ── IR (built-in transceiver) ─────────────────────────────────────────────
constexpr int IR_TX_PIN = 2;   // BOARD_IR_EN
constexpr int IR_RX_PIN = 1;   // BOARD_IR_RX

// ── NFC / PN532 via I2C (built-in on Plus variant, SDA=8/SCL=18) ─────────
constexpr int NFC_SDA_PIN   = 8;
constexpr int NFC_SCL_PIN   = 18;
constexpr int NFC_RESET_PIN = 45;  // active-LOW; drive HIGH to deassert

// ── WS2812 LED ring ───────────────────────────────────────────────────────
constexpr int LED_DATA_PIN = 14;

// ── Power enable ───────────────────────────────────────────────────────────
constexpr int POWER_EN_PIN = 15;

// ── GPS (external module on JST connector — same pins as NRF24_CE/CSN) ───
// Wire GPS module TX → GPIO44 (ESP32 RX), GPS module RX → GPIO43 (ESP32 TX)
constexpr int GPS_RX_PIN    = 44;
constexpr int GPS_TX_PIN    = 43;
constexpr uint32_t GPS_BAUD = 9600;

// ── Stubs for peripherals not present (keeps shared app code compiling) ───
constexpr int LORA_NSS_PIN  = -1;
constexpr int LORA_RST_PIN  = -1;
constexpr int LORA_BUSY_PIN = -1;
constexpr int LORA_DIO1_PIN = -1;
constexpr int NRF24_CSN_PIN = 44;
constexpr int NRF24_CE_PIN  = 43;
