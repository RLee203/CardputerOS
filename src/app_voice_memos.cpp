#include "app_voice_memos.h"
#include "nav.h"
#include "input.h"
#include "config.h"
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>

namespace {
enum class VoiceState { LIST, NEW_NAME, RECORDING, PLAYING };

struct WAVHeader {
    char riff[4]           = {'R', 'I', 'F', 'F'};
    uint32_t fileSize      = 0;
    char wave[4]           = {'W', 'A', 'V', 'E'};
    char fmt[4]            = {'f', 'm', 't', ' '};
    uint32_t fmtSize       = 16;
    uint16_t audioFormat   = 1;
    uint16_t numChannels   = 1;
    uint32_t sampleRate    = 16000;
    uint32_t byteRate      = 16000 * sizeof(int16_t);
    uint16_t blockAlign    = sizeof(int16_t);
    uint16_t bitsPerSample = 16;
    char data[4]           = {'d', 'a', 't', 'a'};
    uint32_t dataSize      = 0;
};

static constexpr size_t RECORD_SAMPLES = 240;
static constexpr size_t PLAY_SAMPLES = 512;
static constexpr uint32_t SAMPLE_RATE = 16000;
static constexpr uint32_t STATUS_MSG_MS = 1400;
static constexpr int PLAYBACK_GAIN_MIN = 1;
static constexpr int PLAYBACK_GAIN_MAX = 4;
static constexpr int16_t RECORD_NOISE_FLOOR = 140;
static constexpr int16_t PLAYBACK_NOISE_FLOOR = 80;

static VoiceState voiceState = VoiceState::LIST;
static bool voiceDirty = true;
static bool sdOk = false;
static String memoFiles[64];
static int memoCount = 0;
static int memoSel = 0;
static String newMemoName;
static String currentMemoPath;
static File recordFile;
static File playFile;
static bool recordingActive = false;
static bool playbackActive = false;
static uint32_t recordedBytes = 0;
static uint32_t recordStartMs = 0;
static String statusLine;
static uint32_t statusUntilMs = 0;
static int16_t recordBuffer[RECORD_SAMPLES];
static int16_t recordProcessedBuffer[RECORD_SAMPLES];
static int16_t playBuffer[PLAY_SAMPLES];
static int16_t playScaledBuffer[PLAY_SAMPLES];
static bool voiceOverlayDirty = true;
static uint32_t lastVoiceOverlayMs = 0;
static int playbackGain = 2;
static float recordDcEstimate = 0.0f;
static float recordSmoothedSample = 0.0f;
static float playbackDcEstimate = 0.0f;
static float playbackSmoothedSample = 0.0f;

void drawStatusBar(const char* title) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, SCREEN_W, STATUS_H, C_STATUS_BG);
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(C_STATUS_FG, C_STATUS_BG);
    d.setCursor(2, 3);
    d.print(title);
    char volBuf[8];
    snprintf(volBuf, sizeof(volBuf), "V%d", playbackGain);
    int volX = SCREEN_W - 62;
    d.setCursor(volX, 3);
    d.print(volBuf);
    drawBatteryWidget(C_STATUS_BG, SCREEN_W - 43);
}

void showStatus(const String& text) {
    statusLine = text;
    statusUntilMs = millis() + STATUS_MSG_MS;
    voiceDirty = true;
}

void clearExpiredStatus() {
    if (statusLine.length() > 0 && millis() >= statusUntilMs) {
        statusLine = "";
        voiceDirty = true;
    }
}

void ensureStorageDir() {
    if (!SD.exists(VOICE_MEMOS_DIR)) SD.mkdir(VOICE_MEMOS_DIR);
}

void loadMemoList() {
    memoCount = 0;
    if (!sdOk) return;
    ensureStorageDir();
    File dir = SD.open(VOICE_MEMOS_DIR);
    if (!dir) return;
    while (true) {
        File f = dir.openNextFile();
        if (!f) break;
        if (!f.isDirectory()) {
            String name = f.name();
            int slash = name.lastIndexOf('/');
            if (slash >= 0) name = name.substring(slash + 1);
            if (name.endsWith(".wav") || name.endsWith(".WAV")) {
                if (memoCount < 64) memoFiles[memoCount++] = name;
            }
        }
        f.close();
    }
    dir.close();
}

