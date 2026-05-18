#include "app_detector.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include "esp_wifi.h"
#include "config.h"
#include "input.h"
#include "nav.h"

// ── Helpers ────────────────────────────────────────────────────────────────

static bool ciContains(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return false;
    size_t nl = strlen(needle), hl = strlen(hay);
    if (nl > hl) return false;
    for (size_t i = 0; i <= hl - nl; i++) {
        bool ok = true;
        for (size_t j = 0; j < nl; j++)
            if (tolower((unsigned char)hay[i+j]) != tolower((unsigned char)needle[j])) { ok=false; break; }
        if (ok) return true;
    }
    return false;
}

// ── State ──────────────────────────────────────────────────────────────────

enum class DetState { MAIN_MENU, THREAT_DONE, WIFI_DONE, DEAUTH_MON, PROBE_SNIFF };
static DetState  s_state    = DetState::MAIN_MENU;
static bool      s_dirty    = true;
static int       s_menuSel  = 0;
static int       s_scroll   = 0;
static uint32_t  s_lastRedraw = 0;
static volatile bool s_bleScanDone = false;

// ── Threat scan ────────────────────────────────────────────────────────────

struct Threat { char source[6]; char label[24]; char name[28]; int rssi; uint8_t level; };
static constexpr int MAX_THREATS = 32;
static Threat s_threats[MAX_THREATS];
static int    s_tCount = 0;

struct TPat { const char* match; const char* label; uint8_t level; };
static const TPat BLE_PATS[] = {
    { "flipper",     "Flipper Zero",       2 },
    { "pwnagotch",   "Pwnagotchi",         2 },
    { "hc-05",       "Skimmer (HC-05)",    2 },
    { "hc-08",       "Skimmer (HC-08)",    2 },
    { "hc05",        "Skimmer (HC-05)",    2 },
    { "hc08",        "Skimmer (HC-08)",    2 },
    { "msr",         "Mag-Stripe Reader",  2 },
    { "skimmer",     "Card Skimmer",       2 },
    { "deep insert",  "Deep Skimmer",      2 },
    { "omg",         "O.MG Cable",         2 },
    { "rnbt",        "RN-BT Module",       1 },
    { "cc2541",      "BLE Skimmer Chip",   1 },
    { "at-09",       "BLE Skimmer Chip",   1 },
    { "jdy-",        "BLE Module",         1 },
    { "dsd tech",    "BLE Module",         1 },
};
static constexpr int N_BLE_PATS = (int)(sizeof(BLE_PATS)/sizeof(BLE_PATS[0]));

static const TPat WIFI_PATS[] = {
    { "flipper",      "Flipper Zero WiFi",  2 },
    { "pwnagotch",    "Pwnagotchi AP",      2 },
    { "omg",          "O.MG Cable",         2 },
    { "o.mg",         "O.MG Cable",         2 },
    { "wifi duck",    "WiFi Duck",          2 },
    { "wifiduck",     "WiFi Duck",          2 },
    { "rubber duck",  "Rubber Ducky",       2 },
    { "evilportal",   "Evil Portal",        2 },
    { "evil twin",    "Evil Twin AP",       2 },
    { "karma",        "KARMA Attack",       2 },
    { "deauth",       "Deauth Tool",        2 },
    { "marauder",     "ESP Marauder",       1 },
    { "pwn",          "Pwn Device",         1 },
    { "hackrf",       "HackRF",             1 },
};
static constexpr int N_WIFI_PATS = (int)(sizeof(WIFI_PATS)/sizeof(WIFI_PATS[0]));

static void addThreat(const char* src, const char* label, const char* name, int rssi, uint8_t lvl) {
    if (s_tCount >= MAX_THREATS) return;
    auto& t = s_threats[s_tCount++];
    strncpy(t.source, src,   sizeof(t.source)-1); t.source[sizeof(t.source)-1]='\0';
    strncpy(t.label,  label, sizeof(t.label)-1);  t.label[sizeof(t.label)-1]='\0';
    strncpy(t.name,   name,  sizeof(t.name)-1);   t.name[sizeof(t.name)-1]='\0';
    t.rssi=rssi; t.level=lvl;
}

class ThreatBleCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        const char* name = dev->haveName() ? dev->getName().c_str() : "";
        int rssi = dev->getRSSI();
        for (int p = 0; p < N_BLE_PATS; p++)
            if (ciContains(name, BLE_PATS[p].match)) { addThreat("BLE", BLE_PATS[p].label, name, rssi, BLE_PATS[p].level); return; }
        if ((!name || !name[0]) && rssi >= -60)
            addThreat("BLE", "Unknown (high RSSI)", "(no name)", rssi, 1);
    }
};
static ThreatBleCallbacks s_threatBleCallbacks;
static bool     s_bleInited = false;
static NimBLEScan* s_bleScan   = nullptr;

static void onThreatBleScanDone(NimBLEScanResults) {
    s_bleScanDone = true;
}

static void shutdownThreatBle() {
    if (s_bleScan && s_bleScan->isScanning()) {
        s_bleScan->stop();
    }
    if (s_bleInited) {
        NimBLEDevice::deinit(true);
        s_bleScan = nullptr;
        s_bleInited = false;
    }
}

static void runThreatScan() {
    s_tCount = 0;
    s_bleScanDone = false;
    // WiFi
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG); d.setFont(&fonts::Font0); d.setTextSize(2);
    d.setTextColor(C_FG, C_BG);
    d.setCursor(20, 50); d.print("Scanning WiFi...");
    WiFi.mode(WIFI_STA); WiFi.disconnect(false, false); delay(100);
    int n = WiFi.scanNetworks(false, true);
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i); int rssi = WiFi.RSSI(i);
        for (int p = 0; p < N_WIFI_PATS; p++)
            if (ciContains(ssid.c_str(), WIFI_PATS[p].match)) { addThreat("WiFi", WIFI_PATS[p].label, ssid.c_str(), rssi, WIFI_PATS[p].level); break; }
    }
    WiFi.scanDelete(); WiFi.mode(WIFI_OFF);
    // BLE
    d.setCursor(20, 80); d.print("Scanning BLE...");
    WiFi.disconnect(true, true);
    delay(80);
    WiFi.mode(WIFI_OFF);
    delay(180);
    if (!s_bleInited) { NimBLEDevice::init(""); s_bleInited = true; }
    if (!s_bleScan) {
        s_bleScan = NimBLEDevice::getScan();
        s_bleScan->setActiveScan(true); s_bleScan->setInterval(100); s_bleScan->setWindow(99);
    }
    s_bleScan->setAdvertisedDeviceCallbacks(&s_threatBleCallbacks);
    s_bleScan->clearResults();
    s_bleScan->start(3, onThreatBleScanDone, false);
}

// ── WiFi Analyzer ──────────────────────────────────────────────────────────

struct ApInfo {
    char ssid[33]; char bssid[18];
    int rssi; uint8_t ch;
    char enc[7];
    bool flagOpen; bool flagTwin;
};
static constexpr int MAX_APS = 32;
static ApInfo s_aps[MAX_APS];
static int    s_apCount = 0;

static const char* encStr(wifi_auth_mode_t m) {
    switch (m) {
        case WIFI_AUTH_OPEN:          return "Open";
        case WIFI_AUTH_WEP:           return "WEP";
        case WIFI_AUTH_WPA_PSK:       return "WPA";
        case WIFI_AUTH_WPA2_PSK:      return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA/2";
        case WIFI_AUTH_WPA3_PSK:      return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WP2/3";
        default:                       return "?";
    }
}

