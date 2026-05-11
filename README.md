# CardputerOS

A multi-app operating system for the **M5Stack Cardputer** (ESP32-S3), built with PlatformIO and the Arduino framework.

> **Early Development Notice**
> This project is in early development stages. Bugs are expected and features may change between releases. Please report issues on the GitHub Issues page.

---

## Features

### Launcher
- Icon-based home screen with 6 apps in a 3×2 grid
- Quick-launch by hotkey (s=SSH, m=MP3, n=Notes, g=Games, p=Settings, f=Files)
- Live WiFi status and battery indicator in the status bar

### SSH Terminal
- Full SSH client using libssh over WiFi
- Scrollable terminal with 40-column × 15-row display
- Saved connection profiles (host, port, username, password)
- Ctrl+C / Ctrl+D / Ctrl+Z via fn+C / fn+D / fn+Z
- Command history navigation, Tab completion passthrough
- Cursor-position editing in the input line

### MP3 Player
- Plays MP3 files from a microSD card
- Browse and select tracks from a file list
- Play / Pause, Previous / Next track controls
- Volume up / down

### Notes
- Create, edit, and delete plain-text notes stored on internal flash (LittleFS)
- Full text editor with cursor movement and scrolling
- Save with fn+S

### Game Boy Emulator *(untested)*
- Runs Game Boy (.gb) and Game Boy Color (.gbc) ROMs from microSD card
- Powered by the [Peanut-GB](https://github.com/deltabeard/Peanut-GB) emulator core
- Grayscale display, ~60 fps target
- **Compatible files:** `.gb`, `.gbc`
- Controls: WASD = D-Pad, `\` = A button, Space = B button, Enter = Start, Tab = Select
- > ⚠️ The emulator has not been tested on hardware yet. ROM compatibility and performance are unknown.

### File Manager
- Browse files on microSD card or internal flash (LittleFS)
- Tab to switch between SD and internal storage
- Delete files with fn+D

### Settings
- **WiFi Networks** — scan nearby access points, connect, and save up to 5 networks; reconnect to saved networks without retyping passwords; forget individual networks
- **Brightness** — adjust screen brightness with fn+, / fn+/
- **Text Color** — cycle through 4 color themes (Green, White, Cyan, Amber)
- **Key Reference** — built-in controls cheatsheet
- **About** — version and current IP address

---

## Compatible File Formats

| App | Format | Location |
|-----|--------|----------|
| MP3 Player | `.mp3` | microSD card root |
| Game Boy Emulator | `.gb`, `.gbc` | microSD card root |
| Notes | `.txt` | Internal flash (`/notes/`) |
| SSH Profiles | JSON (auto-managed) | Internal flash |

---

## Controls Reference

| Key | Action |
|-----|--------|
| fn + backspace | Back / Home |
| fn + Q | Home (backup) |
| fn + ; | Up / History up |
| fn + . | Down / History down |
| fn + , | Left / Brightness − / Theme prev |
| fn + / | Right / Brightness + / Theme next |

### SSH Terminal
| Key | Action |
|-----|--------|
| fn + C / D / Z | Ctrl+C / Ctrl+D / Ctrl+Z |
| Tab | Tab complete |

### Game Boy
| Key | Action |
|-----|--------|
| W A S D | D-Pad |
| `\` | A button |
| Space | B button |
| Enter | Start |
| Tab | Select |

### MP3 Player
| Key | Action |
|-----|--------|
| Enter | Play / Pause |
| fn + ; / . | Previous / Next track |
| + / − | Volume up / down |

---

## Flashing the Firmware

### Option A — Pre-built binary (easiest)
Download `cardputer-os-v1.1-merged.bin` from the [Releases](../../releases) page and flash with [esptool](https://github.com/espressif/esptool):

```bash
esptool.py --chip esp32s3 --port COM3 write_flash 0x0 cardputer-os-v1.1-merged.bin
```

Replace `COM3` with your actual port (`/dev/ttyUSB0` on Linux/Mac).

### Option B — Build from source
Requires [PlatformIO](https://platformio.org/).

```bash
git clone https://github.com/RLee203/CardputerOS.git
cd CardputerOS
pio run --target upload
```

---

## Hardware Requirements

- **M5Stack Cardputer** (ESP32-S3FN8)
- microSD card (FAT32) — required for MP3 and Game Boy apps
- WiFi network — required for SSH

---

## Libraries Used

| Library | Purpose |
|---------|---------|
| M5Cardputer | Hardware HAL (display, keyboard, power) |
| LibSSH-ESP32 | SSH client |
| ESP32-audioI2S | MP3 audio decoding and I2S output |
| Peanut-GB | Game Boy emulator core |
| ArduinoJson | JSON config persistence |

---

## License

MIT
