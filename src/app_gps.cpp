#include "app_gps.h"

#include <M5Cardputer.h>
#include <SD.h>
#include <TinyGPSPlus.h>
#include <WiFi.h>
#include "config.h"
#include "input.h"
#include "nav.h"

namespace {

enum class GpsView { MENU, STATUS, TRACKER, WARDRIVING };

struct MenuItem {
    const char* label;
    GpsView view;
};

constexpr MenuItem MENU_ITEMS[] = {
    {"Status", GpsView::STATUS},
    {"Tracker", GpsView::TRACKER},
    {"Wardriving", GpsView::WARDRIVING},
};
constexpr int MENU_COUNT = sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]);

HardwareSerial gpsSerial(2);
TinyGPSPlus gps;
constexpr uint32_t GPS_BAUD_OPTIONS[] = {115200, 9600, 38400};
constexpr int GPS_BAUD_COUNT = sizeof(GPS_BAUD_OPTIONS) / sizeof(GPS_BAUD_OPTIONS[0]);

GpsView g_view = GpsView::MENU;
int g_menuSel = 0;
bool g_dirty = true;
bool g_gpsStarted = false;
bool g_trackerRunning = false;
bool g_trackerNeedsHeader = false;
String g_trackerFile;
bool g_wardrivingRunning = false;
String g_wardriveFile;
uint32_t g_lastWardriveScanMs = 0;
uint32_t g_wardriveRows = 0;
uint32_t g_wardriveNetworks = 0;
uint32_t g_lastUiUpdate = 0;
uint32_t g_lastSampleMs = 0;
double g_lastTrackLat = 0.0;
double g_lastTrackLon = 0.0;
double g_distanceMeters = 0.0;
uint32_t g_trackPoints = 0;
bool g_haveTrackPoint = false;
int g_baudIndex = 0;
uint32_t g_bytesSeen = 0;
uint32_t g_sentencesSeen = 0;
uint32_t g_lastByteMs = 0;
String g_lastNmeaLine;
String g_lineBuf;
GpsView g_lastDrawnView = GpsView::MENU;
bool g_staticFrameDirty = true;

void markDirty() { g_dirty = true; }

