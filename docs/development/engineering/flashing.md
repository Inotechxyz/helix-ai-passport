<p align="right">
  <a href="flashing.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Flash Firmware to the Device

This guide covers finding the serial port, choosing a merged or segmented image,
writing it to the ESP32-C3, and checking startup. Build and validate the
firmware first — see [build and test](build-and-test.md). Flashing proves only
that an image was written; it is not hardware acceptance.

## Before you write

- **No original-firmware backup is required.** Do not block flashing on a
  full-Flash readback. Flashing replaces the installed firmware, and this
  workflow keeps no automatic rollback copy.
- **Firmware and user data are separate.** The merged image can reset NVS and
  PHY data. If settings, profiles, or records must survive, export them first
  and follow the data policy in
  [flashing and stored data](firmware-layout.md#flashing-and-stored-data).
- **Bind the flash to exact facts.** Confirm the port, the image path and hash,
  the write offsets, and the intended data impact before writing. A connected
  device or an earlier approval for another image is not authorization.
- **Stop anything else using the port.** Monitors, log watchers, and terminal
  programs hold the serial port and cause access errors. Do not kill unrelated
  processes to free it.

The target is an ESP32-C3 with 8 MB Flash. It normally exposes Espressif's
native USB Serial/JTAG; boards with a USB-UART bridge enumerate differently.

## Find the serial port

Port names change between connections. Discover the port instead of reusing an
old value.

Windows PowerShell:

```powershell
Get-CimInstance Win32_PnPEntity |
  Where-Object { $_.Name -match 'COM\d+' } |
  Select-Object Name, DeviceID
```

Linux:

```bash
python -m serial.tools.list_ports -v
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

macOS:

```bash
python -m serial.tools.list_ports -v
ls /dev/cu.usbmodem* /dev/cu.usbserial-* 2>/dev/null
```

Identify the device by its USB IDs or port description:

- Espressif USB Serial/JTAG: vendor ID `303A`, product ID `1001`. Linux exposes
  it as `/dev/ttyACM*`; Windows assigns a `COM` number.
- USB-UART bridge: for example `10C4` (CP210x) or `1A86` (CH34x). Linux exposes
  it as `/dev/ttyUSB*`.

If several candidates exist or the description is ambiguous, ask the user to
identify the intended device. Do not assume the first port is the target.

## Choose the image

| Goal | Artifact | Write range | Data impact |
| --- | --- | --- | --- |
| Blank device or intentional complete refresh | Verified `build/FoloToy-AI-Passport-full.bin` (merged) | From `0x0` | May reset NVS and PHY data |
| Keep settings and records | Bootloader, partition table, and application from one build | Actual configured offsets | Does not touch NVS unless an image overlaps it |

Produce and verify a merged image:

```bash
./tools/validate.sh --firmware
python3 tools/archive_firmware.py verify build/firmware/<full-bin-sha256>/
```

The gate writes `build/FoloToy-AI-Passport-full.bin` and archives a bundle with
the matching ELF and images under `build/firmware/<full-bin-sha256>/`. Compare
the image hash with the bundle's `manifest.json` before delivery.

For incremental development, `idf.py build` also produces `build/flash_args`
with the configured offsets. Read it as data; never execute it as a shell
script. Never write the application-only `build/FoloToy-AI-Passport.bin` at
`0x0`, and never mix images from different builds.

The layout currently tracked by `partitions.csv` writes the bootloader at
`0x0`, the partition table at `0x8000`, and the factory application at
`0x30000`. Confirm these values against `flash_args` and `partitions.csv`
before flashing; custom layouts and the upstream default (application at
`0x10000`) differ.

## Flash

Complete refresh with the merged image, from `0x0`:

```text
esptool --chip esp32c3 -p <port> -b 460800 write-flash \
  --flash-mode dio --flash-size 8MB --flash-freq 80m \
  0x0 build/FoloToy-AI-Passport-full.bin
```

The command spelling follows esptool v5. For esptool v4, use `esptool.py` and
underscored commands such as `write_flash` and `--flash_mode`. Check the
installed tool's help instead of assuming a spelling.

Segmented flash that preserves NVS, using the activated ESP-IDF environment:

```bash
idf.py -p <port> flash
```

`idf.py flash` writes only the images listed in `build/flash_args`. The
equivalent explicit esptool command is:

```text
esptool --chip esp32c3 -p <port> -b 460800 write-flash \
  --flash-mode dio --flash-size 8MB --flash-freq 80m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x30000 build/FoloToy-AI-Passport.bin
```

Use the paths and offsets that match the build you are flashing. To update only
the application after the bootloader and partition table are already correct,
run `idf.py -p <port> app-flash`.

esptool verifies written data by default and reports `Hash of data verified`.
If verification fails, stop and check the cable, power, port, and image
instead of rewriting blindly. A successful write ends with a hard reset, and
the device starts the new firmware.

## Check startup

With ESP-IDF activated:

```bash
idf.py -p <port> monitor
```

Without ESP-IDF, a plain serial terminal at 115200 baud works, for example:

```text
python -m serial.tools.miniterm <port> 115200
```

Opening a monitor can reset the board, so expect a reboot. Collect a bounded
log window, then stop the monitor before running other tools or handing the
port back. If there is no output, the application may be idle or in deep
sleep; reset once and recheck rather than rewriting the image.

Check for the bootloader handoff and application startup messages. Then
exercise the changed behavior on the device using the applicable
[hardware checklist](../../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md).
A clean boot log proves neither display, audio, button, nor radio behavior;
report device-test results separately from the build.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Port busy or access denied | Stop monitors and log watchers; confirm no other program owns the port. Do not kill unrelated processes. |
| Failed to connect | Use a data-capable cable, confirm the device is powered, and recheck the port. Native USB Serial/JTAG normally enters download mode automatically; manual boot mode is a fallback only where the board supports it. |
| Hash verification failed | Stop after one retry. Check cable, power, port, and image identity; do not change the approved target or write range without new authorization. |
| No boot log | Reset once, confirm 115200 baud and the correct port. The application may be idle or in deep sleep. |
| Suspected Flash corruption | Do not erase the whole chip as routine repair. Verify the image and offsets first; save user data before any erase. |

`idf.py erase-flash` and whole-chip erase delete NVS, settings, and records.
Use them only when a complete erase is explicitly intended and required data
has already been saved.

## Record the flash

Capture the port, image path and SHA-256, write offsets, esptool result, a
bounded startup log, and the behavior you observed. Keep raw logs local and
sanitize credentials, tokens, and personal data before sharing. Route crashes
to the debugging workflow with the matching ELF from the same build.

Related documents: [build and test](build-and-test.md),
[firmware layout and data policy](firmware-layout.md#flashing-and-stored-data),
[environment setup](environment-setup.md),
[hardware guide](../../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md), and
[AI development guide](../ai-guide.md).
