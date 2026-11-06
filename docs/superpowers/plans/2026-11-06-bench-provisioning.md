# Bench Provisioning and Device Label Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One bench command that flashes a new board (or reads an existing one), locks its default web password, makes its 40 x 40 mm label and logs it.

**Architecture:** Firmware gets two copies of the default web password, a lock file that blocks menu option 7, and hidden menu commands `benchinfo`, `pwlock`, `pwunlock`; the decisions are pure code in `lib/web_ui` and `lib/bench_info` with native tests. The PC side is four small Python modules in `tools/` (device I/O and UF2 check, label image, CSV log, the command) with `test/tools` unit tests.

**Tech Stack:** C++ (Arduino-Pico, LittleFS, Unity native tests); Python 3 with pyserial, Pillow, segno.

**Spec:** `docs/superpowers/specs/2026-11-06-bench-provisioning-design.md`

## Global Constraints

- Project rules in `.clinerules`: pure logic in `lib/` without `Arduino.h`/`LittleFS.h`; every logic change with tests in the same commit; native builds `-Wall -Wextra -Werror`; parsers of device/flash input get truncated and corrupted input tests and pass `scripts/sanitize.sh`; all functions documented; no buffer > ~256 bytes on core 0's stack.
- Firmware version becomes `3.5.0` (`include/version.h`); `CHANGELOG.md`, `README.md` (menu), `docs/TESTING.md`, `TODO.md` updated.
- Devices with no lock file behave as unlocked. Copy A keeps the existing file name `data/web_default.txt`, so older firmware (rollback) still finds the password.
- Only a release `-usb.uf2` is ever copied to a BOOTSEL drive, never `firmware.uf2`.
- The script never reads, prints or logs anything from `secret/` (the field key id comes from `include/fw_public_key.h`, which is public).
- Default password: 10 characters from `abcdefghijkmnpqrstuvwxyz23456789`, user `admin`.
- Hostname default: `RF-Power-` + last 6 MAC hex digits, upper case; hostnames are at most 32 characters (`HOSTNAME_MAX_LENGTH`).
- Label: 40 x 40 mm at 300 dpi = 472 x 472 dots, margins >= 2 mm (24 dots). Lines top to bottom: `RF Power Monitor` (bold, large), `rev 2` (small), rule, hostname (bold), `MAC  xx:xx:xx:xx:xx:xx`, `User admin`, `PW   <password>`; bottom left `S/N <serial>`, `FW <version>`, `YYYY-MM-DD`; bottom right QR about 12 mm.
- QR text: `RFPM;H=<hostname>;M=<12 upper-case hex digits>;S=<serial>;FW=<version>`; no password in it.
- FW version and date on the label are as shipped (from the bench run).
- Bench output (labels, `devices.csv` with passwords) goes in `bench/`, which is gitignored.
- Python dependencies for the bench tool: `pyserial`, `Pillow`, `segno` (pure Python), listed in `tools/requirements-bench.txt`.

## Review Focus

- A board whose `sysName` was set by hand (no-flash mode on an older board): hostname up to 32 characters must still fit on the label (text shrinks to fit), and the hostname/MAC check only warns. Test in Task 5 and Task 7.
- Serial output with noise around the `benchinfo` block (boot messages, a half-printed menu, a missing END line): the parser takes the last complete block and refuses a truncated one. Test in Task 4.
- Two RP2040 serial ports connected at once in no-flash mode: the script refuses and asks for `--port` rather than picking one. Test in Task 7.
- Flash wiped or both password copies corrupt on a locked device: the firmware reports "lost" instead of silently printing a new password as if nothing happened. Test in Task 1 (decision, native) and Task 7 (`lost=1` reaches the person at the bench); wiping LittleFS on the real device is not HIL-tested.
- A UF2 signed with the development key, or a `firmware.uf2` without boot program: refused before anything is copied. Test in Task 4 and Task 7.

---

### Task 1: Default password copy choice (pure)

**Files:**
- Create: `lib/web_ui/include/default_password.h`, `lib/web_ui/src/default_password.cpp`
- Test: `test/native/test_web_ui/test_main.cpp` (add tests and `RUN_TEST` lines)

