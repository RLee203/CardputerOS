#include "app_photos.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <Stream.h>

namespace {
enum class PhotosScene { LIST, VIEW };

static PhotosScene photosScene = PhotosScene::LIST;
static bool photosDirty = true;
static bool sdOk = false;
static String photoFiles[128];
static int photoCount = 0;
static int photoSel = 0;
static String photoStatus;

bool hasImageExt(const String& name) {
    String lower = name;
    lower.toLowerCase();
    return lower.endsWith(".jpg")
        || lower.endsWith(".jpeg")
        || lower.endsWith(".png")
        || lower.endsWith(".bmp");
}

const char* baseName(const String& path) {
    const char* name = path.c_str();
    const char* slash = strrchr(name, '/');
    return slash ? slash + 1 : name;
}

String normalizePhotoPath(const String& path) {
    if (path.startsWith("/")) return path;
    return String("/") + path;
}

void loadPhotos() {
    photoCount = 0;
    if (!sdOk) return;
    File root = SD.open("/");
    if (!root) return;
    while (photoCount < 128) {
        File f = root.openNextFile();
        if (!f) break;
        if (!f.isDirectory()) {
            String name = f.name();
            if (hasImageExt(name)) {
                photoFiles[photoCount++] = name;
            }
        }
        f.close();
    }
    root.close();
    if (photoSel >= photoCount) photoSel = photoCount > 0 ? photoCount - 1 : 0;
}

void drawStatusBar(const char* title) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_STATUS_FG, C_STATUS_BG);
    d.setCursor(2, 3);
    d.print(title);
    drawBatteryWidget(C_STATUS_BG, SCREEN_W - 43);
}

void drawList() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("Photos");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    if (!sdOk) {
        d.setTextColor(C_ERROR, C_BG);
        d.setCursor(4, STATUS_H + 8);
        d.print("SD card not found.");
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(4, STATUS_H + 22);
        d.print("Insert SD with images.");
        d.setCursor(4, SCREEN_H - FONT_H - 2);
        d.print("fn+bksp=home");
        return;
    }

    if (photoCount == 0) {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(4, STATUS_H + 8);
        d.print("No images found.");
        d.setCursor(4, STATUS_H + 22);
        d.print("Use SD root for JPG/PNG/BMP.");
        d.setCursor(4, SCREEN_H - FONT_H - 2);
        d.print("fn+bksp=home");
        return;
    }

    static constexpr int VIS_ROWS = (SCREEN_H - FONT_H - 2 - STATUS_H) / (FONT_H + 2);
    int off = (photoSel >= VIS_ROWS) ? photoSel - VIS_ROWS + 1 : 0;
    for (int i = 0; i < VIS_ROWS; ++i) {
        int idx = i + off;
        if (idx >= photoCount) break;
        bool sel = idx == photoSel;
        int y = STATUS_H + i * (FONT_H + 2);
        uint32_t bg = sel ? C_HIGHLIGHT : C_BG;
        d.fillRect(0, y, SCREEN_W, FONT_H + 2, bg);
        d.setTextColor(sel ? (uint32_t)C_INPUT : (uint32_t)C_FG, bg);
        d.setCursor(4, y + 1);
        d.print(baseName(photoFiles[idx]));
    }

    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 2);
    d.print("Enter=view  fn+bksp=home");
}

bool drawCurrentPhoto() {
    if (photoSel < 0 || photoSel >= photoCount) return false;
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("Photos");
    String path = normalizePhotoPath(photoFiles[photoSel]);
    String lower = path;
    lower.toLowerCase();

    bool ok = false;
    int y = STATUS_H + 2;
    int h = SCREEN_H - STATUS_H - 2;
    File img = SD.open(path.c_str(), FILE_READ);
    if (!img) {
        ok = false;
    } else if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) {
        ok = d.drawJpg(&img, 0, y, SCREEN_W, h, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
    } else if (lower.endsWith(".png")) {
        ok = d.drawPng(&img, 0, y, SCREEN_W, h, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
    } else if (lower.endsWith(".bmp")) {
        ok = d.drawBmp(&img, 0, y, SCREEN_W, h, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
    }
    if (img) img.close();

    d.fillRect(0, SCREEN_H - 10, SCREEN_W, 10, C_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(ok ? (uint32_t)C_DIM : (uint32_t)C_ERROR, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 1);
    if (ok) {
        d.printf("%d/%d  </>=prev/next", photoSel + 1, photoCount);
    } else {
        d.print("Could not draw image");
    }
    return ok;
}
}

void appPhotosEnter() {
    suspendWifiForSd();
    photosScene = PhotosScene::LIST;
    photosDirty = true;
    photoStatus = "";
    photoSel = 0;
    digitalWrite(LORA_NSS_PIN, HIGH);
    digitalWrite(LORA_RST_PIN, LOW);
    SD.end();
    delay(40);
    sdOk = SD.begin(SD_CS_PIN, SPI, 25000000);
    loadPhotos();
}

void appPhotosLoop() {
    if (photosDirty) {
        if (photosScene == PhotosScene::LIST) drawList();
        else drawCurrentPhoto();
        photosDirty = false;
    }

    auto ev = readKeys();
    if (!ev.changed) return;

    if (ev.back) {
        if (photosScene == PhotosScene::VIEW) {
            photosScene = PhotosScene::LIST;
            photosDirty = true;
            return;
        }
        goHome();
        return;
    }

    if (photosScene == PhotosScene::LIST) {
        if (ev.up && photoSel > 0) {
            --photoSel;
            photosDirty = true;
        }
        if (ev.down && photoSel < photoCount - 1) {
            ++photoSel;
            photosDirty = true;
        }
        if (ev.enter && photoCount > 0) {
            photosScene = PhotosScene::VIEW;
            photosDirty = true;
        }
        if (ev.fnKey) {
            for (char c : ev.chars) {
                if (c == 'q' || c == 'Q') {
                    goHome();
                    return;
                }
            }
        }
        return;
    }

    if (ev.left && photoSel > 0) {
        --photoSel;
        photosDirty = true;
    }
    if (ev.right && photoSel < photoCount - 1) {
        ++photoSel;
        photosDirty = true;
    }
    if (ev.enter) {
        photosScene = PhotosScene::LIST;
        photosDirty = true;
    }
}
