# Update server (device check-in, chosen firmware updates, shared key) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Devices check in with a Docker website every 30 minutes (serial, hostname, addresses, version, update state). The admin uploads a signed `.upd` and ticks devices. Those devices download it at their next check-in and install it through the existing A/B update path. Every check-in and reply is authenticated with a shared key.

**Architecture:** The firmware side follows the usual split. A pure library `lib/update_client` holds the wire format, the keyed BLAKE2b MACs, URL and HTTP response parsing, the check-in scheduler and the update-settings codec, and is host tested. `src/update_service.cpp` runs it on core 0 with lwIP's raw TCP and DNS callback APIs (never blocking) and streams a download into `fwUpdateUploadBegin/Feed/Finish`. The URL and key live in their own A/B file pair, so the main settings file and older firmware are untouched. The site is a stdlib-only Python 3.11+ program in `update-server/` (SQLite, `http.server`), so its tests run with the PlatformIO Python, in CI, and in the HIL test without Docker. Docker only packages it.

**Tech Stack:** Arduino-Pico + lwIP raw API (`tcp_*`, `dns_gethostbyname`), Monocypher `crypto_blake2b_keyed` (already vendored in `lib/monocypher`), Python 3.11+ stdlib (`http.server`, `sqlite3`, `hashlib.blake2b`, `email` for multipart), Docker Compose.

**Spec:** `docs/superpowers/specs/2026-11-06-update-server-design.md`

## Global Constraints

- `.clinerules` applies to every task:
  - pure logic goes in `lib/` with native tests; `src/` stays thin;
  - native code builds with `-Wall -Wextra -Werror`;
  - every function is documented;
  - no buffer or struct over ~256 bytes on the stack (make it static);
  - parsers of network or flash input get truncated and corrupted input tests and pass `scripts/sanitize.sh`;
  - a test must be able to fail;
  - tests never touch the device's real settings (`test/` paths).
- Core 0 must never block. Check-ins and downloads use lwIP raw callbacks under the core's lwIP lock, as `src/net_stats.cpp` does. No `WiFiClient::connect()`: it waits for the connection. SNMP stays within 100 ms, the CLI within 50 ms.
- **Check-in timing:**
  - first check-in **60 s** (`60000` ms) after the network is ready;
  - then every **30 min** (`1800000` ms);
  - after a failed check-in, retry after **5 min** (`300000` ms);
  - after a state change worth reporting (download failed or refused, trial confirmed), check in again after **60 s**.
  - Timeouts: DNS + connect **10 s**, whole check-in **15 s**, download idle **30 s**.
- **Shared key:**
  - 32 bytes, shown and entered as 64 lowercase hex characters, never printed back in full ("set" / "not set");
  - cleared by a factory reset;
  - never from DHCP;
  - URL without a key means no check-ins (status `no key`).
- **MAC:** BLAKE2b, 32-byte output, keyed with the 32-byte key, sent as 64 lowercase hex.
- **Update URL:**
  - `http://host[:port][/path]` only, at most **127** characters;
  - host is a DNS name, an IPv4 dotted quad, or `[IPv6]`;
  - default port 80; the path is a prefix (no trailing `/` needed).
- **DHCP option 225:** a text option holding the URL, used only while "update URL from DHCP" is on, and only from an ACK lwIP accepted (the same gate as option 224). Printable ASCII, 1–127 bytes; longer or non-printable values are rejected, never truncated.
- **Update settings storage:**
  - their own files `/update_a.json` / `/update_b.json` (A/B, sequence number, read-back verify, like `SettingsStore`), JSON with `"version": 1`;
  - the main `Settings` struct, `SETTINGS_JSON_MAX` (1024) and `SETTINGS_VERSION` (1) stay unchanged. The worst case main file is already ~970 bytes, and older firmware enforces the same limit;
  - missing or invalid files mean defaults (URL empty, from-DHCP off, no key).
- **Site:**
  - port **8071**;
  - data dir from `RFUPDATE_DATA` (default `/data` in the container);
  - admin password from `RFUPDATE_ADMIN_PASSWORD` (no default: the site refuses logins until it is set);
  - Docker `restart: unless-stopped` and a named volume `rfupdate-data`.
- **Site rules:**
  - offline after **65 min** (`3900` s) without a check-in;
  - a release's identity is its version string, and a duplicate version is refused;
  - releases older than **`3.4.0`** (the first firmware with check-ins) are refused;
  - an assignment ends on done / rolled back / refused, after **3** download failures, or as **lost** **2 h** (`7200` s) after the device was first told;
  - nonces seen per serial are kept for 24 h and a repeat is refused;
  - largest upload **1 761 344** bytes (`64 + 2 * 0xD7000`, = `FW_UPLOAD_MAX`).
