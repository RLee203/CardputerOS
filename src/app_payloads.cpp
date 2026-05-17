#include "app_payloads.h"
#include "app_hid_keyboard.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>
#include <SD.h>

// Mirror USBHIDKeyboard constants without the include conflict
static constexpr uint8_t HID_RETURN    = 0xB0;
static constexpr uint8_t HID_ESC       = 0xB1;
static constexpr uint8_t HID_BACKSPACE = 0xB2;
static constexpr uint8_t HID_TAB       = 0xB3;
static constexpr uint8_t HID_DELETE    = 0xD4;
static constexpr uint8_t HID_HOME      = 0xD2;
static constexpr uint8_t HID_END       = 0xD5;
static constexpr uint8_t HID_INSERT    = 0xD1;
static constexpr uint8_t HID_PAGE_UP   = 0xD3;
static constexpr uint8_t HID_PAGE_DOWN = 0xD6;
static constexpr uint8_t HID_UP        = 0xDA;
static constexpr uint8_t HID_DOWN      = 0xD9;
static constexpr uint8_t HID_LEFT      = 0xD8;
static constexpr uint8_t HID_RIGHT     = 0xD7;
static constexpr uint8_t HID_F1        = 0xC2;
static constexpr uint8_t HID_CAPS_LOCK = 0xC1;
static constexpr uint8_t HID_CTRL      = 0x80;
static constexpr uint8_t HID_SHIFT     = 0x81;
static constexpr uint8_t HID_ALT       = 0x82;
static constexpr uint8_t HID_GUI       = 0x83;

static constexpr int MAX_FILES = 24;
static constexpr int ROWS_VIS  = 10;

namespace {

enum class PayState { FILE_LIST, CONFIRM, RUNNING };

PayState s_state    = PayState::FILE_LIST;
bool     s_dirty    = true;
char     s_files[MAX_FILES][33];
int      s_fileCount = 0;
int      s_sel       = 0;
int      s_scroll    = 0;
bool     s_sdOk      = false;

void drawStatusBar() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_STATUS_FG, C_STATUS_BG);
    d.setCursor(2, 3);
    d.print("Payloads");
    drawBatteryWidget(C_STATUS_BG, SCREEN_W - 43);
}

void loadFileList() {
    s_fileCount = 0;
    digitalWrite(LORA_NSS_PIN, HIGH);
    digitalWrite(LORA_RST_PIN, LOW);
    s_sdOk = SD.begin(SD_CS_PIN, SPI, 25000000);
    if (!s_sdOk) return;
    if (!SD.exists("/payloads")) { SD.mkdir("/payloads"); return; }
    File dir = SD.open("/payloads");
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f && s_fileCount < MAX_FILES) {
        if (!f.isDirectory()) {
            const char* name = f.name();
            int len = strlen(name);
            bool isTxt = len > 4 && strcasecmp(name + len - 4, ".txt") == 0;
            bool isDs  = len > 3 && strcasecmp(name + len - 3, ".ds")  == 0;
            if (isTxt || isDs) {
                strncpy(s_files[s_fileCount], name, 32);
                s_files[s_fileCount][32] = '\0';
                s_fileCount++;
            }
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
}

void drawFileList() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar();
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    if (!s_sdOk) {
        d.setTextColor(C_ERROR, C_BG);
        d.setCursor(14, 50); d.print("SD card not found");
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(14, 62); d.print("Enter=retry  fn+bksp=back");
        return;
    }
    if (s_fileCount == 0) {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(14, 46); d.print("No payloads in /payloads/");
        d.setCursor(14, 58); d.print("Add DuckyScript .txt or .ds");
        d.setCursor(14, 70); d.print("to SD card and press Enter");
        d.setCursor(2, SCREEN_H - FONT_H - 1); d.print("Enter=refresh  fn+bksp=back");
        return;
    }

    for (int i = 0; i < ROWS_VIS && (s_scroll + i) < s_fileCount; i++) {
        int idx = s_scroll + i;
        int y   = STATUS_H + 2 + i * 11;
        bool sel = (idx == s_sel);
        d.fillRect(0, y, SCREEN_W, 11, sel ? C_HIGHLIGHT : C_BG);
        d.setTextColor(sel ? C_FG : C_DIM, sel ? C_HIGHLIGHT : C_BG);
        d.setCursor(4, y + 2);
        d.print(s_files[idx]);
    }

    d.fillRect(0, SCREEN_H - FONT_H - 2, SCREEN_W, FONT_H + 2, C_BG);
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 1);
    char footer[48];
    snprintf(footer, sizeof(footer), "%d files  Enter=select  fn+bksp=back", s_fileCount);
    d.print(footer);
}

