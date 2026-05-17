#include "app_mp3.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <Audio.h>

static Audio    audio;
static String   mp3Files[64];
static int      mp3Count   = 0;
static int      mp3Sel     = 0;
static bool     mp3Playing = false;
static bool     mp3Paused  = false;
static bool     sdOk       = false;
static uint8_t  mp3Vol     = 12;    // 0-21

void audio_eof_mp3(const char* /*info*/) {}

static void drawMp3Status() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_STATUS_FG, C_STATUS_BG);
    d.setCursor(2, 3);
    d.print("MP3 Player");
    char vol[6];
    snprintf(vol, sizeof(vol), "V:%d", mp3Vol);
    int vw = strlen(vol) * FONT_W;
    d.setTextColor(C_DIM, C_STATUS_BG);
    d.setCursor(SCREEN_W - vw - 2, 3);
    d.print(vol);
    drawBatteryWidget(C_STATUS_BG);   // centred as per every other app
}

static void drawList() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, STATUS_H, SCREEN_W, TERM_H, C_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    if (!sdOk) {
        d.setTextColor(C_ERROR, C_BG);
        d.setCursor(4, STATUS_H + 4); d.print("SD card not found.");
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(4, STATUS_H + 16); d.print("fn+bksp = home");
        return;
    }
    if (mp3Count == 0) {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(4, STATUS_H + 4); d.print("No .mp3 files on SD.");
        d.setCursor(4, STATUS_H + 16); d.print("fn+bksp = home");
        return;
    }

    int vis = TERM_ROWS - 1;
    int off = (mp3Sel >= vis) ? mp3Sel - vis + 1 : 0;
    for (int i = 0; i < vis; i++) {
        int idx = i + off;
        if (idx >= mp3Count) break;
        bool sel = (idx == mp3Sel);
        int y = STATUS_H + i * (FONT_H + 2);
        uint32_t bg = sel ? C_HIGHLIGHT : C_BG;
        d.fillRect(0, y, SCREEN_W, FONT_H + 2, bg);
        d.setTextColor(sel ? (uint32_t)C_INPUT : (uint32_t)C_FG, bg);
        d.setCursor(4, y + 1);
        d.print(mp3Files[idx].c_str());
    }
    d.setCursor(2, SCREEN_H - FONT_H - 2);
    if (mp3Playing || mp3Paused) {
        // Bottom line shows current song name with play/pause indicator
        String line = (mp3Playing ? String(">> ") : String("|| ")) + mp3Files[mp3Sel];
        if ((int)line.length() > 38) line = line.substring(0, 36) + "..";
        d.setTextColor(mp3Playing ? (uint32_t)C_INPUT : (uint32_t)C_DIM, C_BG);
        d.print(line.c_str());
    } else {
        d.setTextColor(C_DIM, C_BG);
        d.print("Ent=Play  +/-=Vol  bksp=Home");
    }
}

static void stopPlayback() {
    if (mp3Playing || mp3Paused) {
        audio.stopSong();
        mp3Playing = false;
        mp3Paused  = false;
    }
    M5Cardputer.Speaker.begin();
}

static void startPlayback() {
    if (mp3Count == 0) return;
    stopPlayback();
    M5Cardputer.Speaker.end();
    audio.setPinout(I2S_BCLK_PIN, I2S_LRCLK_PIN, I2S_DOUT_PIN);
    audio.setVolume(mp3Vol);
    String path = "/" + mp3Files[mp3Sel];
    audio.connecttoFS(SD, path.c_str());
    mp3Playing = true;
    mp3Paused  = false;
    drawMp3Status();
    drawList();
}

static void playNext() {
    if (mp3Count == 0) return;
    mp3Sel = (mp3Sel + 1) % mp3Count;
    startPlayback();
}

static void playPrev() {
    if (mp3Count == 0) return;
    mp3Sel = (mp3Sel + mp3Count - 1) % mp3Count;
    startPlayback();
}

void appMp3Enter() {
    mp3Sel = 0; mp3Playing = false; mp3Paused = false; mp3Count = 0;
    digitalWrite(LORA_NSS_PIN, HIGH);
    digitalWrite(LORA_RST_PIN, LOW);
    sdOk = SD.begin(SD_CS_PIN, SPI, 25000000);
    if (sdOk) {
        File root = SD.open("/");
        while (true) {
            File f = root.openNextFile();
            if (!f) break;
            if (!f.isDirectory()) {
                String name = f.name();
                if (name.endsWith(".mp3") || name.endsWith(".MP3"))
                    if (mp3Count < 64) mp3Files[mp3Count++] = name;
            }
            f.close();
        }
        root.close();
    }
    drawMp3Status();
    drawList();
}

void appMp3Loop() {
    if (mp3Playing) {
        audio.loop();
        if (!audio.isRunning()) {
            // Song ended — auto-play next
            mp3Playing = false;
            mp3Paused  = false;
            if (mp3Count > 1) {
                mp3Sel = (mp3Sel + 1) % mp3Count;
                startPlayback();
            } else {
                M5Cardputer.Speaker.begin();
                drawMp3Status();
                drawList();
            }
            return;
        }
    }

    auto ev = readKeys();
    if (!ev.changed) return;

    // fn+backspace = home (always)
    if (ev.back) { stopPlayback(); goHome(); return; }

    // fn+Q = home (backup)
    if (ev.fnKey) {
        for (char c : ev.chars)
            if (c == 'q' || c == 'Q') { stopPlayback(); goHome(); return; }
    }

    // Volume with + and - (always available, regardless of fn state)
    for (char c : ev.chars) {
        if (c == '+' || c == '=') {
            if (mp3Vol < 21) { mp3Vol++; if (mp3Playing) audio.setVolume(mp3Vol); }
            drawMp3Status();
            return;
        }
        if (c == '-') {
            if (mp3Vol > 0) { mp3Vol--; if (mp3Playing) audio.setVolume(mp3Vol); }
            drawMp3Status();
            return;
        }
    }

    if (mp3Playing || mp3Paused) {
        // Controls while playing or paused
        if (ev.enter) {
            // Pause / Resume
            audio.pauseResume();
            mp3Paused  = mp3Playing;   // was playing → now paused
            mp3Playing = !mp3Playing;
            drawMp3Status();
            drawList();
            return;
        }
        if (ev.up)    { playPrev(); return; }
        if (ev.down)  { playNext(); return; }
    } else {
        // Browse mode
        if (ev.up   && mp3Sel > 0)            { mp3Sel--; drawList(); }
        if (ev.down && mp3Sel < mp3Count - 1) { mp3Sel++; drawList(); }
        if (ev.enter && mp3Count > 0)         startPlayback();
    }
}