String makeMemoPath(const String& fileName) {
    return String(VOICE_MEMOS_DIR) + "/" + fileName;
}

String defaultMemoName() {
    int next = 1;
    while (true) {
        char buf[20];
        snprintf(buf, sizeof(buf), "memo%03d.wav", next);
        if (!SD.exists(makeMemoPath(buf))) return String(buf);
        ++next;
    }
}

void drawFooterHint(const char* hint) {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, SCREEN_H - 10, SCREEN_W, 10, C_BG);
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(2, SCREEN_H - FONT_H - 1);
    d.print(hint);
}

void applySpeakerVolume() {
    M5Cardputer.Speaker.setVolume(255);
}

int16_t clampSample(int32_t sample) {
    if (sample > 32767) return 32767;
    if (sample < -32768) return -32768;
    return static_cast<int16_t>(sample);
}

void resetAudioFilters() {
    recordDcEstimate = 0.0f;
    recordSmoothedSample = 0.0f;
    playbackDcEstimate = 0.0f;
    playbackSmoothedSample = 0.0f;
}

void cleanRecordedSamples(const int16_t* input, int16_t* output, size_t samples) {
    for (size_t i = 0; i < samples; ++i) {
        float sample = static_cast<float>(input[i]);
        recordDcEstimate += (sample - recordDcEstimate) * 0.004f;
        float centered = sample - recordDcEstimate;
        if (centered > -RECORD_NOISE_FLOOR && centered < RECORD_NOISE_FLOOR) {
            centered *= 0.35f;
        } else {
            centered *= 1.18f;
        }
        recordSmoothedSample = recordSmoothedSample * 0.10f + centered * 0.90f;
        output[i] = clampSample(static_cast<int32_t>(recordSmoothedSample));
    }
}

void preparePlaybackSamples(const int16_t* input, int16_t* output, size_t samples) {
    int32_t peak = 0;
    for (size_t i = 0; i < samples; ++i) {
        int32_t mag = input[i] < 0 ? -static_cast<int32_t>(input[i]) : static_cast<int32_t>(input[i]);
        if (mag > peak) peak = mag;
    }

    float normalize = 1.0f;
    if (peak > 0 && peak < 14000) {
        normalize = 14000.0f / static_cast<float>(peak);
        if (normalize > 1.9f) normalize = 1.9f;
    }

    float playbackGainScale = 0.95f + static_cast<float>(playbackGain - 1) * 0.28f;

    for (size_t i = 0; i < samples; ++i) {
        float sample = static_cast<float>(input[i]);
        playbackDcEstimate += (sample - playbackDcEstimate) * 0.003f;
        float centered = sample - playbackDcEstimate;
        if (centered > -PLAYBACK_NOISE_FLOOR && centered < PLAYBACK_NOISE_FLOOR) {
            centered *= 0.45f;
        }
        centered *= normalize * playbackGainScale;
        playbackSmoothedSample = playbackSmoothedSample * 0.08f + centered * 0.92f;
        output[i] = clampSample(static_cast<int32_t>(playbackSmoothedSample));
    }
}

void adjustPlaybackGain(int delta) {
    int next = playbackGain + delta;
    if (next < PLAYBACK_GAIN_MIN) next = PLAYBACK_GAIN_MIN;
    if (next > PLAYBACK_GAIN_MAX) next = PLAYBACK_GAIN_MAX;
    if (next == playbackGain) return;
    playbackGain = next;
    showStatus(String("Playback vol ") + playbackGain + "/" + PLAYBACK_GAIN_MAX);
    if (voiceState == VoiceState::RECORDING || voiceState == VoiceState::PLAYING) {
        voiceOverlayDirty = true;
    } else {
        voiceDirty = true;
    }
}