bool ensureGpsStarted() {
    if (g_gpsStarted) return true;
    gpsSerial.begin(GPS_BAUD_OPTIONS[g_baudIndex], SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    g_gpsStarted = true;
    return true;
}

void restartGps() {
    if (g_gpsStarted) gpsSerial.end();
    gps = TinyGPSPlus();
    g_bytesSeen = 0;
    g_sentencesSeen = 0;
    g_lastByteMs = 0;
    g_lastNmeaLine = "";
    g_lineBuf = "";
    g_gpsStarted = false;
    ensureGpsStarted();
    g_staticFrameDirty = true;
    markDirty();
}

void serviceGps() {
    if (!g_gpsStarted) return;
    while (gpsSerial.available() > 0) {
        char ch = static_cast<char>(gpsSerial.read());
        ++g_bytesSeen;
        g_lastByteMs = millis();
        if (ch == '\n') {
            g_lastNmeaLine = g_lineBuf;
            g_lineBuf = "";
            if (g_lastNmeaLine.length()) ++g_sentencesSeen;
        } else if (ch != '\r') {
            if (g_lineBuf.length() < 80) g_lineBuf += ch;
        }
        gps.encode(ch);
    }
}

String trackerFilename() {
    if (gps.date.isValid() && gps.time.isValid()) {
        char buf[40];
        snprintf(
            buf,
            sizeof(buf),
            "/track_%02d%02d%02d_%02d%02d%02d.gpx",
            gps.date.year() % 100,
            gps.date.month(),
            gps.date.day(),
            gps.time.hour(),
            gps.time.minute(),
            gps.time.second());
        return String(GPS_DIR) + buf;
    }

    char fallback[40];
    snprintf(fallback, sizeof(fallback), "/track_%08lu.gpx", millis());
    return String(GPS_DIR) + fallback;
}

String wardriveFilename() {
    if (gps.date.isValid() && gps.time.isValid()) {
        char buf[40];
        snprintf(
            buf,
            sizeof(buf),
            "/wardrive_%02d%02d%02d_%02d%02d%02d.csv",
            gps.date.year() % 100,
            gps.date.month(),
            gps.date.day(),
            gps.time.hour(),
            gps.time.minute(),
            gps.time.second());
        return String(GPS_DIR) + buf;
    }

    char fallback[40];
    snprintf(fallback, sizeof(fallback), "/wardrive_%08lu.csv", millis());
    return String(GPS_DIR) + fallback;
}

void writeTrackerHeader(File& file) {
    file.println("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    file.println("<gpx version=\"1.1\" creator=\"CardputerOS\" xmlns=\"http://www.topografix.com/GPX/1/1\">");
    file.println("  <trk>");
    file.println("    <name>Cardputer GPS Track</name>");
    file.println("    <trkseg>");
}

void writeTrackerFooter(File& file) {
    file.println("    </trkseg>");
    file.println("  </trk>");
    file.println("</gpx>");
}

void stopTracker() {
    if (!g_trackerRunning) return;
    digitalWrite(LORA_NSS_PIN, HIGH);
    digitalWrite(LORA_RST_PIN, LOW);
    if (SD.begin(SD_CS_PIN, SPI, 25000000)) {
        File file = SD.open(g_trackerFile, FILE_APPEND);
        if (file) {
            writeTrackerFooter(file);
            file.close();
        }
    }
    g_trackerRunning = false;
    g_trackerNeedsHeader = false;
    markDirty();
}

void startTracker() {
    digitalWrite(LORA_NSS_PIN, HIGH);
    digitalWrite(LORA_RST_PIN, LOW);
    if (!SD.begin(SD_CS_PIN, SPI, 25000000)) return;
    if (!SD.exists(GPS_DIR)) SD.mkdir(GPS_DIR);

    g_trackerFile = trackerFilename();
    File file = SD.open(g_trackerFile, FILE_WRITE);
    if (!file) return;
    writeTrackerHeader(file);
    file.close();

    g_trackerRunning = true;
    g_trackerNeedsHeader = false;
    g_distanceMeters = 0.0;
    g_trackPoints = 0;
    g_haveTrackPoint = false;
    g_lastSampleMs = 0;
    markDirty();
}

void stopWardriving() {
    if (!g_wardrivingRunning) return;
    g_wardrivingRunning = false;
    WiFi.scanDelete();
    markDirty();
}

void startWardriving() {
    digitalWrite(LORA_NSS_PIN, HIGH);
    digitalWrite(LORA_RST_PIN, LOW);
    if (!SD.begin(SD_CS_PIN, SPI, 25000000)) return;

    if (!SD.exists(GPS_DIR)) SD.mkdir(GPS_DIR);
    g_wardriveFile = wardriveFilename();
    File file = SD.open(g_wardriveFile, FILE_WRITE);
    if (!file) { g_wardriveFile = ""; return; }
    file.println("time_utc,ssid,bssid,rssi,channel,encryption,latitude,longitude,altitude_m,satellites");
    file.close();

    // Full WiFi radio reset before scanning.
    WiFi.mode(WIFI_OFF);
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(300);

    g_wardrivingRunning = true;
    g_lastWardriveScanMs = 0;
    g_wardriveRows = 0;
    g_wardriveNetworks = 0;
    markDirty();
}

String csvEscape(const String& in) {
    String out = "\"";
    for (size_t i = 0; i < in.length(); ++i) {
        char c = in[i];
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += "\"";
    return out;
}

String encryptionName(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN: return "OPEN";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        default: return "UNKNOWN";
    }
}

void runWardrivingScan() {
    if (!g_wardrivingRunning) return;
    uint32_t now = millis();
    if (now - g_lastWardriveScanMs < 5000) return;
    g_lastWardriveScanMs = now;

    // Synchronous scan — ~2 s blocking with 150 ms/chan across 13 channels.
    // GPS hardware buffers NMEA sentences so we don't lose fixes during this.
    int found = WiFi.scanNetworks(false, true, false, 150);
    if (found <= 0) { WiFi.scanDelete(); markDirty(); return; }

    // Count networks found regardless of whether the SD write succeeds
    g_wardriveNetworks += found;

    File file = SD.open(g_wardriveFile, FILE_APPEND);
    if (file) {
        String utc = "";
        if (gps.date.isValid() && gps.time.isValid()) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                gps.date.year(), gps.date.month(), gps.date.day(),
                gps.time.hour(), gps.time.minute(), gps.time.second());
            utc = buf;
        }
        bool hasLoc = gps.location.isValid();
        for (int i = 0; i < found; ++i) {
            file.print(csvEscape(utc));                                           file.print(',');
            file.print(csvEscape(WiFi.SSID(i)));                                  file.print(',');
            file.print(csvEscape(WiFi.BSSIDstr(i)));                              file.print(',');
            file.print(WiFi.RSSI(i));                                             file.print(',');
            file.print(WiFi.channel(i));                                          file.print(',');
            file.print(csvEscape(encryptionName(WiFi.encryptionType(i))));        file.print(',');
            if (hasLoc) {
                file.print(String(gps.location.lat(), 6));
                file.print(',');
                file.print(String(gps.location.lng(), 6));
            } else {
                file.print(',');  // empty lat, empty lon when no fix
            }
            file.print(',');
            file.print(gps.altitude.isValid() ? String(gps.altitude.meters(), 1) : "");
            file.print(',');
            file.println(gps.satellites.isValid() ? String(gps.satellites.value()) : "");
            ++g_wardriveRows;
        }
        file.close();
    }

    WiFi.scanDelete();
    markDirty();
}