static void runWifiAnalyzer() {
    s_apCount = 0;
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG); d.setFont(&fonts::Font0); d.setTextSize(2);
    d.setTextColor(C_FG, C_BG);
    d.setCursor(20, 55); d.print("Scanning APs...");
    WiFi.mode(WIFI_STA); WiFi.disconnect(true); delay(100);
    int n = WiFi.scanNetworks(false, true);
    int stored = min(n, MAX_APS);
    for (int i = 0; i < stored; i++) {
        auto& a = s_aps[s_apCount++];
        String ssid = WiFi.SSID(i);
        strncpy(a.ssid, ssid.c_str(), 32); a.ssid[32]='\0';
        String bssid = WiFi.BSSIDstr(i);
        strncpy(a.bssid, bssid.c_str(), 17); a.bssid[17]='\0';
        a.rssi = WiFi.RSSI(i);
        a.ch   = (uint8_t)WiFi.channel(i);
        strncpy(a.enc, encStr(WiFi.encryptionType(i)), 6); a.enc[6]='\0';
        a.flagOpen = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
        a.flagTwin = false;
    }
    // Evil twin detection: same SSID, different BSSID
    for (int i = 0; i < s_apCount; i++)
        for (int j = i+1; j < s_apCount; j++)
            if (strcmp(s_aps[i].ssid, s_aps[j].ssid) == 0)
                { s_aps[i].flagTwin = true; s_aps[j].flagTwin = true; }
    WiFi.scanDelete(); WiFi.mode(WIFI_OFF);
    // Sort by RSSI descending (bubble)
    for (int i = 0; i < s_apCount-1; i++)
        for (int j = 0; j < s_apCount-1-i; j++)
            if (s_aps[j].rssi < s_aps[j+1].rssi) { ApInfo tmp=s_aps[j]; s_aps[j]=s_aps[j+1]; s_aps[j+1]=tmp; }
}

// ── Promiscuous mode (shared Deauth Monitor + Probe Sniffer) ──────────────

struct DeauthEntry { uint8_t bssid[6]; int count; int rssi; };
static constexpr int MAX_DEAUTHS = 16;
static DeauthEntry s_deauths[MAX_DEAUTHS];
static volatile int s_dCount = 0;

struct ProbeEntry { char ssid[33]; int count; int rssi; };
static constexpr int MAX_PROBES = 20;
static ProbeEntry s_probes[MAX_PROBES];
static volatile int s_pCount = 0;

static volatile uint32_t s_monFrames = 0;
static uint8_t  s_monCh     = 6;
static uint8_t  s_promiscMode = 0;  // 1=deauth  2=probe

static void IRAM_ATTR promiscCb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t* d = pkt->payload;
    uint32_t len = pkt->rx_ctrl.sig_len;
    if (len < 24) return;
    uint8_t fsubtype = (d[0] >> 4) & 0xF;
    uint8_t ftype    = (d[0] >> 2) & 0x3;
    if (ftype != 0) return;
    s_monFrames++;

    if (s_promiscMode == 1) {
        if (fsubtype != 0xC && fsubtype != 0xA) return;
        const uint8_t* bssid = d + 16;
        for (int i = 0; i < s_dCount; i++) {
            if (memcmp(s_deauths[i].bssid, bssid, 6) == 0) { s_deauths[i].count++; s_deauths[i].rssi = pkt->rx_ctrl.rssi; return; }
        }
        if (s_dCount < MAX_DEAUTHS) { memcpy(s_deauths[s_dCount].bssid, bssid, 6); s_deauths[s_dCount].count=1; s_deauths[s_dCount].rssi=pkt->rx_ctrl.rssi; s_dCount++; }
    } else if (s_promiscMode == 2) {
        if (fsubtype != 0x4) return;
        if (len < 26) return;
        const uint8_t* body = d + 24;
        uint32_t blen = len - 24;
        char ssid[33] = "(wildcard)";
        if (blen >= 2 && body[0] == 0 && body[1] > 0 && body[1] <= 32 && blen >= (uint32_t)(2+body[1])) {
            uint8_t sl = body[1]; memcpy(ssid, body+2, sl); ssid[sl]='\0';
        }
        for (int i = 0; i < s_pCount; i++) {
            if (strncmp(s_probes[i].ssid, ssid, 32)==0) { s_probes[i].count++; s_probes[i].rssi=pkt->rx_ctrl.rssi; return; }
        }
        if (s_pCount < MAX_PROBES) { strncpy(s_probes[s_pCount].ssid, ssid, 32); s_probes[s_pCount].count=1; s_probes[s_pCount].rssi=pkt->rx_ctrl.rssi; s_pCount++; }
    }
}

