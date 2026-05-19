# CardputerOS

CardputerOS is a split-mode firmware for the **M5Stack Cardputer** built with PlatformIO and Arduino.

## CardputerOS 2.3

`2.3` keeps the restart-based mode split that separates local SD/media tools from radio-heavy tools so the Cardputer stays stable.

- `Multimedia`: MP3, Notes, Games, Files, IR Remote, Photos, Voice Memos, HID Keyboard, USB Storage, Timer, Payloads, SD Health, Settings
- `Radio`: SSH, GPS, LoRa, NFC, BLE, Detector, WiFi, CC1101, nRF24, ESP-NOW, Settings

## Highlights

- Mode-aware launcher with restart-based switching between `Multimedia` and `Radio`
- MP3 player, file browser, photos, notes, voice memos, timer/stopwatch, and USB storage
- SD health / storage info screen with mount, size, free space, and basic read-write test
- IR learn/save/replay
- USB HID keyboard and payload support over USB-C
- GPS status, tracker, and wardriving logs to SD
- LoRa raw text chat for the LoRa/GNSS cap
- WiFi tools, PMKID capture support, BLE tools, detector / threat scan, and rogue AP scan
- NFC tools, SSH terminal, and USB device testing
- Sub-GHz RF capture, detect, replay, and spectrum via `CC1101` (315/433/868/915 MHz + custom frequency)
- 2.4 GHz spectrum scan, packet sniffer, and Mousejack mode via `nRF24`
- Device-to-device wireless chat via `ESP-NOW` (no router required)

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
- SD Health
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

Download `cardputer-os-v2.3-merged.bin` from the [Releases](../../releases) page and flash:

```bash
esptool.py --chip esp32s3 --port YOUR_PORT write_flash 0x0 cardputer-os-v2.3-merged.bin
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
- **PINGEQUA CC1101 / nRF24 hat:** supported by the `CC1101` and `nRF24` radio tools (`CC1101: CS=G13, IO0=G5`; `nRF24: CS=G6, IO0/CE=G4`)
- **NFC Module (PN532):** connect via Grove on the LoRa cap (`G8=SDA`, `G9=SCL`, `3.3V`, `GND`)

### IR Receiver Wiring

- Cardputer `G` -> receiver `GND`
- Cardputer `5V` -> receiver `VCC`
- Cardputer `G2` / `GPIO2` -> receiver `OUT`

## 2.3 Notes

- Added `CC1101` sub-GHz RF app: OOK capture, raw replay, spectrum view, custom frequency input
- Added `CC1101 Detect` mode for quick RF burst analysis and signal summary
- Fixed CC1101 PLL calibration bug that prevented 315 MHz reception (bad `setClb` overrides removed)
- Added `nRF24` 2.4 GHz app: 126-channel spectrum scan, packet sniffer, Mousejack/ESB promiscuous mode
- Added `ESP-NOW` messenger: broadcast chat between ESP32 devices, no router required
- Added `SD Health` in `Multimedia` for mount, capacity, free-space, and write/read test checks
- Removed `Key Fob` rolling-code analyzer app
- Removed CC1101 squelch scanner mode
- `CC1101` and `nRF24` hardware-verified with PINGEQUA module

## 2.2 Notes

- Renamed the visible mode labels to `Multimedia` and `Radio`
- Moved `Payloads` into `Multimedia`, where it fits the SD + USB workflow better
- Fixed the list-scroll cursor issue in MP3 and Games
- Fixed MP3 teardown and playback startup regressions after the newer radio additions

## Credits

- [Launcher](https://github.com/bmorcelli/Launcher) helped inspire the USB storage app flow and status presentation
- [Bruce Firmware](https://github.com/pr3y/Bruce) helped inform parts of the IR learn/save/replay, GPS/wardriving direction, and the RF tool workflow
- [bmorcelli/rc-switch](https://github.com/bmorcelli/rc-switch) powers the working CC1101 raw receive / scan-copy path used by the RF tools

## License

MIT
