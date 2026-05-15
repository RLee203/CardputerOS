#include "app_ir_remote.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#define SEND_PWM_BY_TIMER
#define SEND_LEDC_CHANNEL 0
#define NO_LED_FEEDBACK_CODE
#include <IRremote.hpp>

namespace {
enum class IrScene { LIST, LEARN_WAIT, LEARN_NAME, DELETE_CONFIRM };
enum class IrCodeKind : uint8_t { Decoded = 0, Raw = 1 };

struct IrCode {
    String name;
    IrCodeKind kind = IrCodeKind::Decoded;
    uint8_t protocol = UNKNOWN;
    uint16_t address = 0;
    uint16_t command = 0;
    uint16_t bits = 0;
    uint16_t frequency = 38;
    uint16_t rawLen = 0;
    uint16_t rawData[RAW_BUFFER_LENGTH] = {};
};

static constexpr int MAX_IR_CODES = 32;
static IrCode codes[MAX_IR_CODES];
static int codeCount = 0;
static int selected = 0;
static int deleteTarget = -1;
static bool dirty = true;
static IrScene scene = IrScene::LIST;
static String learnName;
static IrCode pendingCode = {};
static bool irReady = false;
static String learnStatus = "Waiting for IR signal...";
static bool lastRxLevel = true;
static uint32_t rxEdgeCount = 0;
static uint32_t lastStatusMs = 0;
static bool learnOverlayDirty = true;
static volatile bool rawCaptureActive = false;
static volatile uint32_t rawLastEdgeUs = 0;
static volatile uint32_t rawLastActivityUs = 0;
static volatile uint16_t rawCaptureLen = 0;
static volatile uint16_t rawCaptureData[RAW_BUFFER_LENGTH] = {};
static volatile bool rawCaptureOverflow = false;
static constexpr uint32_t RAW_CAPTURE_GAP_US = 18000;
static constexpr uint32_t RAW_CAPTURE_MIN_EDGES = 24;
static constexpr uint8_t IR_SEND_REPEATS = 4;
static String listStatus;
static uint32_t listStatusUntilMs = 0;

void IRAM_ATTR onIrRxEdge() {
    uint32_t nowUs = micros();
    if (!rawCaptureActive) {
        rawCaptureActive = true;
        rawCaptureLen = 0;
        rawLastEdgeUs = nowUs;
        rawLastActivityUs = nowUs;
        rawCaptureOverflow = false;
        return;
    }

    uint32_t delta = nowUs - rawLastEdgeUs;
    rawLastEdgeUs = nowUs;
    rawLastActivityUs = nowUs;
    if (rawCaptureLen < RAW_BUFFER_LENGTH) {
        rawCaptureData[rawCaptureLen++] = static_cast<uint16_t>(delta > 65535 ? 65535 : delta);
    } else {
        rawCaptureOverflow = true;
    }
}

const char* protocolName(uint8_t protocol) {
    switch (protocol) {
        case NEC: return "NEC";
        case SONY: return "SONY";
        case SAMSUNG: return "SAMSUNG";
        case LG: return "LG";
        case JVC: return "JVC";
        case PANASONIC: return "PANASONIC";
        default: return "RAW";
    }
}

const char* kindName(const IrCode& code) {
    return code.kind == IrCodeKind::Raw ? "RAW" : protocolName(code.protocol);
}

void logSend(const IrCode& code, const char* source) {
    Serial.printf("[IR] %s kind=%s", source, kindName(code));
    if (code.kind == IrCodeKind::Raw) {
        Serial.printf(" freq=%ukHz pulses=%u repeats=%u\n", code.frequency, code.rawLen, IR_SEND_REPEATS);
    } else {
        Serial.printf(" proto=%s addr=%u cmd=%u bits=%u repeats=%u\n",
                      protocolName(code.protocol), code.address, code.command, code.bits, IR_SEND_REPEATS);
    }
}

bool canReplayProtocol(uint8_t protocol) {
    switch (protocol) {
        case NEC:
        case SONY:
        case SAMSUNG:
        case LG:
        case JVC:
        case PANASONIC:
            return true;
        default:
            return false;
    }
}

void saveCodes() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < codeCount; ++i) {
        JsonObject obj = arr.add<JsonObject>();
        obj["name"] = codes[i].name;
        obj["kind"] = codes[i].kind == IrCodeKind::Raw ? "raw" : "decoded";
        obj["protocol"] = codes[i].protocol;
        obj["address"] = codes[i].address;
        obj["command"] = codes[i].command;
        obj["bits"] = codes[i].bits;
        obj["frequency"] = codes[i].frequency;
        JsonArray raw = obj["rawData"].to<JsonArray>();
        for (uint16_t j = 0; j < codes[i].rawLen; ++j) raw.add(codes[i].rawData[j]);
    }
    File f = LittleFS.open(IR_CODES_PATH, "w");
    if (!f) return;
    serializeJson(doc, f);
    f.close();
}