static void startPromiscuous(uint8_t mode) {
    s_promiscMode = mode;
    s_monFrames   = 0;
    s_dCount      = 0;
    s_pCount      = 0;
    WiFi.mode(WIFI_STA); WiFi.disconnect(); delay(100);
    esp_wifi_set_channel(s_monCh, WIFI_SECOND_CHAN_NONE);
    wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&f);
    esp_wifi_set_promiscuous_rx_cb(promiscCb);
    esp_wifi_set_promiscuous(true);
}

static void stopPromiscuous() {
    s_promiscMode = 0;
    esp_wifi_set_promiscuous(false);
    WiFi.mode(WIFI_OFF);
    s_state = DetState::MAIN_MENU;
    s_dirty = true;
}

// ── Display ────────────────────────────────────────────────────────────────

static void drawBar(const char* title) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0,0,SCREEN_W,STATUS_H,C_STATUS_BG);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(C_STATUS_FG,C_STATUS_BG);
    d.setCursor(2,3); d.print(title);
    drawBatteryWidget(C_STATUS_BG, SCREEN_W-43);
}

static const char* MENU_LABELS[4] = {
    "Threat Scan",
    "WiFi Analyzer",
    "Deauth Monitor",
    "Probe Sniffer"
};
static const char* MENU_DESC[4] = {
    "BLE+WiFi malware signatures",
    "All APs, enc, evil twin flag",
    "Detect deauth/disassoc floods",
    "What SSIDs devices seek"
};
static const uint32_t MENU_COLS[4] = { 0xFF3333, 0x0066CC, 0xFF8800, 0x00AA66 };

static void drawMainMenu() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG); drawBar("RF Monitor");
    d.setFont(&fonts::Font0); d.setTextSize(1);
    for (int i = 0; i < 4; i++) {
        int y = 15 + i * 28;
        bool sel = (i == s_menuSel);
        uint32_t col = MENU_COLS[i];
        if (sel) { d.fillRoundRect(2, y, SCREEN_W-4, 26, 4, col); d.setTextColor(0x000000, col); }
        else      { d.drawRoundRect(2, y, SCREEN_W-4, 26, 4, col); d.setTextColor(col, C_BG); }
        d.setCursor(10, y+5); d.print(MENU_LABELS[i]);
        d.setTextColor(sel ? (uint32_t)0x333333 : (uint32_t)C_DIM, sel ? col : (uint32_t)C_BG);
        d.setCursor(10, y+15); d.print(MENU_DESC[i]);
    }
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H-10);
    d.print("up/dn=select  Enter=start  fn+bksp=back");
}

