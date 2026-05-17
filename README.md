# CardputerOS

CardputerOS is a multi-app firmware for the **M5Stack Cardputer** built with PlatformIO and Arduino.

## Highlights

- Multi-page launcher with page wrap navigation
- SSH terminal over WiFi
- MP3 player from microSD
- Notes on internal flash
- Voice Memos with record, playback, delete, and volume control
- IR Remote with learn, save, delete, and resend
- Photos viewer for microSD images
- USB HID Keyboard mode over USB-C
- USB Storage mode for sharing the microSD card over USB-C
- Timer with presets, manual keypad entry, and Stopwatch mode
- GPS status, tracker, and wardriving tools for the LoRa/GNSS cap
- Basic LoRa raw-text chat for the LoRa/GNSS cap
- File Manager for SD and internal flash
- Game Boy emulator support

## Current App Pages

### Page 1
- SSH
- MP3
- Notes
- Games
- Settings
- Files

### Page 2
- IR Remote
- Photos
- Voice Memos
- HID Keyboard
- USB Storage
- Timer

### Page 3
- GPS
- LoRa
- NFC

## Supported Files

| App | Format | Location |
|-----|--------|----------|
| MP3 | `.mp3` | microSD root |
| Photos | `.jpg`, `.jpeg`, `.png`, `.bmp` | microSD root |
| Game Boy | `.gb`, `.gbc` | microSD root |
| Voice Memos | `.wav` | microSD `/voice/` |
| Notes | `.txt` | internal flash `/notes/` |
| GPS Tracker | `.gpx` | microSD `/gps/` |
| Wardriving | `.csv` | microSD `/gps/` |
| NFC Dumps | `.txt` | microSD `/nfc/` |

## Quick Controls

- `fn + backspace`: back / home
- `Tab`: switch pages or app mode where supported
- `Enter`: select / start / pause in most apps
- `Del`: reset or delete where supported

### IR Remote
- `Tab`: learn new IR code
- `Enter`: send selected code
- `fn + D`: delete selected code
- `fn + T`: send test NEC frame

### Voice Memos
- `Enter`: open / play / stop / save
- `-` / `=`: playback volume down / up
- `fn + D`: delete selected memo

### USB Storage
- `Enter`: toggle SD sharing over USB-C
- `fn + backspace`: exit and remount SD

### Timer
- `Tab`: switch Timer / Stopwatch
- `Enter`: start / pause / resume
- `Del`: reset / clear
- Number keys: manual timer entry in `mmss`

### GPS
- `Enter`: open selected GPS mode / return to menu
- `Tracker`: `Enter` start/stop, `Del` reset
- `Wardriving`: `Enter` start/stop, `Del` reset

### LoRa
- Type text on the keyboard
- `Enter`: send current message
- `Del`: backspace input
- `fn + D`: clear message log

### NFC
- `fn + ; / .`: navigate menu
- `Enter`: select mode / start scan
- `Bksp`: back / home
- **Scan**: detect and display tag UID, type, SAK, and NDEF URI
- **Read**: full memory dump saved to microSD `/nfc/`
- **Clone**: copy source tag to a blank writable tag
- **Write NDEF**: write a URL or URI to an NTAG tag
- **Emulate**: spoof a scanned or loaded tag UID to a reader (UID-only; readers that require full MIFARE sector auth will show auth failed — normal)
- **Load**: browse and load saved dumps from microSD for Clone or Emulate

## Flashing

### Prebuilt

Download `cardputer-os-v1.8-merged.bin` from the [Releases](../../releases) page and flash:

```bash
esptool.py --chip esp32s3 --port YOUR_PORT write_flash 0x0 cardputer-os-v1.8-merged.bin
```

### Build From Source

```bash
git clone https://github.com/RLee203/CardputerOS.git
cd CardputerOS
pio run --target upload
```

## Hardware Notes

- **Device:** M5Stack Cardputer
- **microSD:** required for MP3, Photos, Voice Memos, and Game Boy
- **WiFi:** required for SSH
- **External IR receiver:** optional, only needed for learning new IR codes
- **LoRa/GNSS Cap:** required for the GPS and LoRa apps
- **NFC Module (PN532):** required for the NFC app — connect via Grove on the LoRa cap (G8=SDA, G9=SCL, 3.3V, GND)

### IR Receiver Wiring

- Cardputer `G` -> receiver `GND`
- Cardputer `5V` -> receiver `VCC`
- Cardputer `G2` / `GPIO2` -> receiver `OUT`

Tested module wiring:

- black -> `GND`
- red -> `VCC`
- white -> `OUT`

## v1.8

- Added full NFC app replacing the placeholder: Scan, Read (full dump to SD), Clone, Write NDEF, Emulate, and Load from SD
- NFC Emulate spoofs UID/ATQA/SAK to a reader using TgInitAsTarget; ATQA is captured from the live scan and saved in dump files so Load → Emulate uses the correct values
- Fixed NFC emulate freezing after a reader scans — TgRelease is now sent to cleanly end the RF session before re-arming
- Fixed NFC app freezing on back-to-home — Wire1 is initialized once and never deinitialized (Wire1.end() deadlocks the ESP32 I2C peripheral when no slave responds)
- Added PIN lock screen with configurable PIN via Settings — lock activates at boot and clears for the session once unlocked
- Added PIN confirmation (enter twice) when setting a new PIN in Settings

## v1.7

- Fixed SD card failing to mount when the LoRa/GNSS cap is attached — LoRa chip CS pin is now forced HIGH at boot so it does not collide with the SD card on the shared SPI bus
- Fixed LoRa app breaking SD card for all other apps on first open (removed mid-session SPI reinitialization)
- Fixed wardriving not scanning any networks — WiFi radio is now fully reset before each session, and scans run without requiring a GPS fix
- Added NFC placeholder on launcher page 3

## v1.6

- Added a working GPS app with `Status`, `Tracker`, and `Wardriving` modes
- Added GPX track logging and CSV wardriving logs to `/gps` on the SD card
- Added a working first-pass LoRa raw-text chat app for the LoRa/GNSS cap
- Promoted GPS and LoRa from placeholders to real app functionality

## v1.5

- Added working USB Storage over USB-C
- Added working Timer and Stopwatch
- Hid unused launcher slots on partial pages
- Kept the working IR Remote, Photos, Voice Memos, and HID Keyboard feature set

## Future Ideas

- ESP-NOW text messaging
- TV-B-Gone style IR utility

## Credits

- [Launcher](https://github.com/bmorcelli/Launcher) helped inspire the USB storage app flow and status presentation
- [Bruce Firmware](https://github.com/pr3y/Bruce) helped inform parts of the IR learn/save/replay direction

## License

MIT