void drawMemoList() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("Voice Memos");
    d.setFont(&fonts::Font0);
    d.setTextSize(1);

    if (!sdOk) {
        d.setTextColor(C_ERROR, C_BG);
        d.setCursor(4, STATUS_H + 6);
        d.print("SD card not available.");
        drawFooterHint("fn+bksp=home");
        return;
    }

    int total = memoCount + 1;
    int maxVis = TERM_ROWS - 1;
    int offset = 0;
    if (memoSel >= maxVis) offset = memoSel - maxVis + 1;

    for (int i = 0; i < maxVis; ++i) {
        int idx = i + offset;
        if (idx >= total) break;
        bool sel = idx == memoSel;
        int y = STATUS_H + i * (FONT_H + 2);
        uint32_t bg = sel ? C_HIGHLIGHT : C_BG;
        d.fillRect(0, y, SCREEN_W, FONT_H + 2, bg);
        d.setTextColor(sel ? (uint32_t)C_INPUT : (uint32_t)C_FG, bg);
        d.setCursor(4, y + 1);
        if (idx < memoCount) d.print(memoFiles[idx].c_str());
        else d.print("+ New Voice Memo");
    }

    d.fillRect(0, SCREEN_H - 10, SCREEN_W, 10, C_BG);
    if (statusLine.length() > 0) {
        d.setTextColor(C_ACCENT, C_BG);
        d.setCursor(2, SCREEN_H - FONT_H - 1);
        d.print(statusLine.c_str());
    } else {
        d.setTextColor(C_DIM, C_BG);
        d.setCursor(2, SCREEN_H - FONT_H - 1);
        d.print("Enter=open -/=vol fn+D=del");
    }
}

void drawNewName() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("New Voice Memo");
    d.setTextColor(C_FG, C_BG);
    d.setCursor(4, STATUS_H + 8);
    d.print("Filename (.wav):");
    d.setTextColor(C_INPUT, C_BG);
    d.setCursor(4, STATUS_H + 22);
    d.print(newMemoName.c_str());
    d.fillRect(4 + newMemoName.length() * FONT_W, STATUS_H + 22, FONT_W, FONT_H, C_INPUT);
    drawFooterHint("Enter=record del=erase");
}

void drawRecording() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("Voice Memos");
    d.setTextColor(C_ERROR, C_BG);
    d.setCursor(8, 28);
    d.print("Recording...");
    d.fillCircle(SCREEN_W - 26, 30, 7, C_ERROR);
    d.setTextColor(C_FG, C_BG);
    d.setCursor(8, 46);
    d.print(currentMemoPath.substring(currentMemoPath.lastIndexOf('/') + 1).c_str());
    d.fillRect(0, 60, SCREEN_W, 26, C_BG);
    uint32_t secs = (millis() - recordStartMs) / 1000;
    d.setCursor(8, 64);
    d.printf("Length: %lus", static_cast<unsigned long>(secs));
    d.setCursor(8, 78);
    d.printf("Saved: %lu KB", static_cast<unsigned long>(recordedBytes / 1024));
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(8, 104);
    d.print("Press Enter to stop/save");
    d.setCursor(8, 116);
    d.print("fn+bksp cancels");
}

void drawPlaying() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawStatusBar("Voice Memos");
    d.setTextColor(C_ACCENT, C_BG);
    d.setCursor(8, 28);
    d.print("Playing...");
    d.setTextColor(C_FG, C_BG);
    d.setCursor(8, 46);
    d.print(currentMemoPath.substring(currentMemoPath.lastIndexOf('/') + 1).c_str());
    d.fillRect(0, 60, SCREEN_W, 18, C_BG);
    d.setCursor(8, 64);
    d.print("Playing audio...");
    d.setTextColor(C_DIM, C_BG);
    d.setCursor(8, 104);
    d.print("Enter=stop -/=vol fn+bksp");
}

void drawVoiceOverlay() {
    if (voiceState == VoiceState::RECORDING) {
        auto& d = M5Cardputer.Display;
        d.fillRect(0, 60, SCREEN_W, 26, C_BG);
        d.setTextColor(C_FG, C_BG);
        uint32_t secs = (millis() - recordStartMs) / 1000;
        d.setCursor(8, 64);
        d.printf("Length: %lus", static_cast<unsigned long>(secs));
        d.setCursor(8, 78);
        d.printf("Saved: %lu KB", static_cast<unsigned long>(recordedBytes / 1024));
    } else if (voiceState == VoiceState::PLAYING) {
        auto& d = M5Cardputer.Display;
        d.fillRect(0, 60, SCREEN_W, 18, C_BG);
        d.setTextColor(C_FG, C_BG);
        d.setCursor(8, 64);
        d.print("Playing audio...");
    }
}