static void drawThreatResults() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG); drawBar("Threat Scan");
    d.setFont(&fonts::Font0); d.setTextSize(1);
    if (s_tCount == 0) {
        d.setTextColor(0x00CC44, C_BG); d.setTextSize(2);
        const char* ok = "All Clear";
        d.setCursor((SCREEN_W-(int)strlen(ok)*FONT_W*2)/2, 40); d.print(ok);
        d.setTextSize(1); d.setTextColor(C_DIM, C_BG);
        d.setCursor(14, 70); d.print("No known threats detected.");
        d.setCursor(2, SCREEN_H-10); d.print("Enter=rescan  fn+bksp=menu");
    } else {
        constexpr int ROW_H=14, ROWS=(SCREEN_H-STATUS_H-12)/ROW_H;
        for (int i=0; i<ROWS && (s_scroll+i)<s_tCount; i++) {
            const auto& t=s_threats[s_scroll+i]; int y=STATUS_H+2+i*ROW_H;
            uint32_t col=(t.level>=2)?(uint32_t)0xFF3333:(t.level==1)?(uint32_t)0xFFAA00:(uint32_t)0x00AAFF;
            d.fillRect(0,y,3,ROW_H-1,col);
            d.setTextColor(col,C_BG); d.setCursor(6,y+3); d.print(t.source);
            d.setTextColor(0xFFFFFF,C_BG);
            char buf[32]; snprintf(buf,sizeof(buf),"%-20s%4ddBm",t.label,t.rssi);
            d.setCursor(30,y+3); d.print(buf);
        }
        d.setTextColor(C_DIM,C_BG);
        char foot[48]; snprintf(foot,sizeof(foot),"%d threat%s  up/dn  Enter=rescan",s_tCount,s_tCount==1?"":"s");
        d.setCursor(2,SCREEN_H-10); d.print(foot);
    }
}

static void drawWifiResults() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG); drawBar("WiFi Analyzer");
    d.setFont(&fonts::Font0); d.setTextSize(1);
    if (s_apCount == 0) {
        d.setTextColor(C_DIM,C_BG); d.setCursor(14,60); d.print("No APs found.");
    } else {
        constexpr int ROW_H=12, ROWS=(SCREEN_H-STATUS_H-14)/ROW_H;
        for (int i=0; i<ROWS && (s_scroll+i)<s_apCount; i++) {
            const auto& a=s_aps[s_scroll+i]; int y=STATUS_H+1+i*ROW_H;
            bool flagged = a.flagOpen || a.flagTwin;
            uint32_t col = a.flagTwin?(uint32_t)0xFF3333 : a.flagOpen?(uint32_t)0xFFAA00 : (uint32_t)C_FG;
            // CH
            d.setTextColor(0x888888,C_BG); char ch[4]; snprintf(ch,4,"%2d",a.ch);
            d.setCursor(0,y); d.print(ch);
            // Enc
            d.setTextColor(col,C_BG); d.setCursor(14,y); d.print(a.enc);
            // RSSI
            d.setTextColor(0x888888,C_BG); char rs[5]; snprintf(rs,5,"%4d",a.rssi);
            d.setCursor(50,y); d.print(rs);
            // SSID
            d.setTextColor(col,C_BG);
            char ssid[21]; strncpy(ssid,a.ssid,19); ssid[19]='\0';
            d.setCursor(80,y); d.print(ssid);
            // Flag
            if (a.flagTwin) { d.setTextColor(0xFF3333,C_BG); d.setCursor(200,y); d.print("TWIN"); }
            else if (a.flagOpen) { d.setTextColor(0xFFAA00,C_BG); d.setCursor(200,y); d.print("OPEN"); }
        }
        d.setTextColor(C_DIM,C_BG);
        char foot[48]; snprintf(foot,sizeof(foot),"%d APs  up/dn=scroll  Enter=rescan",s_apCount);
        d.setCursor(2,SCREEN_H-10); d.print(foot);
    }
}

