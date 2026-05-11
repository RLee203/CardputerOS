#include "app_notes.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>
#include <LittleFS.h>

enum class NotesState { NOTES_LIST, NOTES_NEW_NAME, NOTES_EDIT };

static NotesState notesState = NotesState::NOTES_LIST;
static bool       notesDirty = true;

static String noteFiles[64];
static int    noteCount  = 0;
static int    noteSel    = 0;

static String noteContent;
static int    noteCursor = 0;
static int    noteScroll = 0;
static String editPath;
static String newNameBuf;

static void drawStatusBar(const char* msg) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_STATUS_FG, C_STATUS_BG);
    d.setCursor(2, 3);
    d.print(msg);
    drawBatteryWidget(C_STATUS_BG, SCREEN_W - 43);
}

static void loadFileList() {
    noteCount = 0;
    File dir = LittleFS.open(NOTES_DIR);
    if (!dir) return;
    while (true) {
        File f = dir.openNextFile();
        if (!f) break;
        String name = f.name();
        if (name.endsWith(".txt") || name.endsWith(".TXT")) {
            if (noteCount < 64) noteFiles[noteCount++] = name;
        }
        f.close();
    }
    dir.close();
}

static void drawNotesList() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("Notes");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    int total   = noteCount + 1; // files + "+ New Note"
    int maxVis  = TERM_ROWS - 1;
    int offset  = 0;
    if (noteSel >= maxVis) offset = noteSel - maxVis + 1;

    for (int i = 0; i < maxVis; i++) {
        int idx = i + offset;
        if (idx >= total) break;
        bool sel = (idx == noteSel);
        int y = STATUS_H + i * (FONT_H + 2);
        if (sel) {
            d.fillRect(0, y, SCREEN_W, FONT_H + 2, C_HIGHLIGHT);
            d.setTextColor(C_INPUT, C_HIGHLIGHT);
        } else {
            d.fillRect(0, y, SCREEN_W, FONT_H + 2, C_BG);
            d.setTextColor(C_FG, C_BG);
        }
        d.setCursor(4, y + 1);
        if (idx < noteCount) d.print(noteFiles[idx].c_str());
        else                 d.print("+ New Note");
    }

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 2);
    d.print("Enter=open fn+D=del fn+Q=home");
}

static void drawEditor() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar(("Edit: " + editPath).c_str());
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    // Split content into lines
    String lines[TERM_ROWS + 1];
    int lineCount = 0;
    int lineStart = 0;
    for (int i = 0; i <= (int)noteContent.length(); i++) {
        if (i == (int)noteContent.length() || noteContent[i] == '\n') {
            lines[lineCount++] = noteContent.substring(lineStart, i);
            lineStart = i + 1;
            if (lineCount >= TERM_ROWS + 1) break;
        }
    }

    // Find cursor line/col
    int curLine = 0, curCol = 0;
    int pos = 0;
    for (int i = 0; i < (int)noteContent.length() && pos < noteCursor; i++, pos++) {
        if (noteContent[i] == '\n') { curLine++; curCol = 0; }
        else                        { curCol++; }
    }

    // Adjust scroll
    if (curLine < noteScroll) noteScroll = curLine;
    if (curLine >= noteScroll + TERM_ROWS - 1) noteScroll = curLine - TERM_ROWS + 2;

    d.setTextColor(C_FG, C_BG);
    for (int r = 0; r < TERM_ROWS - 1 && (r + noteScroll) < lineCount; r++) {
        int y = STATUS_H + r * FONT_H;
        d.setCursor(0, y);
        String& ln = lines[r + noteScroll];
        // Truncate to TERM_COLS
        if ((int)ln.length() > TERM_COLS) ln = ln.substring(0, TERM_COLS);
        d.print(ln.c_str());
    }

    // Draw cursor block
    int dispRow = curLine - noteScroll;
    if (dispRow >= 0 && dispRow < TERM_ROWS - 1) {
        int cx = curCol * FONT_W;
        int cy = STATUS_H + dispRow * FONT_H;
        d.fillRect(cx, cy, FONT_W, FONT_H, C_INPUT);
    }

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 2);
    d.print("fn+S=save fn+Q=back fn+;./,/=nav");
}

static void openEditor(const String& path, bool newFile) {
    editPath    = path;
    noteContent = "";
    noteCursor  = 0;
    noteScroll  = 0;

    if (!newFile && LittleFS.exists(path)) {
        File f = LittleFS.open(path, "r");
        if (f) {
            while (f.available()) noteContent += (char)f.read();
            f.close();
        }
    }

    notesState = NotesState::NOTES_EDIT;
    notesDirty = true;
}

// ── State handlers ─────────────────────────────────────────────────────────

static void handleNotesList() {
    if (notesDirty) { drawNotesList(); notesDirty = false; }
    auto ev = readKeys();
    if (!ev.changed) return;
    if (ev.back) { goHome(); return; }

    int total = noteCount + 1;
    if (ev.up   && noteSel > 0)        { noteSel--; notesDirty = true; }
    if (ev.down && noteSel < total - 1){ noteSel++; notesDirty = true; }

    if (ev.enter) {
        if (noteSel == noteCount) {
            // New note
            newNameBuf = "";
            notesState = NotesState::NOTES_NEW_NAME;
            notesDirty = true;
        } else {
            String path = String(NOTES_DIR) + "/" + noteFiles[noteSel];
            openEditor(path, false);
        }
    }

    if (ev.fnKey) {
        for (char c : ev.chars) {
            if ((c == 'd' || c == 'D') && noteSel < noteCount) {
                String path = String(NOTES_DIR) + "/" + noteFiles[noteSel];
                LittleFS.remove(path);
                loadFileList();
                if (noteSel >= noteCount) noteSel = noteCount > 0 ? noteCount - 1 : 0;
                notesDirty = true;
                return;
            }
            if (c == 'q' || c == 'Q') { goHome(); return; }
        }
    }
}