void drawConfirm() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar();
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    d.setTextColor(C_FG, C_BG);
    d.setCursor(14, 22); d.print("Run payload:");
    d.setTextColor(C_ACCENT, C_BG);
    d.setCursor(14, 34); d.print(s_files[s_sel]);

    if (!hidKbIsEnabled()) {
        d.fillRoundRect(10, 52, 220, 32, 5, 0x1A1000);
        d.drawRoundRect(10, 52, 220, 32, 5, 0xCCAA00);
        d.setTextColor(0xFFDD55, 0x1A1000);
        d.setCursor(18, 60); d.print("USB HID not enabled.");
        d.setCursor(18, 70); d.print("Enter will enable + run.");
    } else {
        d.fillRoundRect(10, 52, 220, 24, 5, 0x001A00);
        d.drawRoundRect(10, 52, 220, 24, 5, 0x00AA00);
        d.setTextColor(0x00FF00, 0x001A00);
        d.setCursor(18, 60); d.print("USB HID active - ready to inject");
    }

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 1);
    d.print("Enter=run  fn+bksp=cancel");
}

// Map DuckyScript key name to HID keycode
static uint8_t duckKeyCode(const char* name) {
    if (!name || !*name) return 0;
    if (name[1] == '\0') {
        char c = name[0];
        if (c >= 'A' && c <= 'Z') c += 32;
        return (uint8_t)c;
    }
    if (strcasecmp(name, "ENTER")     == 0 || strcasecmp(name, "RETURN")    == 0) return HID_RETURN;
    if (strcasecmp(name, "ESCAPE")    == 0 || strcasecmp(name, "ESC")       == 0) return HID_ESC;
    if (strcasecmp(name, "BACKSPACE") == 0)                                        return HID_BACKSPACE;
    if (strcasecmp(name, "DELETE")    == 0 || strcasecmp(name, "DEL")       == 0) return HID_DELETE;
    if (strcasecmp(name, "TAB")       == 0)                                        return HID_TAB;
    if (strcasecmp(name, "UPARROW")   == 0 || strcasecmp(name, "UP")        == 0) return HID_UP;
    if (strcasecmp(name, "DOWNARROW") == 0 || strcasecmp(name, "DOWN")      == 0) return HID_DOWN;
    if (strcasecmp(name, "LEFTARROW") == 0 || strcasecmp(name, "LEFT")      == 0) return HID_LEFT;
    if (strcasecmp(name, "RIGHTARROW")== 0 || strcasecmp(name, "RIGHT")     == 0) return HID_RIGHT;
    if (strcasecmp(name, "HOME")      == 0)                                        return HID_HOME;
    if (strcasecmp(name, "END")       == 0)                                        return HID_END;
    if (strcasecmp(name, "INSERT")    == 0)                                        return HID_INSERT;
    if (strcasecmp(name, "PAGEUP")    == 0)                                        return HID_PAGE_UP;
    if (strcasecmp(name, "PAGEDOWN")  == 0)                                        return HID_PAGE_DOWN;
    if (strcasecmp(name, "SPACE")     == 0)                                        return ' ';
    if (strcasecmp(name, "CAPSLOCK")  == 0)                                        return HID_CAPS_LOCK;
    if ((name[0] == 'F' || name[0] == 'f') && isdigit((unsigned char)name[1])) {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 12) return (uint8_t)(HID_F1 + n - 1);
    }
    // fallback: first char lowercased
    char c = name[0];
    if (c >= 'A' && c <= 'Z') c += 32;
    return (uint8_t)c;
}