static void drawDeauthMon() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG); drawBar("Deauth Monitor");
    d.setFont(&fonts::Font0); d.setTextSize(1);
    // Channel + frame count header
    d.setTextColor(C_DIM,C_BG);
    char hdr[48]; snprintf(hdr,sizeof(hdr),"CH:%d  Frames:%lu  </> change ch",s_monCh,(unsigned long)s_monFrames);
    d.setCursor(2,STATUS_H+2); d.print(hdr);
    if (s_dCount == 0) {
        d.setTextColor(0x00CC44,C_BG); d.setTextSize(2);
        const char* ok="No Deauths";
        d.setCursor((SCREEN_W-(int)strlen(ok)*FONT_W*2)/2,50); d.print(ok);
        d.setTextSize(1); d.setTextColor(C_DIM,C_BG);
        d.setCursor(4,80); d.print("No deauth/disassoc frames seen.");
        d.setCursor(4,90); d.print("Listening on CH"); d.print(s_monCh);
    } else {
        constexpr int ROW_H=13, ROWS=(SCREEN_H-STATUS_H-26)/ROW_H;
        for (int i=0; i<ROWS && i<s_dCount; i++) {
            const auto& e=s_deauths[i]; int y=STATUS_H+14+i*ROW_H;
            bool attack=(e.count>=10);
            uint32_t col=attack?(uint32_t)0xFF3333:(uint32_t)0xFFAA00;
            d.fillRect(0,y,3,ROW_H-1,col);
            char mac[18]; snprintf(mac,sizeof(mac),"%02X:%02X:%02X:%02X:%02X:%02X",
                e.bssid[0],e.bssid[1],e.bssid[2],e.bssid[3],e.bssid[4],e.bssid[5]);
            d.setTextColor(col,C_BG); d.setCursor(6,y+2); d.print(mac);
            char cnt[12]; snprintf(cnt,sizeof(cnt),"x%d",e.count);
            d.setTextColor(attack?(uint32_t)0xFF3333:(uint32_t)0xFFAA00,C_BG);
            d.setCursor(SCREEN_W-30,y+2); d.print(cnt);
        }
        if (s_dCount > 0) {
            d.setTextColor(C_DIM,C_BG); d.setCursor(2,SCREEN_H-10);
            d.print("red>=10 frames = likely attack");
        }
    }
    d.setTextColor(C_DIM,C_BG); d.setCursor(SCREEN_W-78,SCREEN_H-10); d.print("bksp=stop");
}

static void drawProbeSniff() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG); drawBar("Probe Sniffer");
    d.setFont(&fonts::Font0); d.setTextSize(1);
    char hdr[48]; snprintf(hdr,sizeof(hdr),"CH:%d  Frames:%lu  </> change ch",s_monCh,(unsigned long)s_monFrames);
    d.setTextColor(C_DIM,C_BG); d.setCursor(2,STATUS_H+2); d.print(hdr);
    if (s_pCount == 0) {
        d.setTextColor(0x00AA66,C_BG); d.setTextSize(2);
        const char* ok="Listening...";
        d.setCursor((SCREEN_W-(int)strlen(ok)*FONT_W*2)/2,50); d.print(ok);
        d.setTextSize(1); d.setTextColor(C_DIM,C_BG);
        d.setCursor(4,80); d.print("No probe requests yet.");
    } else {
        // Sort by count descending
        for (int i=0;i<s_pCount-1;i++)
            for (int j=0;j<s_pCount-1-i;j++)
                if (s_probes[j].count<s_probes[j+1].count) { ProbeEntry tmp=s_probes[j]; s_probes[j]=s_probes[j+1]; s_probes[j+1]=tmp; }
        constexpr int ROW_H=12, ROWS=(SCREEN_H-STATUS_H-26)/ROW_H;
        for (int i=0; i<ROWS && (s_scroll+i)<s_pCount; i++) {
            const auto& p=s_probes[s_scroll+i]; int y=STATUS_H+14+i*ROW_H;
            d.setTextColor(0x00AA66,C_BG); d.setCursor(0,y);
            char cnt[8]; snprintf(cnt,sizeof(cnt),"%4d",p.count); d.print(cnt);
            d.setTextColor(0xFFFFFF,C_BG); d.setCursor(26,y);
            char ssid[29]; strncpy(ssid,p.ssid,28); ssid[28]='\0'; d.print(ssid);
            d.setTextColor(0x666666,C_BG);
            char rs[6]; snprintf(rs,sizeof(rs),"%4d",p.rssi);
            d.setCursor(SCREEN_W-28,y); d.print(rs);
        }
        d.setTextColor(C_DIM,C_BG); d.setCursor(2,SCREEN_H-10);
        char foot[32]; snprintf(foot,sizeof(foot),"%d SSIDs seen  bksp=stop",s_pCount);
        d.print(foot);
    }
}

