#include "app_espnow.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <esp_now.h>
#include <string.h>

// ── Layout ────────────────────────────────────────────────────────────────────
static constexpr int INPUT_H    = 14;
static constexpr int LOG_LINE_H = 11;
static constexpr int LOG_AREA_H = SCREEN_H - STATUS_H - INPUT_H; // 108px
static constexpr int VISIBLE    = LOG_AREA_H / LOG_LINE_H;        // 9 lines

// ── Buffers ───────────────────────────────────────────────────────────────────
static constexpr int MAX_LOG  = 32;
static constexpr int MAX_MSG  = 80;
static constexpr int MAX_NAME = 9;

struct ChatMsg {
    char name[MAX_NAME];
    char text[MAX_MSG];
    bool mine;
};

static ChatMsg s_log[MAX_LOG];
static int     s_logHead  = 0;
static int     s_logCount = 0;
static int     s_scroll   = 0;   // 0 = newest, N = scrolled back N lines

static char s_input[MAX_MSG] = {};
static int  s_inputLen = 0;

// ── State ─────────────────────────────────────────────────────────────────────
static char s_myName[MAX_NAME] = {};
static bool s_inited    = false;
static bool s_initError = false;
static bool s_dirty     = true;

// Thread-safe single-slot receive queue
// The ESP-NOW recv callback runs in the WiFi task (separate from loop()).
static volatile bool s_msgPending = false;
static volatile char s_pendingName[MAX_NAME];
static volatile char s_pendingText[MAX_MSG];

// Peer tracking by display name (no MAC management needed)
static char s_seenNames[8][MAX_NAME];
static int  s_seenCount = 0;

// ── Packet ────────────────────────────────────────────────────────────────────
#pragma pack(push, 1)
struct EspNowPkt {
    uint8_t type;           // 'M' = message
    char    name[MAX_NAME];
    char    text[MAX_MSG];
};
#pragma pack(pop)

static const uint8_t BROADCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// ── Helpers ───────────────────────────────────────────────────────────────────
static void registerSender(const char* name) {
    for (int i = 0; i < s_seenCount; i++)
        if (strncmp(s_seenNames[i], name, MAX_NAME) == 0) return;
    if (s_seenCount < 8)
        strncpy(s_seenNames[s_seenCount++], name, MAX_NAME - 1);
}

static void pushMsg(const char* name, const char* text, bool mine) {
    int idx = (s_logHead + s_logCount) % MAX_LOG;
    if (s_logCount == MAX_LOG) {
        s_logHead = (s_logHead + 1) % MAX_LOG;
    } else {
        s_logCount++;
    }
    strncpy(s_log[idx].name, name, MAX_NAME - 1);
    s_log[idx].name[MAX_NAME - 1] = 0;
    strncpy(s_log[idx].text, text, MAX_MSG - 1);
    s_log[idx].text[MAX_MSG - 1] = 0;
    s_log[idx].mine = mine;
    s_scroll = 0;   // snap to newest
    s_dirty  = true;
}

// ── ESP-NOW receive callback (WiFi task context) ──────────────────────────────
static void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
    (void)mac;
    if (len < (int)(sizeof(uint8_t) + MAX_NAME) || s_msgPending) return;
    const EspNowPkt* pkt = (const EspNowPkt*)data;
    if (pkt->type != 'M') return;
    for (int i = 0; i < MAX_NAME; i++) s_pendingName[i] = pkt->name[i];
    for (int i = 0; i < MAX_MSG;  i++) s_pendingText[i] = pkt->text[i];
    s_msgPending = true;
}

// ── Init / deinit ─────────────────────────────────────────────────────────────
static bool initEspNow() {
    if (s_inited) return true;

    if (WiFi.getMode() == WIFI_OFF) {
        WiFi.mode(WIFI_STA);
        delay(80);
    }

    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(s_myName, sizeof(s_myName), "CP%02X%02X", mac[4], mac[5]);

    if (esp_now_init() != ESP_OK) return false;
    esp_now_register_recv_cb(onRecv);

    if (!esp_now_is_peer_exist(BROADCAST)) {
        esp_now_peer_info_t p = {};
        memcpy(p.peer_addr, BROADCAST, 6);
        p.channel = 0;
        p.encrypt = false;
        esp_now_add_peer(&p);
    }

    s_inited = true;
    return true;
}

static void sendMsg(const char* text) {
    if (!s_inited || !text[0]) return;
    EspNowPkt pkt;
    pkt.type = 'M';
    strncpy(pkt.name, s_myName, MAX_NAME - 1);
    pkt.name[MAX_NAME - 1] = 0;
    strncpy(pkt.text, text, MAX_MSG - 1);
    pkt.text[MAX_MSG - 1] = 0;
    esp_now_send(BROADCAST, (uint8_t*)&pkt, sizeof(pkt));
    pushMsg(s_myName, text, true);
}