void logTrackerPoint() {
    if (!g_trackerRunning || !gps.location.isValid()) return;

    const uint32_t now = millis();
    if (now - g_lastSampleMs < 1500) return;
    g_lastSampleMs = now;

    double lat = gps.location.lat();
    double lon = gps.location.lng();
    if (g_haveTrackPoint) {
        g_distanceMeters += TinyGPSPlus::distanceBetween(g_lastTrackLat, g_lastTrackLon, lat, lon);
    }
    g_lastTrackLat = lat;
    g_lastTrackLon = lon;
    g_haveTrackPoint = true;

    digitalWrite(LORA_NSS_PIN, HIGH);
    digitalWrite(LORA_RST_PIN, LOW);
    if (!SD.begin(SD_CS_PIN, SPI, 25000000)) return;
    File file = SD.open(g_trackerFile, FILE_APPEND);
    if (!file) return;

    file.printf("      <trkpt lat=\"%.6f\" lon=\"%.6f\">", lat, lon);
    if (gps.altitude.isValid()) file.printf("<ele>%.2f</ele>", gps.altitude.meters());
    if (gps.date.isValid() && gps.time.isValid()) {
        char iso[32];
        snprintf(
            iso,
            sizeof(iso),
            "%04d-%02d-%02dT%02d:%02d:%02dZ",
            gps.date.year(),
            gps.date.month(),
            gps.date.day(),
            gps.time.hour(),
            gps.time.minute(),
            gps.time.second());
        file.printf("<time>%s</time>", iso);
    }
    file.println("</trkpt>");
    file.close();

    ++g_trackPoints;
}

void drawFrame(const char* title) {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    d.drawRect(0, 0, SCREEN_W, SCREEN_H, C_FG);
    d.drawFastHLine(0, 14, SCREEN_W, C_FG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_FG, C_BG);
    int tw = strlen(title) * FONT_W;
    d.setCursor((SCREEN_W - tw) / 2, 4);
    d.print(title);
    drawBatteryWidget(C_BG, SCREEN_W - 43);
}

void drawMenu() {
    auto& d = M5Cardputer.Display;
    if (g_staticFrameDirty || g_lastDrawnView != GpsView::MENU) {
        drawFrame("GPS");
        g_lastDrawnView = GpsView::MENU;
        g_staticFrameDirty = false;
    }

    d.fillRect(0, 15, SCREEN_W, SCREEN_H - 15, C_BG);

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(12, 22);
    d.print("Choose a GPS tool");

    for (int i = 0; i < MENU_COUNT; ++i) {
        int y = 42 + i * 22;
        bool sel = i == g_menuSel;
        uint32_t bg = sel ? C_HIGHLIGHT : C_BG;
        uint32_t fg = sel ? C_INPUT : C_FG;
        d.fillRoundRect(14, y - 2, 212, 16, 4, bg);
        d.setTextColor(fg, bg);
        d.setCursor(24, y);
        d.print(MENU_ITEMS[i].label);
    }

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(18, SCREEN_H - 12);
    d.print("Enter open  Tab home  fn+bksp home");
}

