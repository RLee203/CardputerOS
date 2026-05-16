#include "app_lora.h"

#include <M5Cardputer.h>
#include <RadioLib.h>
#include <SPI.h>
#include "config.h"
#include "input.h"
#include "nav.h"

namespace {

SX1262 g_lora = new Module(LORA_NSS_PIN, LORA_DIO1_PIN, LORA_RST_PIN, LORA_BUSY_PIN);

bool g_inited = false;
bool g_initOk = false;
bool g_dirty = true;
String g_input;
String g_msgs[8];
int g_msgCount = 0;
bool g_rxFlag = false;
bool g_txBusy = false;

void markDirty() { g_dirty = true; }

void IRAM_ATTR onLoraReceive() {
    g_rxFlag = true;
}

void pushMsg(const String& line) {
    if (g_msgCount < 8) {
        g_msgs[g_msgCount++] = line;
    } else {
        for (int i = 0; i < 7; ++i) g_msgs[i] = g_msgs[i + 1];
        g_msgs[7] = line;
    }
    markDirty();
}

void clearMsgs() {
    for (int i = 0; i < 8; ++i) g_msgs[i] = "";
    g_msgCount = 0;
    markDirty();
}

void drawFrame() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.drawRect(0, 0, SCREEN_W, SCREEN_H, C_FG);
    d.drawFastHLine(0, 14, SCREEN_W, C_FG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_FG, C_BG);
    d.setCursor((SCREEN_W - (8 * FONT_W)) / 2, 4);
    d.print("LoRa Chat");
    drawBatteryWidget(C_BG, SCREEN_W - 43);
}

void drawUi() {
    auto& d = M5Cardputer.Display;
    drawFrame();

    d.setTextColor(g_initOk ? 0x33CC66 : C_ERROR, C_BG);
    d.setCursor(8, 20);
    d.print(g_initOk ? "READY" : "INIT FAILED");

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(60, 20);
    d.print("868MHz raw text");

    d.drawRect(6, 30, SCREEN_W - 12, 74, C_DIM);
    for (int i = 0; i < g_msgCount; ++i) {
        d.setTextColor(C_FG, C_BG);
        d.setCursor(10, 34 + i * 8);
        String line = g_msgs[i];
        if (line.length() > 36) line = line.substring(0, 36);
        d.print(line);
    }

    d.drawRect(6, 108, SCREEN_W - 12, 16, C_DIM);
    d.setTextColor(C_INPUT, C_BG);
    d.setCursor(10, 112);
    String shown = g_input;
    if (shown.length() > 34) shown = shown.substring(shown.length() - 34);
    d.print(shown);

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(12, SCREEN_H - 10);
    d.print("Enter send  fn+D clear  home");
    g_dirty = false;
}

void ensureInit() {
    if (g_inited) return;
    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    int state = g_lora.begin(868.0, 500.0, 7, 5, 0x34, 10, 10);
    g_initOk = (state == RADIOLIB_ERR_NONE);
    if (g_initOk) {
        g_lora.setDio2AsRfSwitch(true);
        g_lora.setCurrentLimit(140.0);
        g_lora.setPacketReceivedAction(onLoraReceive);
        g_lora.startReceive();
        pushMsg("LoRa initialized");
    } else {
        pushMsg("LoRa init failed");
    }
    g_inited = true;
}

void handleReceive() {
    if (!g_rxFlag || !g_initOk) return;
    g_rxFlag = false;
    String msg;
    int state = g_lora.readData(msg);
    if (state == RADIOLIB_ERR_NONE) {
        pushMsg("RX: " + msg);
    } else {
        pushMsg("RX error");
    }
    g_lora.startReceive();
    g_txBusy = false;
}

void sendMessage() {
    if (!g_initOk || !g_input.length()) return;
    pushMsg("TX: " + g_input);
    int state = g_lora.transmit(g_input);
    if (state == RADIOLIB_ERR_NONE) {
        g_input = "";
    } else {
        pushMsg("Send failed");
    }
    g_lora.startReceive();
    g_txBusy = false;
}

}  // namespace

void appLoraEnter() {
    ensureInit();
    g_input = "";
    markDirty();
}

void appLoraLoop() {
    handleReceive();
    if (g_dirty) drawUi();

    auto ev = readKeys();
    if (!ev.changed) return;

    if (ev.back) {
        goHome();
        return;
    }

    if (ev.fnKey) {
        for (char c : ev.chars) {
            if (c == 'q' || c == 'Q') {
                goHome();
                return;
            }
            if (c == 'd' || c == 'D') {
                clearMsgs();
                return;
            }
        }
    }

    if (ev.del) {
        if (g_input.length()) {
            g_input.remove(g_input.length() - 1);
        }
        markDirty();
        return;
    }

    if (ev.enter) {
        sendMessage();
        markDirty();
        return;
    }

    if (!ev.fnKey) {
        for (char c : ev.chars) {
            if (c >= 32 && c <= 126) {
                g_input += c;
            }
        }
        markDirty();
    }
}
