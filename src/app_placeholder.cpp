#include "app_placeholder.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>

static const char* g_placeholderTitle = "Feature";
static bool g_placeholderDirty = true;

void appPlaceholderEnter(const char* title) {
    g_placeholderTitle = title ? title : "Feature";
    g_placeholderDirty = true;
}

void appPlaceholderLoop() {
    auto& d = M5Cardputer.Display;
    if (g_placeholderDirty) {
        d.fillScreen(C_BG);
        d.setFont(&fonts::Font0);
        d.setTextSize(1);
        d.setTextColor(C_FG, C_BG);
        d.drawRect(0, 0, SCREEN_W, SCREEN_H, C_FG);

        int tw = strlen(g_placeholderTitle) * FONT_W;
        d.setCursor((SCREEN_W - tw) / 2, 6);
        d.print(g_placeholderTitle);
        d.drawFastHLine(0, 14, SCREEN_W, C_FG);

        d.setTextColor(C_ACCENT, C_BG);
        d.setCursor(64, 42);
        d.print("COMING SOON");

        d.setTextColor(C_DIM, C_BG);
        d.setCursor(35, 64);
        d.print("This feature is planned.");
        d.setCursor(35, 76);
        d.print("UI is ready for wiring.");
        d.setCursor(24, SCREEN_H - FONT_H - 8);
        d.print("Tab pages launcher  fn+bksp home");

        drawBatteryWidget(C_BG, SCREEN_W - 43);
        g_placeholderDirty = false;
    }

    auto ev = readKeys();
    if (!ev.changed) return;

    if (ev.back || ev.tab) {
        goHome();
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
}