void printRow(int y, const String& label, const String& value, uint32_t col = C_FG) {
    auto& d = M5Cardputer.Display;
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(12, y);
    d.print(label);
    d.setTextColor(col, C_BG);
    d.setCursor(88, y);
    d.print(value);
}

String fmtCoord(double val) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%.6f", val);
    return String(buf);
}

void drawStatus() {
    auto& d = M5Cardputer.Display;
    if (g_staticFrameDirty || g_lastDrawnView != GpsView::STATUS) {
        drawFrame("GPS Status");
        g_lastDrawnView = GpsView::STATUS;
        g_staticFrameDirty = false;
    }

    d.fillRect(8, 20, SCREEN_W - 16, SCREEN_H - 20, C_BG);

    String fix = gps.location.isValid() ? "FIX" : "SEARCHING";
    uint32_t fixColor = gps.location.isValid() ? 0x33CC66 : 0xCCAA00;
    printRow(24, "State", fix, fixColor);
    printRow(36, "Satellites", gps.satellites.isValid() ? String(gps.satellites.value()) : "--");
    printRow(48, "Latitude", gps.location.isValid() ? fmtCoord(gps.location.lat()) : "--");
    printRow(60, "Longitude", gps.location.isValid() ? fmtCoord(gps.location.lng()) : "--");
    printRow(72, "Altitude", gps.altitude.isValid() ? String(gps.altitude.meters(), 1) + " m" : "--");
    printRow(84, "Speed", gps.speed.isValid() ? String(gps.speed.kmph(), 1) + " km/h" : "--");

    String utc = "--";
    if (gps.time.isValid()) {
        char buf[20];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", gps.time.hour(), gps.time.minute(), gps.time.second());
        utc = buf;
    }
    printRow(96, "UTC", utc);
    printRow(108, "Link", g_sentencesSeen > 0 ? "CONNECTED" : "WAITING", g_sentencesSeen > 0 ? 0x33CC66 : 0xCCAA00);

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(18, SCREEN_H - 12);
    d.print("Enter menu  fn+bksp home");
}

void drawTracker() {
    auto& d = M5Cardputer.Display;
    if (g_staticFrameDirty || g_lastDrawnView != GpsView::TRACKER) {
        drawFrame("GPS Tracker");
        g_lastDrawnView = GpsView::TRACKER;
        g_staticFrameDirty = false;
    }

    d.fillRect(8, 20, SCREEN_W - 16, SCREEN_H - 20, C_BG);

    printRow(24, "State", g_trackerRunning ? "RECORDING" : "IDLE", g_trackerRunning ? 0xFF5555 : C_FG);
    printRow(36, "Fix", gps.location.isValid() ? "READY" : "WAITING", gps.location.isValid() ? 0x33CC66 : 0xCCAA00);
    printRow(48, "Points", String(g_trackPoints));
    printRow(60, "Distance", String(g_distanceMeters / 1000.0, 2) + " km");
    printRow(72, "File", g_trackerFile.length() ? g_trackerFile.substring(g_trackerFile.lastIndexOf('/') + 1) : "--");
    printRow(84, "Latitude", gps.location.isValid() ? fmtCoord(gps.location.lat()) : "--");
    printRow(96, "Longitude", gps.location.isValid() ? fmtCoord(gps.location.lng()) : "--");

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(14, SCREEN_H - 12);
    d.print("Enter start/stop  Del reset  fn+bksp home");
}

