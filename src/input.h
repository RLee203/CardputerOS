#pragma once
#include <Arduino.h>

struct KeyEvent {
    bool changed;
    bool enter, del, tab, fnKey;
    bool up, down, left, right;
    bool back;   // fn+backspace — universal "go home/back"
    String chars;
};

KeyEvent readKeys();
void     resetSleepTimer();  // call when non-keyboard activity should prevent sleep
bool     screenJustWoke();   // true once per wake event — cleared after first read
