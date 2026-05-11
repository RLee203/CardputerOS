#include "wifi_mgr.h"
#include "config.h"
#include <WiFi.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

WifiManager WifiMgr;

bool WifiManager::begin() {
    if (!LittleFS.exists(WIFI_PATH)) return false;
    File f = LittleFS.open(WIFI_PATH, "r");
    if (!f) return false;
    JsonDocument doc;
    if (deserializeJson(doc, f)) { f.close(); return false; }
    f.close();

    _count = 0;

    // Migrate old single-network format: {"ssid":"...", "pass":"..."}
    if (doc["ssid"].is<const char*>()) {
        const char* s = doc["ssid"] | "";
        const char* p = doc["pass"] | "";
        if (s[0]) {
            _nets[0].ssid = s;
            _nets[0].pass = p;
            _count = 1;
            save();
        }
        return _count > 0;
    }

    // New format: {"nets":[{"s":"...","p":"..."},...]}
    JsonArray arr = doc["nets"].as<JsonArray>();
    for (JsonObject obj : arr) {
        if (_count >= MAX_NETS) break;
        const char* s = obj["s"] | "";
        const char* p = obj["p"] | "";
        if (s[0]) {
            _nets[_count].ssid = s;
            _nets[_count].pass = p;
            _count++;
        }
    }
    return _count > 0;
}

bool WifiManager::save() {
    JsonDocument doc;
    JsonArray arr = doc["nets"].to<JsonArray>();
    for (int i = 0; i < _count; i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["s"] = _nets[i].ssid;
        obj["p"] = _nets[i].pass;
    }
    File f = LittleFS.open(WIFI_PATH, "w");
    if (!f) return false;
    serializeJson(doc, f);
    f.close();
    return true;
}

WifiState WifiManager::connect(int idx) {
    if (idx < 0 || idx >= _count) return WifiState::FAILED;
    return connect(_nets[idx].ssid, _nets[idx].pass);
}

WifiState WifiManager::connect(const String& ssid, const String& pass) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(200);
        if (millis() - t > WIFI_TIMEOUT_MS) return WifiState::FAILED;
    }
    return WifiState::CONNECTED;
}

void WifiManager::addNet(const String& ssid, const String& pass) {
    // Update password if SSID already saved
    for (int i = 0; i < _count; i++) {
        if (_nets[i].ssid == ssid) {
            _nets[i].pass = pass;
            save();
            return;
        }
    }
    // If full, evict oldest entry
    if (_count >= MAX_NETS) {
        for (int i = 0; i < MAX_NETS - 1; i++) _nets[i] = _nets[i + 1];
        _count = MAX_NETS - 1;
    }
    _nets[_count].ssid = ssid;
    _nets[_count].pass = pass;
    _count++;
    save();
}

void WifiManager::removeNet(int idx) {
    if (idx < 0 || idx >= _count) return;
    for (int i = idx; i < _count - 1; i++) _nets[i] = _nets[i + 1];
    _count--;
    save();
}

void WifiManager::disconnect() {
    WiFi.disconnect(false);
}

WifiState WifiManager::state() const {
    return (WiFi.status() == WL_CONNECTED) ? WifiState::CONNECTED : WifiState::IDLE;
}

String WifiManager::localIP() const {
    return WiFi.localIP().toString();
}
