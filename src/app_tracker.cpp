#include "app_tracker.h"
#include <M5Cardputer.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEUUID.h>
#include "config.h"
#include "input.h"
#include "nav.h"

// ── Hit table ──────────────────────────────────────────────────────────────

struct TrackerHit {
    char label[28];
    char addr[18];    // BLE MAC for dedup (xx:xx:xx:xx:xx:xx)
    char prox[8];     // CLOSE / NEAR / FAR
    int  rssi;
    uint8_t level;    // 1=info  2=danger
};

static constexpr int MAX_HITS = 24;
static TrackerHit s_hits[MAX_HITS];
static int        s_count  = 0;
static int        s_scroll = 0;

// ── State ──────────────────────────────────────────────────────────────────

enum class TrkState { MENU, RESULTS };
static TrkState s_state    = TrkState::MENU;
static bool     s_dirty    = true;
static bool     s_bleInited = false;
static NimBLEScan* s_bleScan   = nullptr;
static volatile bool s_scanDone = false;

// ── Helpers ────────────────────────────────────────────────────────────────

static bool ciContains(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return false;
    size_t nl = strlen(needle), hl = strlen(hay);
    if (nl > hl) return false;
    for (size_t i = 0; i <= hl - nl; i++) {
        bool ok = true;
        for (size_t j = 0; j < nl; j++) {
            if (tolower((unsigned char)hay[i+j]) != tolower((unsigned char)needle[j])) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

static const char* toProx(int rssi) {
    if (rssi >= -55) return "CLOSE";
    if (rssi >= -75) return "NEAR";
    return "FAR";
}

static void addHit(const char* label, int rssi, uint8_t level, const char* addr) {
    for (int i = 0; i < s_count; i++) {
        if (strncmp(s_hits[i].addr, addr, 17) == 0) {
            if (rssi > s_hits[i].rssi) {
                s_hits[i].rssi = rssi;
                strncpy(s_hits[i].prox, toProx(rssi), sizeof(s_hits[i].prox) - 1);
            }
            return;
        }
    }
    if (s_count >= MAX_HITS) return;
    auto& h = s_hits[s_count++];
    strncpy(h.label, label, sizeof(h.label) - 1); h.label[sizeof(h.label) - 1] = '\0';
    strncpy(h.addr,  addr,  sizeof(h.addr)  - 1); h.addr[sizeof(h.addr)  - 1] = '\0';
    strncpy(h.prox,  toProx(rssi), sizeof(h.prox) - 1); h.prox[sizeof(h.prox) - 1] = '\0';
    h.rssi  = rssi;
    h.level = level;
}

// ── BLE scan callbacks ─────────────────────────────────────────────────────

struct NamePat { const char* match; const char* label; uint8_t level; };
static const NamePat NAME_PATS[] = {
    // Known tracker brands
    { "airtag",    "AirTag",                2 },
    { "smarttag",  "Samsung SmartTag",      2 },
    { "chipolo",   "Chipolo Tracker",       2 },
    { "trackr",    "TrackR",                2 },
    { "nutfind",   "Nut Tracker",           2 },
    { "pebblebee", "PebbleBee Tracker",     2 },
    // GPS tracker modules (often have BLE management interface)
    { "gt06",      "GPS Tracker GT06",      2 },
    { "gt02",      "GPS Tracker GT02",      2 },
    { "coban",     "Coban GPS Tracker",     2 },
    { "queclink",  "Queclink GPS",          2 },
    { "teltonika", "Teltonika GPS",         2 },
    { "concox",    "Concox GPS Tracker",    2 },
    { "meitrack",  "Meitrack GPS",          2 },
    { "sinotrack", "Sinotrack GPS",         2 },
    { "bouncie",   "Bouncie GPS",           2 },
    { "optimus",   "Optimus GPS",           2 },
    { "landair",   "LandAirSea GPS",        2 },
    { "motosafety","MOTOsafety GPS",        2 },
    // Generic suspicious patterns
    { "tracker",   "Tracking Device",       1 },
    { "locator",   "Locator Device",        1 },
    { "gps",       "GPS Device",            1 },
    { "track",     "Tracking Device",       1 },
    { "nut",       "Nut Tracker",           1 },
    { "tile",      "Tile Tracker",          2 },
    { "safeguard", "SafeGuard Tracker",     1 },
    { "findmy",    "Apple FindMy Device",   1 },
};
static constexpr int N_NAME_PATS = (int)(sizeof(NAME_PATS) / sizeof(NAME_PATS[0]));

class TrkCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        int   rssi    = dev->getRSSI();
        char  addr[18];
        snprintf(addr, sizeof(addr), "%s", dev->getAddress().toString().c_str());
        const char* name = dev->haveName() ? dev->getName().c_str() : "";

        // ── Manufacturer data: Apple (AirTag / FindMy) ─────────────────────
        if (dev->haveManufacturerData()) {
            std::string mfr = dev->getManufacturerData();
            if (mfr.length() >= 2) {
                uint16_t cid = (uint8_t)mfr[0] | ((uint16_t)(uint8_t)mfr[1] << 8);

                if (cid == 0x004C && mfr.length() >= 4) {   // Apple
                    uint8_t type    = (uint8_t)mfr[2];
                    uint8_t payLen  = (uint8_t)mfr[3];
                    if (type == 0x12) {
                        // FindMy protocol: 0x19 payload = AirTag, otherwise other FindMy accessory
                        if (payLen == 0x19) {
                            addHit("AirTag", rssi, 2, addr);
                        } else {
                            addHit("Apple FindMy Device", rssi, 1, addr);
                        }
                        return;
                    }
                }

                if (cid == 0x0075) {   // Samsung Electronics
                    addHit("Samsung SmartTag", rssi, 2, addr);
                    return;
                }

                // Tile uses service UUID but also has company data sometimes
                if (cid == 0x000D) {   // Texas Instruments (some Tile hardware)
                    if (ciContains(name, "tile")) {
                        addHit("Tile Tracker", rssi, 2, addr);
                        return;
                    }
                }
            }
        }

        // ── Service UUID: Tile (0xFEED) ────────────────────────────────────
        {
            NimBLEUUID tileUUID((uint16_t)0xFEED);
            if (dev->haveServiceUUID() && dev->isAdvertisingService(tileUUID)) {
                addHit("Tile Tracker", rssi, 2, addr);
                return;
            }
        }

        // ── Service UUID: Chipolo (0xFE07) ─────────────────────────────────
        {
            NimBLEUUID chipoloUUID((uint16_t)0xFE07);
            if (dev->haveServiceUUID() && dev->isAdvertisingService(chipoloUUID)) {
                addHit("Chipolo Tracker", rssi, 2, addr);
                return;
            }
        }

        // ── Name-based detection ───────────────────────────────────────────
        if (name && name[0]) {
            for (int p = 0; p < N_NAME_PATS; p++) {
                if (ciContains(name, NAME_PATS[p].match)) {
                    addHit(NAME_PATS[p].label, rssi, NAME_PATS[p].level, addr);
                    return;
                }
            }
        }
    }
};
static TrkCallbacks s_trkCallbacks;

static void onTrackerScanDone(NimBLEScanResults) {
    s_scanDone = true;
}

// ── Display ────────────────────────────────────────────────────────────────

static void drawBar() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_STATUS_FG, C_STATUS_BG);
    d.setCursor(2, 3);
    d.print("Tracker Scanner");
    drawBatteryWidget(C_STATUS_BG, SCREEN_W - 43);
}