void loadCodes() {
    codeCount = 0;
    File f = LittleFS.open(IR_CODES_PATH, "r");
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f) != DeserializationError::Ok) {
        f.close();
        return;
    }
    f.close();
    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject obj : arr) {
        if (codeCount >= MAX_IR_CODES) break;
        IrCode& code = codes[codeCount];
        code = {};
        code.name = String((const char*)(obj["name"] | "Unnamed"));
        String kind = String((const char*)(obj["kind"] | "decoded"));
        code.kind = kind.equalsIgnoreCase("raw") ? IrCodeKind::Raw : IrCodeKind::Decoded;
        code.protocol = obj["protocol"] | 0;
        code.address = obj["address"] | 0;
        code.command = obj["command"] | 0;
        code.bits = obj["bits"] | 0;
        code.frequency = obj["frequency"] | 38;
        if (code.frequency > 1000) code.frequency /= 1000;
        JsonArray raw = obj["rawData"].as<JsonArray>();
        for (JsonVariant v : raw) {
            if (code.rawLen >= RAW_BUFFER_LENGTH) break;
            code.rawData[code.rawLen++] = v.as<uint16_t>();
        }
        ++codeCount;
    }
}

void drawHeader(const char* title) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_STATUS_FG, C_STATUS_BG);
    d.setCursor(2, 3);
    d.print(title);
    drawBatteryWidget(C_STATUS_BG);
}

void drawList() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawHeader("IR Remote");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    int y = STATUS_H + 4;
    if (codeCount == 0) {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(4, y);
        d.print("No saved IR codes yet.");
        d.setCursor(4, y + 12);
        d.print("Tab=learn  Enter=send");
    } else {
        int start = (selected >= 9) ? selected - 8 : 0;
        for (int i = 0; i < 9 && i + start < codeCount; ++i) {
            int idx = i + start;
            bool sel = idx == selected;
            uint32_t bg = sel ? C_HIGHLIGHT : C_BG;
            d.fillRect(0, y - 1, SCREEN_W, FONT_H + 2, bg);
            d.setTextColor(sel ? C_INPUT : C_FG, bg);
            d.setCursor(2, y);
            char line[40];
            snprintf(line, sizeof(line), "%-18s %s", codes[idx].name.c_str(), kindName(codes[idx]));
            d.print(line);
            y += FONT_H + 3;
        }
    }
    d.setTextColor(C_DIM, C_BG);
    d.fillRect(0, SCREEN_H - 12, SCREEN_W, 12, C_BG);
    if (listStatus.length() > 0 && millis() < listStatusUntilMs) {
        d.setTextColor(C_ACCENT, C_BG);
        d.setCursor(2, SCREEN_H - FONT_H - 2);
        d.print(listStatus);
    } else {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(2, SCREEN_H - FONT_H - 2);
        d.print("Ent=send Tab=learn fn+D/T");
    }
}

