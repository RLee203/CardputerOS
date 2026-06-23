#pragma once
// T-Embed compatibility shim.
// Presents the same M5Cardputer.Display / Speaker / update() / begin() API as
// the real M5Cardputer library so every app file compiles without modification.
// Resolved before the real library header via -I src/boards/tembed in build_flags.

#include <LovyanGFX.hpp>
#include "../../config.h"   // brings in boards/tembed.h for DISP_* pin constants

// Expose lgfx::fonts as bare 'fonts::' so all app code using fonts::Font0 etc.
// compiles unchanged.  We cannot 'using namespace lgfx' here because lgfx
// also declares millis()/delay() stubs that conflict with Arduino's globals.
namespace fonts = lgfx::fonts;

// ── LovyanGFX panel config for ST7789V3 170×320 ───────────────────────────
class LGFX_TEmbed : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789  _panel;
    lgfx::Bus_SPI       _bus;
    lgfx::Light_PWM     _light;
public:
    LGFX_TEmbed() {
        {
            auto cfg = _bus.config();
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_3wire   = false;
            cfg.use_lock    = false;  // LovyanGFX must not cache the Arduino spi_t*; CC1101
                                      // SpiStart() calls SPI.end() to reset SPI2, which frees
                                      // that pointer.  use_lock=false means LovyanGFX never
                                      // stores/uses it, so the free is harmless.  LovyanGFX
                                      // reconfigures all SPI2 registers fresh at every draw.
            cfg.dma_channel = 0;  // CPU-mode polling; DMA on SPI2 breaks Arduino SPI's
                                  // cmd.update/cmd.usr handshake → CC1101/NRF24 hang
            cfg.pin_sclk    = DISP_SCK;
            cfg.pin_mosi    = DISP_MOSI;
            cfg.pin_miso    = DISP_MISO;
            cfg.pin_dc      = DISP_DC;
            cfg.freq_write  = 80000000;
            cfg.freq_read   = 16000000;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs       = DISP_CS;
            cfg.pin_rst      = -1;
            cfg.pin_busy     = -1;
            cfg.panel_width  = 170;   // portrait physical size
            cfg.panel_height = 320;
            cfg.offset_x     = 35;   // ST7789 170px panel in 240px horizontal frame
            cfg.offset_y     = 0;
            cfg.invert       = true;
            cfg.rgb_order    = false;
            cfg.dlen_16bit   = false;
            cfg.bus_shared   = true;  // share bus with SD + CC1101
            _panel.config(cfg);
            setPanel(&_panel);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl      = DISP_BL;
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
    }
};

// Forward-declare globals defined in tembed_hal.cpp
extern LGFX_TEmbed  gTEmbedDisplay;

// ── Speaker stub (I2S managed directly by Audio library) ─────────────────
struct _TEmbed_Speaker_t {
    void begin()                                            {}
    void end()                                              {}
    void setVolume(uint8_t)                                 {}
    bool isPlaying()                                        { return false; }
    void tone(uint16_t, uint32_t = 0, int8_t = -1)        {}
    void playRaw(const int16_t*, size_t, uint32_t = 44100,
                 bool = false, uint32_t = 1, int8_t = -1)  {}
};

// ── M5Cardputer-compatible wrapper ────────────────────────────────────────
struct _TEmbed_t {
    LGFX_TEmbed&       Display;
    _TEmbed_Speaker_t  Speaker;

    _TEmbed_t() : Display(gTEmbedDisplay) {}

    // Accept any args (Cardputer passes M5.config() + bool); just init display.
    template<typename... A>
    void begin(A&&...) {
        gTEmbedDisplay.init();
        gTEmbedDisplay.setBrightness(128);
    }

    void update() {}   // no-op — encoder input is interrupt-driven
};

extern _TEmbed_t M5Cardputer;
