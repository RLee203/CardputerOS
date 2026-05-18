#include "app_ble.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>

static constexpr int MAX_DEVICES = 48;
static constexpr int ROWS_VIS    = 10;
static constexpr int SCAN_SECS   = 1;

struct BLEEntry {
    char mac[18];
    char name[21];
    int  rssi;
};

static BLEEntry s_devices[MAX_DEVICES];
static int      s_devCount = 0;

class BLEScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        if (s_devCount >= MAX_DEVICES) return;
        auto& e = s_devices[s_devCount++];
        strncpy(e.mac,  dev->getAddress().toString().c_str(), 17); e.mac[17] = '\0';
        strncpy(e.name, dev->haveName() ? dev->getName().c_str() : "(unknown)", 20);
        e.name[20] = '\0';
        e.rssi = dev->getRSSI();
    }
};

// ── Tracker data ───────────────────────────────────────────────────────────────

struct TrackerHit { char label[28]; char addr[18]; char prox[8]; int rssi; uint8_t level; };
static constexpr int MAX_TRACKER_HITS = 24;
static TrackerHit s_trkHits[MAX_TRACKER_HITS];
static int        s_trkCount = 0;

static bool trkCiContains(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return false;
    size_t nl=strlen(needle), hl=strlen(hay);
    if (nl>hl) return false;
    for (size_t i=0; i<=hl-nl; i++) {
        bool ok=true;
        for (size_t j=0; j<nl; j++) if (tolower((unsigned char)hay[i+j])!=tolower((unsigned char)needle[j])) {ok=false;break;}
        if (ok) return true;
    }
    return false;
}

static const char* trkProx(int rssi) { return rssi>=-55?"CLOSE":rssi>=-75?"NEAR":"FAR"; }

static void addTrkHit(const char* label, int rssi, uint8_t level, const char* addr) {
    for (int i=0;i<s_trkCount;i++) {
        if (strncmp(s_trkHits[i].addr,addr,17)==0) {
            if (rssi>s_trkHits[i].rssi) { s_trkHits[i].rssi=rssi; strncpy(s_trkHits[i].prox,trkProx(rssi),7); }
            return;
        }
    }
    if (s_trkCount>=MAX_TRACKER_HITS) return;
    auto& h=s_trkHits[s_trkCount++];
    strncpy(h.label,label,27); h.label[27]='\0';
    strncpy(h.addr, addr, 17); h.addr[17]='\0';
    strncpy(h.prox, trkProx(rssi),7); h.prox[7]='\0';
    h.rssi=rssi; h.level=level;
}

struct TrkNamePat { const char* match; const char* label; uint8_t level; };
static const TrkNamePat TRK_PATS[] = {
    {"airtag",    "AirTag",             2},{"smarttag",  "Samsung SmartTag",   2},
    {"chipolo",   "Chipolo Tracker",    2},{"trackr",    "TrackR",             2},
    {"nutfind",   "Nut Tracker",        2},{"pebblebee", "PebbleBee Tracker",  2},
    {"tile",      "Tile Tracker",       2},{"gt06",      "GPS Tracker GT06",   2},
    {"gt02",      "GPS Tracker GT02",   2},{"coban",     "Coban GPS Tracker",  2},
    {"queclink",  "Queclink GPS",       2},{"teltonika", "Teltonika GPS",      2},
    {"concox",    "Concox GPS",         2},{"meitrack",  "Meitrack GPS",       2},
    {"sinotrack", "Sinotrack GPS",      2},{"bouncie",   "Bouncie GPS",        2},
    {"optimus",   "Optimus GPS",        2},{"landair",   "LandAirSea GPS",     2},
    {"tracker",   "Tracking Device",   1},{"locator",   "Locator Device",     1},
    {"gps",       "GPS Device",         1},{"nut",       "Nut Tracker",        1},
    {"findmy",    "Apple FindMy Dev",   1},
};
static constexpr int N_TRK_PATS=(int)(sizeof(TRK_PATS)/sizeof(TRK_PATS[0]));

class TrkCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        int rssi=dev->getRSSI();
        char addr[18]; snprintf(addr,sizeof(addr),"%s",dev->getAddress().toString().c_str());
        const char* name=dev->haveName()?dev->getName().c_str():"";
        if (dev->haveManufacturerData()) {
            std::string mfr=dev->getManufacturerData();
            if (mfr.length()>=2) {
                uint16_t cid=(uint8_t)mfr[0]|((uint16_t)(uint8_t)mfr[1]<<8);
                if (cid==0x004C && mfr.length()>=4 && (uint8_t)mfr[2]==0x12) {
                    addTrkHit((uint8_t)mfr[3]==0x19?"AirTag":"Apple FindMy Device",rssi,(uint8_t)mfr[3]==0x19?2:1,addr); return;
                }
                if (cid==0x0075) { addTrkHit("Samsung SmartTag",rssi,2,addr); return; }
            }
        }
        { NimBLEUUID tileUUID((uint16_t)0xFEED); if (dev->haveServiceUUID()&&dev->isAdvertisingService(tileUUID)) { addTrkHit("Tile Tracker",rssi,2,addr); return; } }
        { NimBLEUUID chipoloUUID((uint16_t)0xFE07); if (dev->haveServiceUUID()&&dev->isAdvertisingService(chipoloUUID)) { addTrkHit("Chipolo Tracker",rssi,2,addr); return; } }
        if (name&&name[0]) for (int p=0;p<N_TRK_PATS;p++) if (trkCiContains(name,TRK_PATS[p].match)) { addTrkHit(TRK_PATS[p].label,rssi,TRK_PATS[p].level,addr); return; }
    }
};
static TrkCallbacks s_trkCallbacks;

namespace {

enum class BleState { MENU, LIST, TRACKER_SCAN, TRACKER_DONE };
enum class ScanMode { DEVICES, TRACKERS };

bool            s_bleInited = false;
BleState        s_state     = BleState::MENU;
bool            s_dirty     = true;
int             s_scroll    = 0;
int             s_menuSel   = 0;   // 0=SCAN  1=TRACKER
NimBLEScan*     s_scan      = nullptr;
BLEScanCallbacks s_callbacks;
volatile bool   s_scanDone  = false;
ScanMode        s_scanMode  = ScanMode::DEVICES;

void shutdownBle() {
    if (s_scan && s_scan->isScanning()) {
        s_scan->stop();
    }
    if (s_bleInited) {
        NimBLEDevice::deinit(true);
        s_scan = nullptr;
        s_bleInited = false;
    }
}

void onScanComplete(NimBLEScanResults) {
    s_scanDone = true;
}

void ensureBleInit() {
    if (s_bleInited) return;
    if (WiFi.getMode() != WIFI_OFF) {
        WiFi.disconnect(true, true);
        delay(80);
        WiFi.mode(WIFI_OFF);
        delay(180);
    }
    NimBLEDevice::init("");
    s_scan = NimBLEDevice::getScan();
    s_scan->setAdvertisedDeviceCallbacks(&s_callbacks);
    s_scan->setActiveScan(true);
    s_scan->setInterval(100);
    s_scan->setWindow(99);
    s_bleInited = true;
}

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

void drawMenu() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("BLE Scanner");
    d.setFont(&fonts::Font0); d.setTextSize(1);

    struct CardDef { const char* title; const char* sub; uint32_t col; uint32_t bg; };
    static const CardDef CARDS[] = {
        {"SCAN",    "BLE device discovery",    0x0055CC, 0x001A40},
        {"TRACKER", "AirTag, Tile, GPS...",    0xCC0044, 0x1A0010},
    };

    for (int i = 0; i < 2; i++) {
        int y = 24 + i * 46;
        bool sel = (i == s_menuSel);
        uint32_t col = CARDS[i].col;
        uint32_t bg  = sel ? col : CARDS[i].bg;
        if (sel) d.fillRoundRect(4, y, 232, 36, 5, col);
        else     d.fillRoundRect(4, y, 232, 36, 5, CARDS[i].bg);
        d.drawRoundRect(4, y, 232, 36, 5, col);
        d.setTextSize(2);
        d.setTextColor(sel ? (uint32_t)0x000000 : col, bg);
        d.setCursor(14, y+6); d.print(CARDS[i].title);
        d.setTextSize(1);
        d.setTextColor(sel ? (uint32_t)0x333333 : (uint32_t)C_DIM, bg);
        d.setCursor(14, y+24); d.print(CARDS[i].sub);
    }
    d.setTextColor(C_DIM, C_BG); d.setCursor(2, 113);
    d.print("up/dn=select  Enter=start  bksp=back");
}