// ── Start a mode ───────────────────────────────────────────────────────────

static void startMode() {
    s_scroll = 0;
    switch (s_menuSel) {
        case 0: runThreatScan();  s_state=DetState::THREAT_DONE; s_dirty=true; break;
        case 1: runWifiAnalyzer(); s_state=DetState::WIFI_DONE;  s_dirty=true; break;
        case 2: startPromiscuous(1); s_state=DetState::DEAUTH_MON;  s_lastRedraw=0; break;
        case 3: startPromiscuous(2); s_state=DetState::PROBE_SNIFF; s_lastRedraw=0; break;
    }
}

// ── Public ─────────────────────────────────────────────────────────────────

void appDetectorEnter() {
    if (s_state==DetState::DEAUTH_MON || s_state==DetState::PROBE_SNIFF) stopPromiscuous();
    s_state=DetState::MAIN_MENU; s_dirty=true; s_menuSel=0; s_scroll=0;
}

void appDetectorLoop() {
    if (s_bleScanDone) {
        s_bleScanDone = false;
        if (s_state == DetState::THREAT_DONE) s_dirty = true;
    }

    // Live monitoring modes — update on timer, check keys
    if (s_state==DetState::DEAUTH_MON || s_state==DetState::PROBE_SNIFF) {
        if (millis()-s_lastRedraw > 800) {
            if (s_state==DetState::DEAUTH_MON) drawDeauthMon();
            else drawProbeSniff();
            s_lastRedraw=millis();
        }
        auto ev=readKeys();
        if (!ev.changed) return;
        if (ev.back) { stopPromiscuous(); return; }
        if (ev.left  && s_monCh>1)  { s_monCh--; esp_wifi_set_channel(s_monCh,WIFI_SECOND_CHAN_NONE); s_lastRedraw=0; }
        if (ev.right && s_monCh<13) { s_monCh++; esp_wifi_set_channel(s_monCh,WIFI_SECOND_CHAN_NONE); s_lastRedraw=0; }
        if (ev.up   && s_scroll>0)  { s_scroll--; s_lastRedraw=0; }
        if (ev.down)                 { s_scroll++; s_lastRedraw=0; }
        return;
    }

    if (s_dirty) {
        switch (s_state) {
            case DetState::MAIN_MENU:   drawMainMenu();     break;
            case DetState::THREAT_DONE: drawThreatResults(); break;
            case DetState::WIFI_DONE:   drawWifiResults();   break;
            default: break;
        }
        s_dirty=false;
    }

    auto ev=readKeys();
    if (!ev.changed) return;

    switch (s_state) {
        case DetState::MAIN_MENU:
            if (ev.back)  { shutdownThreatBle(); goHome(); return; }
            if (ev.up   && s_menuSel>0) { s_menuSel--; s_dirty=true; }
            if (ev.down && s_menuSel<3) { s_menuSel++; s_dirty=true; }
            if (ev.enter) startMode();
            break;
        case DetState::THREAT_DONE:
        case DetState::WIFI_DONE:
            if (ev.back)  { shutdownThreatBle(); s_state=DetState::MAIN_MENU; s_dirty=true; }
            if (ev.up   && s_scroll>0) { s_scroll--; s_dirty=true; }
            if (ev.down)               { s_scroll++; s_dirty=true; }
            if (ev.enter) startMode();
            break;
        default: break;
    }
}
