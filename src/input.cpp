#include "input.h"
#include <M5Cardputer.h>

KeyEvent readKeys() {
    KeyEvent ev = {};
    M5Cardputer.update();
    if (!M5Cardputer.Keyboard.isChange()) return ev;
    if (!M5Cardputer.Keyboard.isPressed()) return ev;

    ev.changed = true;
    auto s = M5Cardputer.Keyboard.keysState();
    ev.enter = s.enter;
    ev.del   = s.del;
    ev.tab   = s.tab;
    ev.fnKey = s.fn;
    for (char c : s.word) ev.chars += c;

    if (s.fn) {
        if (s.del) {
            ev.back = true;   // fn+backspace = go home/back
            ev.del  = false;  // don't also delete text
        }
        for (char c : s.word) {
            if (c == ';') { ev.up    = true; ev.chars = ""; }
            if (c == '.') { ev.down  = true; ev.chars = ""; }
            if (c == ',') { ev.left  = true; ev.chars = ""; }
            if (c == '/') { ev.right = true; ev.chars = ""; }
        }
    }
    return ev;
}
