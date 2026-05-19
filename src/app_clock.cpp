#include "app_clock.h"
#include "app_settings.h"
#include "config.h"
#include "input.h"
#include "nav.h"
#include <M5Cardputer.h>
#include <LittleFS.h>
#include <cstdio>
#include <cstring>

enum class CalendarState { VIEW, EDIT_NOTE };

static CalendarState s_state = CalendarState::VIEW;
static bool s_dirty = true;
static int s_year = 2026;
static int s_month = 1;
static int s_day = 1;
static String s_noteText;
static String s_notePath;
static int s_noteCursor = 0;

static const char* MONTH_NAMES[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static constexpr const char* CALENDAR_DIR = "/calendar";

static bool isLeap(int year) {
    return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}

static int daysInMonth(int year, int month) {
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 2) return isLeap(year) ? 29 : 28;
    return days[month - 1];
}

static int dayOfWeek(int year, int month, int day) {
    if (month < 3) {
        month += 12;
        year--;
    }
    int k = year % 100;
    int j = year / 100;
    int h = (day + (13 * (month + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    return (h + 6) % 7; // 0 = Sunday
}

static String notePathFor(int year, int month, int day) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%s/%04d-%02d-%02d.txt", CALENDAR_DIR, year, month, day);
    return String(buf);
}

static bool noteExistsFor(int year, int month, int day) {
    return LittleFS.exists(notePathFor(year, month, day));
}

static void clampDay() {
    int dim = daysInMonth(s_year, s_month);
    if (s_day < 1) s_day = 1;
    if (s_day > dim) s_day = dim;
}

static void loadSelectedNote() {
    s_notePath = notePathFor(s_year, s_month, s_day);
    s_noteText = "";
    s_noteCursor = 0;
    File f = LittleFS.open(s_notePath, "r");
    if (!f) return;
    while (f.available()) s_noteText += (char)f.read();
    f.close();
    s_noteCursor = s_noteText.length();
}

static bool saveSelectedNote() {
    if (s_noteText.length() == 0) {
        if (LittleFS.exists(s_notePath)) LittleFS.remove(s_notePath);
        return true;
    }
    File f = LittleFS.open(s_notePath, "w");
    if (!f) return false;
    f.print(s_noteText);
    f.close();
    return true;
}

static void drawStatus(const char* title) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_STATUS_FG, C_STATUS_BG);
    d.setCursor(2, 3);
    d.print(title);
    drawBatteryWidget(C_STATUS_BG, SCREEN_W - 43);
}

static void drawCalendar() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatus("Calendar");

    char head[32];
    snprintf(head, sizeof(head), "%s %d", MONTH_NAMES[s_month - 1], s_year);
    d.setFont(&fonts::Font0);
    d.setTextColor(C_FG, C_BG);
    d.setTextSize(2);
    int hw = (int)strlen(head) * FONT_W * 2;
    d.setCursor((SCREEN_W - hw) / 2, 18);
    d.print(head);

    static const char* wd[] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
    d.setTextSize(1);
    for (int i = 0; i < 7; ++i) {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(8 + i * 32, 38);
        d.print(wd[i]);
    }

    int firstDow = dayOfWeek(s_year, s_month, 1);
    int dim = daysInMonth(s_year, s_month);
    int startX = 8;
    int startY = 52;
    int cellW = 32;
    int cellH = 12;

    for (int day = 1; day <= dim; ++day) {
        int idx = firstDow + day - 1;
        int col = idx % 7;
        int row = idx / 7;
        int x = startX + col * cellW;
        int y = startY + row * cellH;
        bool sel = (day == s_day);
        bool hasNote = noteExistsFor(s_year, s_month, day);
        char dbuf[4];
        snprintf(dbuf, sizeof(dbuf), "%2d", day);
        uint32_t bg = sel ? C_HIGHLIGHT : C_BG;
        d.fillRect(x - 2, y - 1, 22, 10, bg);
        d.setTextColor(sel ? (uint32_t)C_INPUT : (uint32_t)C_FG, bg);
        d.setCursor(x, y);
        d.print(dbuf);
        if (hasNote) {
            d.fillCircle(x + 18, y + 3, 1, sel ? (uint32_t)C_INPUT : (uint32_t)0x7DFFB2);
        }
    }

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(6, 110);
    d.print("fn+arrows=day  N/P=month");
    d.setCursor(6, 120);
    d.print("Enter=note   Del=clear note  fn+bksp=home");
}