// ── Draw ──────────────────────────────────────────────────────────────────────
static void drawStatusBar() {
    auto& d = M5Cardputer.Display;
    constexpr uint32_t SBG = 0x001025;
    d.fillRect(0, 0, SCREEN_W, STATUS_H, SBG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(0x00AAFF, SBG);
    d.setCursor(2, 3);
    d.print("ESP-NOW");

    char buf[16];
    snprintf(buf, sizeof(buf), "near:%d", s_seenCount);
    d.setTextColor(s_seenCount > 0 ? (uint32_t)0x00FF88 : (uint32_t)0x444444, SBG);
    d.setCursor(74, 3);
    d.print(buf);

    d.setTextColor(0x336688, SBG);
    int nw = (int)strlen(s_myName) * FONT_W;
    d.setCursor(SCREEN_W - nw - 2, 3);
    d.print(s_myName);
}

static void drawLog() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, STATUS_H, SCREEN_W, LOG_AREA_H, C_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    if (s_logCount == 0) {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(4, STATUS_H + 18); d.print("No messages yet.");
        d.setCursor(4, STATUS_H + 32); d.print("Type a message, press Enter.");
        d.setCursor(4, STATUS_H + 48); d.print("Works with any ESP32 running");
        d.setCursor(4, STATUS_H + 62); d.print("ESP-NOW — no router needed.");
        return;
    }

    int start = s_logCount - VISIBLE - s_scroll;
    if (start < 0) start = 0;
    int end = start + VISIBLE;
    if (end > s_logCount) end = s_logCount;

    for (int i = start; i < end; i++) {
        int di = (s_logHead + i) % MAX_LOG;
        const ChatMsg& m = s_log[di];
        int y = STATUS_H + (i - start) * LOG_LINE_H + 1;

        int textX;
        if (m.mine) {
            d.setTextColor(0x0077BB, C_BG);
            d.setCursor(2, y); d.print(">");
            d.setTextColor(0x55CCFF, C_BG);
            textX = 2 + 2 * FONT_W;
        } else {
            d.setTextColor(0xFFAA00, C_BG);
            d.setCursor(2, y);
            d.print(m.name); d.print(":");
            d.setTextColor(0xFFCC88, C_BG);
            textX = 2 + ((int)strlen(m.name) + 1) * FONT_W + 2;
        }

        int avail = (SCREEN_W - textX) / FONT_W;
        if (avail < 1) avail = 1;
        if (avail > MAX_MSG - 1) avail = MAX_MSG - 1;
        char trunc[MAX_MSG];
        strncpy(trunc, m.text, avail);
        trunc[avail] = 0;
        d.setCursor(textX, y);
        d.print(trunc);
    }

    if (s_scroll > 0) {
        d.setTextColor(0x444444, C_BG);
        char sb[8];
        snprintf(sb, sizeof(sb), "^%d", s_scroll);
        d.setCursor(SCREEN_W - (int)strlen(sb) * FONT_W - 2, STATUS_H + 1);
        d.print(sb);
    }
}

static void drawInputLine() {
    auto& d = M5Cardputer.Display;
    int y = SCREEN_H - INPUT_H;
    constexpr uint32_t IBG = 0x080C18;
    d.fillRect(0, y, SCREEN_W, INPUT_H, IBG);
    d.drawFastHLine(0, y, SCREEN_W, 0x002244);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(0x004488, IBG);
    d.setCursor(2, y + 3);
    d.print(">");
    d.setTextColor(0xFFFFFF, IBG);
    int avail = (SCREEN_W - 14) / FONT_W;
    int start = s_inputLen > avail ? s_inputLen - avail : 0;
    d.setCursor(10, y + 3);
    d.print(s_input + start);
    int cx = 10 + (s_inputLen - start) * FONT_W;
    if (cx < SCREEN_W - 2) d.fillRect(cx, y + 2, 2, INPUT_H - 4, 0x0088FF);
}

static void drawAll() {
    drawStatusBar();
    drawLog();
    drawInputLine();
}

static void drawError() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, STATUS_H, SCREEN_W, SCREEN_H - STATUS_H, C_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_ERROR, C_BG);
    d.setCursor(8, 44); d.print("ESP-NOW init failed!");
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(8, 60); d.print("Enable WiFi in Settings,");
    d.setCursor(8, 72); d.print("then retry.");
    d.setCursor(8, 92); d.print("Press any key...");
}

// ── Public ────────────────────────────────────────────────────────────────────
void appEspnowEnter() {
    s_dirty      = true;
    s_initError  = false;
    s_scroll     = 0;
    s_msgPending = false;

    if (!s_inited) {
        if (!initEspNow()) {
            s_initError = true;
        } else {
            pushMsg("sys", "Ready. Type to broadcast to all nearby ESP-NOW devices.", false);
        }
    }
}

void appEspnowLoop() {
    auto ev = readKeys();

    if (s_initError) {
        if (s_dirty) { drawStatusBar(); drawError(); s_dirty = false; }
        if (ev.changed) { s_initError = false; goHome(); }
        return;
    }

    // Consume pending incoming message (safe copy from volatile buffer)
    if (s_msgPending) {
        char name[MAX_NAME], text[MAX_MSG];
        for (int i = 0; i < MAX_NAME; i++) name[i] = (char)s_pendingName[i];
        for (int i = 0; i < MAX_MSG;  i++) text[i] = (char)s_pendingText[i];
        s_msgPending = false;
        registerSender(name);
        pushMsg(name, text, false);
    }

    if (ev.changed) {
        if (ev.back) {
            if (s_inited) { esp_now_deinit(); s_inited = false; s_seenCount = 0; }
            goHome();
            return;
        }
        if (ev.enter) {
            if (s_inputLen > 0) {
                sendMsg(s_input);
                s_input[0] = 0;
                s_inputLen = 0;
                s_dirty = true;
            }
        } else if (ev.up) {
            int maxScroll = s_logCount > VISIBLE ? s_logCount - VISIBLE : 0;
            if (s_scroll < maxScroll) { s_scroll++; s_dirty = true; }
        } else if (ev.down) {
            if (s_scroll > 0) { s_scroll--; s_dirty = true; }
        } else {
            for (char c : ev.chars) {
                if (c == 8 || c == 127) {
                    if (s_inputLen > 0) { s_input[--s_inputLen] = 0; s_dirty = true; }
                } else if (c >= 32 && c < 127 && s_inputLen < MAX_MSG - 1) {
                    s_input[s_inputLen++] = c;
                    s_input[s_inputLen]   = 0;
                    s_dirty = true;
                }
            }
        }
    }

    if (s_dirty) { drawAll(); s_dirty = false; }
}