void drawTrackerResults() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("Tracker Scan");
    d.setFont(&fonts::Font0); d.setTextSize(1);
    if (s_trkCount == 0) {
        d.setTextColor(0x00CC44, C_BG); d.setTextSize(2);
        const char* ok = "No Trackers";
        d.setCursor((SCREEN_W-(int)strlen(ok)*FONT_W*2)/2, 16); d.print(ok);
        d.setTextSize(1); d.setTextColor(C_DIM, C_BG);
        d.setCursor(4, 44); d.print("No known tracking devices found.");
        d.setCursor(4, 56); d.print("AirTags randomize MAC ~15 min.");
        d.setCursor(4, 66); d.print("Run multiple scans if concerned.");
        d.setCursor(4, 80); d.print("GPS trackers (cellular-only)");
        d.setCursor(4, 90); d.print("cannot be detected via BLE.");
        d.setCursor(4, 108); d.print("Enter=rescan  bksp=menu");
    } else {
        for (int i=0;i<s_trkCount-1;i++)
            for (int j=0;j<s_trkCount-1-i;j++)
                if (s_trkHits[j].rssi<s_trkHits[j+1].rssi) { TrackerHit tmp=s_trkHits[j]; s_trkHits[j]=s_trkHits[j+1]; s_trkHits[j+1]=tmp; }
        bool anyClose=false;
        for (int i=0;i<s_trkCount;i++) if (s_trkHits[i].prox[0]=='C') {anyClose=true;break;}
        uint32_t hcol=anyClose?(uint32_t)0xFF3333:(uint32_t)0xFFAA00;
        d.setTextColor(hcol,C_BG); d.setTextSize(2);
        char hdr[20]; snprintf(hdr,sizeof(hdr),"%d FOUND!",s_trkCount);
        d.setCursor((SCREEN_W-(int)strlen(hdr)*FONT_W*2)/2,16); d.print(hdr);
        d.setTextSize(1);
        constexpr int ROW_H=14, ROWS=(SCREEN_H-STATUS_H-32)/ROW_H;
        for (int i=0;i<ROWS&&(s_scroll+i)<s_trkCount;i++) {
            const auto& h=s_trkHits[s_scroll+i]; int y=STATUS_H+26+i*ROW_H;
            uint32_t col=(h.prox[0]=='C')?(uint32_t)0xFF3333:(h.prox[0]=='N')?(uint32_t)0xFFAA00:(uint32_t)0xAAAAAA;
            d.fillRect(0,y,3,ROW_H-1,col);
            d.setTextColor(col,C_BG); d.setCursor(6,y+3); d.print(h.prox);
            d.setTextColor(h.level>=2?(uint32_t)0xFFFFFF:(uint32_t)0xAAAAAA,C_BG);
            char buf[28]; snprintf(buf,sizeof(buf),"%-20s",h.label); d.setCursor(42,y+3); d.print(buf);
            d.setTextColor(0x777777,C_BG); char dbm[8]; snprintf(dbm,sizeof(dbm),"%4d",h.rssi);
            d.setCursor(SCREEN_W-26,y+3); d.print(dbm);
        }
        d.setTextColor(C_DIM,C_BG);
        char foot[48]; snprintf(foot,sizeof(foot),"%d device%s  up/dn  Enter=rescan",s_trkCount,s_trkCount==1?"":"s");
        d.setCursor(2,SCREEN_H-10); d.print(foot);
    }
}

void drawScanning(bool tracker) {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar(tracker ? "Tracker Scan" : "BLE Scanner");
    d.setFont(&fonts::Font0); d.setTextSize(2);
    d.setTextColor(tracker ? (uint32_t)0xFF6688 : (uint32_t)C_FG, C_BG);
    const char* msg = "Scanning...";
    d.setCursor((SCREEN_W-(int)strlen(msg)*FONT_W*2)/2, 36); d.print(msg);
    d.setTextSize(1); d.setTextColor(C_DIM, C_BG);
    const char* sub = tracker ? "BLE 8 sec -- stay still" : "Please wait...";
    d.setCursor((SCREEN_W-(int)strlen(sub)*FONT_W)/2, 64); d.print(sub);
}