// Returns true if the line was a recognized + executed command
bool executeDuckLine(const char* line) {
    if (!line || !line[0]) return false;
    if (strncasecmp(line, "REM", 3) == 0 && (line[3] == ' ' || line[3] == '\0')) return false;

    if (strncasecmp(line, "STRING ", 7) == 0) {
        hidKbTypeString(line + 7); return true;
    }
    if (strncasecmp(line, "DELAY ", 6) == 0) {
        int ms = atoi(line + 6);
        if (ms > 0 && ms <= 30000) delay(ms);
        return true;
    }

    // Single-key commands
    struct { const char* cmd; uint8_t key; } singles[] = {
        {"ENTER",      HID_RETURN    }, {"RETURN",     HID_RETURN    },
        {"BACKSPACE",  HID_BACKSPACE }, {"ESCAPE",     HID_ESC       },
        {"ESC",        HID_ESC       }, {"TAB",        HID_TAB       },
        {"DELETE",     HID_DELETE    }, {"DEL",        HID_DELETE    },
        {"HOME",       HID_HOME      }, {"END",        HID_END       },
        {"INSERT",     HID_INSERT    }, {"PAGEUP",     HID_PAGE_UP   },
        {"PAGEDOWN",   HID_PAGE_DOWN }, {"SPACE",      ' '           },
        {"CAPSLOCK",   HID_CAPS_LOCK }, {"UPARROW",    HID_UP        },
        {"DOWNARROW",  HID_DOWN      }, {"LEFTARROW",  HID_LEFT      },
        {"RIGHTARROW", HID_RIGHT     },
    };
    for (auto& s : singles) {
        if (strcasecmp(line, s.cmd) == 0) { hidKbPressRelease(s.key); return true; }
    }

    // F1-F12 standalone
    if ((line[0] == 'F' || line[0] == 'f') && isdigit((unsigned char)line[1])) {
        int n = atoi(line + 1);
        if (n >= 1 && n <= 12 && (line[2] == '\0' || (line[2] == '\r'))) {
            hidKbPressRelease((uint8_t)(HID_F1 + n - 1)); return true;
        }
    }

    // Modifier combos — ordered longest-match first
    struct { const char* prefix; uint8_t m1; uint8_t m2; } combos[] = {
        {"CTRL-ALT-SHIFT ", HID_CTRL,  HID_ALT  },  // m2=ALT, SHIFT pressed via key
        {"CTRL-ALT ",       HID_CTRL,  HID_ALT  },
        {"CTRL-SHIFT ",     HID_CTRL,  HID_SHIFT},
        {"ALT-SHIFT ",      HID_ALT,   HID_SHIFT},
        {"CTRL ",           HID_CTRL,  0        },
        {"ALT ",            HID_ALT,   0        },
        {"SHIFT ",          HID_SHIFT, 0        },
        {"GUI ",            HID_GUI,   0        },
        {"WINDOWS ",        HID_GUI,   0        },
        {"COMMAND ",        HID_GUI,   0        },
    };
    for (auto& c : combos) {
        int plen = strlen(c.prefix);
        if (strncasecmp(line, c.prefix, plen) == 0) {
            uint8_t key = duckKeyCode(line + plen);
            if (c.m2) hidKbMod2Key(c.m1, c.m2, key);
            else      hidKbModKey(c.m1, key);
            return true;
        }
    }

    return false;
}

