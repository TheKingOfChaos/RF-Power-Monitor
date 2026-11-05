# Firmware update over the web page (A/B slots, signed, rollback) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Update the firmware of field devices over the network, from the web page or a script. Only files signed with the user's Ed25519 key are accepted, written to the inactive of two flash slots, started on trial and rolled back automatically when the new firmware crashes or cannot be reached.

**Architecture:** Our own boot program sits at the start of flash, where the core's 12 KB OTA loader is today. It reads a power-safe boot record, verifies a slot's signature, counts crash restarts in watchdog scratch registers and jumps to slot A or B. Each release is linked twice, once per slot address, and the `.upd` file carries both images. The device writes the one for its inactive slot. The boot program never writes flash; only the firmware writes the boot record. All decisions (file parsing, record, boot choice, trial verdict) live in pure libraries with native tests.

**Tech Stack:** Arduino-Pico 5.4.3 (pinned), PlatformIO, lwIP, Monocypher 4 (vendored, Ed25519 + SHA-512), Python 3 (PlatformIO's penv), MinGW/GNU g++ for the PC signing tool.

**Spec:** `docs/superpowers/specs/2026-11-05-firmware-update-ab-design.md`

## Global Constraints

- Flash 2 MB. LittleFS stays at `0x1BF000`–`0x1FF000` (256 KB) and the EEPROM sector at `0x1FF000`, so settings are kept.
- Flash layout (offsets from `0x10000000`):
  - boot2 `0x000000` (256 B)
  - boot program `0x000100`–`0x00E000`
  - boot record copies `0x00E000` and `0x00F000` (4 KB each)
  - slot A `0x010000`, slot B `0x0E7000`, each `0xD7000` (860 KB) long
  - `0x1BE000`–`0x1BF000` unused
  - In each slot, a 256-byte slot header comes first and the image is linked at slot base + `0x100`.
- Signature: Ed25519 (RFC 8032, SHA-512), `crypto_ed25519_check` from Monocypher's `monocypher-ed25519.c`. Never write our own crypto.
- Rules from `.clinerules` that bite here:
  - no buffer larger than ~256 bytes on the stack (sector buffers are static);
  - native code builds with `-Wall -Wextra -Werror`;
  - every parser of network or flash data gets tests with truncated and corrupted input and passes `scripts/sanitize.sh`;
  - every function is documented;
  - a test must be able to fail.
- Trial rules:
  - reachable = a valid authenticated web request or a valid SNMP request (right community) within **10 min** (`600000` ms), counted only while the link is up and the device has an IPv4 or IPv6 address;
  - hard limit **60 min** (`3600000` ms) on trial, whatever the link did;
  - **3** crash restarts in a row trigger the fallback, in trial and in kept state;
  - the crash count clears after **5 min** (`300000` ms) of uptime or when a trial is confirmed.
- What counts as a crash: only a watchdog timeout the firmware did not ask for. Power-on, brown-out and RUN-pin resets never count, nor do planned restarts (`rp2040.reboot()`, i.e. `watchdog_reboot`).
- Downgrades are allowed. Uploads are refused while the running firmware is on trial, so the known-good fallback is never overwritten during a trial.
- Private keys:
  - the field key is in `secret/fw_signing_key` (gitignored). Never print, log or commit it.
  - the development key is in `keys/dev/fw_signing_key`. It is committed and documented as public; it exists only for CI and tests.
  - A build uses the field public key only with `RFPOWER_FW_KEY=field`.
- Version becomes `3.3.0`. Changes go in `CHANGELOG.md`.

## Review Focus

1. **An upload that stops halfway**: a closed browser tab, a network drop or a script killed mid-upload. The running firmware and the boot record must be unchanged. The inactive slot must read as Empty, never as half-valid. The next upload must work. Tests: `test_receiver_truncated_body_fails` (Task 6), `test_aborted_upload_leaves_device_running` (Task 12).
2. **A file whose header lies**: offsets past the end, overlapping blobs, a blob longer than a slot, or a blob for the other slot. Nothing may be written outside the target slot. Tests: Task 6 `test_receiver_rejects_*`.
3. **An upload or manual switch while on trial, or a second upload in parallel**: answer 409 and write nothing. Tests: `test_upload_refused_while_on_trial` (Task 7), `test_second_upload_while_busy_is_refused` (Task 7), `test_upload_during_trial_is_refused` (Task 12).
4. **A switch to a slot that is empty or fails verification**: refused, not attempted. Tests: `test_switch_refused_when_other_slot_invalid` (Task 7), `test_decide_never_picks_invalid_slot` (Task 4).
5. **A power cut while the boot record is being written**: the other copy wins and the device boots. Tests: `test_newest_ignores_copy_with_bad_crc` and `test_newest_handles_sequence_wraparound` (Task 4).

---

### Task 1: Slot-linked firmware and a minimal boot program (go / no-go)

The spec's recommended approach depends on linking the Arduino-Pico firmware at a slot address and starting it from our own boot program. This task proves that on the device and keeps the build plumbing. **If it cannot be made to work, stop and report to the user.** The fallback is the copy-based scheme (spec, "How it gets made", option 2), and that needs a new plan.

**Files:**
- Create: `scripts/slot_build.py` (PlatformIO `pre:` script: linker script per slot, `FW_SLOT` define; `post:` part: full UF2 and upload)
- Create: `scripts/build_boot.py` (compiles `boot/` with the core's `arm-none-eabi-g++`; offline)
- Create: `scripts/uf2.py` (`def write_uf2(path: str, chunks: list[tuple[int, bytes]]) -> None`, family `0xe48bff56`)
- Create: `boot/boot.ld`, `boot/startup.S`, `boot/boot_main.cpp` (minimal: jump to slot A)
- Modify: `platformio.ini` (`[env:pico]` = slot A; new `[env:pico_b]` extends it with slot B)

**Interfaces:**
- Produces:
  - `-D FW_SLOT=0` (A) / `1` (B) in the firmware build.
  - `.pio/build/<env>/firmware.bin`: the image, linked at slot base + `0x100`.
  - `.pio/boot/boot.bin`: boot2 + the boot program, starting at flash offset 0.
  - `.pio/build/pico/firmware_full.uf2`: boot program + erased boot record sectors + slot A blob. `pio run -e pico -t upload` and `pio test -e pico` upload this file.
  - `scripts/slot_build.py` exports `slot_blob(env) -> bytes`. Task 3 replaces its unsigned placeholder header with a signed one.

- [ ] **Step 1:** Write `scripts/slot_build.py`. It generates `$BUILD_DIR/memmap_slot.ld` from the core's `lib/rp2040/memmap_default.ld`, with the same `__FLASH_LENGTH__`/`__FS_START__`/... substitutions as `framework-arduinopico/tools/platformio-build.py` lines 343-357, plus these changes:
  - `FLASH ORIGIN` = `0x10000000 + slot offset + 0x100` and `LENGTH` = `0xD7000 - 0x100`;
  - the `.boot2`, `.ota` and `.partition` output sections are removed;
  - the image starts with `.text` (the vector table).

  It sets `board_build.ldscript` to that file before the core's builder runs, and adds `-D FW_SLOT=<n>` from the custom option `custom_fw_slot`. Check that `ota.o`, still linked by the core, ends up in no output section (`arm-none-eabi-objdump -h`). If it does, discard `.OTA` in the linker script.
- [ ] **Step 2:** Write `scripts/build_boot.py`. It assembles the board's boot2 (`build.arduino.earlephilhower.boot2_source` from the board JSON, default `boot2_generic_03h_2_padded_checksum.S`, under `framework-arduinopico/boot2/rp2040/`). It compiles `boot/*.cpp` and `startup.S` with `-mcpu=cortex-m0plus -mthumb -Os -ffreestanding -fno-exceptions -fno-rtti -nostdlib`, links them with `boot/boot.ld` (boot2 at `0x10000000`, vectors at `0x10000100`, code ≤ `0xE000`) and writes `.pio/boot/boot.bin`. It rebuilds only when a source is newer than the output, and fails the build if the code passes `0xE000`.
- [ ] **Step 3:** Minimal `boot/boot_main.cpp`: set `VTOR` to `0x10010100`, load `MSP` from word 0, branch to word 1. No clock setup yet.
- [ ] **Step 4:** In `slot_build.py`'s post action, build `firmware_full.uf2` with `uf2.py`:
  - `boot.bin` at `0x10000000`;
  - `0xFF` × 8 KB at `0x1000E000`;
  - a 256-byte placeholder header (all `0xFF`) plus `firmware.bin` at `0x10010000`.

  Make `upload` (picotool / mass storage, whichever the env uses) send this UF2 instead of the ELF-derived one.
- [ ] **Step 5:** Build both slots: `pio run -e pico` and `pio run -e pico_b`. Expected: both link, and `arm-none-eabi-nm firmware.elf | grep __logical_binary_start` shows `10010100` for A and `100e7100` for B.
- [ ] **Step 6:** `pio run -e pico -t upload` on COM4, then run the existing HIL serial menu test: `python.exe -m unittest discover -s test/hil -p "test_serial_menu.py" -v`. Expected: PASS. The firmware runs from slot A behind our boot program, with USB, settings (LittleFS at its old place), core 1 and the network all working.
- [ ] **Step 7:** `pio test -e pico` (all embedded suites). Expected: PASS. Reflash the firmware afterwards.
- [ ] **Step 8:** Go/no-go. If steps 5-7 pass, commit. If not, stop and report what failed.

```bash
git add scripts/slot_build.py scripts/build_boot.py scripts/uf2.py boot/ platformio.ini
git commit -m "Build: firmware linked per slot behind our own boot program"
```

### Task 2: Monocypher and `lib/fw_image` (layout, slot header, verification)

**Files:**
- Create: `lib/monocypher/` (Monocypher 4.0.2 `monocypher.c/.h`, `optional/monocypher-ed25519.c/.h`, its LICENCE, `library.json`; the version and source URL go in a README line). Pin the release tarball's SHA-256 in that README.
- Create: `lib/fw_image/include/fw_layout.h`, `lib/fw_image/include/fw_image.h`, `lib/fw_image/src/fw_image.cpp`
- Test: `test/native/test_fw_image/test_main.cpp`

**Interfaces:**
- Produces (`fw_layout.h`):
  - `FW_FLASH_XIP_BASE = 0x10000000u`
  - `FW_BOOT_RECORD_OFFSET[2] = {0xE000u, 0xF000u}`
  - `FW_SLOT_OFFSET[2] = {0x10000u, 0xE7000u}`
  - `FW_SLOT_SIZE = 0xD7000u`
  - `FW_SLOT_HEADER_SIZE = 0x100u`
  - `FW_IMAGE_MAX = FW_SLOT_SIZE - FW_SLOT_HEADER_SIZE`
  - `FW_SECTOR_SIZE = 4096u`
- Produces (`fw_image.h`):
  - Slot header byte layout, little endian:
    - `[0,64)` signature
    - `[64,68)` magic `0x57464652` ("RFFW")
    - `[68,72)` header version `1`
    - `[72,76)` slot (0/1)
    - `[76,80)` image length
    - `[80,96)` version, NUL-padded ASCII
    - `[96,104)` key id
    - rest `0xFF`
  - The signed message is bytes `[64, 256 + imageLength)` of the slot. It is contiguous in flash, so it is verified straight from XIP.
  - `struct SlotHeader { uint32_t slot; uint32_t imageLength; char version[17]; uint8_t keyId[8]; };`
  - `enum class SlotCheck : uint8_t { Ok, Empty, BadMagic, WrongSlot, TooLong, BadVersion, WrongKey, BadSignature };`
  - `SlotCheck slotHeaderParse(const uint8_t* bytes, size_t len, uint32_t expectedSlot, const uint8_t keyId[8], SlotHeader& out);` It checks everything except the signature. `len < 256` returns `Empty` if all bytes are `0xFF`, else `BadMagic`.
  - `SlotCheck slotVerify(const uint8_t* slot, uint32_t expectedSlot, const uint8_t publicKey[32], SlotHeader& out);` Here `slot` points at `FW_SLOT_SIZE` readable bytes.
  - `void fwKeyId(const uint8_t publicKey[32], uint8_t out[8]);` (first 8 bytes of SHA-512 of the key)
  - `void fwKeyFingerprint(const uint8_t publicKey[32], char out[17]);` (those 8 bytes as lowercase hex)
  - `const char* slotCheckName(SlotCheck c);` (e.g. `"bad signature"`)

- [ ] **Step 1: Write the failing tests** in `test/native/test_fw_image/test_main.cpp`. Build signed slots in a test helper with Monocypher's `crypto_ed25519_sign`, from a fixed 32-byte seed.
  - `test_ed25519_matches_rfc8032_test1`: RFC 8032 §7.1 TEST 1 (empty message). Secret key `9d61b19d…7f60`, expected public key `d75a9801…511a`, expected signature `e5564300…100b`. Copy the hex verbatim from the RFC. Assert that `crypto_ed25519_key_pair` gives the public key, that signing gives the signature, and that `crypto_ed25519_check` returns 0.
  - `test_verify_accepts_signed_slot`: `slotVerify` gives `Ok`; `out.version` = `"3.3.0"`; `out.imageLength` = `1000`.
  - `test_verify_rejects_flipped_image_byte`: flip byte `256 + 500` → `BadSignature`.
  - `test_verify_rejects_flipped_header_byte`: flip a byte of the version → `BadSignature`.
  - `test_verify_rejects_other_key`: sign with seed 2 → `WrongKey` (key id mismatch).
  - `test_verify_rejects_wrong_slot`: header slot 1, expected 0 → `WrongSlot`.
  - `test_verify_rejects_length_over_slot`: imageLength = `FW_IMAGE_MAX + 1` → `TooLong`.
  - `test_verify_accepts_length_exactly_max`: `FW_IMAGE_MAX` → `Ok`.
  - `test_verify_reports_erased_slot_as_empty`: all `0xFF` → `Empty`.
  - `test_parse_rejects_bad_magic`.
  - `test_parse_rejects_unterminated_or_nonprintable_version` → `BadVersion`.
  - `test_parse_rejects_truncated_header`: `len` 0, 63, 255 → not `Ok`.
  - `test_fingerprint_is_16_lowercase_hex`.
- [ ] **Step 2:** `pio test -e native -f native/test_fw_image`. Expected: FAIL, missing `fw_image.h`.
- [ ] **Step 3:** Vendor Monocypher and implement `fw_image.cpp`. Leave `native` env's `-Werror` untouched. If Monocypher warns, fix the build flags in `library.json` only for that library's language standard (C99), never `-Wno-*`.
- [ ] **Step 4:** `pio test -e native -f native/test_fw_image`. Expected: PASS. Then `pio test -e native` and `wsl -d Ubuntu -- bash scripts/sanitize.sh`. Expected: PASS.
- [ ] **Step 5:** Commit `lib/monocypher lib/fw_image test/native/test_fw_image` with "fw_image: slot header and Ed25519 verification (Monocypher)".

### Task 3: Keys and signing (`fwsign`, `fw_keygen.py`, release files)

**Files:**
- Create: `tools/fwsign/fwsign.cpp` (host program; links `lib/monocypher` and `lib/fw_image`)
- Create: `tools/fwtool.py` (`def fwsign_path() -> str`, which builds `.pio/fwsign/fwsign(.exe)` with `g++ -O2 -std=gnu++17 -Wall -Wextra -Werror` when missing or older than its sources; `def sign_slot(seed_path, slot, version, image_path) -> bytes`; `def public_key(seed_path) -> bytes`)
- Create: `tools/fw_keygen.py`, `tools/fw_release.py`
- Create: `keys/dev/fw_signing_key` (32 random bytes, committed), `keys/dev/README.md` ("public development key, CI and tests only")
- Create: `include/fw_dev_public_key.h`, `include/fw_key.h`
- Modify: `scripts/slot_build.py` (signed slot blob in `firmware_full.uf2`)
- Test: `test/tools/test_fw_tools.py` (Python `unittest`, no hardware)

**Interfaces:**
- Consumes: `fw_image.h`, `scripts/uf2.py`, `slot_blob(env)`.
- Produces:
  - `fwsign pubkey <seedfile>` prints 64 hex characters.
  - `fwsign sign <seedfile> <slot> <version> <image.bin> <out.blob>` writes header + image.
  - `fwsign verify <pubkeyhex> <slot> <blob>` exits 0 for `Ok`; otherwise it prints `slotCheckName` and exits 1.
  - `include/fw_key.h` declares `extern const uint8_t FW_PUBLIC_KEY[32];` and defines it as `static const` from `fw_public_key.h` (when `RFPOWER_FW_FIELD_KEY` is defined) or from `fw_dev_public_key.h`. If the field header is missing, `#error "Run tools/fw_keygen.py"`.
  - `.upd` layout, little endian, 64-byte header:
    - magic `0x50554652` ("RFUP");
    - format `1`;
    - version[16];
    - `slotOffset[2]`, `slotLength[2]` (u32);
    - rest `0xFF`;
    - then the slot A blob and the slot B blob.
  - `tools/fw_release.py [--field] [--test-variant crash|noconfirm|shorttrial]`:
    - builds `pico` and `pico_b` (with `RFPOWER_FW_KEY=field` for `--field`);
    - signs both slots;
    - writes `release/rf-power-monitor-<version>.upd` and `release/rf-power-monitor-<version>-usb.uf2` (`release/` is gitignored);
    - test variants go to `.pio/fwtest/<variant>.upd` and use the development key only.

- [ ] **Step 1: Write the failing tests** in `test/tools/test_fw_tools.py`. Use a temp dir; never touch `secret/`. Run `fw_keygen.py` with its `--out` option pointing into the temp dir.
  - `test_keygen_writes_32_byte_key_and_public_header`
  - `test_keygen_refuses_to_overwrite_existing_key`: second run exits non-zero, file bytes unchanged.
  - `test_keygen_force_needs_typed_confirmation`: `--force` with stdin `no` → unchanged; with stdin `REPLACE` → changed.
  - `test_keygen_never_prints_private_key`: the hex of the key file's bytes is not in stdout/stderr.
  - `test_keygen_prints_fingerprint`: stdout contains `fwKeyFingerprint` of the key (16 hex).
  - `test_sign_then_verify_ok` and `test_verify_fails_with_other_key`. Both use `fwsign verify`; the device code is in the loop through `fw_image`.
  - `test_upd_layout_offsets_and_lengths`: build an `.upd` from two dummy images; parse the header; the blobs are at the stated offsets and are `256 + len` long.
- [ ] **Step 2:** `python.exe -m unittest discover -s test/tools -v`. Expected: FAIL.
- [ ] **Step 3:** Implement `fwsign.cpp`, `fwtool.py`, `fw_keygen.py` and `fw_release.py`. `fw_keygen.py` takes its seed from `secrets.token_bytes(32)`, creates the key file with `os.open(..., O_CREAT | O_EXCL)` and prints the fingerprint and a backup reminder. It derives the public key through `fwsign pubkey` and writes `include/fw_public_key.h` (for `--out`, into the temp dir). Then generate `keys/dev/fw_signing_key` and `include/fw_dev_public_key.h` with `fw_keygen.py --dev`.
- [ ] **Step 4:** Make `slot_build.py` sign the slot A blob with the development key (or with `secret/fw_signing_key` under `RFPOWER_FW_KEY=field`, failing with "Run tools/fw_keygen.py" if it is missing). Add `release/` to `.gitignore`.
- [ ] **Step 5:** Run the tests again. Expected: PASS. Then `pio run -e pico`: `firmware_full.uf2` contains a slot header that `fwsign verify` accepts.
- [ ] **Step 6:** Add a CI step in `.github/workflows/tests.yml` that runs `test/tools`. Commit with "Firmware signing: key generation, fwsign, release files".

### Task 4: Boot record and boot decision (`lib/fw_update`, pure)

**Files:**
- Create: `lib/fw_update/include/boot_record.h`, `boot_decision.h`; `src/boot_record.cpp`, `src/boot_decision.cpp`
- Test: `test/native/test_fw_boot/test_main.cpp`

**Interfaces:**
- Produces (`boot_record.h`):
  - `enum class FwResult : uint8_t { None = 0, Installed = 1, RolledBackCrash = 2, RolledBackUnreachable = 3, FallbackCrash = 4 };`
  - `static const uint8_t FW_NO_SLOT = 0xFF;`
  - `struct BootRecord { uint32_t sequence; uint8_t active; uint8_t trial; FwResult lastResult; bool trapPending; char lastVersion[17]; };` (`lastVersion` is the version the result is about)
  - `static const size_t BOOT_RECORD_SIZE = 64;`
  - `void bootRecordEncode(const BootRecord& r, uint8_t out[BOOT_RECORD_SIZE]);`. It uses magic `0x52464252` ("RBFR") and a CRC-32 of the first 60 bytes. Reuse the CRC-32 in `lib/settings_codec` if there is one, else add `uint32_t fwCrc32(const uint8_t*, size_t)` here.
  - `bool bootRecordDecode(const uint8_t* in, size_t len, BootRecord& out);` It rejects a bad magic, a bad CRC, a slot > 1 other than `FW_NO_SLOT`, `active == FW_NO_SLOT`, `trial == active`, a result out of range, an unterminated version, and `len < 64`.
  - `int bootRecordNewest(bool ok0, const BootRecord& r0, bool ok1, const BootRecord& r1);` returns 0, 1 or −1 (neither valid). The newer copy is the one with `(int32_t)(a.sequence - b.sequence) > 0`.
- Produces (`boot_decision.h`):
  - `enum class ResetKind : uint8_t { PowerOn, ResetPin, Planned, Crash };`
  - `ResetKind classifyReset(uint32_t watchdogReason, uint32_t scratch4, uint32_t chipReset);` The rules:
    - watchdog reason ≠ 0 and `scratch4 == 0x6ab73121` (SDK `WATCHDOG_NON_REBOOT_MAGIC`) → `Crash`;
    - watchdog reason ≠ 0 otherwise → `Planned`;
    - `chipReset & HAD_RUN` (bit 16) → `ResetPin`;
    - else `PowerOn`.
  - `enum class BootReason : uint8_t { Normal, Trial, Fallback, NoRecord };`
  - `struct BootDecision { uint8_t slot; BootReason reason; uint32_t crashScratch; };` (`slot == FW_NO_SLOT`: nothing bootable)
  - `BootDecision decideBoot(bool haveRecord, const BootRecord& rec, const bool slotOk[2], ResetKind reset, uint32_t crashScratch);`
  - `crashScratch` encoding: `0xC7A50000 | slot << 8 | count`. Any other value means count 0.
  - `uint32_t bootInfoEncode(uint8_t slot, BootReason reason, ResetKind reset, uint8_t failedSlot);` (magic `0xB1` in bits 24-31) and `bool bootInfoDecode(uint32_t v, uint8_t& slot, BootReason& reason, ResetKind& reset, uint8_t& failedSlot);`
  - `static const uint8_t FW_CRASH_LIMIT = 3;`

  `decideBoot` rules:
  - The target is `rec.trial` if it is not `FW_NO_SLOT`, else `rec.active`. Without a record, the target is slot 0 (reason `NoRecord`).
  - If the target is not `slotOk`, boot the other slot when it is OK (reason `Fallback`), else `FW_NO_SLOT`.
  - Count: on `Crash` with the counter's slot == target, add 1. Otherwise a `Crash` sets it to 1 for the target, and any other reset kind sets it to 0.
  - When the count reaches `FW_CRASH_LIMIT` and the other slot is OK, boot the other slot (`Fallback`) with its count at 0. If the other slot is not OK, stay on the target.

- [ ] **Step 1: Write the failing tests:**
  - `test_record_roundtrip`
  - `test_decode_rejects_bad_crc`
  - `test_decode_rejects_truncated` (0, 63)
  - `test_decode_rejects_trial_equal_active`
  - `test_decode_rejects_erased_sector` (all `0xFF`)
  - `test_newest_picks_higher_sequence`
  - `test_newest_ignores_copy_with_bad_crc`
  - `test_newest_handles_sequence_wraparound` (`0xFFFFFFFF` vs `0` → 0 is newer)
  - `test_newest_none_valid_returns_minus_one`
  - `test_classify_watchdog_timeout_is_crash`
  - `test_classify_reboot_is_planned` (`scratch4 = 0xb007c0d3` and `0`)
  - `test_classify_power_on`
  - `test_classify_run_pin`
  - `test_decide_boots_active_when_no_trial`
  - `test_decide_boots_trial_slot`
  - `test_decide_without_record_boots_slot_a`
  - `test_decide_without_record_and_bad_a_boots_b`
  - `test_decide_third_crash_falls_back_on_trial`
  - `test_decide_third_crash_falls_back_when_kept`
  - `test_decide_two_crashes_do_not_fall_back`
  - `test_decide_power_on_resets_count` (crash, crash, power-on, crash → no fallback)
  - `test_decide_planned_reboot_resets_count`
  - `test_decide_never_picks_invalid_slot`
  - `test_decide_nothing_bootable_returns_no_slot`
  - `test_decide_count_for_other_slot_restarts_at_one`
  - `test_boot_info_roundtrip`
  - `test_boot_info_rejects_garbage`
- [ ] **Step 2:** `pio test -e native -f native/test_fw_boot`. Expected: FAIL.
- [ ] **Step 3:** Implement. The code must build freestanding (no heap, no `<string>`, no `printf`), because the boot program compiles it.
- [ ] **Step 4:** Run `pio test -e native` and the sanitizers. Expected: PASS. Commit with "fw_update: boot record and boot decision".

### Task 5: The real boot program

**Files:**
- Modify: `boot/boot_main.cpp`. Create `boot/boot_clocks.cpp` (port of `framework-arduinopico/ota/ota_clocks.c`: XOSC + PLL_SYS to 125 MHz with register structs from `pico-sdk/src/rp2040/hardware_structs`) and `boot/boot_led.cpp`.
- Modify: `scripts/build_boot.py`, which also compiles `lib/fw_image/src`, `lib/fw_update/src/boot_record.cpp`, `boot_decision.cpp`, `lib/monocypher` and `include/fw_key.h`, so the boot program and the firmware embed the same key.

**Interfaces:**
- Consumes: `slotVerify`, `bootRecordDecode`, `bootRecordNewest`, `classifyReset`, `decideBoot`, `bootInfoEncode`, `FW_PUBLIC_KEY`.
- Produces, for the firmware (Task 8). Watchdog scratch registers, all set by the boot program just before the jump:
  - `scratch0` = `bootInfoEncode(...)`;
  - `scratch1` = crash counter;
  - `scratch2` = microseconds the verification took;
  - the SDK owns scratch4-7. First grep the core and `libpico` for other users of scratch0-3; if any, pick free ones and note them in `fw_layout.h`.

  Watchdog: before the jump, the boot program enables it at the RP2040 maximum (`0xFFFFFF` ticks ≈ 8.3 s), with `scratch4 = 0x6ab73121`. A firmware that hangs in `setup()` therefore restarts as a crash.

- [ ] **Step 1:** Implement the boot sequence:
  1. read the reset reason (`watchdog_hw->reason`, `scratch4`, `vreg_and_chip_reset_hw->chip_reset`) before touching anything;
  2. clocks to 125 MHz;
  3. read both boot record copies from XIP and pick the newest;
  4. `slotVerify` the slots it may need (target first, the other one only if needed);
  5. `decideBoot`;
  6. write the scratch registers and arm the watchdog;
  7. jump (`VTOR`, `MSP`, reset vector).

  When nothing is bootable, the LED (`include/pins.h`) blinks 3 short / 1 long forever and the watchdog stays off.
- [ ] **Step 2:** Build and flash `firmware_full.uf2`. Add to `test/hil/test_fw_update.py` (created here, more cases in Task 12):
  - `test_boot_program_starts_slot_a_after_usb_flash`: the status screen (Task 10 adds the line; until then the boot banner printed by Task 8's `fwUpdateBegin`) shows slot A.
  - `test_boot_verification_takes_under_1500_ms`: it reads `Boot: slot A verified in <n> ms` and asserts `n < 1500`.

  Mark both `@unittest.skip` until Task 8 lands, then unskip them there.
- [ ] **Step 3:** Run the full HIL suite and `pio test -e pico`. Expected: PASS. A slower boot of up to 1.5 s must not break `test_serial_menu` timing.
- [ ] **Step 4:** Commit with "Boot program: verify slots, pick by boot record and crash count".

### Task 6: Update file receiver (pure, streaming)

**Files:**
- Create: `lib/fw_update/include/update_receiver.h`, `lib/fw_update/src/update_receiver.cpp`
- Test: `test/native/test_fw_receiver/test_main.cpp`

**Interfaces:**
- Consumes: `fw_layout.h`, `slotHeaderParse`.
- Produces:
  - `class SlotWriter { public: virtual ~SlotWriter() {} virtual bool eraseSector(uint32_t offsetInSlot) = 0; virtual bool writeSector(uint32_t offsetInSlot, const uint8_t* data) = 0; };` (`data` is `FW_SECTOR_SIZE` bytes)
  - `enum class UpdateError : uint8_t { None, BadMagic, BadFormat, SlotMissing, TooLarge, WrongSlot, BadHeader, Truncated, TooLong, WriteFailed };`
  - `enum class UpdateStatus : uint8_t { InProgress, Done, Failed };`
  - `class UpdateReceiver { public: void begin(uint8_t targetSlot, uint32_t contentLength, const uint8_t keyId[8], SlotWriter& w); UpdateStatus feed(const uint8_t* data, size_t len); UpdateStatus finish(); UpdateError error() const; const char* version() const; private: uint8_t _sector[FW_SECTOR_SIZE]; uint8_t _first[FW_SECTOR_SIZE]; ... };`
  - `const char* updateErrorName(UpdateError e);`

  Order of writes, which makes an aborted upload leave the slot Empty:
  - `begin` does no flash I/O.
  - The first `feed` that completes the 64-byte file header and the target blob's 256-byte slot header validates them and calls `eraseSector(0)`.
  - Sectors 1..n are erased and written as they fill; the last one is padded with `0xFF`.
  - Sector 0 is kept in `_first` and written last, in `finish()`, only when exactly `contentLength` bytes have arrived.

  Bytes outside the target blob are skipped. The file header is valid only when:
  - both blobs lie within `contentLength`;
  - they don't overlap and don't overlap the 64-byte header;
  - each length is ≥ 256 and ≤ `FW_SLOT_SIZE`;
  - the target blob's slot header passes `slotHeaderParse(…, targetSlot, keyId, …)` (so a wrong key fails before anything is erased) and its `256 + imageLength` equals the blob length.

- [ ] **Step 1: Write the failing tests.** Use a fake `SlotWriter` that records erases and writes into an 860 KB vector, and that can be told to fail on sector N. Feed the data in chunks of 1, 7 and 4096 bytes.
  - `test_receiver_writes_target_blob_only`: slot B blob lands at offset 0..; slot A bytes are never written.
  - `test_receiver_writes_first_sector_last`
  - `test_receiver_rejects_bad_magic`
  - `test_receiver_rejects_unknown_format`
  - `test_receiver_rejects_blob_past_content_length`
  - `test_receiver_rejects_overlapping_blobs`
  - `test_receiver_rejects_blob_over_slot_size`
  - `test_receiver_rejects_blob_for_wrong_slot`
  - `test_receiver_rejects_other_key_before_erasing`: no erase recorded.
  - `test_receiver_rejects_image_length_mismatch`
  - `test_receiver_truncated_body_fails`: `finish()` after fewer bytes → `Truncated`, and sector 0 was erased but never written, so the slot reads as Empty.
  - `test_receiver_extra_bytes_fail`
  - `test_receiver_write_failure_stops`
  - `test_receiver_header_split_across_feeds`
  - `test_receiver_empty_body_fails`
  - `test_receiver_exact_slot_size_blob_ok`
- [ ] **Step 2:** Run. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `pio test -e native` and the sanitizers. Expected: PASS. Commit with "fw_update: streaming update file receiver".

### Task 7: Trial monitor, HTTP streaming and the Firmware page (pure)

**Files:**
- Create: `lib/fw_update/include/trial_monitor.h`, `src/trial_monitor.cpp`
- Modify: `lib/web_ui/include/http_request.h`, `src/http_request.cpp`, `lib/web_ui/include/web_app.h`, `src/web_app.cpp`
- Test: `test/native/test_fw_trial/test_main.cpp`, additions to `test/native/test_web_ui/`

**Interfaces:**
- Produces (`trial_monitor.h`):
  - `#ifndef FW_TRIAL_REACH_MS` → `600000u`; `#ifndef FW_TRIAL_LIMIT_MS` → `3600000u`; `#ifndef FW_HEALTHY_MS` → `300000u`. These can be overridden for the `shorttrial` HIL build variant.
  - `enum class TrialVerdict : uint8_t { Pending, Confirmed, Unreachable };`
  - `class TrialMonitor { public: void begin(uint32_t nowMs); void noteValidRequest(); TrialVerdict poll(uint32_t nowMs, bool netReady); };`

    It counts the time with `netReady` between polls (wrap-safe), returns `Unreachable` when that reaches `FW_TRIAL_REACH_MS` or when the time since `begin` reaches `FW_TRIAL_LIMIT_MS`, and returns `Confirmed` on the first poll after a valid request.
- Produces (`http_request.h`):
  - new `HttpParse::Streaming`, `void HttpRequest::streamPath(const char* path);` (the path whose POST body is not buffered), `size_t HttpRequest::contentLength() const;` and `const uint8_t* HttpRequest::bodyPrefix(size_t& len) const;` (body bytes that arrived with the head)
  - No `Content-Length`, or one over `0xD7000 * 2 + 64`, on the stream path → `TooLarge`/`Bad` as today.
- Produces (`web_app.h`):
  - `struct WebFirmware { uint8_t runningSlot; char runningVersion[17]; bool trial; char otherVersion[17]; char otherProblem[24]; char lastResult[64]; char keyFingerprint[17]; bool uploading; };` Here `otherVersion` is empty when the other slot is not OK, and `otherProblem` holds its `slotCheckName`.
  - `WebBackend` gains `virtual void firmware(WebFirmware& out) = 0;` and `virtual bool switchSlot() = 0;` (false when the other slot is not OK or the device is on trial; on success a reboot follows).
  - `int WebApp::authorizeUpload(const HttpRequest& req, const SourceKey& source, uint32_t nowMs, WebBackend& backend);` It returns 0 when the session cookie is valid and the `X-CSRF-Token` header matches, else the HTTP status code (401/403/409). 409 covers trial or `uploading`.
  - `const char* WebApp::uploadResult(int code, const char* text, size_t& len);` gives a `text/plain` body. On success the body is `ok <version>`, e.g. `ok 3.3.0`; on failure it is the error name.
  - Routes:
    - `GET /firmware`: the page. Layout:
      - a status table;
      - a file input and an Upload button; a few lines of inline JS do `fetch('/firmware/upload', {method:'POST', body:file, headers:{'X-CSRF-Token':csrf}})` with progress text, and show the response text;
      - a form `POST /firmware/switch` with csrf and a `confirm` checkbox.
    - `POST /firmware/switch`: the switch.
    - `GET /firmware/status`: `text/plain`, logged in only. One `key=value` per line: `running_slot=A`, `running_version=`, `state=trial|kept`, `slot_a=`, `slot_b=`, `last_result=`, `key=`. `fw_update.py` reads this.
  - A link to the Firmware page in the existing page's navigation.

- [ ] **Step 1: Write the failing tests:**
  - TrialMonitor:
    - `test_trial_confirms_after_valid_request`
    - `test_trial_unreachable_after_10_min_with_network`
    - `test_trial_time_without_network_does_not_count`
    - `test_trial_hard_limit_60_min_without_network`
    - `test_trial_survives_millis_wraparound`
    - `test_trial_request_before_network_still_confirms`
  - Web:
    - `test_stream_path_returns_streaming_with_prefix`
    - `test_stream_path_without_content_length_is_bad`
    - `test_stream_path_over_max_is_too_large`
    - `test_other_paths_still_buffered`
    - `test_upload_needs_session` (401)
    - `test_upload_needs_csrf_header` (403)
    - `test_upload_refused_while_on_trial` (409)
    - `test_second_upload_while_busy_is_refused` (409)
    - `test_firmware_page_shows_slots_and_fingerprint`
    - `test_firmware_page_escapes_versions`
    - `test_switch_needs_confirm_and_csrf`
    - `test_switch_refused_when_other_slot_invalid` (backend returns false → notice, no reboot requested)
    - `test_status_text_lists_all_keys`
    - `test_status_text_needs_login`
    - `test_longest_values_still_fit_the_page` (extend the existing one to the Firmware page)
- [ ] **Step 2:** Run. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `pio test -e native` and the sanitizers. Expected: PASS. Commit with "web_ui: Firmware page, streaming upload request; fw_update: trial monitor".

### Task 8: Firmware side: update service, boot record writes, health, planned restarts

**Files:**
- Create: `src/fw_update_service.h`, `src/fw_update_service.cpp`
- Modify: `src/web_service.cpp` (stream path, receiver, authenticated request → health), `src/snmp_service.cpp` (valid request → health), `src/watchdog.cpp` (`readCause()` from boot info), `src/main.cpp` (begin/poll), `src/cli.cpp` and `src/factory_reset.cpp` (unchanged calls; `rp2040.reboot()` already reads as planned)
- Test: `test/embedded/test_fw_flash/test_main.cpp`

**Interfaces:**
- Consumes:
  - `UpdateReceiver` and `SlotWriter` (Task 6);
  - `TrialMonitor` (Task 7);
  - the boot record and boot info (Task 4);
  - `slotVerify` and `FW_PUBLIC_KEY`;
  - the scratch registers (Task 5).
- Produces (used by web, SNMP, CLI, Task 9-10):
  - `void fwUpdateBegin();` (in `setup()` before `watchdogBegin()`) and `void fwUpdatePoll(bool netReady);` (in `loop()`)
  - `void fwUpdateNoteValidRequest();`
  - `bool fwUpdateUploadBegin(uint32_t contentLength, const char*& error);`, `bool fwUpdateUploadFeed(const uint8_t* data, size_t len);`, `bool fwUpdateUploadFinish(char* versionOut, const char*& error);` (verifies from flash; on success writes the record `trial = inactive` and requests a planned reboot) and `void fwUpdateUploadAbort();`
  - `bool fwUpdateSwitchSlot();`
  - `void fwUpdateStatus(WebFirmware& out);`
  - `struct FwSnmpState { uint8_t runningSlot; bool trial; char slotVersion[2][17]; FwResult lastResult; };` and `void fwUpdateSnmpState(FwSnmpState& out);`
  - `bool fwUpdateTakeTrap(FwResult& result);` (one pending trap: kept or rolled back)
  - `RestartCause fwUpdateRestartCause();` (from boot info; `Planned` → `Reboot`, `Crash` → `Watchdog`)

  Flash writes use `rp2040.idleOtherCore()` + `noInterrupts()` + `flash_range_erase/program` + `interrupts()` + `rp2040.resumeOtherCore()`, the way LittleFS does. The watchdog is fed after each sector. The record is written to the copy that is *not* the newest valid one, with `sequence + 1`.

  Boot behaviour in `fwUpdateBegin`:
  - It prints `Boot: slot A verified in <n> ms (normal|trial|fallback)`.
  - `Fallback`: write `{active = running, trial = NO, lastResult = (failed slot was the record's trial ? RolledBackCrash : FallbackCrash), lastVersion = failed slot's version, trapPending = true}`.
  - `Trial`: start the `TrialMonitor`. `Confirmed` → write `{active = running, trial = NO, Installed, trapPending}` and clear the crash scratch. `Unreachable` → write `{active = other, trial = NO, RolledBackUnreachable, trapPending}` and `rp2040.reboot()`.
  - After `FW_HEALTHY_MS` of uptime, clear the crash scratch.
  - A file that is not accepted is reported only in RAM (`lastResult` text on the page and in the HTTP response). The inactive slot is then Empty; the page says so.

  Test-variant builds (Task 3's `--test-variant`):
  - `RFPOWER_FW_TEST_CRASH` hangs in `setup()` after `fwUpdateBegin`;
  - `RFPOWER_FW_TEST_NOCONFIRM` never calls `noteValidRequest`;
  - `shorttrial` sets `FW_TRIAL_REACH_MS=60000 FW_TRIAL_LIMIT_MS=180000`.

  `web_service.cpp` changes:
  - `request.streamPath("/firmware/upload")`.
  - On `Streaming`, it calls `app.authorizeUpload`. If that returns non-zero, it answers that code and closes.
  - Otherwise it enters a new `Conn::Uploading` state: feed `bodyPrefix`, then feed from `client.read()` into a static 512-byte buffer (not the stack).
  - Its own idle timeout is 30 s without data; on timeout it calls `fwUpdateUploadAbort()`.
  - When done, it answers `app.uploadResult` and requests the reboot.
  - A valid session on any request calls `fwUpdateNoteValidRequest()`.

  `snmp_service.cpp`: `Responded`, `RespondedBulk` and `NoRoom` call `fwUpdateNoteValidRequest()`.

- [ ] **Step 1: Write the failing embedded tests** in `test/embedded/test_fw_flash`. They write only the slot that is not running and restore it to erased at the end.
  - `test_slot_writer_erases_and_writes_inactive_sector` (read back through XIP)
  - `test_slot_writer_refuses_running_slot`
  - `test_record_write_alternates_copies_and_increments_sequence`. Save and restore both record sectors around the test.
  - `test_upload_path_stack_headroom`: feed a dev-signed blob through `fwUpdateUploadFeed` and measure core 0's high-water mark with `src/stack_watch.cpp`. Assert at least as much headroom as the existing stack tests require.
- [ ] **Step 2:** `pio test -e pico -f embedded/test_fw_flash`. Expected: FAIL.
- [ ] **Step 3:** Implement the service and wire it in. Unskip the two HIL tests from Task 5.
- [ ] **Step 4:** Run `pio test -e pico` (all embedded), reflash `pio run -e pico -t upload`, then run the HIL `test_fw_update.py`, `test_web.py`, `test_snmp.py` and `test_watchdog.py`. Expected: PASS. If `test_watchdog.py` causes 3 stalls within 5 min, it now falls back to the other slot. Make it wait for `FW_HEALTHY_MS` between stalls, or stall at most twice per boot cycle, and say which in the commit.
- [ ] **Step 5:** Commit with "Firmware update service: upload to the inactive slot, trial, rollback".

### Task 9: SNMP objects, traps, MIB and LibreNMS

Follow the pattern of `d1b7e2f` (snmp_agent object + trap) and `b224ed1`/`43816c0` (firmware wiring, LibreNMS).

**Files:** `lib/snmp_agent/*` (state struct, MIB table, trap builder), `src/snmp_service.cpp`, `mibs/RF-POWER-MONITOR-MIB.txt`, `docs/MIB.md`, `librenms/os_discovery/rfupsmonitor.yaml`, `librenms/test/snmptrap-agent-addr.php`, `test/native/test_snmp_agent/`, `test/native/test_snmp_trap/`, `test/hil/test_snmp.py`

**Interfaces:**
- Consumes: `fwUpdateSnmpState`, `fwUpdateTakeTrap`.
- Produces the OIDs:
  - group `1.3.6.1.4.1.63050.7` (`fwUpdate`), walked after `mainsPowerAlarm`:
    - `.7.1.0` `fwRunningSlot` INTEGER `slotA(1) slotB(2)`
    - `.7.2.0` `fwSlotAVersion` DisplayString (empty = no valid image)
    - `.7.3.0` `fwSlotBVersion` DisplayString
    - `.7.4.0` `fwState` INTEGER `kept(1) trial(2)`
    - `.7.5.0` `fwLastResult` INTEGER `none(0) installed(1) rolledBackCrash(2) rolledBackUnreachable(3) fallbackCrash(4)`
  - Traps, v1 enterprise-specific under `63050` and v2c notifications, with varbinds `fwRunningSlot, fwSlotAVersion, fwSlotBVersion, fwLastResult` (v2c also `snmpTrapAddress.0`, as the mains traps):
    - `63050.0.4` `firmwareUpdateKept`
    - `63050.0.5` `firmwareRolledBack`

- [ ] **Step 1: Failing native tests:**
  - `test_get_fw_running_slot`
  - `test_get_fw_slot_versions_empty_when_invalid`
  - `test_walk_goes_from_mains_alarm_into_fw_group`
  - `test_getnext_after_fw_group_ends_enterprise_tree` (v1 noSuchName / v2c endOfMibView as today)
  - `test_fw_kept_trap_v1_and_v2c_bytes`
  - `test_fw_rolled_back_trap_v1_and_v2c_bytes`

  Compare against net-snmp: extend `scripts/host_agent_check.sh`.
- [ ] **Step 2:** Run. Expected: FAIL.
- [ ] **Step 3:** Implement. Update the MIB file (validate it with `wsl -d Ubuntu -- snmptranslate -M +./mibs -m +RF-POWER-MONITOR-MIB -Tp` showing the group) and `docs/MIB.md`. In LibreNMS, add a state sensor "Firmware" on `fwState` (trial = warning) and include `fwLastResult` in the device's overview. The trap script also polls on `63050.0.4`/`.0.5`.
- [ ] **Step 4:** Run `pio test -e native`, the sanitizers, `wsl -d Ubuntu -- bash scripts/host_agent_check.sh`, and HIL `test_snmp.py` with a new `test_walk_includes_fw_group`. Expected: PASS. Commit.

### Task 10: Serial menu and the update script

**Files:** `src/cli.cpp` (status screen), `tools/fw_update.py`, `test/hil/device.py` (parse the new status lines), `test/tools/test_fw_update_script.py`

**Interfaces:**
- Consumes: `fwUpdateStatus`, `GET /firmware/status`, `POST /login`, `GET /firmware` (csrf from the hidden field), `POST /firmware/upload`.
- Produces:
  - Status screen lines:
    - `Firmware: slot A, 3.3.0, kept` (or `trial`)
    - `Other slot: B, 3.2.9` (or `B, empty` / `B, bad signature`)
    - `Last update: installed 3.3.0` (or `rolled back (crash) 3.4.0` / `none`)
    - `Firmware key: <fingerprint>`
  - `tools/fw_update.py <file.upd> <host>... [--hosts-file F] [--continue] [--timeout-min 15]`. The password comes from `RFPOWER_WEB_PASSWORD` or `getpass`; never from an argument. Per device, it:
    1. logs in;
    2. reads the status (fails with "unreachable" if it cannot);
    3. checks the `.upd` file's key id against the device's `key=` (fails with "different key" before uploading);
    4. uploads;
    5. waits for the reboot;
    6. logs in again and polls `/firmware/status` until `state=kept` with `running_version` = the file's version.

    It prints one line per host: `updated`, `rejected: <reason>`, `rolled back: <last_result>`, `unreachable` or `timeout`. It stops at the first non-`updated` result unless `--continue` is given, and its exit code is 0 only when all hosts are updated. It uses only the standard library (`http.client`).

- [ ] **Step 1: Failing tests** in `test/tools/test_fw_update_script.py`, against a local fake device (`http.server` in a thread that mimics the endpoints):
  - `test_script_updates_and_waits_for_kept`
  - `test_script_stops_at_first_failure`
  - `test_script_continue_flag_runs_all`
  - `test_script_reports_rolled_back`
  - `test_script_refuses_key_mismatch_before_upload`
  - `test_script_never_puts_password_in_argv_or_output`
- [ ] **Step 2:** Run. Expected: FAIL.
- [ ] **Step 3:** Implement. Add the status lines to the CLI and parse them in `device.py`'s `status()`.
- [ ] **Step 4:** Run the tests and HIL `test_serial_menu.py`. Expected: PASS. Commit.

### Task 11: Settings rule for later layout changes

**Files:** `.clinerules` (Configuration Storage), `docs/TESTING.md`

- [ ] **Step 1:** Add to `.clinerules` under Configuration Storage: "A firmware that changes `SETTINGS_VERSION` writes the new layout to new files and keeps the old layout's files intact and updated, so a rollback or downgrade to older firmware comes up with its settings. The change comes with a test that an older-layout load still succeeds after a save by the new code." No code change now, because the layout is still version 1.
- [ ] **Step 2:** Commit with "Rules: settings must survive firmware rollback and downgrade".

### Task 12: End-to-end HIL tests of update and rollback

**Files:** `test/hil/test_fw_update.py`, `docs/TESTING.md`

**Interfaces:**
- Consumes:
  - `tools/fw_release.py --test-variant`;
  - `tools/fw_update.py` (imported as a module for its HTTP helpers);
  - `Device.status()`;
  - `snmp_tools`.

The tests are slow (builds and reboots). They run only with `RFPOWER_FW_UPDATE_TEST=1`. Each test ends with the device on a dev-signed build of the current version, kept, in either slot. Opt-in power-cycle cases need `RFPOWER_POE_CYCLE=1` and must be confirmed with the user before the first run, since they change PoE on ge-0/0/9 of r169.

- [ ] **Step 1: Write the tests:**
  - `test_upload_installs_and_is_kept_after_snmp_get`: running slot flips; state trial then kept; `fwLastResult` = installed; a `firmwareUpdateKept` trap reaches the trap receiver used by `test_snmp_traps.py`.
  - `test_upload_with_other_key_is_rejected`: 403 or `rejected: different key`; running slot and version unchanged.
  - `test_corrupted_upload_is_rejected_and_device_keeps_running`: flip a byte in the target image.
  - `test_aborted_upload_leaves_device_running`: close the socket after 100 KB; status unchanged except other slot = empty; a full upload afterwards works.
  - `test_upload_during_trial_is_refused`: 409.
  - `test_trial_crash_rolls_back`: `crash` variant; within 3 × (8.3 s + boot) + margin the old slot runs, `last_result` = rolled back (crash), and a `firmwareRolledBack` trap arrives.
  - `test_trial_unreachable_rolls_back`: `noconfirm` + `shorttrial`; within ~90 s it is back on the old slot with rolled back (unreachable).
  - `test_kept_firmware_crash_loop_falls_back`: after a kept update, `stall0` three times, without 5 healthy minutes in between; it then runs the other slot with fallback (crash).
  - `test_manual_switch_runs_other_slot_on_trial`: via `POST /firmware/switch`.
  - `test_core0_stack_headroom_after_upload`: the status screen's stack line stays above the existing threshold.
  - `test_power_cycles_during_trial_do_not_roll_back` (opt-in `RFPOWER_POE_CYCLE=1`): 4 PoE off/on cycles during a trial, still on trial or kept afterwards.
- [ ] **Step 2:** Run the opt-in set on the device, COM4, `RFPOWER_FW_UPDATE_TEST=1`. Expected: PASS. Run the whole HIL suite without the opt-in. Expected: PASS.
- [ ] **Step 3:** Document the opt-in variables and run times in `docs/TESTING.md`. Commit.

### Task 13: Documentation, version and release

**Files:** `include/version.h` (`3.3.0`), `CHANGELOG.md`, `TODO.md`, `docs/USER_MANUAL.md`, `docs/SETUP_MANUAL.md`, `CLAUDE.md` (Layout: `boot/`, `lib/fw_image`, `lib/fw_update`, `lib/monocypher`, `tools/`, `keys/dev`; Commands: release build, `fw_update.py`)

- [ ] **Step 1:** User manual:
  - the Firmware page;
  - what trial, kept and rolled back mean;
  - `fw_update.py` usage;
  - that a rejected upload empties the other slot until the next good upload.
- [ ] **Step 2:** Setup manual:
  - generate the key once (`tools/fw_keygen.py`) and back up `secret/fw_signing_key` off the PC;
  - what losing it or leaking it means;
  - field builds (`tools/fw_release.py --field`) and the first USB flash with `-usb.uf2`;
  - that a device flashed with a development build accepts only development-signed files.
- [ ] **Step 3:** `CHANGELOG.md` 3.3.0 entry, with a breaking note: the flash layout changed and the first install needs USB. Update `TODO.md` and `CLAUDE.md`.
- [ ] **Step 4:** Final verification:
  - `pio test -e native`;
  - the sanitizers;
  - `host_agent_check.sh`;
  - `pio test -e pico`, then reflash;
  - the full HIL suite;
  - the opt-in `RFPOWER_FW_UPDATE_TEST=1` set;
  - `pio run -e pico` flash use ≤ 70 % of a slot and RAM ≤ 50 %.

  Expected: PASS. Then commit.
