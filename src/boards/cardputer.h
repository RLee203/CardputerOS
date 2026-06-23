#pragma once

// ── Display ────────────────────────────────────────────────────────────────
constexpr int SCREEN_W = 240;
constexpr int SCREEN_H = 135;

// ── I2S / Audio (MAX98357A) ───────────────────────────────────────────────
constexpr int I2S_BCLK_PIN  = 41;
constexpr int I2S_LRCLK_PIN = 43;
constexpr int I2S_DOUT_PIN  = 42;

// ── SD card SPI ────────────────────────────────────────────────────────────
constexpr int SD_SCK_PIN  = 40;
constexpr int SD_MISO_PIN = 39;
constexpr int SD_MOSI_PIN = 14;
constexpr int SD_CS_PIN   = 12;

// ── IR ─────────────────────────────────────────────────────────────────────
constexpr int IR_TX_PIN = 44;
constexpr int IR_RX_PIN = 2;

// ── NFC / PN532 (Grove on LoRa cap: G8=SDA, G9=SCL) ──────────────────────
constexpr int NFC_SDA_PIN = 8;
constexpr int NFC_SCL_PIN = 9;

// ── GPS / LoRa Cap ────────────────────────────────────────────────────────
constexpr int GPS_TX_PIN      = 13;
constexpr int GPS_RX_PIN      = 15;
constexpr uint32_t GPS_BAUD   = 115200;
constexpr int LORA_NSS_PIN    = 5;
constexpr int LORA_RST_PIN    = 3;
constexpr int LORA_BUSY_PIN   = 6;
constexpr int LORA_DIO1_PIN   = 4;

// ── PINGEQUA CC1101 / nRF24 hat ───────────────────────────────────────────
// Shares SPI bus (SCK=40, MISO=39, MOSI=14) with SD card slot.
// CC1101: CS=G13, GDO0=G5  |  nRF24: CS=G6, CE=G4
constexpr int CC1101_CS_PIN   = 13;
constexpr int CC1101_GDO0_PIN = 5;
constexpr int NRF24_CSN_PIN   = 6;
constexpr int NRF24_CE_PIN    = 4;
