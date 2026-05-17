#pragma once
#include <stdint.h>

void appHidKeyboardEnter();
void appHidKeyboardLoop();

// Shared USB HID API used by other apps (e.g. Payloads).
// Only one app uses these at a time; USB HID stays active until reboot.
bool hidKbEnsureEnabled();
bool hidKbIsEnabled();
void hidKbTypeString(const char* s);
void hidKbPressRelease(uint8_t key);
void hidKbModKey(uint8_t mod, uint8_t key);
void hidKbMod2Key(uint8_t mod1, uint8_t mod2, uint8_t key);
void hidKbReleaseAll();
