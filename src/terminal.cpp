#include "terminal.h"
#include <M5Cardputer.h>
#include <cstdarg>

static auto& disp = M5Cardputer.Display;

// ── Private helpers ────────────────────────────────────────────────────────

void Terminal::renderChar(int row, int col) {
    disp.setTextColor(C_FG, C_BG);
    disp.setCursor(charX(col), charY(row));
    disp.print(_buf[row][col]);
}

void Terminal::renderRow(int row) {
    int y = charY(row);
    disp.fillRect(0, y, SCREEN_W, FONT_H, C_BG);
    disp.setTextColor(C_FG, C_BG);
    disp.setCursor(0, y);
    disp.print(_buf[row]);
}

void Terminal::scrollUp() {
    for (int r = 0; r < TERM_ROWS - 1; r++) {
        memcpy(_buf[r], _buf[r + 1], TERM_COLS + 1);
    }
    memset(_buf[TERM_ROWS - 1], ' ', TERM_COLS);
    _buf[TERM_ROWS - 1][TERM_COLS] = '\0';
    // Redraw whole terminal area
    for (int r = 0; r < TERM_ROWS; r++) renderRow(r);
}

void Terminal::renderCursor() {
    int x = charX(_col);
    int y = charY(_row);
    disp.fillRect(x, y, FONT_W, FONT_H, C_CURSOR);
    _cursorVisible = true;
}

void Terminal::clearCursor() {
    int x = charX(_col);
    int y = charY(_row);
    disp.fillRect(x, y, FONT_W, FONT_H, C_BG);
    // Restore any character that was under the cursor
    char c = _buf[_row][_col];
    if (c != ' ' && c != '\0') {
        disp.setTextColor(C_FG, C_BG);
        disp.setCursor(x, y);
        disp.print(c);
    }
    _cursorVisible = false;
}

void Terminal::renderInputLine() {
    int y = charY(_row);
    int startCol = _col;
    disp.fillRect(charX(startCol), y, SCREEN_W - charX(startCol), FONT_H, C_BG);
    disp.setTextColor(C_INPUT, C_BG);
    for (int i = 0; i < (int)_inputLine.length(); i++) {
        int col = startCol + i;
        if (col >= TERM_COLS) break;
        disp.setCursor(charX(col), y);
        disp.print(_inputLine[i]);
    }
    // Draw input cursor
    int icol = startCol + _inputCursorPos;
    if (icol < TERM_COLS) {
        disp.fillRect(charX(icol), y, FONT_W, FONT_H, C_INPUT);
    }
    _inputDirty = false;
}

// ── Public API ─────────────────────────────────────────────────────────────

void Terminal::begin() {
    disp.setFont(&fonts::Font0);
    disp.setTextSize(1);
    clear();
}

void Terminal::clear() {
    memset(_buf, ' ', sizeof(_buf));
    for (int r = 0; r < TERM_ROWS; r++) _buf[r][TERM_COLS] = '\0';
    _row = 0; _col = 0;
    _inEsc = false; _escLen = 0;
    disp.fillRect(0, TERM_Y, SCREEN_W, TERM_H, C_BG);
}

void Terminal::write(char c) {
    // ANSI escape accumulation
    if (_inEsc) {
        if (_escLen < (int)sizeof(_escBuf) - 1) _escBuf[_escLen++] = c;
        // Escape sequence ends on a letter
        if (isalpha(c)) {
            _escBuf[_escLen] = '\0';
            _inEsc = false; _escLen = 0;
            // Handle clear-screen: ESC[2J → terminal clear
            if (c == 'J' && strstr(_escBuf, "2J")) clear();
        }
        return;
    }
    if (c == '\033') { _inEsc = true; _escLen = 0; return; }

    switch (c) {
        case '\r':
            _col = 0;
            break;
        case '\n':
            _col = 0;
            _row++;
            if (_row >= TERM_ROWS) { scrollUp(); _row = TERM_ROWS - 1; }
            break;
        case '\b': case 127:
            if (_col > 0) {
                _col--;
                _buf[_row][_col] = ' ';
                renderChar(_row, _col);
            }
            break;
        default:
            if ((uint8_t)c >= 0x20 && (uint8_t)c < 0x7F) {
                _buf[_row][_col] = c;
                renderChar(_row, _col);
                _col++;
                if (_col >= TERM_COLS) {
                    _col = 0; _row++;
                    if (_row >= TERM_ROWS) { scrollUp(); _row = TERM_ROWS - 1; }
                }
            }
            break;
    }
}

void Terminal::write(const char* data, size_t len) {
    for (size_t i = 0; i < len; i++) write(data[i]);
}

void Terminal::print(const char* str) {
    while (*str) write(*str++);
}

void Terminal::println(const char* str) {
    print(str);
    write('\n');
}

void Terminal::printf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    print(buf);
}

void Terminal::setInputLine(const String& line, int cursorPos) {
    _inputLine       = line;
    _inputCursorPos  = cursorPos;
    _inputDirty      = true;
}

void Terminal::update() {
    if (_inputDirty) renderInputLine();

    unsigned long now = millis();
    if (now - _cursorTimer >= CURSOR_BLINK_MS) {
        _cursorTimer = now;
        if (_cursorVisible) clearCursor();
        else                renderCursor();
    }
}

void Terminal::drawStatusBar(const char* left, const char* right) {
    disp.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS_BG);
    disp.setTextColor(C_STATUS_FG, C_STATUS_BG);
    disp.setCursor(2, 3);
    disp.print(left);
    if (right && *right) {
        int rw = strlen(right) * FONT_W;
        disp.setCursor(SCREEN_W - rw - 2, 3);
        disp.print(right);
    }
}