- Version `3.4.0` in `include/version.h`, with a `CHANGELOG.md` entry, ticked `TODO.md` items, and docs (`docs/USER_MANUAL.md`, `docs/SETUP_MANUAL.md`, `docs/TESTING.md`, `CLAUDE.md` Layout and Commands).
- Never read, print or commit anything in `secret/`. HIL and tool tests use the development key and their own temp data dirs.

## Wire format (fixed here; both sides test against the vectors in Task 1)

Check-in: `POST <urlpath>/api/checkin`, `Content-Type: text/plain`, header `X-RFPower-Auth: <MAC(key, body)>`. Body: LF-terminated `key=value` lines, in this order (the `ipv6` line repeats 0–4 times):

```
serial=<flash unique id, 16 upper hex>
hostname=<hostnameCurrent()>
ipv4=<dotted quad or empty>
ipv6=<RFC 5952 text>
mac=<aa:bb:cc:dd:ee:ff>
version=<FIRMWARE_VERSION>
slot=<A|B>
trial=<0|1>
update=<idle|downloading>
fail=<empty | <version> refused <reason> | <version> download <reason>>
last=<none | installed <v> | rolledback-crash <v> | rolledback-unreachable <v> | fallback-crash <v>>
nonce=<32 lower hex, 16 bytes from rp2040.hwrand32()>
```

Reply 200, `text/plain`, at most 512 bytes:

```
status=ok
auth=<MAC(key, nonce + "\n" + every byte before the auth line)>
```

or

```
status=update
version=<release version>
path=<absolute path on the same host:port, e.g. /releases/3.4.1.upd>
auth=<...>
```

Errors carry no `auth` line, and the device treats them as a failed check-in, showing the status word: 401 `status=badauth`, 409 `status=replay`, 400 `status=bad`.

## Review Focus

1. **The site is down, slow or answers garbage** (port closed, DNS name unknown, half a reply, a 2 MB reply, a reply without `auth`, a wrong `auth`, an old reply replayed). The device keeps serving SNMP and the web page, shows a clear check-in error and retries after 5 min. It never acts on an unauthenticated reply. Tests: `test_parse_reply_*` (Task 1), `test_http_response_*` (Task 2), `test_checkin_unreachable_server_keeps_snmp_answering` (Task 10).
2. **A download that stops halfway or is the wrong file** (connection dropped, a `.upd` whose header version differs from the one the reply named, a corrupted signature). The device aborts the upload (`fwUpdateUploadAbort`), keeps running, and reports `fail=<v> download ...` or `fail=<v> refused ...`. The site ends the assignment and never loops. Tests: `test_download_version_mismatch_is_refused` (Task 6), `test_site_ends_assignment_after_three_download_failures` (Task 8), `test_corrupt_release_is_refused_and_device_keeps_running` (Task 10).
3. **Both DHCP options in one request**: lwIP leaves few padding bytes. Adding 224 and 225 must both fit, or the device must report `NoRoom` (counter) rather than corrupt the frame. Test: `test_request_two_options_in_captured_discover` (Task 3).
4. **A factory reset or a rollback to 3.3.0**: the reset clears the URL and key, and 3.3.0 ignores the new files and keeps its own settings. Tests: `test_factory_reset_clears_update_settings` (Task 4), `test_main_settings_file_unchanged_by_update_settings_save` (Task 4).
5. **The admin assigns a release the device already runs, or one older than 3.4.0**: the first is marked done without a download, the second is refused at upload. Tests: `test_assignment_of_running_version_is_done_at_once`, `test_upload_older_than_min_version_is_refused` (Task 8).

---

### Task 1: Wire format and MACs (`lib/update_client`, Python twin)

**Files:**
- Create: `lib/update_client/library.json` (depends on `monocypher`), `lib/update_client/include/update_protocol.h`, `lib/update_client/src/update_protocol.cpp`
- Create: `update-server/rfupdate/__init__.py`, `update-server/rfupdate/protocol.py`
- Create: `update-server/tests/vectors.json` (MAC test vectors computed once with `hashlib.blake2b(digest_size=32, key=...)`)
- Test: `test/native/test_update_protocol/test_main.cpp`, `update-server/tests/test_protocol.py`

