# Bench provisioning and device label: design

Date: 2026-11-06

## Intent

The rev 2 board has the RP2040 on board. New boards come back blank, and
Kenneth sets them up one at a time at the bench: put the first firmware on,
check the device came up, and stick a label on it with what is needed to
find and log in to it later (hostname, MAC, web password). Today this is
all by hand: copying a UF2, opening PuTTY, reading the status screen and
writing things down.

The goal is one PC script that does a board in a single run, and a label
that stays correct for the life of the device.

## Goals

- One command per board: flash, wait for boot, read the device data, lock
  the password, make the label, log the device.
- A 40 x 40 mm white-on-black label (NIIMBOT M2, thermal transfer, white
  ribbon on black labels) with the layout below.
- The web password on the label stays valid: through factory resets,
  firmware updates and the serial menu, unless deliberately unlocked at the
  bench.
- A record of every device built, enough to reprint any label.

## Non-goals

- Setting a custom hostname: devices keep the automatic name
  `RF-Power-<last 6 MAC hex digits>` (`hostnameForDevice`, `lib/netconfig`).
- Setting network, SNMP or update-site settings during provisioning.
- Several boards at once.
- Registering devices on the update site or in NetBox (the QR code makes
  that possible later; not part of this work).

## Anti-goals

- Flashing `firmware.uf2` (no boot program; the device will not start).
  Only the release `-usb.uf2` is ever copied.
- A label that can silently become wrong (password regenerated after the
  label was printed).
- The script ever printing or logging anything from `secret/`.

## What exists today (no change needed)

- Hostname: with `sysName` empty the device uses `RF-Power-` + last 6 MAC
  hex digits (upper case), for DHCP and mDNS.
- MAC: made by the W5500 lwIP driver from the RP2040 board ID, so it is
  unique per board and known only once the firmware runs.
- Serial number: shown on the status screen (`Serial number:` line,
  `src/cli.cpp`), from the board ID (`src/host_stats.cpp`).
- Web password: on first boot each device makes a random 10-character
  default password (alphabet `abcdefghijkmnpqrstuvwxyz23456789`, user
  `admin`) and stores it in `data/web_default.txt` on LittleFS
  (`src/web_service.cpp`). A factory reset clears the user's own password
  (`webPassword`) but not this file, so the device goes back to the default
  password. Firmware updates do not touch LittleFS.
- Release files: `python tools/fw_release.py --field` writes
  `release/rf-power-monitor-<version>-usb.uf2` (boot program + slot A,
  signed with the field key). The field key exists in `secret/`.

## Decisions

### 1. Firmware: password lock

Serial menu option 7 ("New default web password") replaces the default
password, which would make the label wrong. Kenneth wants to be able to
disable it, and to unlock it again.

- New lock flag stored on LittleFS in its own file next to
  `data/web_default.txt` (e.g. `data/web_default.lock`). It is not part of
  the settings JSON and is not touched by a factory reset, so it survives
  resets and firmware updates the same way the password file does.
- While locked, option 7 refuses with a message saying the default password
  is locked because it is printed on the device label, and does not
  change anything.
- Hidden menu commands (not listed in the menu, like `checkin` and
  `lldpdump`): one to lock, one to unlock. Unlock is for the bench, when
  the label will be reprinted.
- Option 6 (own password) keeps working while locked: it does not change
  the default password.

