#pragma once
#include <Arduino.h>

void          appSettingsEnter();
void          appSettingsLoop();
void          settingsLoadFromFS();   // call once at boot after LittleFS is mounted
bool          settingsLockEnabled();
const String& settingsLockPin();
