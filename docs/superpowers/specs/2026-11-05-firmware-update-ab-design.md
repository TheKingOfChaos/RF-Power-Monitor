# Firmware update over the web page, A/B slots with rollback: design

## Intent

Devices will be installed in the field, where a USB visit means a drive out. The user wants to update their
firmware over the network: one device at a time from the web page, and many devices from a script on the
PC. Above all, an update must never leave a device dead or unreachable. If the new firmware crashes, hangs or
comes up but cannot be reached over the network, the device must go back to the firmware it had before,
by itself.

Only firmware signed with the user's key may be installed. The web password alone is not enough: someone
who knows it, or someone on the network path (the web page is plain HTTP), must not be able to install
their own firmware.

No devices are in the field yet. The boot program can therefore be installed over USB in the workshop.
No in-field migration from today's layout is needed.

## Goals

- **Two firmware slots, A and B**, plus a small boot program of our own that decides which slot to start.
  An update is always written to the slot that is not running. The running firmware is never touched while
  an update is uploaded.
- **Signed updates.** A release is one update file (*designer's call*: `rf-power-monitor-<version>.upd`).
  It holds the image, its version and an Ed25519 signature over both. The device accepts a file only when
  the signature checks out against the public key compiled into the firmware and the boot program.
  Unsigned, wrongly signed, corrupted or truncated files are rejected before anything switches.
- **The boot program verifies the signature of a slot before it starts it.** A slot damaged in flash is
  never run. If neither slot verifies, the boot program stays put and blinks the LED in a distinct pattern.
  A USB visit is the only way out of that state, and it should be impossible to reach without flash failure.
- **Trial boot and automatic rollback.** After a verified upload the device restarts into the new slot
  marked *trial*. The trial firmware must prove it is healthy, or the boot program goes back to the old
  slot. Two things count as failure:
  - it does not boot: a crash or a hang (the existing 8 s watchdog), 3 crash restarts in a row (see
    "What counts as a crash" below);
  - it boots but is not reachable over the network (*designer's call*): no valid authenticated web request
    and no valid SNMP request (right community) within 10 minutes. The 10 minutes count only while the
    link is up and the device has an IPv4 or IPv6 address, so a switch that is down or slow DHCP does not
    throw away a good release. A firmware that breaks the network driver would never see a link, so there
    is also a hard limit: 60 minutes on trial without proof, whatever the link did, and it rolls back. That
    is always safe, because the old slot is known good.

  On success the new slot becomes the active one and the old one stays as the fallback. After a rollback
  the old slot stays active, and the device reports that the update was rolled back and why.
  **Forgetting a failed release** (the user's decision, 2026-11-06, after the final review): after a
  rollback (crash or unreachable) the firmware that runs again erases the failed slot's header at
  start-up, so that slot reads Empty, as after a rejected upload. The boot program still never writes
  flash. A later fallback or a manual switch can then only reach firmware that never failed: without
  this, a kept firmware's crash loop could fall back to a release known to be unreachable, keep it
  without a trial, and leave the device unreachable for good. A verified upload whose boot record write
  fails is erased the same way (it never ran a trial).
- **Fallback after a version is kept** (the user's decision): if the active, kept firmware later has 3
  crash restarts in a row (for example a bug that shows only after days), the boot program starts the
  other slot, provided its signature verifies. The device reports this like a rollback. The network rule
  applies only during a trial, which is safe because the other slot only ever holds the previous kept
  release (a rolled-back release is erased, see above), or nothing. One gap remains: a power cut during
  the 1-2 s signature check at the end of an upload, after the slot header was written and before the
  boot record names it, leaves a verified image that never ran a trial; a later crash loop could fall
  back to it. Closing it would need the signature checked before the header page is written.
- **What counts as a crash** (the user's decision, mechanism *designer's call*): only a watchdog reset that
  the firmware did not ask for. A hang or a crash ends in the watchdog. Power-on resets (a power cut, the
  PoE switch restarting) never count, however many in a row, in trial or kept. That includes the trial,
  so flickering power cannot cause a rollback on its own. Planned restarts (menu, web page, factory reset,
  the restart after an upload or a slot switch) leave a "planned" mark in a watchdog scratch register that
  survives the reset, and do not count either. The crash count resets after a boot that runs long enough
  to be called healthy (*designer's call*: 5 minutes, or the trial being confirmed).
- **Power-safe at every step.** A power cut during the upload, during the switch or during a trial boot
  still leaves a device that boots a verified firmware. The boot state (active slot, trial slot, trial
  boot count, last result) is stored power-safe (*designer's call*: two flash sectors with a sequence
  number and a checksum, newest valid one wins, the same pattern as the settings A/B files).
- **Downgrades are allowed.** Any correctly signed file is accepted, older versions included (the user's
  decision).
- **Settings survive rollbacks and downgrades.** Today any firmware rejects a settings file with a different
  layout version and falls back to defaults, which for a field device means losing its static IP and
  community. Rule (*designer's call*): a firmware that changes `SETTINGS_VERSION` writes the new layout
  to new files and leaves the old layout's files intact and up to date, so an older firmware comes up with
  the settings it knew. Nothing changes yet, since the layout is still version 1. The rule goes into
  `.clinerules` (Configuration Storage) and must be covered by tests when the first bump happens.
- **Web page "Firmware"**, behind the existing login. It shows the running version and slot, the version
  in the other slot, whether the running one is on trial or kept, and the last update's result (installed,
  rejected and why, rolled back and why). Upload: pick the `.upd` file, press upload, see progress, then
  "verified, restarting". After the restart the page shows trial and then kept. *Designer's call:* a button
  "Switch to the other slot" (with a confirmation) that starts the other verified slot as a trial. That is
  a manual way back after a bad release the automatic checks did not catch, since downgrades are allowed
  anyway.
- **The upload is plain HTTP**, the same request the page makes, so a script can do it.
  *Designer's call:* `POST /firmware` with the raw file as body (`application/octet-stream`) and the session
  cookie from the normal login. The page sends it with a few lines of JavaScript. This avoids
  multipart parsing on the device.
- **Script `tools/fw_update.py`** (*designer's call*: Python, as the HIL tests): takes the `.upd` file, a
  list of devices and the web password (from an environment variable or a prompt, never on the command line
  history). It updates one device at a time: log in, upload, wait until the device reports the new version
  as kept, then the next. It prints one line per device: updated, rejected, rolled back, unreachable.
  *Designer's call (the user did not object):* it stops at the first failure, so a bad release does not
  reach the whole fleet. A `--continue` flag carries on.
- **Visible over SNMP and in LibreNMS.** New objects under the enterprise tree for the running slot, each
  slot's version, trial/kept and the last update result. A trap when an update is kept and when it is
  rolled back (*designer's call*: two traps, v1 and v2c, varbinds with the versions and the reason).
  LibreNMS definition updated so the event shows up there. `docs/MIB.md` and `mibs/` updated.
- **Serial menu**: the status screen shows the slots, their versions and the trial state.
- **Generating the firmware key** (the user asked for this explicitly). One command creates the signing key
  pair (*designer's call*: `tools/fw_keygen.py`):
  - The private key is 32 random bytes from the operating system's secure random source (Python's
    `secrets`/`os.urandom`), never from a seeded or ordinary random number generator.
  - The private key is written to `secret/fw_signing_key` (gitignored), never printed and never logged.
    The file is created only if it does not exist: the tool refuses to overwrite an existing key, since
    that would lock out every device signed with the old one. Replacing a key needs a deliberate
    `--force` and a typed confirmation.
  - The public key is written into the repo as a source file the firmware and the boot program compile
    in (*proposal*: `include/fw_public_key.h`), to be committed.
  - It prints only the public key's fingerprint (e.g. the first 16 hex digits of its SHA-256), which the
    status screen, the web page's Firmware page and `fw_update.py` also show. That way the user can see
    which key a device trusts.
  - It prints a reminder to back up `secret/fw_signing_key` somewhere safe outside this PC.
  - A development key is generated the same way by the builder for CI and tests (see "Development key
    vs the user's key" below). It is never the field key.
- **Signing a release.** A build step turns `firmware.bin` into the signed `.upd` file with the private key.
  If the key file is missing, it fails with a message pointing to `fw_keygen.py`. The user's manual
  explains the key's life cycle: generate once, back up, keep secret. Losing it means every device can
  only be updated over USB again. A leaked key means anyone can sign, and changing it takes a USB visit.
- Ed25519 keys and signatures must be byte-compatible between the PC tools and the device. A test signs
  with the PC tool and verifies with the device code (native test), and the reverse is checked against a
  published Ed25519 test vector (RFC 8032).

## Non-goals

- Key rotation or revoking a leaked key over the network. A key change takes a USB visit.
- Encrypting the firmware. It is signed, not secret.
- HTTPS. The signature protects the firmware. The web password still travels in clear text, as today.
- Updates pulled by the device from a server, or pushed over SNMP or TFTP.
- Updating the boot program itself over the network. It changes only over USB.
- Downgrade protection (anti-rollback counters).

## Anti-goals (would make this a failure even if it technically works)

- Any sequence of upload, power cut, bad image or bad firmware that leaves a device that does not boot, or
  that boots but can never be reached again over the network.
- A device that accepts firmware not signed with the user's key.
- An update or rollback that resets the device's settings.
- Breaking USB flashing (`pio run -e pico -t upload`), the on-device tests (`pio test -e pico`) or the HIL
  tests.

## Constraints

- RP2040, 2 MB flash. The RP2040 cannot remap flash, so it cannot by itself start a firmware stored at
  another address (unlike the RP2350, which has A/B boot in its ROM). Today's firmware is about 370 KB.
  LittleFS is 256 KB at the end of flash and stays where it is, so settings are kept.
- The Arduino-Pico core already puts its own 12 KB "OTA" stage-3 bootloader at the start of every image
  (`framework-arduinopico/ota/`, linker script `lib/rp2040/memmap_default.ld`). It copies an image from a
  LittleFS file into flash. It is not usable as is: there is no rollback and no signature, and our 256 KB
  file system cannot hold the image. Ours replaces it.
- Flash writes pause both cores (the core idles core 1, as for settings saves). Each erase/write takes
  around 50 ms. **Decided by the user:** during an upload, slower mains detection (tens of ms instead of the
  10 ms firmware share) is acceptable. No RAM-resident core 1 code is needed. Outside uploads the 10 ms rule
  still holds.
- The web server handles at most `HTTP_REQUEST_MAX` = 2048 bytes per request today. The upload needs a
  streaming path for this one endpoint. The image goes to flash a block at a time, and no buffer larger
  than ~256 bytes goes on core 0's 8 KB stack (`.clinerules`). A 4 KB flash sector buffer must be static.
- All project rules apply: pure logic in `lib/` with native tests (update file parsing, signature check
  wrapper, boot state record, trial/rollback decision, upload state machine), on-device tests for flash
  writes and the boot path, HIL tests for the full update and rollback, tests with truncated and corrupted
  input, sanitizers. Every function documented. `CHANGELOG.md`, `TODO.md`, `docs/USER_MANUAL.md`,
  `docs/SETUP_MANUAL.md`, `docs/MIB.md` and `docs/TESTING.md` updated. Minor version bump.
- Crypto (*designer's call*): Monocypher (public domain, small) for Ed25519 verification and its SHA-512.
  It is vendored into the repo so builds work offline. We do not write our own Ed25519. The no-external-
  libraries rule covers only the SNMP protocol.
- The boot program verifies a slot on every boot. Ed25519 over ~370 KB on a Cortex-M0+ at 125 MHz should
  take well under a second (*to be measured*). A slower boot by that much is acceptable.

## How it gets made (left to the builder, with the designer's recommendation)

- **Starting a firmware from either slot.** Two ways, both fit the flash:
  1. *Recommended:* each release is built twice, linked for slot A's and slot B's address. The update file
     carries both images, and the device writes the one for its inactive slot. The boot program just checks
     a slot and jumps to it, and the slots never need copying, so power safety comes almost for free. The
     cost: two builds per release and an upload of about twice the size.
  2. A fixed run slot plus a staging slot, with the boot program copying the new image into the run slot
     and keeping a backup of the old one for rollback (MCUboot-style swap). One build, but the copy and swap
     must survive power cuts, and that is the hard part.

  The builder should spike option 1 first: link the Arduino-Pico firmware at a different address with the
  core's PlatformIO build, and start it from a minimal boot program. Fall back to option 2 only if that
  fails.
- **Flash layout** (*proposal*): boot2 + boot program + boot state at the start (≤ 64 KB), slot A, slot B,
  then the core's EEPROM sector and LittleFS where they are today. That leaves ~830 KB per slot, more than
  twice today's firmware.
- **USB images.** `pio run -e pico -t upload` and `pio test -e pico` must keep working. The likely shape is
  a combined image (boot program + slot A + a boot state that selects A) built by a PlatformIO extra script.
  The builder decides how the on-device test firmware is signed and booted.
- **Development key vs the user's key.** Development builds, CI and tests need a key. The user's field
  devices need the user's key. The builder proposes how the two are kept apart so that a device flashed for
  the field never accepts development-signed files.
- How the PC tools compute Ed25519: Python's standard library has no Ed25519. Either a Python package in
  PlatformIO's Python environment (must then be documented and work offline once installed), or a small
  host program built from the same vendored Monocypher, so the PC signs with the very code the device
  verifies with. The designer leans to the latter.
- The exact health condition for "reachable" (which requests count, from where) within the rule above.
- How the boot program reads the reset reason and the "planned" mark before the firmware's own startup
  code clears them, and how the firmware's hidden `stall0`/`stall1` watchdog test commands fit in (they
  should count as crashes, which also gives the HIL rollback test a way to make a firmware crash).
- The boot program's size, its LED pattern and whether it is a separate PlatformIO environment.