void closeRecordingFile(bool keepFile) {
    if (recordFile) {
        WAVHeader header;
        header.fileSize = 36 + recordedBytes;
        header.dataSize = recordedBytes;
        recordFile.seek(0);
        recordFile.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
        recordFile.close();
    }
    if (!keepFile && currentMemoPath.length() > 0) {
        SD.remove(currentMemoPath);
    }
    M5Cardputer.Mic.end();
    recordingActive = false;
}

bool beginRecording(const String& path) {
    currentMemoPath = path;
    if (SD.exists(path)) SD.remove(path);
    recordFile = SD.open(path, FILE_WRITE);
    if (!recordFile) return false;

    WAVHeader header;
    recordFile.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
    recordedBytes = 0;
    recordStartMs = millis();
    recordDcEstimate = 0.0f;
    recordSmoothedSample = 0.0f;
    M5Cardputer.Speaker.end();
    M5Cardputer.Mic.begin();
    recordingActive = true;
    voiceState = VoiceState::RECORDING;
    voiceOverlayDirty = true;
    voiceDirty = true;
    return true;
}

void stopRecording(bool keepFile) {
    closeRecordingFile(keepFile);
    M5Cardputer.Speaker.begin();
    applySpeakerVolume();
    if (keepFile) {
        loadMemoList();
        memoSel = memoCount > 0 ? memoCount - 1 : 0;
        showStatus("Saved voice memo");
    } else {
        showStatus("Recording canceled");
    }
    voiceState = VoiceState::LIST;
    voiceDirty = true;
}

bool beginPlayback(const String& path) {
    currentMemoPath = path;
    playFile = SD.open(path, FILE_READ);
    if (!playFile) return false;
    playFile.seek(sizeof(WAVHeader));
    playbackDcEstimate = 0.0f;
    playbackSmoothedSample = 0.0f;
    M5Cardputer.Mic.end();
    M5Cardputer.Speaker.begin();
    applySpeakerVolume();
    playbackActive = true;
    voiceState = VoiceState::PLAYING;
    voiceOverlayDirty = true;
    voiceDirty = true;
    return true;
}

void stopPlayback() {
    if (playFile) playFile.close();
    while (M5Cardputer.Speaker.isPlaying()) delay(1);
    M5Cardputer.Speaker.end();
    M5Cardputer.Mic.begin();
    playbackActive = false;
    voiceState = VoiceState::LIST;
    voiceDirty = true;
}

void recordChunk() {
    if (!recordingActive) return;
    if (M5Cardputer.Mic.record(recordBuffer, RECORD_SAMPLES, SAMPLE_RATE)) {
        cleanRecordedSamples(recordBuffer, recordProcessedBuffer, RECORD_SAMPLES);
        size_t written = recordFile.write(reinterpret_cast<const uint8_t*>(recordProcessedBuffer), sizeof(recordProcessedBuffer));
        recordedBytes += written;
        voiceOverlayDirty = true;
    }
}

void playChunk() {
    if (!playbackActive) return;
    if (M5Cardputer.Speaker.isPlaying()) return;
    size_t bytesRead = playFile.read(reinterpret_cast<uint8_t*>(playBuffer), sizeof(playBuffer));
    if (bytesRead == 0) {
        stopPlayback();
        showStatus("Playback complete");
        return;
    }
    size_t samples = bytesRead / sizeof(int16_t);
    preparePlaybackSamples(playBuffer, playScaledBuffer, samples);
    M5Cardputer.Speaker.playRaw(playScaledBuffer, samples, SAMPLE_RATE, false, 1, 0);
    voiceOverlayDirty = true;
}
}

void appVoiceMemosEnter() {
    digitalWrite(LORA_NSS_PIN, HIGH);
    digitalWrite(LORA_RST_PIN, LOW);
    sdOk = SD.begin(SD_CS_PIN, SPI, 25000000);
    if (sdOk) ensureStorageDir();
    loadMemoList();
    memoSel = 0;
    newMemoName = sdOk ? defaultMemoName() : "";
    statusLine = "";
    statusUntilMs = 0;
    recordingActive = false;
    playbackActive = false;
    voiceOverlayDirty = true;
    lastVoiceOverlayMs = 0;
    applySpeakerVolume();
    voiceState = VoiceState::LIST;
    voiceDirty = true;
}