static void drawMenu() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawBar();
    d.setFont(&fonts::Font0);

    d.setTextSize(2);
    d.setTextColor(0xFF6688, C_BG);
    const char* title = "Anti-Tracker";
    d.setCursor((SCREEN_W - (int)strlen(title) * FONT_W * 2) / 2, 16);
    d.print(title);
    d.setTextSize(1);

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(4, 38);
    d.print("Scans for hidden tracking devices");
    d.setCursor(4, 47);
    d.print("using BLE signatures.");

    // Box: what it detects
    d.fillRoundRect(4, 58, 232, 46, 4, 0x1A0010);
    d.drawRoundRect(4, 58, 232, 46, 4, 0x880044);
    d.setTextColor(0xFF6688, 0x1A0010);
    d.setCursor(10, 63);
    d.print("Detects:");
    d.setTextColor(0xDDDDDD, 0x1A0010);
    d.setCursor(10, 73);
    d.print("AirTag  Tile  Samsung SmartTag");
    d.setCursor(10, 83);
    d.print("Chipolo  TrackR  NutFind  GPS mods");
    d.setCursor(10, 93);
    d.print("GT06/GT02  Coban  Teltonika + more");

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(4, 110);
    d.print("[Enter] scan (8 sec)  fn+bksp=back");
    d.setCursor(4, 120);
    d.print("CLOSE<-55  NEAR<-75  FAR>=-76 dBm");
}