**Interfaces:**
- Produces:
  - `bool webDefaultPasswordIsValid(const char* text)`: exactly `WEB_DEFAULT_PASSWORD_LEN` characters, all from the default alphabet; false for nullptr.
  - `enum class DefaultPasswordSource : uint8_t { CopyA, CopyB, None };`
  - `struct DefaultPasswordChoice { DefaultPasswordSource source; bool writeA; bool writeB; bool lostWhileLocked; };`
  - `DefaultPasswordChoice chooseDefaultPassword(const char* a, const char* b, bool locked)`: `a`/`b` are the file contents (nullptr when the file is missing). A valid A wins (A is written first on every save, so it is the newer one). `writeA`/`writeB` say which copy must be (re)written: the other copy when it is invalid or differs; both when `source == None`. `lostWhileLocked = (source == None && locked)`.

- [ ] **Step 1: Write the failing tests** in `test/native/test_web_ui/test_main.cpp`:

```cpp
static void test_default_password_valid_needs_ten_alphabet_chars() {
    TEST_ASSERT_TRUE(webDefaultPasswordIsValid("k7mq2xr9tp"));
    TEST_ASSERT_FALSE(webDefaultPasswordIsValid(nullptr));
    TEST_ASSERT_FALSE(webDefaultPasswordIsValid(""));
    TEST_ASSERT_FALSE(webDefaultPasswordIsValid("k7mq2xr9t"));    // 9
    TEST_ASSERT_FALSE(webDefaultPasswordIsValid("k7mq2xr9tpa"));  // 11
    TEST_ASSERT_FALSE(webDefaultPasswordIsValid("k7mq2xr9t0"));   // 0 not in the alphabet
    TEST_ASSERT_FALSE(webDefaultPasswordIsValid("K7MQ2XR9TP"));   // upper case
}
static void test_default_password_both_copies_equal_writes_nothing() {
    DefaultPasswordChoice c = chooseDefaultPassword("k7mq2xr9tp", "k7mq2xr9tp", false);
    TEST_ASSERT_EQUAL(DefaultPasswordSource::CopyA, c.source);
    TEST_ASSERT_FALSE(c.writeA); TEST_ASSERT_FALSE(c.writeB); TEST_ASSERT_FALSE(c.lostWhileLocked);
}
static void test_default_password_missing_b_is_repaired_from_a() {
    DefaultPasswordChoice c = chooseDefaultPassword("k7mq2xr9tp", nullptr, true);
    TEST_ASSERT_EQUAL(DefaultPasswordSource::CopyA, c.source);
    TEST_ASSERT_FALSE(c.writeA); TEST_ASSERT_TRUE(c.writeB);
}
static void test_default_password_corrupt_a_uses_b_and_repairs_a() {
    DefaultPasswordChoice c = chooseDefaultPassword("k7mq2x\xff", "k7mq2xr9tp", true);
    TEST_ASSERT_EQUAL(DefaultPasswordSource::CopyB, c.source);
    TEST_ASSERT_TRUE(c.writeA); TEST_ASSERT_FALSE(c.writeB); TEST_ASSERT_FALSE(c.lostWhileLocked);
}
static void test_default_password_copies_differ_a_wins() {
    DefaultPasswordChoice c = chooseDefaultPassword("k7mq2xr9tp", "abcdefghij", false);
    TEST_ASSERT_EQUAL(DefaultPasswordSource::CopyA, c.source);
    TEST_ASSERT_TRUE(c.writeB);
}
static void test_default_password_old_firmware_single_file_is_copy_a() {
    // A device updated from 3.4.0 has only data/web_default.txt (copy A)
    DefaultPasswordChoice c = chooseDefaultPassword("k7mq2xr9tp", nullptr, false);
    TEST_ASSERT_EQUAL(DefaultPasswordSource::CopyA, c.source);
    TEST_ASSERT_TRUE(c.writeB);
}
static void test_default_password_none_valid_unlocked_is_not_lost() {
    DefaultPasswordChoice c = chooseDefaultPassword(nullptr, "", false);
    TEST_ASSERT_EQUAL(DefaultPasswordSource::None, c.source);
    TEST_ASSERT_TRUE(c.writeA); TEST_ASSERT_TRUE(c.writeB); TEST_ASSERT_FALSE(c.lostWhileLocked);
}
static void test_default_password_none_valid_locked_is_lost() {
    DefaultPasswordChoice c = chooseDefaultPassword("garbage", "k7mq2", true);
    TEST_ASSERT_EQUAL(DefaultPasswordSource::None, c.source);
    TEST_ASSERT_TRUE(c.lostWhileLocked);
}
```