**Losing the password file (Claude's call):** today, a missing or
invalid `data/web_default.txt` makes the firmware silently create a new
password, which would make the label wrong. The default password gets two
copies (A/B, like the settings files); at boot the firmware uses a valid
copy and repairs the other. Only when no copy is valid does it make a new
password, and if the lock file says the password was locked it reports
this loudly (boot message and status screen: "default web password lost,
label is wrong, relabel at the bench"). A fully reformatted LittleFS
loses the lock file too and cannot be detected; that risk is accepted.
Devices with the old single file are migrated by reading it as copy A.

### 2. Firmware: machine-readable device info

A hidden menu command for the script that prints the device data in a
fixed, easy-to-parse form (one `key=value` per line between a start and an
end marker): hostname, MAC, serial number, firmware version, default web
password, lock state. The script never scrapes the human status screen.

Printing the default password over USB serial is already possible (option 7
shows a new one; the status/password screens are on the same local port),
so this does not widen access: USB serial is physical access.

### 3. Bench script

A Python script in `tools/` (stdlib plus `pyserial`, which the HIL tests
already use, Pillow for the image, and `segno` for the QR code: pure
Python, no native libraries; Claude's call). Steps for one board:

1. **Firmware file**: the newest `release/rf-power-monitor-<version>-usb.uf2`
   built with `--field`, or one given on the command line. The script
   refuses any file that is not a release `-usb.uf2`.
2. **Flash**: Kenneth holds BOOTSEL and plugs the board in; the script
   waits for the `RPI-RP2` drive and copies the UF2 onto it.
3. **Wait for boot**: find the board's new USB serial port (the port that
   appears after the copy), open it with DTR set, wait for the menu.
4. **Read** the device info (decision 2) and check it: hostname matches the
   MAC tail, password is 10 characters from the known alphabet, firmware
   version matches the UF2's version.
5. **Lock** the password (decision 1) and read back that it is locked.
6. **Label**: make the label PNG (layout below) at 300 dpi and save it.
7. **Print**: if direct printing to the M2 works (see "Printing"), print it;
   otherwise tell Kenneth where the PNG is, to print from the NIIMBOT app.
8. **Log**: append a row to a CSV: date/time, hostname, MAC, serial number,
   firmware version, board revision, password, label file.

The CSV holds the web passwords, so it lives in a gitignored folder.

The board revision ("rev 2") cannot be read from the firmware; it is a
setting of the script (default `rev 2`, overridable on the command line).

A reprint mode makes the label again from a CSV row (by hostname, MAC or
serial number) without touching a device.

**Existing boards (Kenneth's decision):** the script can also run on a
board that already has firmware and is connected by USB, without flashing
(steps 3 to 8). Cases: a board that was unlocked and got a new password
with option 7, or an older carrier board. It reads the device, locks it,
makes a new label and appends a new CSV row. For the same MAC the newest
row wins in reprint mode; old rows stay as history. Running it on a board
that is already locked is fine (lock is then a no-op).

**Which UF2 (Claude's call):** `fw_release.py` writes the same file name
with or without `--field`, so the script does not trust the name. It
checks the slot A image inside the UF2 with the field public key (the
same check the boot program makes) and refuses a development-signed file
unless given an explicit `--dev` flag (for testing the script itself).
A dev-signed board in the field would reject field-signed updates.

### 4. Label layout (agreed from mockups)

40 x 40 mm, white print on a black label, 300 dpi (472 x 472 dots).
Top to bottom:

| Line | Content | Style |
|---|---|---|
| 1 | `RF Power Monitor` | large, bold |
| 2 | `rev 2` | small |
| | horizontal line across | |
| 3 | hostname, e.g. `RF-Power-A1B2C3` | bold, a little smaller than line 1 |
| 4 | `MAC  2C:CF:67:A1:B2:C3` | monospace, label in bold |
| 5 | `User admin` | |
| 6 | `PW   k7mq2xr9tp` | monospace |
| bottom left | `S/N <serial>`, `FW <version>`, date `YYYY-MM-DD` | small |
| bottom right | QR code, about 12 mm | white on black |

QR content: `RFPM;H=<hostname>;M=<MAC, 12 hex digits>;S=<serial>;FW=<version>`
(no password in the QR code).

FW version and date are as-shipped values from the bench run (Claude's
call): the label is not reprinted after field updates, and anything that
needs the current version reads it from the device (SNMP, web page).

Margins at least 2 mm. The reference mockup is the agreed "E1 revised"
page; the builder reproduces it in Pillow, not pixel-exact.

### 5. Printing

The NIIMBOT M2 has USB-C and Bluetooth, 300 dpi, 48 mm (576-dot) head.
NIIMBOT does not publish its protocol. The open-source NiimBlue /
niimbluelib project supports "M2 (2024)" but the M2_H is not on its tested
list, and the Python `niimprint` library does not list the M2.

So the PNG is the firm deliverable. Direct printing from the script is a
second step, built and tested only once Kenneth has the printer. If it does
not work, the PNG is printed from the NIIMBOT app. Do not update the
printer's firmware for this: the M2_H refuses to go back to an older
version.

## Constraints

- Project rules in `.clinerules` apply: pure logic in `lib/` with native
  tests (the lock decision and the device-info output format), thin LittleFS
  layer in `src/`, every change with tests, CHANGELOG, README (menu docs),
  version bump.
- Firmware stays compatible: devices that have no lock file behave as
  unlocked (today's behaviour).
- Never print, log or commit anything from `secret/`.

## Testing

- Native: lock logic (option 7 refused while locked, allowed when
  unlocked), device-info formatting.
- Native: password A/B copy selection (both valid, one corrupt, both
  corrupt, old single file), and the "lost while locked" report.
- Tool tests (`test/tools`): label layout from fixed data (image size,
  white-on-black, the QR modules in the image match the matrix `segno`
  makes for the expected text; no native QR decoder needed), CSV append and
  reprint lookup (newest row per MAC wins), UF2 selection (refuses
  `firmware.uf2` and dev-signed files without `--dev`), parsing of the
  device-info block including truncated output.
- HIL: lock, option 7 refused, factory reset keeps lock and password,
  unlock, option 7 works again; device-info command output. The HIL test
  restores the device's lock state afterwards.
- The flashing step itself is tested by hand at the bench (needs BOOTSEL).

## Left to the builder

- Exact names of the hidden commands, the lock file and the script.
- Fonts (bundled free fonts so the label looks the same on any PC).
- How the script picks the new serial port (port that appears after the
  copy, matched by the Pico's USB VID/PID).
- Whether the label PNG is rotated for the printer's feed direction.
