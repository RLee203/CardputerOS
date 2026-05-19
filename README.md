# CardputerOS

CardputerOS is a split-mode firmware for the **M5Stack Cardputer** built with PlatformIO and Arduino.

## CardputerOS 2.2

`2.2` keeps the restart-based mode split that separates local SD/media tools from radio-heavy tools so the Cardputer stays stable.

- `Multimedia`: MP3, Notes, Games, Files, IR Remote, Photos, Voice Memos, HID Keyboard, USB Storage, Timer, Payloads, Settings
- `Radio`: SSH, GPS, LoRa, NFC, BLE, Detector, WiFi, CC1101, nRF24, Key Fob, ESP-NOW, Settings

## Highlights

- Mode-aware launcher with restart-based switching between `Multimedia` and `Radio`
- MP3 player, file browser, photos, notes, voice memos, timer/stopwatch, and USB storage
- IR learn/save/replay
- USB HID keyboard and payload support over USB-C
- GPS status, tracker, and wardriving logs to SD
- LoRa raw text chat for the LoRa/GNSS cap
- WiFi tools, PMKID capture support, BLE tools, detector / threat scan, and rogue AP scan
- NFC tools, SSH terminal, and USB device testing
- Additional radio-side tools for `CC1101`, `nRF24`, `Key Fob`, and `ESP-NOW`

## App Layout

### Multimedia
- MP3
- Notes
- Games
- Files
- IR Remote
- Photos
- Voice Memos
- HID Keyboard
- USB Storage
- Timer
- Payloads
- Settings

### Radio
- SSH
- GPS
- LoRa
- NFC
- BLE
- Detector
- WiFi
- CC1101
- nRF24
- Key Fob
- ESP-NOW
- Settings

## File Support

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

## Flashing

### Prebuilt

Download `cardputer-os-v2.2-merged.bin` from the [Releases](../../releases) page and flash:

```bash
esptool.py --chip esp32s3 --port YOUR_PORT write_flash 0x0 cardputer-os-v2.2-merged.bin
```

### Build From Source

```bash
git clone https://github.com/RLee203/CardputerOS.git
cd CardputerOS
pio run --target upload
```

## Hardware Notes

- **Device:** M5Stack Cardputer
- **microSD:** required for MP3, Photos, Games, Voice Memos, GPS logs, wardriving logs, payloads, and NFC dumps
- **External IR receiver:** optional, only needed for learning new IR codes
- **LoRa/GNSS Cap:** required for GPS and LoRa apps
- **NFC Module (PN532):** connect via Grove on the LoRa cap (`G8=SDA`, `G9=SCL`, `3.3V`, `GND`)

### IR Receiver Wiring

- Cardputer `G` -> receiver `GND`
- Cardputer `5V` -> receiver `VCC`
- Cardputer `G2` / `GPIO2` -> receiver `OUT`

Tested module wiring:

- black -> `GND`
- red -> `VCC`
- white -> `OUT`

## 2.2 Notes

- Renamed the visible mode labels to `Multimedia` and `Radio`
- Added and wired in `CC1101`, `nRF24`, `Key Fob`, and `ESP-NOW`
- Moved `Payloads` into `Multimedia`, where it fits the SD + USB workflow better
- Fixed the list-scroll cursor issue in MP3 and Games
- Fixed MP3 teardown and playback startup regressions after the newer radio additions

Not fully tested yet:

- `CC1101`
- `nRF24`
- `Key Fob`
- `ESP-NOW`

## Credits

- [Launcher](https://github.com/bmorcelli/Launcher) helped inspire the USB storage app flow and status presentation
- [Bruce Firmware](https://github.com/pr3y/Bruce) helped inform parts of the IR learn/save/replay and GPS/wardriving direction

## License

MIT