void drawLearnWait() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawHeader("IR Learn");
    d.setTextColor(C_FG, C_BG);
    d.setCursor(12, 24);
    d.print("Point remote at receiver");
    d.setCursor(12, 36);
    d.print("and press a button.");
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(12, 52);
    d.print("Receiver OUT -> GPIO2");
    d.setCursor(12, 64);
    d.print("Built-in IR sends on G44");
    d.fillRect(0, 78, SCREEN_W, 40, C_BG);
    d.setCursor(12, 80);
    d.printf("RX level: %s", lastRxLevel ? "HIGH" : "LOW ");
    d.setCursor(12, 92);
    d.printf("RX edges: %lu", static_cast<unsigned long>(rxEdgeCount));
    d.setTextColor(C_ACCENT, C_BG);
    d.setCursor(12, 108);
    d.print(learnStatus);
    d.setCursor(12, SCREEN_H - FONT_H - 4);
    d.print("Tab/Enter/fn+bksp=cancel");
}

void drawLearnOverlay() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 78, SCREEN_W, 40, C_BG);
    d.setTextColor(C_FG, C_BG);
    d.setCursor(12, 80);
    d.printf("RX level: %s", lastRxLevel ? "HIGH" : "LOW ");
    d.setCursor(12, 92);
    d.printf("RX edges: %lu", static_cast<unsigned long>(rxEdgeCount));
    d.setTextColor(C_ACCENT, C_BG);
    d.setCursor(12, 108);
    d.print(learnStatus);
}

void drawLearnName() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawHeader("Save IR Code");
    d.setTextColor(C_FG, C_BG);
    d.setCursor(8, 24);
    d.print("Captured:");
    d.setCursor(8, 36);
    if (pendingCode.kind == IrCodeKind::Raw) {
        d.printf("RAW %u pulses", pendingCode.rawLen);
        d.setCursor(8, 48);
        d.printf("%u kHz", pendingCode.frequency);
    } else {
        d.print(protocolName(pendingCode.protocol));
        d.setCursor(8, 48);
        d.printf("Addr:%u Cmd:%u", pendingCode.address, pendingCode.command);
    }
    d.setCursor(8, 68);
    d.print("Name:");
    d.fillRect(8, 80, SCREEN_W - 16, 12, 0x101010);
    d.setCursor(10, 82);
    d.print(learnName);
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(8, SCREEN_H - FONT_H - 8);
    d.print("Enter=save  del=backspace");
}

void drawDeleteConfirm() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawHeader("Delete IR Code");
    d.setTextColor(C_ERROR, C_BG);
    d.setCursor(8, 30);
    d.print("Delete this code?");
    d.setTextColor(C_FG, C_BG);
    d.setCursor(8, 44);
    d.print(codes[deleteTarget].name);
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(8, 64);
    d.print("Enter=YES fn+bksp=No");
}

void sendCode(const IrCode& code) {
    logSend(code, "send");
    if (code.kind == IrCodeKind::Raw) {
        if (code.rawLen > 0) {
            uint8_t sendKhz = static_cast<uint8_t>(code.frequency > 0 ? code.frequency : 38);
            for (uint8_t i = 0; i <= IR_SEND_REPEATS; ++i) {
                IrSender.sendRaw(code.rawData, code.rawLen, sendKhz);
                delay(60);
            }
        }
        return;
    }
    switch (code.protocol) {
        case NEC: IrSender.sendNEC(code.address, code.command, IR_SEND_REPEATS); break;
        case SONY: IrSender.sendSony(code.address, code.command, IR_SEND_REPEATS, code.bits ? code.bits : 12); break;
        case SAMSUNG: IrSender.sendSamsung(code.address, code.command, IR_SEND_REPEATS); break;
        case LG: IrSender.sendLG(code.address, code.command, IR_SEND_REPEATS); break;
        case JVC: IrSender.sendJVC((uint8_t)code.address, (uint8_t)code.command, IR_SEND_REPEATS); break;
        case PANASONIC: IrSender.sendPanasonic(code.address, code.command, IR_SEND_REPEATS); break;
        default: break;
    }
}

void sendTestNec() {
    Serial.printf("[IR] test NEC addr=0x1111 cmd=0x34 repeats=%u\n", IR_SEND_REPEATS);
    IrSender.sendNEC(0x1111, 0x34, IR_SEND_REPEATS);
}