**Interfaces:**
- Produces (C++):
  - `static const size_t UPDATE_KEY_LEN = 32, UPDATE_MAC_HEX = 64, UPDATE_NONCE_HEX = 32, UPDATE_REPLY_MAX = 512, UPDATE_VERSION_MAX = 16;`
  - `struct CheckinInfo { char serial[17]; char hostname[64]; char ipv4[16]; char ipv6[4][IPV6_TEXT_MAX]; uint8_t ipv6Count; char mac[18]; char version[UPDATE_VERSION_MAX+1]; char slot; bool trial; bool downloading; char fail[96]; char last[48]; char nonce[UPDATE_NONCE_HEX+1]; };`
  - `size_t buildCheckinBody(const CheckinInfo& in, char* out, size_t size);` (0 if it does not fit)
  - `void updateMacHex(const uint8_t key[UPDATE_KEY_LEN], const uint8_t* data, size_t len, char out[UPDATE_MAC_HEX+1]);`
  - `bool parseKeyHex(const char* text, uint8_t key[UPDATE_KEY_LEN]);` (exactly 64 hex, either case)
  - `enum class ReplyKind : uint8_t { Ok, Update, Malformed, BadAuth };`
  - `struct CheckinReply { ReplyKind kind; char version[UPDATE_VERSION_MAX+1]; char path[128]; };`
  - `ReplyKind parseCheckinReply(const char* body, size_t len, const char* nonce, const uint8_t key[UPDATE_KEY_LEN], CheckinReply& out);`
- Produces (Python `rfupdate.protocol`): `mac_hex(key: bytes, data: bytes) -> str`, `parse_checkin(body: bytes) -> dict` (raises `ValueError`), `build_reply(key: bytes, nonce: str, lines: list[str]) -> bytes`.

- [ ] **Step 1: Write `vectors.json`** with 4 cases, each holding key hex, body or reply text, nonce and expected MAC: an `ok` reply, an `update` reply, a check-in with 0 `ipv6` lines, and one with 4. Compute the MACs with `hashlib`.
- [ ] **Step 2: Write the failing native tests** (vectors copied as C string constants, with a comment naming `vectors.json`):
  - `test_mac_matches_python_vectors`
  - `test_build_checkin_body_matches_vector` and `test_build_checkin_body_returns_zero_when_too_small`
  - `test_parse_reply_ok_with_valid_auth`
  - `test_parse_reply_update_fills_version_and_path`
  - `test_parse_reply_rejects_wrong_auth`, `test_parse_reply_rejects_missing_auth`, `test_parse_reply_rejects_other_nonce` (all `BadAuth`)
  - `test_parse_reply_rejects_truncated_at_every_length` (every prefix gives `Malformed` or `BadAuth`, never `Ok`/`Update`)
  - `test_parse_reply_rejects_update_without_version_or_path`, `test_parse_reply_rejects_relative_path`, `test_parse_reply_rejects_overlong_version`, `test_parse_reply_rejects_unknown_status`
  - `test_parse_key_hex_rejects_63_and_65_chars_and_non_hex`
- [ ] **Step 3: Run** `pio test -e native -f native/test_update_protocol`. Expected: FAIL (does not compile).
- [ ] **Step 4: Implement** `update_protocol.cpp`. The parser checks the `auth` line first: auth must be the last line, and the MAC is over `nonce + "\n" + preceding bytes`, compared in constant time. Only then does it read fields.
- [ ] **Step 5: Run** the native test. Expected: PASS.
- [ ] **Step 6: Write `test_protocol.py`** against the same vectors:
  - `test_mac_matches_vectors`
  - `test_parse_checkin_round_trips_vector`
  - `test_parse_checkin_rejects_missing_nonce`, `test_parse_checkin_rejects_lines_out_of_order`, `test_parse_checkin_rejects_body_over_1024_bytes`
  - `test_build_reply_matches_vector`

  Implement `protocol.py`, then run `python -m unittest discover -s update-server/tests -v`. Expected: PASS.
- [ ] **Step 7: Run** `wsl -d Ubuntu -- bash scripts/sanitize.sh`. Expected: clean.
- [ ] **Step 8: Commit** ("update_client: check-in wire format and keyed BLAKE2b MACs").

### Task 2: URL, HTTP response, `.upd` header version and scheduler (`lib/update_client`)

**Files:**
- Create: `lib/update_client/include/update_url.h`, `http_response.h`, `update_schedule.h` and their `src/*.cpp`
- Test: `test/native/test_update_client/test_main.cpp`