void appVoiceMemosLoop() {
    clearExpiredStatus();
    if (recordingActive) recordChunk();
    if (playbackActive) playChunk();

    if ((voiceState == VoiceState::RECORDING || voiceState == VoiceState::PLAYING)
        && voiceOverlayDirty && !voiceDirty && millis() - lastVoiceOverlayMs > 150) {
        drawVoiceOverlay();
        lastVoiceOverlayMs = millis();
        voiceOverlayDirty = false;
    }

    if (voiceDirty) {
        switch (voiceState) {
            case VoiceState::LIST: drawMemoList(); break;
            case VoiceState::NEW_NAME: drawNewName(); break;
            case VoiceState::RECORDING:
                drawRecording();
                voiceOverlayDirty = false;
                lastVoiceOverlayMs = millis();
                break;
            case VoiceState::PLAYING:
                drawPlaying();
                voiceOverlayDirty = false;
                lastVoiceOverlayMs = millis();
                break;
        }
        voiceDirty = false;
    }

    auto ev = readKeys();
    if (!ev.changed) return;

    for (char c : ev.chars) {
        if (c == '-') adjustPlaybackGain(-1);
        if (c == '=' || c == '+') adjustPlaybackGain(1);
    }

    if (ev.back) {
        if (voiceState == VoiceState::RECORDING) {
            stopRecording(false);
            return;
        }
        if (voiceState == VoiceState::PLAYING) {
            stopPlayback();
            return;
        }
        if (voiceState == VoiceState::NEW_NAME) {
            voiceState = VoiceState::LIST;
            voiceDirty = true;
            return;
        }
        goHome();
        return;
    }

    switch (voiceState) {
        case VoiceState::LIST: {
            int total = memoCount + 1;
            if (ev.up && memoSel > 0) { memoSel--; voiceDirty = true; }
            if (ev.down && memoSel < total - 1) { memoSel++; voiceDirty = true; }
            if (ev.enter) {
                if (memoSel == memoCount) {
                    newMemoName = defaultMemoName();
                    voiceState = VoiceState::NEW_NAME;
                    voiceDirty = true;
                } else {
                    if (!beginPlayback(makeMemoPath(memoFiles[memoSel]))) {
                        showStatus("Could not play memo");
                    }
                }
            }
            if (ev.fnKey) {
                for (char c : ev.chars) {
                    if ((c == 'd' || c == 'D') && memoSel < memoCount) {
                        SD.remove(makeMemoPath(memoFiles[memoSel]));
                        loadMemoList();
                        if (memoSel >= memoCount) memoSel = memoCount > 0 ? memoCount - 1 : 0;
                        showStatus("Deleted voice memo");
                        return;
                    }
                    if (c == 'q' || c == 'Q') {
                        goHome();
                        return;
                    }
                }
            }
            break;
        }

        case VoiceState::NEW_NAME:
            if (ev.del && newMemoName.length() > 0) {
                newMemoName.remove(newMemoName.length() - 1);
                voiceDirty = true;
            }
            for (char c : ev.chars) {
                if (newMemoName.length() < 24) {
                    newMemoName += c;
                    voiceDirty = true;
                }
            }
            if (ev.fnKey) {
                for (char c : ev.chars) {
                    if (c == 'q' || c == 'Q') {
                        voiceState = VoiceState::LIST;
                        voiceDirty = true;
                        return;
                    }
                }
            }
            if (ev.enter && newMemoName.length() > 0) {
                if (!newMemoName.endsWith(".wav") && !newMemoName.endsWith(".WAV")) newMemoName += ".wav";
                if (!beginRecording(makeMemoPath(newMemoName))) {
                    showStatus("Could not start recording");
                    voiceState = VoiceState::LIST;
                    voiceDirty = true;
                }
            }
            break;

        case VoiceState::RECORDING:
            if (ev.enter) {
                stopRecording(true);
            }
            break;

        case VoiceState::PLAYING:
            if (ev.enter) {
                stopPlayback();
                showStatus("Playback stopped");
            }
            break;
    }
}