void showListStatus(const String& status, uint32_t durationMs = 1200) {
    listStatus = status;
    listStatusUntilMs = millis() + durationMs;
    if (scene == IrScene::LIST) dirty = true;
}

void resetRawCapture() {
    detachInterrupt(digitalPinToInterrupt(IR_RX_PIN));
    rawCaptureActive = false;
    rawCaptureLen = 0;
    rawLastEdgeUs = 0;
    rawLastActivityUs = 0;
    rawCaptureOverflow = false;
}

void armRawCapture() {
    resetRawCapture();
    attachInterrupt(digitalPinToInterrupt(IR_RX_PIN), onIrRxEdge, CHANGE);
}

void finalizeRawCapture() {
    detachInterrupt(digitalPinToInterrupt(IR_RX_PIN));

    uint16_t capturedLen = rawCaptureLen;
    bool overflowed = rawCaptureOverflow;
    if (capturedLen < RAW_CAPTURE_MIN_EDGES) {
        learnStatus = "Signal too short, try again";
        learnOverlayDirty = true;
        resetRawCapture();
        return;
    }

    pendingCode = {};
    pendingCode.kind = IrCodeKind::Raw;
    pendingCode.frequency = 38;
    pendingCode.rawLen = min<uint16_t>(capturedLen, RAW_BUFFER_LENGTH);
    for (uint16_t i = 0; i < pendingCode.rawLen; ++i) pendingCode.rawData[i] = rawCaptureData[i];
    learnName = "";
    learnStatus = overflowed
        ? String("Captured RAW: ") + String(pendingCode.rawLen) + " pulses (trimmed)"
        : String("Captured RAW: ") + String(pendingCode.rawLen) + " pulses";
    scene = IrScene::LEARN_NAME;
    dirty = true;
    resetRawCapture();
}

void updateRxMonitor() {
    bool level = digitalRead(IR_RX_PIN);
    if (level != lastRxLevel) {
        if (scene == IrScene::LEARN_WAIT) {
            if (rawCaptureActive) {
                learnStatus = String("Receiving IR burst... ") + String(rawCaptureLen);
                learnOverlayDirty = true;
            }
        }
        lastRxLevel = level;
        ++rxEdgeCount;
        if (scene == IrScene::LEARN_WAIT) learnOverlayDirty = true;
    }
    if (scene == IrScene::LEARN_WAIT && rawCaptureActive) {
        uint32_t nowUs = micros();
        if (nowUs - rawLastActivityUs > RAW_CAPTURE_GAP_US) {
            finalizeRawCapture();
            return;
        }
    }
    if (scene == IrScene::LEARN_WAIT && millis() - lastStatusMs > 250) {
        lastStatusMs = millis();
        learnOverlayDirty = true;
    }
}
}

void appIrRemoteEnter() {
    Serial.begin(115200);
    LittleFS.begin(false);
    loadCodes();
    if (selected >= codeCount) selected = codeCount > 0 ? codeCount - 1 : 0;
    scene = IrScene::LIST;
    dirty = true;
    learnStatus = "Waiting for IR signal...";
    lastRxLevel = true;
    rxEdgeCount = 0;
    lastStatusMs = 0;
    learnOverlayDirty = true;
    listStatus = "";
    listStatusUntilMs = 0;
    resetRawCapture();
    if (!irReady) {
        IrSender.begin(DISABLE_LED_FEEDBACK);
        IrSender.setSendPin(IR_TX_PIN);
        IrReceiver.begin(IR_RX_PIN, DISABLE_LED_FEEDBACK);
        irReady = true;
    }
    pinMode(IR_RX_PIN, INPUT_PULLUP);
    lastRxLevel = digitalRead(IR_RX_PIN);
}