static void drawScanning() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawBar();
    d.setFont(&fonts::Font0);
    d.setTextSize(2);
    d.setTextColor(0xFF6688, C_BG);
    const char* msg = "Scanning...";
    d.setCursor((SCREEN_W - (int)strlen(msg) * FONT_W * 2) / 2, 34);
    d.print(msg);
    d.setTextSize(1);
    d.setTextColor(C_DIM, C_BG);
    const char* sub = "BLE 8 sec — stay still";
    d.setCursor((SCREEN_W - (int)strlen(sub) * FONT_W) / 2, 62);
    d.print(sub);
    const char* tip = "Trackers advertise every 2 sec";
    d.setCursor((SCREEN_W - (int)strlen(tip) * FONT_W) / 2, 76);
    d.print(tip);
}

static uint32_t proxColor(const char* prox) {
    if (prox[0] == 'C') return 0xFF3333;  // CLOSE = red
    if (prox[0] == 'N') return 0xFFAA00;  // NEAR  = orange
    return 0xAAAAAA;                        // FAR   = grey
}

static void drawResults() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawBar();
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    if (s_count == 0) {
        d.setTextColor(0x00CC44, C_BG);
        d.setTextSize(2);
        const char* ok = "No Trackers";
        d.setCursor((SCREEN_W - (int)strlen(ok) * FONT_W * 2) / 2, 16);
        d.print(ok);
        d.setTextSize(1);
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(4, 44);
        d.print("No known tracking devices found.");
        d.setCursor(4, 56);
        d.print("Note: AirTags randomize their MAC");
        d.setCursor(4, 66);
        d.print("every ~15 min when away from owner.");
        d.setCursor(4, 78);
        d.print("Run multiple scans if concerned.");
        d.setCursor(4, 92);
        d.print("GPS trackers with cellular-only");
        d.setCursor(4, 102);
        d.print("(no BLE) cannot be detected here.");
        d.setCursor(4, 118);
        d.print("Enter=rescan  fn+bksp=back");
    } else {
        // Sort by RSSI descending (closest first) — simple bubble sort on small array
        for (int i = 0; i < s_count - 1; i++) {
            for (int j = 0; j < s_count - 1 - i; j++) {
                if (s_hits[j].rssi < s_hits[j+1].rssi) {
                    TrackerHit tmp = s_hits[j];
                    s_hits[j]   = s_hits[j+1];
                    s_hits[j+1] = tmp;
                }
            }
        }

        bool anyClose = false;
        for (int i = 0; i < s_count; i++) if (s_hits[i].prox[0] == 'C') { anyClose = true; break; }

        uint32_t hcol = anyClose ? (uint32_t)0xFF3333 : (uint32_t)0xFFAA00;
        d.setTextColor(hcol, C_BG);
        d.setTextSize(2);
        char hdr[20];
        snprintf(hdr, sizeof(hdr), "%d FOUND!", s_count);
        d.setCursor((SCREEN_W - (int)strlen(hdr) * FONT_W * 2) / 2, 16);
        d.print(hdr);
        d.setTextSize(1);

        constexpr int ROW_H = 14;
        constexpr int ROWS  = (SCREEN_H - STATUS_H - 32) / ROW_H;
        for (int i = 0; i < ROWS && (s_scroll + i) < s_count; i++) {
            const auto& h = s_hits[s_scroll + i];
            int      y    = STATUS_H + 26 + i * ROW_H;
            uint32_t col  = proxColor(h.prox);

            d.fillRect(0, y, 3, ROW_H - 1, col);

            // Proximity badge
            d.setTextColor(col, C_BG);
            d.setCursor(6, y + 3);
            d.print(h.prox);

            // Label
            d.setTextColor(h.level >= 2 ? (uint32_t)0xFFFFFF : (uint32_t)0xAAAAAA, C_BG);
            char buf[28];
            snprintf(buf, sizeof(buf), "%-20s", h.label);
            d.setCursor(42, y + 3);
            d.print(buf);

            // dBm
            d.setTextColor(0x777777, C_BG);
            char dbm[8];
            snprintf(dbm, sizeof(dbm), "%4d", h.rssi);
            d.setCursor(SCREEN_W - 26, y + 3);
            d.print(dbm);
        }

        d.setTextColor(C_DIM, C_BG);
        char foot[48];
        snprintf(foot, sizeof(foot), "%d device%s  up/dn=scroll  Enter=rescan",
                 s_count, s_count == 1 ? "" : "s");
        d.setCursor(2, SCREEN_H - 10);
        d.print(foot);
    }
}