static void drawNoteEditor() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatus("Calendar Note");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_FG, C_BG);

    char title[32];
    snprintf(title, sizeof(title), "%04d-%02d-%02d", s_year, s_month, s_day);
    d.setCursor(8, 20);
    d.print(title);

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(8, 32);
    d.print("Short note:");

    d.fillRect(8, 46, SCREEN_W - 16, 54, C_HIGHLIGHT);
    d.setTextColor(C_INPUT, C_HIGHLIGHT);
    String visible = s_noteText;
    if (visible.length() > 84) visible = visible.substring(visible.length() - 84);
    int x = 12;
    int y = 50;
    for (int i = 0; i < (int)visible.length(); ++i) {
        if ((i % 28) == 0 && i > 0) {
            x = 12;
            y += FONT_H + 2;
        }
        char ch[2] = { visible[i], 0 };
        d.setCursor(x, y);
        d.print(ch);
        x += FONT_W;
    }
    int visLen = visible.length();
    int cx = 12 + (visLen % 28) * FONT_W;
    int cy = 50 + (visLen / 28) * (FONT_H + 2);
    if (cy <= 90) d.fillRect(cx, cy, FONT_W, FONT_H, C_CURSOR);

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(8, 108);
    d.print("Type note  Enter=save  bksp=cancel");
    d.setCursor(8, 120);
    d.print("Del=erase char");
}

void appClockEnter() {
    s_year = settingsCalendarYear();
    s_month = settingsCalendarMonth();
    s_day = settingsCalendarDay();
    clampDay();
    if (!LittleFS.exists(CALENDAR_DIR)) LittleFS.mkdir(CALENDAR_DIR);
    s_state = CalendarState::VIEW;
    s_dirty = true;
}

void appClockLoop() {
    if (s_dirty) {
        if (s_state == CalendarState::VIEW) drawCalendar();
        else drawNoteEditor();
        s_dirty = false;
    }

    auto ev = readKeys();
    if (!ev.changed) return;

    if (s_state == CalendarState::EDIT_NOTE) {
        if (ev.back) {
            s_state = CalendarState::VIEW;
            s_dirty = true;
            return;
        }
        if (ev.del && s_noteText.length() > 0) {
            s_noteText.remove(s_noteText.length() - 1);
            s_noteCursor = s_noteText.length();
            s_dirty = true;
            return;
        }
        if (ev.enter) {
            saveSelectedNote();
            s_state = CalendarState::VIEW;
            s_dirty = true;
            return;
        }
        if (!ev.fnKey) {
            for (char c : ev.chars) {
                if (c >= 32 && c <= 126 && s_noteText.length() < 120) {
                    s_noteText += c;
                    s_noteCursor = s_noteText.length();
                    s_dirty = true;
                }
            }
        }
        return;
    }

    if (ev.back) { goHome(); return; }
    if (ev.fnKey) {
        int firstDow = dayOfWeek(s_year, s_month, 1);
        int idx = firstDow + s_day - 1;
        int dim = daysInMonth(s_year, s_month);
        if (ev.left && s_day > 1) { s_day--; s_dirty = true; return; }
        if (ev.right && s_day < dim) { s_day++; s_dirty = true; return; }
        if (ev.up && idx - 7 >= firstDow) { s_day -= 7; s_dirty = true; return; }
        if (ev.down && idx + 7 < firstDow + dim) { s_day += 7; s_dirty = true; return; }
    }

    if (!ev.fnKey) {
        for (char c : ev.chars) {
            if (c == 'n' || c == 'N') {
                s_month++;
                if (s_month > 12) {
                    s_month = 1;
                    s_year++;
                }
                clampDay();
                s_dirty = true;
                return;
            }
            if (c == 'p' || c == 'P') {
                s_month--;
                if (s_month < 1) {
                    s_month = 12;
                    s_year--;
                }
                clampDay();
                s_dirty = true;
                return;
            }
        }
    }

    if (ev.del) {
        String path = notePathFor(s_year, s_month, s_day);
        if (LittleFS.exists(path)) LittleFS.remove(path);
        s_dirty = true;
        return;
    }
    if (ev.enter) {
        loadSelectedNote();
        s_state = CalendarState::EDIT_NOTE;
        s_dirty = true;
        return;
    }
}