void appIrRemoteLoop() {
    updateRxMonitor();

    if (scene == IrScene::LEARN_WAIT && learnOverlayDirty && !dirty) {
        drawLearnOverlay();
        learnOverlayDirty = false;
    }

    if (scene == IrScene::LIST && listStatus.length() > 0 && millis() >= listStatusUntilMs) {
        listStatus = "";
        dirty = true;
    }

    if (dirty) {
        switch (scene) {
            case IrScene::LIST: drawList(); break;
            case IrScene::LEARN_WAIT:
                drawLearnWait();
                learnOverlayDirty = false;
                break;
            case IrScene::LEARN_NAME: drawLearnName(); break;
            case IrScene::DELETE_CONFIRM: drawDeleteConfirm(); break;
        }
        dirty = false;
    }

    if (scene == IrScene::LEARN_WAIT && IrReceiver.decode()) {
        auto& data = IrReceiver.decodedIRData;
        if (data.protocol != UNKNOWN && canReplayProtocol(data.protocol)) {
            pendingCode = {};
            pendingCode.kind = IrCodeKind::Decoded;
            pendingCode.protocol = data.protocol;
            pendingCode.address = data.address;
            pendingCode.command = data.command;
            pendingCode.bits = data.numberOfBits;
            learnName = "";
            learnStatus = String("Captured: ") + protocolName(pendingCode.protocol);
            scene = IrScene::LEARN_NAME;
            dirty = true;
        } else if (data.protocol == UNKNOWN) {
            learnStatus = "Signal seen, decode failed";
            learnOverlayDirty = true;
        } else {
            learnStatus = String("Unsupported: ") + protocolName(data.protocol);
            learnOverlayDirty = true;
        }
        IrReceiver.resume();
    }

    auto ev = readKeys();
    if (!ev.changed) return;
    if (ev.back) {
        if (scene == IrScene::LIST) goHome();
        else { scene = IrScene::LIST; dirty = true; }
        return;
    }

    switch (scene) {
        case IrScene::LIST:
            if (ev.up && selected > 0) { selected--; dirty = true; }
            if (ev.down && selected < codeCount - 1) { selected++; dirty = true; }
            if (ev.tab) {
                scene = IrScene::LEARN_WAIT;
                learnStatus = "Waiting for IR signal...";
                rxEdgeCount = 0;
                lastRxLevel = digitalRead(IR_RX_PIN);
                armRawCapture();
                learnOverlayDirty = true;
                dirty = true;
            }
            if (ev.enter && codeCount > 0) {
                sendCode(codes[selected]);
                showListStatus(String("Sent: ") + codes[selected].name);
            }
            if (ev.fnKey) {
                for (char c : ev.chars) {
                    if ((c == 'd' || c == 'D') && codeCount > 0) {
                        deleteTarget = selected;
                        scene = IrScene::DELETE_CONFIRM;
                        dirty = true;
                    }
                    if (c == 't' || c == 'T') {
                        sendTestNec();
                        showListStatus("Sent NEC test");
                    }
                    if (c == 'q' || c == 'Q') goHome();
                }
            }
            break;

        case IrScene::LEARN_WAIT:
            if (ev.tab || ev.enter) {
                resetRawCapture();
                scene = IrScene::LIST;
                dirty = true;
                return;
            }
            if (ev.fnKey) {
                for (char c : ev.chars) {
                    if (c == 'q' || c == 'Q') {
                        resetRawCapture();
                        scene = IrScene::LIST;
                        dirty = true;
                    }
                }
            }
            break;

        case IrScene::LEARN_NAME:
            if (ev.del && learnName.length() > 0) {
                learnName.remove(learnName.length() - 1);
                dirty = true;
            }
            for (char c : ev.chars) {
                if (learnName.length() < 18) {
                    learnName += c;
                    dirty = true;
                }
            }
            if (ev.enter && learnName.length() > 0 && codeCount < MAX_IR_CODES) {
                pendingCode.name = learnName;
                codes[codeCount++] = pendingCode;
                selected = codeCount - 1;
                saveCodes();
                scene = IrScene::LIST;
                dirty = true;
            }
            break;

        case IrScene::DELETE_CONFIRM:
            if (ev.enter && deleteTarget >= 0 && deleteTarget < codeCount) {
                for (int i = deleteTarget; i < codeCount - 1; ++i) codes[i] = codes[i + 1];
                codeCount--;
                if (selected >= codeCount && selected > 0) selected--;
                saveCodes();
                scene = IrScene::LIST;
                dirty = true;
            }
            break;
    }
}
