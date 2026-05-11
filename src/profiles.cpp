#include "profiles.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

ProfileManager Profiles;

bool ProfileManager::begin() {
    if (!LittleFS.begin(true)) return false;
    return load();
}

bool ProfileManager::load() {
    _count = 0;
    if (!LittleFS.exists(PROFILES_PATH)) return true; // no file yet is fine

    File f = LittleFS.open(PROFILES_PATH, "r");
    if (!f) return false;

    JsonDocument doc;
    if (deserializeJson(doc, f)) { f.close(); return false; }
    f.close();

    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject obj : arr) {
        if (_count >= MAX_PROFILES) break;
        _profiles[_count++] = {
            .name = obj["name"] | "unnamed",
            .host = obj["host"] | "",
            .port = obj["port"] | SSH_DEFAULT_PORT,
            .user = obj["user"] | "root",
            .pass = obj["pass"] | "",
        };
    }
    return true;
}

bool ProfileManager::save() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < _count; i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["name"] = _profiles[i].name;
        obj["host"] = _profiles[i].host;
        obj["port"] = _profiles[i].port;
        obj["user"] = _profiles[i].user;
        obj["pass"] = _profiles[i].pass;
    }
    File f = LittleFS.open(PROFILES_PATH, "w");
    if (!f) return false;
    serializeJson(doc, f);
    f.close();
    return true;
}

int ProfileManager::count() const { return _count; }

const Profile& ProfileManager::get(int idx) const { return _profiles[idx]; }

bool ProfileManager::add(const Profile& p) {
    if (_count >= MAX_PROFILES) return false;
    _profiles[_count++] = p;
    return save();
}

bool ProfileManager::remove(int idx) {
    if (idx < 0 || idx >= _count) return false;
    for (int i = idx; i < _count - 1; i++) _profiles[i] = _profiles[i + 1];
    _count--;
    return save();
}