void runPayload() {
    char path[64];
    snprintf(path, sizeof(path), "/payloads/%s", s_files[s_sel]);

    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar();
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_FG, C_BG);
    d.setCursor(14, 22); d.print("Running:");
    d.setTextColor(C_ACCENT, C_BG);
    d.setCursor(14, 34); d.print(s_files[s_sel]);

    hidKbEnsureEnabled();
    delay(500);

    digitalWrite(LORA_NSS_PIN, HIGH);
    digitalWrite(LORA_RST_PIN, LOW);
    SD.begin(SD_CS_PIN, SPI, 25000000);
    File f = SD.open(path, FILE_READ);
    if (!f) {
        d.setTextColor(C_ERROR, C_BG);
        d.setCursor(14, 52); d.print("Failed to open file");
        delay(2000);
        return;
    }

    char lineBuf[256];
    char lastExec[256] = {};
    int  lineNum = 0;

    while (f.available()) {
        int i = 0;
        while (f.available() && i < (int)sizeof(lineBuf) - 1) {
            char c = (char)f.read();
            if (c == '\n') break;
            if (c != '\r') lineBuf[i++] = c;
        }
        lineBuf[i] = '\0';
        lineNum++;

        if (strncasecmp(lineBuf, "REPEAT ", 7) == 0 && lastExec[0]) {
            int n = atoi(lineBuf + 7);
            for (int r = 0; r < n && r < 500; r++) executeDuckLine(lastExec);
        } else {
            if (executeDuckLine(lineBuf)) strncpy(lastExec, lineBuf, sizeof(lastExec) - 1);
        }

        if (lineNum % 5 == 0) {
            d.fillRect(0, 50, SCREEN_W, FONT_H + 2, C_BG);
            d.setTextColor(C_DIM, C_BG);
            char prog[24];
            snprintf(prog, sizeof(prog), "Line %d...", lineNum);
            d.setCursor(14, 52); d.print(prog);
        }
    }
    f.close();

    d.fillRect(0, 50, SCREEN_W, 30, C_BG);
    d.setTextColor(0x00FF00, C_BG);
    d.setCursor(14, 52); d.print("Done!");
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(14, 64); d.print("Press any key");
    while (!readKeys().changed) {}
}

} // namespace

void appPayloadsEnter() {
    s_state    = PayState::FILE_LIST;
    s_dirty    = true;
    s_sel      = 0;
    s_scroll   = 0;
    loadFileList();
}

void appPayloadsLoop() {
    if (s_state == PayState::RUNNING) {
        runPayload();
        s_state = PayState::FILE_LIST;
        s_dirty = true;
        return;
    }

    if (s_dirty) {
        switch (s_state) {
            case PayState::FILE_LIST: drawFileList(); break;
            case PayState::CONFIRM:   drawConfirm();  break;
            default: break;
        }
        s_dirty = false;
    }

    auto ev = readKeys();
    if (!ev.changed) return;

    if (ev.back) {
        if (s_state == PayState::CONFIRM) { s_state = PayState::FILE_LIST; s_dirty = true; }
        else goHome();
        return;
    }

    switch (s_state) {
        case PayState::FILE_LIST:
            if (ev.up && s_sel > 0) {
                s_sel--;
                if (s_sel < s_scroll) s_scroll = s_sel;
                s_dirty = true;
            }
            if (ev.down && s_sel < s_fileCount - 1) {
                s_sel++;
                if (s_sel >= s_scroll + ROWS_VIS) s_scroll = s_sel - ROWS_VIS + 1;
                s_dirty = true;
            }
            if (ev.enter) {
                if (s_fileCount == 0 || !s_sdOk) { loadFileList(); s_dirty = true; }
                else                               { s_state = PayState::CONFIRM; s_dirty = true; }
            }
            break;

        case PayState::CONFIRM:
            if (ev.enter) { s_state = PayState::RUNNING; s_dirty = true; }
            break;

        default:
            break;
    }
}
