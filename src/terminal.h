#pragma once
#include <Arduino.h>
#include "config.h"

// Minimal VT100 terminal renderer.
// ANSI escape sequences are stripped; raw text is displayed with
// green-on-black colouring and block cursor with blink.
class Terminal {
public:
    void begin();
    void clear();

    // Write remote data (strips ANSI escape codes)
    void write(const char* data, size_t len);
    void write(char c);

    // Helpers used by the boot / status screens
    void print(const char* str);
    void println(const char* str);
    void printf(const char* fmt, ...);

    // Show the local input line at the bottom of the current row
    void setInputLine(const String& line, int cursorPos);

    // Call every loop() to handle cursor blink
    void update();

    // Status bar (top strip)
    void drawStatusBar(const char* left, const char* right = nullptr);

    // Expose current cursor row so callers can decide scroll behaviour
    int  curRow() const { return _row; }

private:
    char _buf[TERM_ROWS][TERM_COLS + 1];
    int  _row = 0;
    int  _col = 0;

    // Input overlay
    String   _inputLine;
    int      _inputCursorPos = 0;
    bool     _inputDirty     = false;

    // Cursor blink state
    unsigned long _cursorTimer   = 0;
    bool          _cursorVisible = false;

    // ANSI escape parser state
    bool _inEsc   = false;
    char _escBuf[16];
    int  _escLen  = 0;

    void scrollUp();
    void renderChar(int row, int col);
    void renderRow(int row);
    void renderCursor();
    void clearCursor();
    void renderInputLine();

    int charX(int col) const { return col * FONT_W; }
    int charY(int row) const { return TERM_Y + row * FONT_H; }
};