static void handleNewName() {
    auto& d = M5Cardputer.Display;
    if (notesDirty) {
        d.fillScreen(C_BG);
        drawStatusBar("New Note");
        d.setFont(&fonts::Font0);
        d.setTextSize(1);
        d.setTextColor(C_FG, C_BG);
        d.setCursor(4, STATUS_H + 8);
        d.print("Filename (.txt):");
        d.setTextColor(C_INPUT, C_BG);
        d.setCursor(4, STATUS_H + 22);
        d.print(newNameBuf.c_str());
        d.fillRect(4 + newNameBuf.length() * FONT_W, STATUS_H + 22, FONT_W, FONT_H, C_INPUT);
        notesDirty = false;
    }

    auto ev = readKeys();
    if (!ev.changed) return;

    if (ev.del && newNameBuf.length() > 0) {
        newNameBuf.remove(newNameBuf.length() - 1);
        notesDirty = true;
    }
    for (char c : ev.chars) { newNameBuf += c; notesDirty = true; }

    if (ev.fnKey) {
        for (char c : ev.chars)
            if (c == 'q' || c == 'Q') { notesState = NotesState::NOTES_LIST; notesDirty = true; return; }
    }

    if (ev.enter && newNameBuf.length() > 0) {
        if (!newNameBuf.endsWith(".txt")) newNameBuf += ".txt";
        String path = String(NOTES_DIR) + "/" + newNameBuf;
        openEditor(path, true);
    }
}

static void handleEditor() {
    if (notesDirty) { drawEditor(); notesDirty = false; }

    auto ev = readKeys();
    if (!ev.changed) return;

    if (ev.back) {
        loadFileList(); noteSel = 0; notesState = NotesState::NOTES_LIST; notesDirty = true; return;
    }

    // fn+S = save, fn+Q = discard (must check before cursor movement)
    if (ev.fnKey) {
        for (char c : ev.chars) {
            if (c == 's' || c == 'S') {
                File f = LittleFS.open(editPath, "w");
                if (f) { f.print(noteContent); f.close(); }
                drawEditor();
                return;
            }
            if (c == 'q' || c == 'Q') {
                loadFileList();
                noteSel    = 0;
                notesState = NotesState::NOTES_LIST;
                notesDirty = true;
                return;
            }
        }
    }

    // Cursor movement via fn+;/./,// (ev.up/down/left/right set by input.cpp)
    if (ev.up && noteCursor > 0) {
        int pos = noteCursor - 1;
        while (pos > 0 && noteContent[pos - 1] != '\n') pos--;
        int curLineStart = noteCursor;
        while (curLineStart > 0 && noteContent[curLineStart - 1] != '\n') curLineStart--;
        int col = noteCursor - curLineStart;
        noteCursor = pos + col;
        if (noteCursor > (int)noteContent.length()) noteCursor = noteContent.length();
        notesDirty = true;
        return;
    }
    if (ev.down) {
        int pos = noteCursor;
        while (pos < (int)noteContent.length() && noteContent[pos] != '\n') pos++;
        if (pos < (int)noteContent.length()) {
            int curLineStart = noteCursor;
            while (curLineStart > 0 && noteContent[curLineStart - 1] != '\n') curLineStart--;
            int col = noteCursor - curLineStart;
            pos++;
            noteCursor = pos + col;
            if (noteCursor > (int)noteContent.length()) noteCursor = noteContent.length();
            notesDirty = true;
        }
        return;
    }
    if (ev.left  && noteCursor > 0)                         { noteCursor--; notesDirty = true; return; }
    if (ev.right && noteCursor < (int)noteContent.length()) { noteCursor++; notesDirty = true; return; }

    // Text editing (no fn key)
    if (ev.enter) {
        noteContent = noteContent.substring(0, noteCursor) + "\n" + noteContent.substring(noteCursor);
        noteCursor++;
        notesDirty = true;
    } else if (ev.del) {
        if (noteCursor > 0) {
            noteContent.remove(noteCursor - 1, 1);
            noteCursor--;
            notesDirty = true;
        }
    } else if (!ev.fnKey) {
        for (char c : ev.chars) {
            noteContent = noteContent.substring(0, noteCursor) + c + noteContent.substring(noteCursor);
            noteCursor++;
            notesDirty = true;
        }
    }
}

// ── Public API ─────────────────────────────────────────────────────────────

void appNotesEnter() {
    if (!LittleFS.exists(NOTES_DIR)) LittleFS.mkdir(NOTES_DIR);
    loadFileList();
    noteSel    = 0;
    notesState = NotesState::NOTES_LIST;
    notesDirty = true;
}

void appNotesLoop() {
    switch (notesState) {
        case NotesState::NOTES_LIST:     handleNotesList(); break;
        case NotesState::NOTES_NEW_NAME: handleNewName();   break;
        case NotesState::NOTES_EDIT:     handleEditor();    break;
    }
}
