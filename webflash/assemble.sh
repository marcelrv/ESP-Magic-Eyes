#!/usr/bin/env bash
# assemble.sh <out-dir> <version>
#
# Collects one built firmware (run `pio run` and `pio run -t buildfs`
# first) into <out-dir> with an ESP Web Tools manifest.json next to it.
# Used by .github/workflows/webflash.yml for both the "latest" (main) and
# the "stable" (release) channel; the same files are attached to releases.
#
# Separate parts rather than one merge_bin image on purpose: merge_bin
# fills the gaps with 0xFF, which would wipe the NVS partition (WiFi,
# calibration, passwords) on every non-erasing "update" install.
set -euo pipefail

OUT=$1
VER=$2
B=.pio/build/esp32dev

mkdir -p "$OUT"
cp "$B/bootloader.bin" "$B/partitions.bin" "$B/firmware.bin" "$B/littlefs.bin" "$OUT/"
cp ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin "$OUT/"

# App and filesystem offsets come from this checkout's partitions.csv, so
# each channel's manifest matches the partition table it was built with.
# Bootloader / partition table / boot_app0 are fixed ESP32 Arduino offsets.
off() { awk -F, -v n="$1" '$1 ~ "^"n" *$" {gsub(/ /,"",$4); print $4}' partitions.csv; }
APP=$(off app0)
FS=$(off littlefs)
test -n "$APP" && test -n "$FS"

# Part paths are relative to the manifest's own URL.
cat > "$OUT/manifest.json" <<EOF
{
  "name": "ESP Magic Eyes",
  "version": "$VER",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [
        { "path": "bootloader.bin", "offset": 4096 },
        { "path": "partitions.bin", "offset": 32768 },
        { "path": "boot_app0.bin",  "offset": 57344 },
        { "path": "firmware.bin",   "offset": $(printf '%d' "$APP") },
        { "path": "littlefs.bin",   "offset": $(printf '%d' "$FS") }
      ]
    }
  ]
}
EOF
cat "$OUT/manifest.json"