**Interfaces:**
- Produces:
  - `static const size_t UPDATE_URL_MAX = 127;`
  - `struct UpdateUrl { char host[64]; uint16_t port; char path[64]; bool hostIsIPv6; };`
  - `bool parseUpdateUrl(const char* text, UpdateUrl& out);`
  - `class HttpResponseParser { void reset(size_t maxBody); HttpParse feed(const uint8_t* d, size_t n, size_t& consumed); int status() const; uint32_t contentLength() const; };` It parses the status line and headers only; body bytes are left to the caller. It needs `Content-Length` and rejects chunked encoding. `HttpParse` is a new enum here: `{ Incomplete, HeadDone, Malformed }`.
  - `bool updHeaderVersion(const uint8_t* head, size_t len, char out[UPDATE_VERSION_MAX+1]);` (magic `0x50554652`, format 1, version at bytes 8..23, per `tools/fw_release.py build_upd`)
  - `int compareVersions(const char* a, const char* b);` (semver: numeric major.minor.patch; a pre-release ranks below its release)
  - `class CheckinScheduler { void netReady(bool ready, uint32_t nowMs); bool due(uint32_t nowMs) const; void checkinDone(bool ok, uint32_t nowMs); void reportSoon(uint32_t nowMs); void forceNow(uint32_t nowMs); };`, using the timing values from Global Constraints and handling `millis()` wrap-around.

