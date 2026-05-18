#include "app_files.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>

enum class FilesScene  { FILE_LIST, DELETE_CONFIRM };
enum class FilesSource { SOURCE_SD, SOURCE_LFS };

static FilesScene  filesScene  = FilesScene::FILE_LIST;
static FilesSource filesSource = FilesSource::SOURCE_SD;
static bool        filesDirty  = true;

static String fileNames[128];
static size_t fileSizes[128];
static int    fileCount = 0;
static int    fileSel   = 0;
static bool   sdOk      = false;
static bool   lfsOk     = false;
static int    delTarget = -1;

// ── Recursive LittleFS listing ─────────────────────────────────────────────

static void listLFS(const char* dir) {
    File d = LittleFS.open(dir);
    if (!d) return;
    while (fileCount < 128) {
        File f = d.openNextFile();
        if (!f) break;
        String path = f.path();
        if (f.isDirectory()) {
            f.close();
            listLFS(path.c_str());
        } else {
            fileNames[fileCount] = path;
            fileSizes[fileCount] = f.size();
            fileCount++;
            f.close();
        }
    }
    d.close();
}

static void loadFileList() {
    fileCount = 0;
    if (filesSource == FilesSource::SOURCE_SD) {
        if (!sdOk) return;
        File root = SD.open("/");
        if (!root) return;
        while (fileCount < 128) {
            File f = root.openNextFile();
            if (!f) break;
            if (!f.isDirectory()) {
                fileNames[fileCount] = f.name();
                fileSizes[fileCount] = f.size();
                fileCount++;
            }
            f.close();
        }
        root.close();
    } else {
        if (!lfsOk) return;
        listLFS("/");
    }
}

static void drawStatus() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_STATUS_FG, C_STATUS_BG);
    d.setCursor(2, 3);
    d.print(filesSource == FilesSource::SOURCE_SD ? "Files: SD Card" : "Files: Internal");
    // Tab indicator on right
    d.setTextColor(C_DIM, C_STATUS_BG);
    int tw = 3 * FONT_W;
    d.setCursor(SCREEN_W - tw - 2, 3);
    d.print("Tab");
    drawBatteryWidget(C_STATUS_BG);
}

static void drawFileList() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, STATUS_H, SCREEN_W, TERM_H, C_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    bool hasSource = (filesSource == FilesSource::SOURCE_SD) ? sdOk : lfsOk;
    if (!hasSource) {
        d.setTextColor(C_ERROR, C_BG);
        d.setCursor(4, STATUS_H + 4);
        d.print(filesSource == FilesSource::SOURCE_SD ? "SD card not found." : "Internal FS error.");
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(4, STATUS_H + 16); d.print("Tab=switch  fn+bksp=home");
        return;
    }
    if (fileCount == 0) {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(4, STATUS_H + 4); d.print("No files.");
        d.setCursor(4, STATUS_H + 16); d.print("Tab=switch  fn+bksp=home");
        return;
    }

    // Each row is FONT_H+2=10px tall; reserve footer line at bottom
    static constexpr int VIS_ROWS = (SCREEN_H - FONT_H - 2 - STATUS_H) / (FONT_H + 2); // 11
    int vis = VIS_ROWS;
    int off = (fileSel >= vis) ? fileSel - vis + 1 : 0;
    for (int i = 0; i < vis; i++) {
        int idx = i + off;
        if (idx >= fileCount) break;
        bool sel = (idx == fileSel);
        int y = STATUS_H + i * (FONT_H + 2);
        uint32_t bg = sel ? C_HIGHLIGHT : C_BG;
        d.fillRect(0, y, SCREEN_W, FONT_H + 2, bg);
        d.setTextColor(sel ? (uint32_t)C_INPUT : (uint32_t)C_FG, bg);
        d.setCursor(4, y + 1);
        char line[40];
        size_t sz = fileSizes[idx];
        const char* unit = "B";
        size_t disp = sz;
        if (sz >= 1024 * 1024) { disp = sz / (1024 * 1024); unit = "M"; }
        else if (sz >= 1024)   { disp = sz / 1024;           unit = "K"; }
        // Show just the filename (not full path) for readability
        const char* name = fileNames[idx].c_str();
        const char* slash = strrchr(name, '/');
        const char* display = slash ? slash + 1 : name;
        snprintf(line, sizeof(line), "%-26s%4d%s", display, (int)disp, unit);
        d.print(line);
    }
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 2);
    d.print("fn+D=del Tab=switch bksp=home");
}