void drawList() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("BLE Scanner");
    d.setFont(&fonts::Font0); d.setTextSize(1);
    if (s_devCount == 0) {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(14, 54); d.print("No devices found");
    } else {
        for (int i = 0; i < ROWS_VIS && (s_scroll + i) < s_devCount; i++) {
            const auto& dev = s_devices[s_scroll + i];
            int y = STATUS_H + 2 + i * 11;
            uint32_t rssiCol = dev.rssi >= -70 ? (uint32_t)0x00CC00
                             : dev.rssi >= -80 ? (uint32_t)0xCCAA00
                                              : (uint32_t)0xFF5555;
            d.setTextColor(rssiCol, C_BG);
            char rb[5]; snprintf(rb, sizeof(rb), "%4d", dev.rssi);
            d.setCursor(0, y); d.print(rb);
            d.setTextColor(0x888888, C_BG);
            d.setCursor(26, y); d.print(dev.mac + 9);
            d.setTextColor(C_FG, C_BG);
            d.setCursor(82, y);
            char nm[21]; strncpy(nm, dev.name, 20); nm[20] = '\0';
            d.print(nm);
        }
    }
    d.fillRect(0, SCREEN_H-FONT_H-2, SCREEN_W, FONT_H+2, C_BG);
    d.setTextColor(C_DIM, C_BG);
    char footer[48];
    snprintf(footer, sizeof(footer), "%d found  up/dn=scroll  Enter=rescan", s_devCount);
    d.setCursor(2, SCREEN_H-FONT_H-1); d.print(footer);
}

void doScan(int secs) {
    s_scanDone = false;
    s_scan->clearResults();
    s_scan->start(secs, onScanComplete, false);
}

} // namespace

void appBleEnter() {
    s_state    = BleState::MENU;
    s_dirty    = true;
    s_devCount = 0;
    s_scroll   = 0;
    ensureBleInit();
}

void appBleLoop() {
    if (s_scanDone) {
        s_scanDone = false;
        s_state = (s_scanMode == ScanMode::TRACKERS) ? BleState::TRACKER_DONE : BleState::LIST;
        s_dirty = true;
    }

    if (s_dirty) {
        switch (s_state) {
            case BleState::MENU:         drawMenu();           break;
            case BleState::LIST:         drawList();           break;
            case BleState::TRACKER_DONE: drawTrackerResults(); break;
            default: break;
        }
        s_dirty = false;
    }

    auto ev = readKeys();
    if (!ev.changed) return;

    if (ev.back) {
        if (s_state == BleState::MENU) {
            shutdownBle();
            goHome();
            return;
        }
        if (s_scan && s_scan->isScanning()) s_scan->stop();
        s_state = BleState::MENU;
        s_dirty = true;
        return;
    }

    switch (s_state) {
        case BleState::MENU:
            if (ev.up   && s_menuSel > 0) { s_menuSel--; s_dirty = true; break; }
            if (ev.down && s_menuSel < 1) { s_menuSel++; s_dirty = true; break; }
            if (ev.enter) {
                if (s_menuSel == 0) {
                    s_devCount = 0; s_scroll = 0;
                    s_scanMode = ScanMode::DEVICES;
                    s_scan->setAdvertisedDeviceCallbacks(&s_callbacks);
                    drawScanning(false);
                    doScan(SCAN_SECS);
                    s_state = BleState::TRACKER_SCAN;
                } else {
                    s_trkCount = 0; s_scroll = 0;
                    s_scanMode = ScanMode::TRACKERS;
                    drawScanning(true);
                    s_scan->setAdvertisedDeviceCallbacks(&s_trkCallbacks);
                    doScan(8);
                    s_state = BleState::TRACKER_SCAN;
                }
            }
            break;

        case BleState::LIST:
            if (ev.up   && s_scroll > 0)              { s_scroll--; s_dirty = true; }
            if (ev.down && s_scroll < s_devCount - 1) { s_scroll++; s_dirty = true; }
            if (ev.enter) {
                s_devCount = 0; s_scroll = 0;
                s_scanMode = ScanMode::DEVICES;
                s_scan->setAdvertisedDeviceCallbacks(&s_callbacks);
                drawScanning(false);
                doScan(SCAN_SECS);
                s_state = BleState::TRACKER_SCAN;
            }
            break;

        case BleState::TRACKER_DONE:
            if (ev.up   && s_scroll > 0)               { s_scroll--; s_dirty = true; }
            if (ev.down && s_scroll < s_trkCount - 1)  { s_scroll++; s_dirty = true; }
            if (ev.enter) {
                s_trkCount = 0; s_scroll = 0;
                s_scanMode = ScanMode::TRACKERS;
                drawScanning(true);
                s_scan->setAdvertisedDeviceCallbacks(&s_trkCallbacks);
                doScan(8);
                s_state = BleState::TRACKER_SCAN;
            }
            break;

        default:
            break;
    }
}