// ── Scan logic ─────────────────────────────────────────────────────────────

static void runScan() {
    s_count  = 0;
    s_scroll = 0;
    s_scanDone = false;

    drawScanning();

    if (!s_bleInited) {
        NimBLEDevice::init("");
        s_bleInited = true;
    }
    if (!s_bleScan) {
        s_bleScan = NimBLEDevice::getScan();
        s_bleScan->setActiveScan(true);
        s_bleScan->setInterval(100);
        s_bleScan->setWindow(99);
    }
    s_bleScan->setAdvertisedDeviceCallbacks(&s_trkCallbacks);
    s_bleScan->clearResults();
    s_bleScan->start(8, onTrackerScanDone, false);   // 8 sec — AirTags advertise every ~2 sec when separated

    // Sort by RSSI is done in drawResults
    s_state = TrkState::RESULTS;
    s_dirty = true;
}

// ── Public ─────────────────────────────────────────────────────────────────

void appTrackerEnter() {
    s_state  = TrkState::MENU;
    s_dirty  = true;
    s_count  = 0;
    s_scroll = 0;
}

void appTrackerLoop() {
    if (s_scanDone) {
        s_scanDone = false;
        s_dirty = true;
    }

    if (s_dirty) {
        switch (s_state) {
            case TrkState::MENU:    drawMenu();    break;
            case TrkState::RESULTS: drawResults(); break;
        }
        s_dirty = false;
    }

    auto ev = readKeys();
    if (!ev.changed) return;

    switch (s_state) {
        case TrkState::MENU:
            if (ev.back)  { goHome(); return; }
            if (ev.enter) runScan();
            break;

        case TrkState::RESULTS:
            if (ev.up   && s_scroll > 0)           { s_scroll--; s_dirty = true; }
            if (ev.down && s_scroll < s_count - 1) { s_scroll++; s_dirty = true; }
            if (ev.enter) runScan();
            if (ev.back)  { s_state = TrkState::MENU; s_dirty = true; }
            break;
    }
}