void drawWardriving() {
    auto& d = M5Cardputer.Display;
    if (g_staticFrameDirty || g_lastDrawnView != GpsView::WARDRIVING) {
        drawFrame("Wardriving");
        g_lastDrawnView = GpsView::WARDRIVING;
        g_staticFrameDirty = false;
    }

    d.fillRect(8, 20, SCREEN_W - 16, SCREEN_H - 20, C_BG);

    printRow(24, "State", g_wardrivingRunning ? "LOGGING" : "IDLE", g_wardrivingRunning ? 0x33CC66 : C_FG);
    printRow(36, "Fix", gps.location.isValid() ? "READY" : "WAITING", gps.location.isValid() ? 0x33CC66 : 0xCCAA00);
    printRow(48, "Rows", String(g_wardriveRows));
    printRow(60, "Networks", String(g_wardriveNetworks));
    printRow(72, "Lat", gps.location.isValid() ? fmtCoord(gps.location.lat()) : "--");
    printRow(84, "Lon", gps.location.isValid() ? fmtCoord(gps.location.lng()) : "--");
    printRow(96, "File", g_wardriveFile.length() ? g_wardriveFile.substring(g_wardriveFile.lastIndexOf('/') + 1) : "--");

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(14, SCREEN_H - 12);
    d.print("Enter start/stop  Del reset  fn+bksp");
}

void drawCurrent() {
    switch (g_view) {
        case GpsView::MENU: drawMenu(); break;
        case GpsView::STATUS: drawStatus(); break;
        case GpsView::TRACKER: drawTracker(); break;
        case GpsView::WARDRIVING: drawWardriving(); break;
    }
    g_dirty = false;
}

void resetTrackerStats() {
    g_distanceMeters = 0.0;
    g_trackPoints = 0;
    g_haveTrackPoint = false;
    g_trackerFile = "";
    g_lastSampleMs = 0;
    markDirty();
}

}  // namespace

void appGpsEnter() {
    ensureGpsStarted();
    g_view = GpsView::MENU;
    g_menuSel = 0;
    g_staticFrameDirty = true;
    markDirty();
}

void appGpsLoop() {
    serviceGps();
    if (g_trackerRunning) logTrackerPoint();
    if (g_wardrivingRunning) runWardrivingScan();

    auto ev = readKeys();
    if (ev.changed) {
        if (ev.back) {
            if (g_view == GpsView::MENU) {
                goHome();
                return;
            }
            g_view = GpsView::MENU;
            g_staticFrameDirty = true;
            markDirty();
            return;
        }

        if (ev.fnKey) {
            for (char c : ev.chars) {
                if (c == 'q' || c == 'Q') {
                    goHome();
                    return;
                }
            }
        }

        switch (g_view) {
            case GpsView::MENU:
                if (ev.up && g_menuSel > 0) {
                    --g_menuSel;
                    markDirty();
                }
                if (ev.down && g_menuSel < MENU_COUNT - 1) {
                    ++g_menuSel;
                    markDirty();
                }
                if (ev.enter) {
                    g_view = MENU_ITEMS[g_menuSel].view;
                    g_staticFrameDirty = true;
                    markDirty();
                }
                if (ev.tab) {
                    goHome();
                    return;
                }
                break;

            case GpsView::STATUS:
                if (ev.enter || ev.tab) {
                    g_view = GpsView::MENU;
                    g_staticFrameDirty = true;
                    markDirty();
                }
                break;

            case GpsView::TRACKER:
                if (ev.enter) {
                    if (g_trackerRunning) stopTracker();
                    else startTracker();
                }
                if (ev.del) {
                    if (g_trackerRunning) stopTracker();
                    resetTrackerStats();
                }
                if (ev.tab) {
                    g_view = GpsView::MENU;
                    g_staticFrameDirty = true;
                    markDirty();
                }
                break;

            case GpsView::WARDRIVING:
                if (ev.enter) {
                    if (g_wardrivingRunning) stopWardriving();
                    else startWardriving();
                }
                if (ev.del) {
                    if (g_wardrivingRunning) stopWardriving();
                    g_wardriveRows = 0;
                    g_wardriveNetworks = 0;
                    g_wardriveFile = "";
                    markDirty();
                }
                if (ev.tab) {
                    g_view = GpsView::MENU;
                    g_staticFrameDirty = true;
                    markDirty();
                }
                break;
        }
    }

    uint32_t now = millis();
    if (g_dirty || now - g_lastUiUpdate >= 500) {
        drawCurrent();
        g_lastUiUpdate = now;
    }
}
