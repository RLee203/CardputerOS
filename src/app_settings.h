#pragma once
#include <Arduino.h>
#include "nav.h"

void          appSettingsEnter();
void          appSettingsLoop();
void          settingsLoadFromFS();   // call once at boot after LittleFS is mounted
bool          settingsLockEnabled();
const String& settingsLockPin();
DeviceMode    settingsBootMode();
void          settingsSetBootMode(DeviceMode mode);
int           settingsCalendarYear();
int           settingsCalendarMonth();
int           settingsCalendarDay();
void          settingsSetCalendarDate(int year, int month, int day);
