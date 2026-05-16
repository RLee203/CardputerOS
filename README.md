# CardputerOS

A multi-app operating system for the **M5Stack Cardputer** (ESP32-S3), built with PlatformIO and the Arduino framework.

> **Early Development Notice**
> This project is in early development stages. Bugs are expected and features may change between releases. Please report issues on the GitHub Issues page.

---

## Features

### Launcher
- Icon-based home screen with multiple 3×2 pages
- Quick-launch by hotkey across launcher pages
- `Tab` page switching plus left/right page wrap navigation
- Page 2 now includes media/tools apps, and page 3 holds future hardware apps
- Live WiFi status and battery indicator in the status bar

### SSH Terminal
- Full SSH client using libssh over WiFi
- Scrollable terminal with 40-column × 15-row display
- Saved connection profiles (host, port, username, password)
- Ctrl+C / Ctrl+D / Ctrl+Z via fn+C / fn+D / fn+Z
- Command history navigation, Tab completion passthrough
- Cursor-position editing in the input line
- Improved burst-output handling to reduce reboots on noisy Git commands

### MP3 Player
- Plays MP3 files from a microSD card
- Browse and select tracks from a file list
- Play / Pause, Previous / Next track controls
- Volume up / down

### Notes
- Create, edit, and delete plain-text notes stored on internal flash (LittleFS)
- Full text editor with cursor movement and scrolling
- Save with fn+S

### Voice Memos
- Record and save `.wav` voice memos to microSD
- Browse, play back, and delete saved recordings
- In-app playback volume control with top-bar indicator
- Cleaner voice playback path with light filtering and normalization

### IR Remote
- Learn IR signals with an external receiver on `GPIO2`
- Save learned remotes to internal flash
- Replay saved signals with the built-in Cardputer IR transmitter on `GPIO44`
- Supports longer raw captures for better real-device compatibility

### Photos
- Browse supported image files from the microSD card root
- Open photos full-screen on the Cardputer display
- Left / right photo navigation in the viewer
- Supports `.jpg`, `.jpeg`, `.png`, and `.bmp`

### HID Keyboard
- USB-C keyboard mode over the Cardputer's built-in USB device interface
- Explicit on-device enable flow for safer use
- Sends typed keys plus `Tab`, `Enter`, `Backspace`, and arrow keys to the host
- Clean status-card UI while active

### Game Boy Emulator 
- Runs Game Boy (.gb) and Game Boy Color (.gbc) ROMs from microSD card
- Powered by the [Peanut-GB](https://github.com/deltabeard/Peanut-GB) emulator core
- Grayscale display, ~60 fps target
- **Compatible files:** `.gb`, `.gbc`
- Controls: WASD = D-Pad, `\` = A button, Space = B button, Enter = Start, Tab = Select
  

### File Manager
- Browse files on microSD card or internal flash (LittleFS)
- Tab to switch between SD and internal storage
- Delete files with fn+D

### Placeholder Apps
- USB Storage
- Timer
- GPS
- LoRa

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
| Voice Memos | `.wav` | microSD card (`/voice/`) |
| Photos | `.jpg`, `.jpeg`, `.png`, `.bmp` | microSD card root |
| IR Remote | JSON + raw timings (auto-managed) | Internal flash |
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

### Voice Memos
| Key | Action |
|-----|--------|
| Enter | Open / play / stop / save |
| - / = | Playback volume down / up |
| fn + D | Delete selected memo |

### IR Remote
| Key | Action |
|-----|--------|
| Enter | Send selected saved code |
| Tab | Learn new IR code |
| fn + D | Delete selected code |
| fn + T | Send built-in NEC test frame |

### Photos
| Key | Action |
|-----|--------|
| Enter | Open photo / return to list |
| Left / Right | Previous / next photo |

### HID Keyboard
| Key | Action |
|-----|--------|
| Enter | Enable USB keyboard mode |
| fn + backspace | Exit and disable keyboard mode |
| Tab / Enter / Del / Arrows | Sent to host while active |

---

## Flashing the Firmware

### Option A — Pre-built binary (easiest)
Download `cardputer-os-v1.4-merged.bin` from the [Releases](../../releases) page and flash with [esptool](https://github.com/espressif/esptool):

```bash
esptool.py --chip esp32s3 --port (COM) write_flash 0x0 cardputer-os-v1.4-merged.bin
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
- External IR receiver module on `GPIO2` — optional, required only for IR learning

---

## IR Receiver Wiring

For IR learning in the `IR Remote` app, connect a 3-pin IR receiver module to the Cardputer Port A / Grove-style header:

- Cardputer `G` -> receiver `GND`
- Cardputer `5V` -> receiver `VCC`
- Cardputer `G2` / `GPIO2` -> receiver `OUT`

For the receiver module used during testing:

- black wire -> `GND`
- red wire -> `VCC`
- white wire -> `OUT`

The built-in Cardputer IR transmitter handles sending saved codes. The external receiver is only needed when learning new signals.

---

## Release Notes

### v1.4
- Added a working Photos app for SD card image viewing
- Added a working USB HID Keyboard app over USB-C
- Expanded the launcher to three pages and reorganized hardware/tool apps
- Added placeholder entries for USB Storage and Timer
- Kept IR Remote and Voice Memos improvements from v1.3

### v1.3
- Added a multi-page launcher with page wrap navigation and room for future apps
- Added a working IR Remote app with learn, save, delete, and resend support
- Added a working Voice Memos app with record, save, playback, delete, and in-app volume control
- Improved battery reporting for Cardputer power hardware
- Improved SSH receive handling for bursty command output

### Planned / Future Ideas
- USB Storage app
- Timer app
- GPS app
- LoRa app
- TV-B-Gone style IR utility

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