- [ ] **Step 1: Write the failing tests:**
  - URL:
    - `test_url_accepts_name_port_path`, `test_url_accepts_ipv4_and_bracketed_ipv6`, `test_url_default_port_80`
    - `test_url_rejects_https`, `test_url_rejects_128_chars`, `test_url_rejects_port_0_and_65536`, `test_url_rejects_empty_host`, `test_url_rejects_userinfo`
  - HTTP response:
    - `test_http_response_reads_status_and_length`
    - `test_http_response_rejects_chunked`, `test_http_response_rejects_missing_length`, `test_http_response_rejects_head_over_1024_bytes`, `test_http_response_rejects_body_over_max`
    - `test_http_response_every_truncation_is_incomplete_or_malformed`
  - `.upd` header and versions:
    - `test_upd_header_version_from_golden_upd` (the bytes of `test/tools/golden_upd.py`'s header, pasted)
    - `test_upd_header_rejects_bad_magic_and_short`
    - `test_compare_versions_orders_3_10_0_after_3_9_0`, `test_compare_versions_prerelease_below_release`
  - Scheduler:
    - `test_scheduler_first_checkin_60s_after_net_ready`, `test_scheduler_next_after_30_min`, `test_scheduler_retry_after_5_min_on_failure`
    - `test_scheduler_report_soon_is_60s`, `test_scheduler_not_due_while_net_down`, `test_scheduler_survives_millis_wrap`
- [ ] **Step 2: Run** `pio test -e native -f native/test_update_client`. Expected: FAIL.
- [ ] **Step 3: Implement** the four units.
- [ ] **Step 4: Run** the native tests and `scripts/sanitize.sh`. Expected: PASS, clean.
- [ ] **Step 5: Commit.**

### Task 3: DHCP option 225 (text)

**Files:**
- Modify: `lib/dhcp_option/include/dhcp_option.h`, `lib/dhcp_option/src/dhcp_option.cpp`
- Test: `test/native/test_dhcp_option/test_main.cpp` (add cases; reuse its captured frames)

**Interfaces:**
- Produces:
  - `static const uint8_t DHCP_OPTION_UPDATE_URL = 225;`
  - `bool dhcpAckText(const uint8_t* frame, size_t len, uint8_t code, char* out, size_t outSize);` Call it only after `dhcpReadAck()` returned `Option` or `NoOption` for the same frame. It returns true for a printable ASCII option of length 1..`outSize-1`, NUL terminated.

- [ ] **Step 1: Write the failing tests:**
  - `test_ack_text_reads_url_option`, `test_ack_text_rejects_too_long`, `test_ack_text_rejects_non_printable`, `test_ack_text_rejects_zero_length`, `test_ack_text_absent`, `test_ack_text_truncated_frame`
  - `test_request_two_options_in_captured_discover`: on the captured device DISCOVER, add 224 then 225. Assert both appear in option 55, or that the second returns `NoRoom` with the frame unchanged. Either way the UDP checksum stays valid.
- [ ] **Step 2: Run** them. Expected: FAIL.
- [ ] **Step 3: Implement**, reusing the option walker that `dhcpReadAck` uses.
- [ ] **Step 4: Run** the native tests and sanitizers. Expected: PASS.
- [ ] **Step 5: Commit.**

### Task 4: Update settings (codec and A/B files)

**Files:**
- Create: `lib/update_client/include/update_settings.h`, `src/update_settings.cpp` (pure codec)
- Create: `lib/settings/include/update_settings_store.h`, `lib/settings/src/update_settings_store.cpp`
- Modify: `lib/settings/src/settings.cpp`: move the write, read-back and verify file helper into `lib/settings/src/ab_files.cpp` / `ab_files.h` (internal), used by both stores
- Modify: `src/factory_reset.cpp` (clear the update settings too)
- Test: `test/native/test_update_client/` (codec cases), `test/embedded/test_settings_store/` (store cases)

**Interfaces:**
- Produces:
  - `struct UpdateSettings { char url[UPDATE_URL_MAX+1]; bool fromDhcp; bool hasKey; uint8_t key[UPDATE_KEY_LEN]; uint32_t version; uint32_t sequence; };`
  - `void updateSettingsDefaults(UpdateSettings&);`
  - `size_t updateSettingsToJson(const UpdateSettings&, char* out, size_t size);`
  - `bool updateSettingsFromJson(const char* json, UpdateSettings& out);` (rejects a bad URL, a key that is not 64 hex, or a wrong version)
  - `class UpdateSettingsStore { UpdateSettingsStore(const char* a, const char* b); SettingsSlot load(UpdateSettings&); bool save(UpdateSettings&); };`
  - `extern UpdateSettings currentUpdateSettings; bool loadUpdateSettings(); bool saveUpdateSettings();` (paths `/update_a.json`, `/update_b.json`)

- [ ] **Step 1: Write the failing native tests:**
  - `test_update_settings_round_trip`, `test_update_settings_worst_case_fits_512`
  - `test_update_settings_rejects_bad_url`, `test_update_settings_rejects_short_key`, `test_update_settings_rejects_wrong_version`
  - `test_update_settings_every_truncation_rejected`
- [ ] **Step 2: Write the failing embedded tests** (paths under `/test/`):
  - `test_update_store_newest_copy_wins`, `test_update_store_corrupt_copy_falls_back`
  - `test_main_settings_file_unchanged_by_update_settings_save` (compare the bytes of the main settings file before and after)
  - `test_factory_reset_clears_update_settings` (the reset path's clear function on test paths)
- [ ] **Step 3: Run** `pio test -e native -f native/test_update_client` and `pio test -e pico -f embedded/test_settings_store`. Expected: FAIL.
- [ ] **Step 4: Implement** the codec, the shared helper, the store, the load in `setup()` and the factory reset clear.
- [ ] **Step 5: Run** the native, embedded and existing settings tests. Expected: PASS. Reflash with `pio run -e pico -t upload`.
- [ ] **Step 6: Commit.**

### Task 5: Non-blocking HTTP client on lwIP (`src/http_fetch.cpp`)

**Files:**
- Create: `src/http_fetch.h`, `src/http_fetch.cpp`
- Test: `test/hil/test_update_server.py::test_checkin_unreachable_server_keeps_snmp_answering` (written in Task 10; this task tests through the hidden menu command below)

**Interfaces:**
- Consumes: `UpdateUrl`, `HttpResponseParser` (Task 2).
- Produces:
  - `enum class FetchState : uint8_t { Idle, Resolving, Connecting, Sending, Head, Body, Done, Failed };`
  - `bool httpFetchStart(const UpdateUrl& url, const char* method, const char* path, const char* extraHeader, const uint8_t* body, size_t bodyLen, size_t maxBody);`
  - `FetchState httpFetchPoll(uint32_t nowMs);`
  - `int httpFetchStatus();`
  - `size_t httpFetchRead(uint8_t* buf, size_t max);` (body bytes received so far, consumed)
  - `const char* httpFetchError();` (e.g. "dns failed", "connect timeout", "connection closed")
  - `void httpFetchAbort();`

  One fetch at a time. Received pbufs are copied into a static 1460-byte ring in the `tcp_recv` callback. Flow control by `tcp_recved` only after the caller reads, so a slow flash write throttles the sender. All lwIP calls happen under the core's lwIP lock. DNS goes through `dns_gethostbyname` with a callback. IPv6 literals and AAAA work where lwIP resolves them.
- [ ] **Step 1: Add the hidden menu command `fetchtest <url>`** (like `stall0`). It GETs the URL and prints the status, byte count and error. It exists for this task's manual check and the HIL test.
- [ ] **Step 2: Implement** `http_fetch.cpp` with the timeouts from Global Constraints.
- [ ] **Step 3: Verify on the device:**
  - Run `python -m http.server 8072` in a scratch dir on the PC holding a 1 MB file.
  - `fetchtest http://10.132.0.45:8072/big.bin` prints `200 1048576`.
  - `fetchtest http://10.132.0.45:8099/` prints a connect failure within 10 s.
  - `fetchtest http://no-such-host.invalid/` prints `dns failed`.
  - Run `snmpget` in a loop throughout: every answer arrives within 100 ms.
- [ ] **Step 4: Commit.**

### Task 6: Update service (check-in, download, trial confirmation)

**Files:**
- Create: `src/update_service.h`, `src/update_service.cpp`
- Modify: `src/main.cpp` (`updateServiceBegin()` in setup after `loadUpdateSettings()`, `updateServicePoll(networkReady())` in `loop()`), `src/net_stats.cpp` and `.h` (request and read option 225 in the same accepted-ACK path as 224: `void netStatsRequestDhcpUpdateUrl(bool on); bool netStatsDhcpUpdateUrl(char* out, size_t size);`)
- Test: `test/native/test_update_client/` (download decision logic as a pure `UpdateJob` in `lib/update_client/include/update_job.h`)

**Interfaces:**
- Consumes: Tasks 1–5, `fwUpdateUploadBegin/Feed/Finish/Abort`, `fwUpdateNoteValidRequest()`, `fwUpdateSnmpState()`, `hostStatsSnapshot()` (serial), `hostnameCurrent()`.
- Produces:
  - Pure `UpdateJob`:
    - `bool begin(const char* wantedVersion, const char* runningVersion);` (false when equal: nothing to do)
    - `UpdateJob::Step head(const uint8_t* first64);` (`Continue`, or `Refuse` with reason `"version mismatch"` when `updHeaderVersion` differs from the wanted one)
    - `void failDownload(const char* why);`, `void refused(const char* why);`
    - `const char* failLine() const;` (`"<v> refused <reason>"` / `"<v> download <reason>"`, cleared after a check-in reports it)
  - Service: `void updateServiceBegin(); void updateServicePoll(bool netReady); void updateServiceForceCheckin();`
  - `struct UpdateServiceStatus { char urlInUse[UPDATE_URL_MAX+1]; bool fromDhcp; bool hasKey; bool downloading; uint32_t lastCheckinAgeS; char lastResult[48]; };` (`lastResult` e.g. "ok", "update 3.4.1", "badauth", "connect timeout", "no key", "off")
  - `void updateServiceStatus(UpdateServiceStatus& out);`
- Behaviour:
  - **Effective URL:** the DHCP value when `fromDhcp` is on and one was received, else the stored URL.
  - **Authenticated replies:** any authenticated reply calls `fwUpdateNoteValidRequest()`; while on trial it also calls `reportSoon()`.
  - **`Update` reply:** GET the path and feed the body through `fwUpdateUploadBegin/Feed/Finish`, checking `UpdateJob::head` on the first 64 bytes. On a lost connection or timeout, call `fwUpdateUploadAbort()`, set `failDownload`, then `reportSoon`. On a `Finish` refusal, set `refused(error)`, then `reportSoon`. On success, restart as the web upload does.
  - **While on trial:** the upload is refused by the update service (`Busy`). The job reports that as `refused on trial`.
- [ ] **Step 1: Write the failing `UpdateJob` tests:**
  - `test_job_same_version_does_nothing`
  - `test_download_version_mismatch_is_refused`
  - `test_job_fail_line_formats_download_and_refused`, `test_job_fail_line_cleared_after_report`
- [ ] **Step 2: Run** them. Expected: FAIL. **Implement** `UpdateJob`. **Run** them. Expected: PASS.
- [ ] **Step 3: Implement** `update_service.cpp` and the `net_stats` option 225 hooks. Add the hidden menu command `checkin` (calls `updateServiceForceCheckin()`, for the HIL test only; the site still cannot reach the device).
- [ ] **Step 4: Build** with `pio run -e pico`. Expected: success; flash and RAM below 70 % / 50 % (print the figures).
- [ ] **Step 5: Commit.**

### Task 7: Serial menu, status screen and web page

**Files:**
- Modify: `src/cli.cpp` (new menu option "Update server": URL or `dhcp`, key entry (64 hex, empty = keep, `clear` = remove), shown as set/not set; status screen line `Update server: <url> (fixed|dhcp|off), key set|not set, last check-in <n> min ago: <lastResult>`)
- Modify: `lib/web_ui/include/web_app.h`, `lib/web_ui/src/web_app.cpp` (`POST /updateserver` with `url`, `fromdhcp`, `key` (empty = keep), `clearkey`; a status section; the backend gets `virtual bool saveUpdateServer(const char* url, bool fromDhcp, const char* keyHex, bool clearKey, const char*& error) = 0;` and `WebStatus` gains the `UpdateServiceStatus` text fields), `src/web_service.cpp`
- Test: `test/native/test_web_ui/` and `test/hil/test_serial_menu.py`

- [ ] **Step 1: Write the failing web_ui tests:**
  - `test_update_server_form_needs_login_and_csrf`
  - `test_update_server_post_saves_url_and_key`, `test_update_server_post_empty_key_keeps_key`, `test_update_server_post_rejects_bad_url`
  - `test_update_server_page_never_shows_key`
- [ ] **Step 2: Write the failing HIL test** `test_update_server_menu_sets_url_and_key_and_restores`: set the URL and a test key, check the status line, then restore the empty URL and clear the key.
- [ ] **Step 3: Run** both. Expected: FAIL. **Implement** both. **Run** both. Expected: PASS.
- [ ] **Step 4: Commit.**

### Task 8: Site core: store, check-in API, assignment rules

**Files:**
- Create: `update-server/rfupdate/store.py` (SQLite tables `devices`, `releases`, `assignments`, `nonces`, `settings` (site key)), `update-server/rfupdate/upd.py` (header parse, `compare_versions`), `update-server/rfupdate/api.py` (check-in handler: verify MAC with the site key, replay check, update the device row, decide the reply)
- Test: `update-server/tests/test_store.py`, `update-server/tests/test_api.py`

**Interfaces:**
- Produces:
  - `Store(path: str)` with:
    - `upsert_checkin(fields: dict, now: float)`
    - `add_release(version: str, data: bytes) -> None` (raises `ValueError` for duplicate, older than `MIN_VERSION = "3.4.0"`, not a `.upd`, or too large)
    - `assign(serials: list[str], version: str, now: float)`
    - `device_rows(now: float) -> list[dict]` (with `online` and `update_status`)
    - `site_key() -> bytes | None`, `new_site_key() -> bytes`
  - `handle_checkin(store, body: bytes, auth_header: str, now: float) -> tuple[int, bytes]`
- Assignment status from check-ins: `waiting`, `told` (reply sent), `downloading`, `on trial` (runs it with `trial=1`), `done` (runs it with `trial=0`), `rolled back` (`last=rolledback-* <v>`), `refused` (`fail=<v> refused ...`), `failed` (third `fail=<v> download ...`), `lost` (2 h after `told`).
- [ ] **Step 1: Write the failing tests:**
  - Check-in:
    - `test_checkin_with_valid_mac_adds_device`, `test_checkin_same_serial_updates_row_not_new`
    - `test_checkin_bad_mac_is_401`, `test_checkin_replayed_nonce_is_409`, `test_checkin_without_site_key_is_401`, `test_checkin_malformed_is_400`
  - Device list and assignment:
    - `test_device_offline_after_65_min`
    - `test_unassigned_device_gets_ok`, `test_assigned_device_gets_update_with_path`
    - `test_assignment_of_running_version_is_done_at_once`
    - `test_trial_then_kept_marks_done`, `test_rollback_marks_rolled_back_and_stops_sending`, `test_refused_ends_assignment`
    - `test_site_ends_assignment_after_three_download_failures`, `test_assignment_lost_after_2_hours`
  - Releases:
    - `test_upload_older_than_min_version_is_refused`, `test_duplicate_version_is_refused`, `test_upload_non_upd_is_refused`
    - `test_release_with_assignment_cannot_be_deleted`
- [ ] **Step 2: Run** `python -m unittest discover -s update-server/tests -v`. Expected: FAIL.
- [ ] **Step 3: Implement** the three modules.
- [ ] **Step 4: Run** the tests. Expected: PASS.
- [ ] **Step 5: Commit.**

### Task 9: Site web pages, login and Docker

**Files:**
- Create: `update-server/rfupdate/web.py` (`ThreadingHTTPServer`)
  - routes: `GET /` (device list, no login), `GET /login`, `POST /login`, `POST /logout`, `GET /admin` (releases, upload form, device checkboxes plus release select, site key page), `POST /admin/upload` (multipart via `email`), `POST /admin/assign`, `POST /admin/release/delete`, `POST /admin/key/new`, `GET /releases/<version>.upd`, `POST /api/checkin`;
  - session cookie (`secrets.token_urlsafe`, HttpOnly, SameSite=Strict), CSRF token in forms, password compared with `hmac.compare_digest`, 5 failed logins per minute per IP;
  - the new key is shown once after generation and as "set" afterwards.
- Create: `update-server/rfupdate/__main__.py` (`python -m rfupdate --port 8071 --data DIR`)
- Create: `update-server/Dockerfile` (`python:3.12-slim`, non-root user), `update-server/compose.yml` (port `8071:8071`, volume `rfupdate-data:/data`, `restart: unless-stopped`, `RFUPDATE_ADMIN_PASSWORD` from `compose.override.yml`), `update-server/.gitignore` (`compose.override.yml`), `update-server/README.md`
- Modify: `.github/workflows/tests.yml` (run `python -m unittest discover -s update-server/tests`)
- Test: `update-server/tests/test_web.py`

- [ ] **Step 1: Write the failing tests:**
  - Access:
    - `test_device_list_needs_no_login`
    - `test_admin_needs_login`, `test_upload_needs_login_and_csrf`, `test_assign_needs_login_and_csrf`
    - `test_login_rate_limited_after_5_failures`, `test_no_login_without_admin_password_env`
  - Upload and download:
    - `test_upload_then_assign_then_checkin_gets_update`
    - `test_release_download_serves_exact_bytes`, `test_upload_over_max_is_413`
  - Key: `test_key_page_shows_new_key_once`
- [ ] **Step 2: Run** them. Expected: FAIL. **Implement** `web.py`. **Run** them. Expected: PASS.
- [ ] **Step 3: Verify the container:**
  - `docker compose up -d --build` in `update-server/` (Docker Desktop must be running).
  - http://localhost:8071 shows an empty device list.
  - `docker compose restart`, then check that the data volume survives.
- [ ] **Step 4: Commit.**

### Task 10: End to end, HIL, release plumbing and docs

**Files:**
- Modify: `include/version.h` (`3.4.0`, with `#ifndef FIRMWARE_VERSION` around the define so test builds can override it), `tools/fw_release.py` (`--test-version X`: development key only, adds `-DFIRMWARE_VERSION=\"X\"`, writes `.pio/fwtest/<X>.upd`), `test/tools/test_fw_tools.py` (`test_test_version_refused_with_field`)
- Create: `test/hil/test_update_server.py` (opt-in `RFPOWER_UPDATE_SERVER_TEST=1`):
  - runs `python -m rfupdate` from `update-server/` on port 8072 with a temp data dir and a generated key;
  - finds the PC's address toward the device (or uses `RFPOWER_UPDATE_SERVER_HOST`);
  - restores the device's update settings and its 3.4.0 firmware in `tearDownClass`.
- Modify: `CHANGELOG.md`, `TODO.md`, `docs/USER_MANUAL.md` (menu, web page, how updates from the site work), `docs/SETUP_MANUAL.md` (Docker site, firewall rule TCP 8071, DHCP option 225 for Kea/ISC/dnsmasq), `docs/TESTING.md`, `CLAUDE.md` (Layout: `lib/update_client`, `update-server/`; Commands: site tests and the HIL)

- [ ] **Step 1: Write the HIL tests:**
  - `test_checkin_appears_on_site`: set the URL and key, run `checkin`, and the row appears with serial, hostname, IPv4 and version.
  - `test_wrong_key_shows_badauth`
  - `test_checkin_unreachable_server_keeps_snmp_answering`: point the URL at a closed port, run `checkin`, and 20 `snmpget`s each answer within 100 ms.
  - `test_corrupt_release_is_refused_and_device_keeps_running`: a copy of the `3.4.1-hil` `.upd` with one signature byte flipped, uploaded as version `3.4.2-hil`. The site shows `refused` and the device still runs 3.4.0.
  - `test_assigned_release_installs_and_is_kept`: build `fw_release.py --test-version 3.4.1-hil`, upload, assign, `checkin`. Wait for the restart, the trial check-in at 60 s and the report check-in. The site shows `done`, and the device runs `3.4.1-hil` from the other slot, kept.
  - `test_downgrade_back_to_release`: assign `3.4.0` and `checkin`. The device is back on 3.4.0, kept. This doubles as the restore.
- [ ] **Step 2: Run** `RFPOWER_UPDATE_SERVER_TEST=1 python.exe -m unittest test/hil/test_update_server.py -v`. Expected: PASS.
- [ ] **Step 3: Run the full suites:**
  - `pio test -e native` and `scripts/sanitize.sh`;
  - `python -m unittest discover -s test/tools`, `update-server/tests`, and the HIL suite (`test/hil`).

  Expected: all PASS. Reflash with `pio run -e pico -t upload` after any `pio test -e pico`.
- [ ] **Step 4: Update the docs, CHANGELOG and TODO.**
- [ ] **Step 5: Commit.**
