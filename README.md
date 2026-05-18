# CardputerOS

CardputerOS is a multi-app firmware for the **M5Stack Cardputer** built with PlatformIO and Arduino.

## CardputerOS 2.1

`2.1` keeps the split boot-mode design and expands the radio-side tools while cleaning up SD and mode behavior:

- `SD Mode`: MP3, Files, Photos, Games, Voice Memos, IR, USB Storage, Timer, HID Keyboard, Payloads
- `Radio Mode`: WiFi, BLE, Detector, SSH, GPS, LoRa, NFC

Switching between modes uses a restart prompt so SD/audio apps and radio apps do not fight over memory and device state.

## Feature Highlights

- Mode-aware launcher with restart-based `SD Mode` / `Radio Mode`
- SSH terminal over WiFi
- MP3 player from microSD
- Notes on internal flash
- Voice Memos with record, playback, delete, and volume control
- IR Remote with learn, save, delete, and resend
- Photos viewer for microSD images
- USB HID Keyboard and Rubber Ducky payload support over USB-C
- USB Storage mode for sharing the microSD card over USB-C
- Timer with presets, manual keypad entry, and Stopwatch mode
- GPS status, tracker, and wardriving tools for the LoRa/GNSS cap
- Basic LoRa raw-text chat for the LoRa/GNSS cap
- File Manager for SD and internal flash
- Game Boy emulator support
- BLE scanner and BLE spoofer tools
- WiFi radio tools including PMKID capture support
- Threat Detector, Rogue AP scan, NFC tools, and USB device testing

## Launcher Layout

### SD Mode
- MP3
- Notes
- Games
- Settings
- Files
- IR Remote
- Photos
- Voice Memos
- HID Keyboard
- USB Storage
- Timer

### Radio Mode
- SSH
- Settings
- GPS
- LoRa
- NFC
- Payloads
- BLE
- Detector
- WiFi
- USB Test

## Supported Files

| App | Format | Location |
|-----|--------|----------|
| MP3 | `.mp3` | microSD root |
| Photos | `.jpg`, `.jpeg`, `.png`, `.bmp` | microSD root |
| Game Boy | `.gb`, `.gbc` | microSD root |
| Voice Memos | `.wav` | microSD `/voice/` |
| Notes | `.txt` | internal flash `/notes/` |
| GPS Tracker | `.gpx` | microSD `/gps/` |
| GPS Wardriving | `.csv` | microSD `/gps/` |
| NFC Dumps | `.txt` | microSD `/nfc/` |
| Payloads | `.txt`, `.ds` | microSD `/payloads/` |
| BLE Wardriving | `.csv` | microSD `/ble/` |

## Quick Controls

- `fn + backspace`: back / home
- `Tab`: switch app mode where supported
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

## Flashing

### Prebuilt

Download `cardputer-os-v2.1-merged.bin` from the [Releases](../../releases) page and flash:

```bash
esptool.py --chip esp32s3 --port YOUR_PORT write_flash 0x0 cardputer-os-v2.1-merged.bin
```

### Build From Source

```bash
git clone https://github.com/RLee203/CardputerOS.git
cd CardputerOS
pio run --target upload
```

## Hardware Notes

- **Device:** M5Stack Cardputer
- **microSD:** required for MP3, Photos, Voice Memos, Games, GPS logs, wardriving logs, payloads, and NFC dumps
- **External IR receiver:** optional, only needed for learning new IR codes
- **LoRa/GNSS Cap:** required for GPS and LoRa apps
- **NFC Module (PN532):** required for the NFC app — connect via Grove on the LoRa cap (`G8=SDA`, `G9=SCL`, `3.3V`, `GND`)

### IR Receiver Wiring

- Cardputer `G` -> receiver `GND`
- Cardputer `5V` -> receiver `VCC`
- Cardputer `G2` / `GPIO2` -> receiver `OUT`

Tested module wiring:

- black -> `GND`
- red -> `VCC`
- white -> `OUT`

## 2.1 Notes

- Moved `Payloads` into `SD Mode`, where it fits the SD + USB workflow better than the radio stack
- Improved WiFi app flow with scan-first connection, visible password entry, and cleaner reconnect behavior after BLE use
- Expanded `Detector` with additional radio monitoring views, including BLE spam monitoring and Rogue AP detection
- Expanded `BLE` with BLE spoofing tools and safer WiFi shutdown before BLE init
- Added PMKID capture support to the WiFi radio tools
- Fixed MP3 teardown so leaving playback no longer breaks SD mounting for other SD apps
- Kept the split `SD Mode` / `Radio Mode` launcher and restart-based mode switching from `2.0`

## Credits

- [Launcher](https://github.com/bmorcelli/Launcher) helped inspire the USB storage app flow and status presentation
- [Bruce Firmware](https://github.com/pr3y/Bruce) helped inform parts of the IR learn/save/replay and GPS/wardriving direction

## License

MIT
