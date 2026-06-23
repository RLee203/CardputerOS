# CardputerOS v2.4 Releases

## Cardputer
- cardputer/cardputer-os-v2.4-merged.bin
- Flash at `0x0`

## T-Embed CC1101
- tembed/tembed-os-v2.4-debug-merged.bin
- Flash at `0x0`

Split T-Embed bins:
- tembed/bootloader.bin at `0x0000`
- tembed/partitions.bin at `0x8000`
- tembed/firmware.bin at `0x10000`

Notes:
- The T-Embed build in this folder keeps RF debug logging enabled.
- The T-Embed build is the working backup from the validated CC1101 tuning session.
