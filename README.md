# CardputerOS

CardputerOS is a split-mode ESP32-S3 firmware project with active builds for the **M5Stack Cardputer** and the **LILYGO T-Embed CC1101**, built with PlatformIO and Arduino.

## CardputerOS 2.4

`2.4` adds background audio, a calculator, an on-device firmware installer, and a new dual-OTA partition layout â€” while keeping the restart-based mode split that separates local SD/media tools from radio-heavy tools so the Cardputer stays stable.

- `Multimedia`: MP3, Notes, Games, Files, IR Remote, Photos, Voice Memos, HID Keyboard, USB Storage, Timer, Payloads, SD Health, Calculator, Firmware, Settings
- `Radio`: SSH, GPS, LoRa, NFC, BLE, Detector, WiFi, CC1101, nRF24, ESP-NOW, Calculator, Settings

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

- MP3 (background playback supported)
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
- Calculator
- Firmware Installer
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
- Calculator
- Settings

## File Support

| App | Format | Location |
| --- | --- | --- |
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
| Firmware Installer | `.bin` | microSD root |

## Flashing

### Prebuilt

Prebuilt artifacts live in [`releases/v2.4`](./releases/v2.4):
Prebuilt artifacts are split by device:

- Cardputer: [`releases/v2.4`](./releases/v2.4)
- T-Embed CC1101: [`releases/tembed-v2.4.1`](./releases/tembed-v2.4.1)

Flash Cardputer with the merged image:

```bash
esptool.py --chip esp32s3 --port YOUR_PORT write_flash 0x0 releases/v2.4/cardputer/cardputer-os-v2.4-merged.bin
```

Flash T-Embed with the merged image:

```bash
esptool.py --chip esp32s3 --port YOUR_PORT write_flash 0x0 releases/tembed-v2.4.1/tembed-os-v2.4.1-debug-merged.bin
```

Flash the split T-Embed bins if needed:

```bash
esptool.py --chip esp32s3 --port YOUR_PORT write_flash 0x0000 releases/tembed-v2.4.1/bootloader.bin 0x8000 releases/tembed-v2.4.1/partitions.bin 0x10000 releases/tembed-v2.4.1/firmware.bin
```
> **Note:** v2.4 uses a new dual-OTA partition layout. Flashing a merged binary for the first time will reset saved settings (brightness, theme, lock PIN, boot mode) to defaults. This is a one-time migration â€” settings persist normally after that.
> **Note:** the packaged T-Embed build currently keeps RF debug logging enabled because it is the validated CC1101 tuning backup.

### Build From Source

```bash
git clone https://github.com/RLee203/CardputerOS.git
cd CardputerOS
pio run --target upload
```

## Hardware Notes

- **Devices:** M5Stack Cardputer and LILYGO T-Embed CC1101
- **microSD:** required for MP3, Photos, Games, Voice Memos, GPS logs, wardriving logs, payloads, and NFC dumps
- **External IR receiver:** optional, only needed for learning new IR codes
- **LoRa/GNSS Cap:** required for GPS and LoRa apps
- **PINGEQUA CC1101 / nRF24 hat:** supported by the `CC1101` and `nRF24` radio tools (`CC1101: CS=G13, IO0=G5`; `nRF24: CS=G6, IO0/CE=G4`)
- **NFC Module (PN532):** connect via Grove on the LoRa cap (`G8=SDA`, `G9=SCL`, `3.3V`, `GND`)

### IR Receiver Wiring

- Cardputer `G` -> receiver `GND`
- Cardputer `5V` -> receiver `VCC`
- Cardputer `G2` / `GPIO2` -> receiver `OUT`

## 2.4 Notes

- Added **background MP3** â€” leave the MP3 app while a track is playing and music continues in the background. A `(*)` indicator appears in the launcher status bar. Music auto-suspends when an SD-dependent app opens and resumes from the same position when you return home. Apps that don't use the SD card (Notes, Calculator, Timer, Settings) never interrupt playback.
- Added **Calculator** app (hotkey `j`, available in both modes) â€” recursive-descent expression parser with live preview, operator precedence, `x`=multiply shortcut, and scientific functions: `sqrt(`, `sq(`, `abs(`, `pow(x,y)`, `log(`, `ln(`, `floor(`, `ceil(`
- Added **Firmware Installer** app (hotkey `o`, Multimedia mode only) â€” scans microSD root for `.bin` files, auto-detects merged vs app-only binaries, and flashes via OTA with a progress bar. Requires the v2.4 dual-OTA partition layout.
- Added **dual-OTA partition table** (`partitions.csv`) â€” two 2 MB OTA slots + 3.9 MB LittleFS on 8 MB flash. Required for the Firmware Installer; enables flashing other ESP32-S3 firmware directly from the device.
- Added **`fn+M` mode-switch shortcut** in the launcher â€” toggles between Multimedia and Radio mode without navigating back to the boot screen.
- Fixed emulator freeze â€” added `yield()` after each Game Boy frame to prevent FreeRTOS watchdog resets during intensive emulation.
- Fixed SD file delete â€” `SD.remove()` path now correctly includes the leading `/`.
- Fixed mode picker text centering â€” MEDIA/RADIO labels are now dynamically centred.
- Fixed brightness control â€” added a visual fill bar and doubled the step size so changes are visible.
- Fixed launcher navigation highlight â€” `fn+arrow` keys were being swallowed by the `fn+M` handler, preventing selection box updates.
- Fixed background audio heap fragmentation â€” `Audio` object is now reused across track transitions rather than destroyed and recreated, eliminating "mp3decoder could not be initialized" errors after long sessions.

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