static void drawDeleteConfirm() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatus();
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_ERROR, C_BG);
    d.setCursor(4, STATUS_H + 10);
    // Show just filename
    const char* name = fileNames[delTarget].c_str();
    const char* slash = strrchr(name, '/');
    const char* display = slash ? slash + 1 : name;
    char buf[40];
    snprintf(buf, sizeof(buf), "Delete: %.32s?", display);
    d.print(buf);
    d.setTextColor(C_FG, C_BG);
    d.setCursor(4, STATUS_H + 28);
    d.print("Enter=YES   fn+bksp=No");
}

void appFilesEnter() {
    suspendWifiForSd();
    filesScene  = FilesScene::FILE_LIST;
    filesSource = FilesSource::SOURCE_SD;
    filesDirty  = true;
    fileSel     = 0;
    digitalWrite(LORA_NSS_PIN, HIGH);
    digitalWrite(LORA_RST_PIN, LOW);
    SD.end();
    delay(40);
    sdOk  = SD.begin(SD_CS_PIN, SPI, 25000000);
    lfsOk = LittleFS.begin(false);
    loadFileList();
}

void appFilesLoop() {
    if (filesDirty) {
        if (filesScene == FilesScene::FILE_LIST) { drawStatus(); drawFileList(); }
        else                                      { drawDeleteConfirm(); }
        filesDirty = false;
    }

    auto ev = readKeys();
    if (!ev.changed) return;

    // fn+backspace = universal back
    if (ev.back) {
        if (filesScene == FilesScene::DELETE_CONFIRM) {
            filesScene = FilesScene::FILE_LIST; filesDirty = true; return;
        }
        goHome(); return;
    }

    if (filesScene == FilesScene::DELETE_CONFIRM) {
        if (ev.enter) {
            if (filesSource == FilesSource::SOURCE_SD)
                SD.remove(fileNames[delTarget].c_str());
            else
                LittleFS.remove(fileNames[delTarget].c_str());
            if (fileSel >= fileCount - 1 && fileSel > 0) fileSel--;
            loadFileList();
            filesScene = FilesScene::FILE_LIST;
            filesDirty = true;
        }
        if (ev.fnKey) {
            for (char c : ev.chars)
                if (c == 'q' || c == 'Q') { filesScene = FilesScene::FILE_LIST; filesDirty = true; return; }
        }
        return;
    }

    // FILE_LIST
    // Tab = switch between SD and LittleFS
    if (ev.tab) {
        filesSource = (filesSource == FilesSource::SOURCE_SD)
                      ? FilesSource::SOURCE_LFS : FilesSource::SOURCE_SD;
        fileSel = 0;
        loadFileList();
        filesDirty = true;
        return;
    }

    if (ev.fnKey) {
        for (char c : ev.chars) {
            if ((c == 'd' || c == 'D') && fileCount > 0) {
                delTarget  = fileSel;
                filesScene = FilesScene::DELETE_CONFIRM;
                filesDirty = true;
                return;
            }
            if (c == 'q' || c == 'Q') { goHome(); return; }
        }
    }

    if (ev.up   && fileSel > 0)             { fileSel--; filesDirty = true; }
    if (ev.down && fileSel < fileCount - 1) { fileSel++; filesDirty = true; }
}