- [ ] **Step 2: Run** `pio test -e native -f native/test_web_ui`. Expected: compile FAIL (`default_password.h` not found).
- [ ] **Step 3: Implement** the header (documented, pure, no Arduino includes) and `default_password.cpp`. Move the alphabet constant from `web_auth.cpp` so both files share one definition (declare it in `default_password.h`); `webMakeDefaultPassword` keeps working.
- [ ] **Step 4: Run** `pio test -e native -f native/test_web_ui`. Expected: all PASS. Check one test can fail: flip "A wins" to "B wins", see `test_default_password_copies_differ_a_wins` fail, restore.
- [ ] **Step 5: Run** `wsl -d Ubuntu -- bash scripts/sanitize.sh` (after `pio test -e native`). Expected: clean.
- [ ] **Step 6: Commit** `git commit -m "Default web password: choose between two copies, detect loss while locked"`

### Task 2: Bench info block (pure)

**Files:**
- Create: `lib/bench_info/include/bench_info.h`, `lib/bench_info/src/bench_info.cpp`, `lib/bench_info/library.json` (copy the shape of another pure lib's, e.g. `lib/netconfig`)
- Test: `test/native/test_bench_info/test_main.cpp`

**Interfaces:**
- Produces:
  - `struct BenchInfo { const char* hostname; uint8_t mac[6]; const char* serial; const char* firmware; const char* fwKey; const char* password; bool locked; bool passwordLost; };`
  - `size_t formatBenchInfo(const BenchInfo& info, char* out, size_t size)`: writes the block below; returns its length, or 0 (and `out[0] = '\0'` when `size > 0`) when it does not fit. Null string fields print as empty.
  - `static const size_t BENCH_INFO_MAX = 256;`
  - Exact block (lines end in `\r\n`; Task 4 parses it):

```
BENCHINFO BEGIN
hostname=RF-Power-A1B2C3
mac=2C:CF:67:A1:B2:C3
serial=E6614103E7A1B2C3
firmware=3.5.0
fwkey=0123456789abcdef
password=k7mq2xr9tp
locked=1
lost=0
BENCHINFO END
```

- [ ] **Step 1: Write the failing tests**: `test_format_bench_info_exact_block` (the block above, byte for byte, MAC upper case with colons), `test_format_bench_info_too_small_returns_zero` (size = exact length → 0, `out[0] == '\0'`; size = exact length + 1 → length), `test_format_bench_info_null_fields_print_empty` (`serial=nullptr` gives `serial=\r\n`), `test_format_bench_info_longest_hostname_fits` (32-character hostname, 63-character firmware text not needed: use `"3.5.0-rc.12"`, result > 0 with `BENCH_INFO_MAX`).
- [ ] **Step 2: Run** `pio test -e native -f native/test_bench_info`. Expected: FAIL (missing header).
- [ ] **Step 3: Implement** with `snprintf` into `out`; no heap.
- [ ] **Step 4: Run** the test again, then `wsl -d Ubuntu -- bash scripts/sanitize.sh`. Expected: PASS, clean.
- [ ] **Step 5: Commit** `git commit -m "bench_info: the device data block for the bench tool"`

### Task 3: Firmware: two copies, lock, hidden commands

**Files:**
- Modify: `src/web_service.cpp` (load/generate/regenerate, ~267-302), `src/web_service.h`, `src/cli.cpp` (option 7 at ~573 and ~856, hidden commands near `checkin` ~606, status screen ~247), `include/version.h` (`3.5.0`)
- Modify: `test/hil/device.py` (helpers), `test/hil/test_web.py` (tests)
- Modify: `README.md`, `docs/TESTING.md`, `CHANGELOG.md`, `TODO.md`

**Interfaces:**
- Consumes: Task 1 `chooseDefaultPassword`, `webDefaultPasswordIsValid`; Task 2 `formatBenchInfo`, `BenchInfo`, `BENCH_INFO_MAX`.
- Produces (in `src/web_service.h`, documented):
  - Files: copy A `data/web_default.txt` (unchanged), copy B `data/web_default_b.txt`, lock `data/web_default.lock` (exists = locked; content `locked\n`).
  - `bool webDefaultPasswordLocked();`
  - `bool webSetDefaultPasswordLocked(bool locked);` creates/removes the lock file, reads back; false if that failed.
  - `bool webDefaultPasswordLost();` true when boot found no valid copy while locked.
  - `webRegenerateDefaultPassword()` returns false without changing anything when locked.
- Menu (hidden, not in `printMenu`):
  - `benchinfo`: prints the Task 2 block (static buffer, not on the stack), then `> `.
  - `pwlock`: `Default web password locked (it is on the device label)` or `Failed to lock`.
  - `pwunlock`: `Default web password unlocked (option 7 can replace it)` or `Failed to unlock`.
- Option 7 while locked prints `The default web password is locked because it is printed on the device label. Unlock it at the bench first.` and returns to the menu without asking y/n.
- Status screen, after the `Web login:` line: `Default web password: locked (on the label)` or `... unlocked`; and when lost: `Default web password LOST: label is wrong, relabel at the bench`. The same lost text is printed once at boot.
- `MAC` for `benchinfo` comes from `eth.macAddress()` as in `src/main.cpp:244`; `fwkey` is the fingerprint the status screen prints as `Firmware key:` (`WebFirmware.keyFingerprint`); `serial` from `hostStatsSnapshot().serialNumber`; `hostname` from `hostnameCurrent()`.
- HIL helpers in `test/hil/device.py`: `bench_info() -> dict` (send `benchinfo`, parse key=value between the markers), `lock_web_default_password(lock: bool) -> str`.

- [ ] **Step 1: Write the failing HIL tests** in `test/hil/test_web.py` (each restores the lock state it found, in `addCleanup`):
  - `test_benchinfo_reports_hostname_mac_and_default_password`: `bench_info()` has `hostname` == status sysName, `mac` == status MAC, `password` == status default password, `firmware == "3.5.0"`, `fwkey` == status `Firmware key`.
  - `test_locked_default_password_refuses_option_7`: lock; send `7`; output has `is locked because it is printed on the device label`; `bench_info()["password"]` unchanged.
  - `test_unlock_allows_option_7_again`: lock, unlock, option 7 confirmed changes the password (then the test leaves the new one: like `test_new_default_password_from_serial_menu`, this test changes the device's default password; say so in its docstring).
  - `test_lock_and_password_survive_factory_reset`: skip unless the factory reset HIL setup is enabled the same way `test_factory_reset.py` is; lock, factory reset, `bench_info()` has same password and `locked == "1"`.
  - `test_lock_survives_reboot`: lock, reboot (menu reboot helper used elsewhere), `locked == "1"`.
- [ ] **Step 2: Implement** the firmware changes above. Loading: read both copies into static buffers, `chooseDefaultPassword(a, b, lockFileExists)`, write the copies it asks for (A before B), keep `passwordLost`. Regenerate writes A then B.
- [ ] **Step 3: Build** `%USERPROFILE%\.platformio\penv\Scripts\pio.exe run -e pico`. Expected: SUCCESS, flash/RAM within the rules' limits.
- [ ] **Step 4: Flash** `pio run -e pico -t upload` (COM4). Then run `python -m unittest test.hil.test_web -v` style per docs (`-p "test_web.py"`). Expected: new tests PASS, existing `test_new_default_password_from_serial_menu` still PASS (device unlocked).
- [ ] **Step 5: Run** `pio test -e native` and `pio test -e pico` (embedded tests), then reflash `pio run -e pico -t upload`. Expected: all PASS.
- [ ] **Step 6: Docs**: README menu section (hidden commands are for the bench tool; option 7 refuses while locked; lock survives factory reset), docs/TESTING.md (new HIL tests), CHANGELOG 3.5.0 entry, TODO.md.
- [ ] **Step 7: Commit** `git commit -m "Default web password: two copies, bench lock, benchinfo command (3.5.0)"`

### Task 4: Bench device I/O and UF2 check (Python)

**Files:**
- Create: `tools/bench_device.py`, `tools/requirements-bench.txt` (`pyserial>=3.5`, `Pillow>=10`, `segno>=1.6`)
- Test: `test/tools/test_bench_device.py`
- Modify: `.github/workflows/tests.yml` (install `tools/requirements-bench.txt` before "Tool tests")

**Interfaces:**
- Consumes: block format from Task 2; `scripts/uf2.py` constants (`UF2_MAGIC_*`, `PAYLOAD_SIZE`); `tools/fwtool.py` `header_public_key`, `key_fingerprint`, `FIELD_HEADER`; dev header `include/fw_dev_public_key.h`; slot A at flash `0x10010000`, header layout in `lib/fw_image/include/fw_image.h` (magic `0x57464652` at 64, version at 80..96, key id at 96..104).
- Produces:
  - `class BenchError(Exception)`: every refusal, with a message for the person at the bench.
  - `parse_bench_info(text: str) -> dict[str, str]`: the last complete `BENCHINFO BEGIN`..`BENCHINFO END` block; keys `hostname, mac, serial, firmware, fwkey, password, locked, lost`; raises `BenchError` when no complete block or a key is missing.
  - `@dataclass UF2Firmware: path: str; version: str; key_id: str` and `read_release_uf2(path: str) -> UF2Firmware`: refuses (BenchError) a file whose name does not end in `-usb.uf2`, a file without data at flash `0x10000000` (no boot program), or without a valid slot A header.
  - `field_key_id() -> str`, `dev_key_id() -> str` (fingerprints of the two public key headers).
  - `check_signed_for(fw: UF2Firmware, dev: bool) -> None`: BenchError unless `fw.key_id` matches the field key (or the dev key when `dev`).
  - `class BenchSerial` with `__init__(port)`, `open()`, `close()`, `command(line: str, until: str, timeout: float) -> str`, `wait_for_menu(timeout: float)`; same open/DTR rules as `test/hil/device.py`.
  - `read_info(ser: BenchSerial) -> dict`, `lock(ser: BenchSerial) -> None` (sends `pwlock`, then re-reads info and raises unless `locked == "1"`).
  - `pico_ports() -> list[str]`: COM ports with USB VID `0x2E8A`.

- [ ] **Step 1: Write the failing tests** (no hardware; build UF2 files with `scripts/uf2.write_uf2` in a temp dir and a fake slot header with `struct.pack`):
  - `test_parse_takes_last_complete_block` (boot noise, an old block, a new block: values from the new one)
  - `test_parse_rejects_block_without_end`
  - `test_parse_rejects_missing_key` (no `password=` line)
  - `test_parse_keeps_equals_sign_in_value` (`hostname=a=b` → `"a=b"`)
  - `test_read_release_uf2_reads_version_and_key_id`
  - `test_read_release_uf2_refuses_firmware_uf2_name`
  - `test_read_release_uf2_refuses_file_without_boot_program` (only slot A data)
  - `test_read_release_uf2_refuses_bad_slot_magic`
  - `test_check_signed_refuses_dev_key_without_flag` / `test_check_signed_accepts_dev_key_with_flag` / `test_check_signed_accepts_field_key`
  - `test_lock_raises_when_device_still_unlocked` (a fake `BenchSerial` returning `locked=0`)
- [ ] **Step 2: Run** `python -m unittest test.tools.test_bench_device -v` (from the repo root, or `discover -s test/tools -p "test_bench_device.py"`). Expected: FAIL (module missing).
- [ ] **Step 3: Implement** `tools/bench_device.py`; every function documented.
- [ ] **Step 4: Run** the tests. Expected: PASS. Then `pip install -r tools/requirements-bench.txt` and the full `python -m unittest discover -s test/tools`.
- [ ] **Step 5: Commit** `git commit -m "Bench tool: device info, lock and release UF2 check"`

### Task 5: Label image (Python)

**Files:**
- Create: `tools/bench_label.py`, `tools/fonts/` (DejaVu Sans, Sans Bold, Sans Mono, Sans Mono Bold `.ttf` from the DejaVu 2.37 release, and its `LICENSE`)
- Test: `test/tools/test_bench_label.py`

**Interfaces:**
- Produces:
  - `@dataclass LabelData: hostname: str; mac: str; serial: str; firmware: str; password: str; revision: str; date: str` (`mac` as `2C:CF:67:A1:B2:C3`, `date` as `YYYY-MM-DD`).
  - `SIZE_DOTS = 472`, `MARGIN_DOTS = 24`, `QR_MODULE_DOTS = 4`, `QR_BORDER_MODULES = 2`.
  - `qr_text(d: LabelData) -> str`: `RFPM;H=<hostname>;M=<mac without colons, upper>;S=<serial>;FW=<firmware>`.
  - `render_ink(d: LabelData) -> PIL.Image.Image`: mode `"1"`, 472 x 472, **ink = black (0) on white**; this is the file for the printer/NIIMBOT app, because the printer transfers ribbon wherever the image is dark, and the ribbon is white.
  - `render_preview(d: LabelData) -> PIL.Image.Image`: `ImageOps.invert` of the ink image in mode `"L"`: white on black, how the label will look.
  - `save_label(d: LabelData, folder: str) -> tuple[str, str]`: writes `<hostname>.png` (ink, with 300 dpi metadata) and `<hostname>-preview.png`; returns both paths.
  - Layout per Global Constraints; QR from `segno.make(text, error="m", boost_error=False)`, drawn at `QR_MODULE_DOTS` per module, bottom-right inside the margins, with `QR_BORDER_MODULES` quiet zone of no ink. Each text line shrinks its font in 1-dot steps until it fits `SIZE_DOTS - 2 * MARGIN_DOTS` wide (and for the bottom-left lines, left of the QR).
- [ ] **Step 1: Write the failing tests**:
  - `test_qr_text_format` (exact string for the sample data; MAC without colons, upper case)
  - `test_ink_image_is_472_square_bilevel`
  - `test_ink_stays_inside_margins` (no black pixel in the outer 24 dots)
  - `test_long_hostname_still_inside_margins` (32-character hostname)
  - `test_qr_modules_match_segno_matrix`: locate the QR from the constants (bottom-right, size = `(len(matrix) + 2*border) * 4`), sample each module centre, compare with `segno.make(qr_text(d), error="m", boost_error=False).matrix`
  - `test_password_not_in_qr_text`
  - `test_preview_is_inverted_ink` (pixel (0,0) black in preview, white in ink)
  - `test_save_label_writes_both_files_at_300_dpi`
- [ ] **Step 2: Run** `python -m unittest discover -s test/tools -p "test_bench_label.py" -v`. Expected: FAIL.
- [ ] **Step 3: Implement** `tools/bench_label.py`; start sizes (dots, shrink-to-fit applies): title 46 bold, revision 22, hostname 38 bold, MAC/User/PW 30 mono (labels bold), bottom lines 21.
- [ ] **Step 4: Run** the tests. Expected: PASS. Open the preview PNG for the sample data and compare it with the agreed mockup (`E1 revised`: title, `rev 2`, rule, hostname, MAC/User/PW, S/N/FW/date bottom left, QR bottom right).
- [ ] **Step 5: Commit** `git commit -m "Bench tool: 40 x 40 mm label image"`

### Task 6: Device log (Python)

**Files:**
- Create: `tools/bench_log.py`
- Test: `test/tools/test_bench_log.py`
- Modify: `.gitignore` (add `bench/`)

**Interfaces:**
- Produces:
  - `FIELDS = ["time", "hostname", "mac", "serial", "firmware", "revision", "password", "label"]`
  - `append(path: str, row: dict) -> None`: creates the file with a header row if missing; UTF-8; `time` as ISO 8601 local time with seconds.
  - `latest(path: str, key: str) -> dict | None`: newest row (last in file) whose hostname (case-insensitive), MAC (with or without colons, any case) or serial equals `key`; rows for other devices never match on a partial value.
- [ ] **Step 1: Write the failing tests**: `test_append_creates_header_once`, `test_latest_newest_row_wins_for_same_mac`, `test_latest_matches_mac_without_colons_any_case`, `test_latest_matches_hostname_case_insensitive`, `test_latest_none_when_unknown`, `test_latest_none_when_file_missing`, `test_latest_ignores_partial_hostname` (`RF-Power-A1` does not match `RF-Power-A1B2C3`).
- [ ] **Step 2: Run** `python -m unittest discover -s test/tools -p "test_bench_log.py" -v`. Expected: FAIL.
- [ ] **Step 3: Implement** with the `csv` module.
- [ ] **Step 4: Run** the tests. Expected: PASS.
- [ ] **Step 5: Commit** `git commit -m "Bench tool: device log"`

### Task 7: The bench command

**Files:**
- Create: `tools/bench_provision.py`
- Test: `test/tools/test_bench_provision.py`
- Modify: `README.md` (new "Bench setup of a new board" section), `docs/TESTING.md` (tool tests list, how to try the script on the test device), `CLAUDE.md` (Commands table row and `tools/` line), `CHANGELOG.md`

**Interfaces:**
- Consumes: Tasks 4-6.
- Produces: command line

```
python tools/bench_provision.py [--uf2 FILE] [--dev] [--revision "rev 2"] [--out bench]
python tools/bench_provision.py --no-flash [--port COMx] [--revision ...] [--out bench]
python tools/bench_provision.py --reprint <hostname|MAC|serial> [--out bench]
```

  - `main(argv=None, *, io=None) -> int`: `io` is an object with the hardware steps so tests can replace them: `find_bootsel_drive(timeout) -> str`, `copy(src, drive_root) -> None`, `pico_ports() -> list[str]`, `open_serial(port) -> BenchSerial`, `now() -> datetime`. Default `io` uses the real ones.
  - Default UF2: newest `release/rf-power-monitor-*-usb.uf2` by version number (not mtime).
  - Flash flow: `read_release_uf2` + `check_signed_for` **before** asking for BOOTSEL; print "Hold BOOTSEL and plug in the board"; wait for a drive whose `INFO_UF2.TXT` names `RPI-RP2` (up to 120 s); note `pico_ports()` before the copy; copy; wait (60 s) for the new Pico port; open; `wait_for_menu`; `read_info`.
  - Checks after reading, each a BenchError with a clear message: `firmware` equals the UF2 version (flash mode); `fwkey` equals the field key id (or dev with `--dev`) in both modes; hostname equals `RF-Power-<MAC tail>` in flash mode, only a printed warning in no-flash mode; password valid (10 characters, alphabet); `lost == "1"` is reported and the run continues (the new password is what goes on the new label).
  - Then `lock`, `save_label`, `bench_log.append` (with `revision` and label path), print hostname, MAC and the two PNG paths, and the line "Print <label>.png from the NIIMBOT app (40 x 40 mm, white ribbon)". Exit code 0.
  - `--no-flash`: port from `--port`, else the one Pico port; BenchError when there are none or more than one ("several boards connected: use --port").
  - `--reprint`: `bench_log.latest`, re-render with the row's values (as-shipped firmware and the row's date), no device; BenchError if not found.
  - BenchError: message to stderr, exit code 1. Never prints anything from `secret/`.
- [ ] **Step 1: Write the failing tests** with a fake `io` and a fake serial that returns a canned `benchinfo` block (and `locked=1` after `pwlock`), all output in a temp folder:
  - `test_flash_run_writes_label_and_log_row`
  - `test_flash_run_refuses_dev_signed_uf2_before_asking_for_bootsel` (fake `find_bootsel_drive` must not be called)
  - `test_flash_run_refuses_version_mismatch`
  - `test_flash_run_refuses_hostname_not_matching_mac`
  - `test_no_flash_warns_on_custom_hostname_and_continues`
  - `test_no_flash_refuses_two_ports_without_port_option`
  - `test_reprint_uses_newest_row_without_device` (fake `open_serial` must not be called)
  - `test_reprint_unknown_device_exits_1`
  - `test_default_uf2_is_highest_version` (`3.10.0` beats `3.9.0`)
  - `test_lost_password_is_reported_and_labelled`
- [ ] **Step 2: Run** `python -m unittest discover -s test/tools -p "test_bench_provision.py" -v`. Expected: FAIL.
- [ ] **Step 3: Implement** `tools/bench_provision.py` (Windows drive scan: letters `D`..`Z`, `X:\INFO_UF2.TXT`).
- [ ] **Step 4: Run** all tool tests `python -m unittest discover -s test/tools -v`. Expected: PASS.
- [ ] **Step 5: Try it on the test device** (COM4, firmware from Task 3, dev-signed): `python tools/bench_provision.py --no-flash --port COM4 --dev --out bench`. Expected: label PNGs and a CSV row in `bench/`; the device is now locked. Then send `pwunlock` on the device (restore). Note: `--dev` is accepted in no-flash mode only for this check.
- [ ] **Step 6: Docs** (README section with the three commands, the BOOTSEL step, where files go, that `bench/devices.csv` holds passwords; TESTING; CLAUDE.md; CHANGELOG).
- [ ] **Step 7: Commit** `git commit -m "Bench tool: flash, lock, label and log a board in one run"`

### Task 8: First real board (with Kenneth)

Not code: needs a blank rev 2 board and BOOTSEL.

- [ ] **Step 1:** `python tools/fw_release.py --field` (release files with the field key).
- [ ] **Step 2:** Kenneth runs `python tools/bench_provision.py` and plugs in the board with BOOTSEL held. Expected: exit 0, label and CSV row, `benchinfo` shows `locked=1` and the field key id.
- [ ] **Step 3:** Print `<hostname>.png` from the NIIMBOT app once the M2 arrives; check text size and QR scanning with a phone. Direct printing from the script is a later, separate change.
