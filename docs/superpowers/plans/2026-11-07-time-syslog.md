# Time, Syslog and Update-Site SNMP Objects Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The device keeps real time from NTP, sends its events to a syslog server, and reports its update-site state over SNMP (3.6.0).

**Architecture:** lwIP's built-in SNTP client (already compiled by the core) gets its servers from DHCP option 42, DHCPv6 option 56 (lwIP's stateless DHCPv6) or a typed address/name. Pure libraries do the logic (`lib/clock`: civil and Danish local time, DateAndTime; `lib/syslog`: settings codec, RFC 5424 messages, the event queue and throttle); thin services in `src/` (`time_service`, `syslog_service`) wire them to lwIP. The two server settings live in their own A/B files so 3.5.1 still loads its settings after a rollback.

**Tech Stack:** C++17, Arduino-Pico 5.4.3, lwIP 2.2.1 (SNTP, DHCP, raw UDP), ArduinoJson 6, Unity (native/embedded), Python unittest (HIL), LibreNMS YAML.

**Spec:** `docs/superpowers/specs/2026-11-07-time-syslog-design.md`

## Global Constraints

- Static RAM at most 60 % of 262,144 bytes (`.clinerules`; 49.6 % today). Report the build's `RAM:` line in the release task.
- No buffer or struct over ~256 bytes on core 0's stack: static or heap.
- SNMP answers within 100 ms; the firmware's share of mains detection stays within 10 ms. Nothing blocks core 0.
- Main settings file (`data/settings_*.json`, `SETTINGS_JSON_MAX` 1024, 20 JSON fields) is NOT changed: new settings go in their own A/B files (rollback to 3.5.1 must load its settings).
- Factory reset sets NTP and syslog to `dhcp`.
- Never in a syslog message: passwords, keys, the SNMP community.
- Settings values: NTP = IPv4 / IPv6 address / DNS name / `dhcp` / `none`; syslog = IPv4 / IPv6 address with optional port (`host`, `host:port`, `[v6]`, `[v6]:port`, default 514) / `dhcp` / `none`. A typed value wins over DHCP.
- Displayed time: Danish local time, `YYYY-MM-DD hh:mm:ss CET|CEST` (EU rule: CEST from the last Sunday of March 01:00 UTC to the last Sunday of October 01:00 UTC). Before the first NTP answer: uptime, as today.
- Syslog: RFC 5424 over UDP, facility daemon (3), APP-NAME `rfpower`, HOSTNAME = device hostname, timestamp UTC `YYYY-MM-DDThh:mm:ss.mmmZ`, `-` when undated.
- NTP poll every 15 minutes (`SNTP_UPDATE_DELAY=900000`).
- Native code: `-Wall -Wextra -Werror`; parsers get truncated/corrupt input tests; all native suites pass `scripts/sanitize.sh`.
- Each test named `test_<what>_<expected behaviour>`; every bug-fix test fails first.
- HIL tests needing a network resource are opt-in by environment variable.
- Never `docker compose down -v` in `librenms/test`.

## Review Focus

1. A DHCP server that offers several NTP or log-server addresses (option length 8, 12): the first is used, not "no option" — test in Task 4.
2. NTP name that does not resolve (typo): the device keeps running on uptime and the status line says not synced — test in Task 5 (HIL).
3. Clock jump at the first sync while events are queued: events queued before sync are dated from their uptime, not stamped with the sync moment — test in Task 3.
4. A burst of bad-community SNMP requests: at most 5 syslog lines a minute plus one "n suppressed" line — test in Task 3.
5. The DST switch hour (last Sunday of March/October around 01:00 UTC): local time is right on both sides — test in Task 1.

---

### Task 1: `lib/clock` — civil time, Danish local time, DateAndTime

**Files:**
- Create: `lib/clock/library.json` (modelled on `lib/snmp_agent/library.json`), `lib/clock/include/clock.h`, `lib/clock/src/clock.cpp`
- Test: `test/native/test_clock/test_main.cpp`

**Interfaces:**
- Produces:
  - `struct CivilTime { uint16_t year; uint8_t month, day, hour, minute, second; };`
  - `CivilTime civilFromEpoch(uint64_t epochSeconds);`
  - `bool euSummerTime(uint64_t epochSeconds);` — true from last Sunday of March 01:00 UTC to last Sunday of October 01:00 UTC
  - `CivilTime danishLocal(uint64_t epochSeconds, bool& summer);` — UTC+1, +2 in summer
  - `size_t formatLocalTime(uint64_t epochSeconds, char* out, size_t size);` — `2026-11-07 19:42:10 CET`; returns length, 0 if it does not fit
  - `size_t formatUtcRfc5424(uint64_t epochMs, char* out, size_t size);` — `2026-11-07T18:42:10.123Z`
  - `void dateAndTime(uint64_t epochSeconds, uint8_t out[11]);` — SNMPv2-TC DateAndTime, local time with direction `+`, hours 1 or 2, minutes 0
  - `static const uint64_t CLOCK_VALID_AFTER = 1767225600;` (2026-01-01; anything earlier is "not synced", the core's clock starts near 1970)

- [ ] **Step 1: Write the failing tests** — `test_civil_from_epoch_known_dates` (0 → 1970-01-01 00:00:00; 951782400 → 2000-02-29; 1772236799 → 2026-02-27 23:59:59; 4102444800 → 2100-01-01), `test_summer_time_starts_last_sunday_march_0100_utc` (2026-03-29 00:59:59 UTC false, 01:00:00 true), `test_summer_time_ends_last_sunday_october_0100_utc` (2026-10-25 00:59:59 true, 01:00:00 false), `test_format_local_time_winter_and_summer` (1762540930 → `2025-11-07 19:42:10 CET`; a July date → `CEST`, +2 h), `test_format_local_time_too_small_buffer_returns_zero`, `test_format_utc_rfc5424_has_millis_and_z`, `test_date_and_time_bytes` (year big-endian 2 bytes, month, day, hour, minute, second, deci 0, '+', 1 or 2, 0).
- [ ] **Step 2:** `pio test -e native -f "*test_clock*"` — expect build failure (no library).
- [ ] **Step 3:** Implement `clock.cpp` (days-from-civil algorithm; no `time.h` dependency so it is identical on host and Pico).
- [ ] **Step 4:** Run the suite — PASS.
- [ ] **Step 5:** Commit `lib/clock test/native/test_clock` — "Clock: civil and Danish local time, DateAndTime".

### Task 2: Server settings — codec, address parsing, A/B store, factory reset

**Files:**
- Create: `lib/syslog/library.json`, `lib/syslog/include/log_settings.h`, `lib/syslog/src/log_settings.cpp`
- Create: `lib/settings/include/log_settings_store.h`, `lib/settings/src/log_settings_store.cpp` (copy the pattern of `update_settings_store.h/.cpp` and `ab_files.h`; files `data/timelog_a.json`, `data/timelog_b.json`)
- Modify: `src/factory_reset.cpp:73-76` (reset these too), `src/main.cpp` setup (load before Ethernet, like update settings)
- Test: `test/native/test_syslog/test_main.cpp`, `test/embedded/test_settings_store/test_main.cpp` (add)

**Interfaces:**
- Produces:
  - `enum class ServerMode : uint8_t { Dhcp, Typed, None };`
  - `static const size_t LOG_HOST_MAX = 63;`
  - `struct LogSettings { ServerMode ntpMode; char ntpServer[LOG_HOST_MAX + 1]; ServerMode syslogMode; char syslogServer[LOG_HOST_MAX + 1]; uint32_t version, sequence; };` (`LOG_SETTINGS_VERSION = 1`, `LOG_SETTINGS_JSON_MAX = 384`)
  - `void logSettingsDefaults(LogSettings&);` — both `Dhcp`, servers empty
  - `size_t logSettingsToJson(const LogSettings&, char*, size_t);` / `bool logSettingsFromJson(const char*, LogSettings&);` (every field required; a bad value rejects the file)
  - `struct SyslogTarget { bool ipv6; uint8_t addr[16]; uint16_t port; };`
  - `bool parseSyslogTarget(const char* text, SyslogTarget& out);` — `a.b.c.d`, `a.b.c.d:port`, `v6` (bare, no port), `[v6]`, `[v6]:port`; port 1-65535, default 514; uses `parseIPv4`/`parseIPv6` (`lib/netconfig`)
  - `bool ntpServerTextIsValid(const char*);` — IPv4, IPv6, or a DNS name (labels 1-63 of `[A-Za-z0-9-]`, not starting/ending with `-`, total ≤ 63)
  - `enum class ServerInput { Keep, Dhcp, None, Value, Invalid };` and `ServerInput classifyServerInput(const char* line);` — empty → Keep, `dhcp`/`DHCP` → Dhcp, `none`/`-` → None, else Value
  - Store globals: `extern LogSettings currentLogSettings; bool loadLogSettings(); bool saveLogSettings(); void factoryResetLogSettings();`
- [ ] **Step 1: Write failing native tests** — `test_log_settings_defaults_are_dhcp`, `test_log_settings_json_round_trip`, `test_log_settings_rejects_missing_field`, `test_log_settings_rejects_bad_mode`, `test_log_settings_rejects_truncated_json` (every prefix of a valid document), `test_log_settings_rejects_overlong_server`, `test_parse_syslog_target_forms` (each form above, ports 1 and 65535), `test_parse_syslog_target_rejects_bad_input` (`""`, port 0, 65536, `[::1`, `1.2.3.4:`, `host.name` — names are not allowed for syslog), `test_ntp_server_text_accepts_names_and_addresses` (`pool.ntp.org`, `10.0.0.1`, `2001:db8::1`), `test_ntp_server_text_rejects_bad_names` (`-a.b`, `a..b`, 64 chars, `a b`), `test_classify_server_input`.
- [ ] **Step 2:** Run `pio test -e native -f "*test_syslog*"` — fails to build.
- [ ] **Step 3:** Implement `log_settings.cpp`.
- [ ] **Step 4:** Native suite PASS.
- [ ] **Step 5: Embedded tests** in `test_settings_store`: `test_log_settings_save_load_round_trip` (paths under `test/`), `test_log_settings_corrupt_copy_uses_other`, `test_factory_reset_sets_log_settings_to_dhcp`, `test_log_settings_leave_main_settings_file_unchanged` (main file bytes identical before/after a log-settings save). Implement the store; `pio test -e pico -f embedded/test_settings_store` PASS; reflash `pio run -e pico -t upload`.
- [ ] **Step 6:** Commit — "Settings: NTP and syslog servers in their own A/B files".

### Task 3: Syslog messages, event queue and throttle (`lib/syslog`)

**Files:**
- Create: `lib/syslog/include/syslog_message.h`, `lib/syslog/src/syslog_message.cpp`, `lib/syslog/include/syslog_queue.h`, `lib/syslog/src/syslog_queue.cpp`
- Test: `test/native/test_syslog/test_main.cpp` (add)

**Interfaces:**
- Consumes: `formatUtcRfc5424` (Task 1)
- Produces:
  - `enum class SyslogSeverity : uint8_t { Critical = 2, Error = 3, Warning = 4, Notice = 5, Info = 6 };`
  - `static const uint8_t SYSLOG_FACILITY_DAEMON = 3; static const size_t SYSLOG_TEXT_MAX = 100; static const size_t SYSLOG_PACKET_MAX = 256;`
  - `size_t syslogFormat(char* out, size_t size, SyslogSeverity sev, bool dated, uint64_t epochMs, const char* hostname, const char* msgId, const char* text);` — `<PRI>1 TIMESTAMP HOST rfpower - MSGID - TEXT`; `-` for an empty hostname or undated; text cut at `SYSLOG_TEXT_MAX`, control characters replaced by `?`
  - `struct SyslogEntry { uint32_t uptimeMs; SyslogSeverity severity; char msgId[9]; char text[SYSLOG_TEXT_MAX + 1]; };`
  - `class SyslogQueue { public: static const uint8_t CAPACITY = 16; bool push(const SyslogEntry&); /* drops the oldest when full, counts it */ bool peek(SyslogEntry&) const; void pop(); uint8_t size() const; uint32_t takeDropped(); /* returns and clears the drop count */ };`
  - `class SyslogThrottle { public: SyslogThrottle(uint8_t perWindow, uint32_t windowMs); bool allow(uint32_t nowMs); uint32_t takeSuppressed(); };` — the SNMP access events use `SyslogThrottle(5, 60000)`
  - `uint64_t syslogEventEpochMs(uint32_t eventUptimeMs, uint32_t nowUptimeMs, uint64_t nowEpochMs);` — `nowEpochMs - (nowUptimeMs - eventUptimeMs)` (wrap-safe)
- [ ] **Step 1: Failing tests** — `test_syslog_format_rfc5424_layout` (exact string for severity Warning: `<28>1 2026-11-07T18:42:10.123Z rf-power-abcd rfpower - MAINS - mains power lost`), `test_syslog_format_undated_uses_dash`, `test_syslog_format_cuts_long_text`, `test_syslog_format_replaces_control_chars`, `test_syslog_format_too_small_buffer_returns_zero`, `test_queue_keeps_order`, `test_queue_full_drops_oldest_and_counts`, `test_throttle_allows_five_per_minute_then_counts` (6th in the window suppressed, `takeSuppressed()` = 1, window rolls over), `test_event_epoch_dated_from_uptime_not_sync_moment` (event at uptime 5,000 ms, sync at uptime 65,000 ms to epoch E → E − 60,000), `test_event_epoch_survives_millis_wrap`.
- [ ] **Step 2:** Run — FAIL. **Step 3:** Implement. **Step 4:** Run — PASS; also `wsl -d Ubuntu -- bash scripts/sanitize.sh` PASS.
- [ ] **Step 5:** Commit — "Syslog: RFC 5424 messages, event queue and throttle".

### Task 4: DHCP option 7 (log server)

**Files:**
- Modify: `lib/dhcp_option/include/dhcp_option.h`, `lib/dhcp_option/src/dhcp_option.cpp`, `src/net_stats.cpp`, `src/net_stats.h`
- Test: `test/native/test_dhcp_option/test_main.cpp`

**Interfaces:**
- Produces:
  - `static const uint8_t DHCP_OPTION_LOG_SERVER = 7;`
  - `DhcpAck dhcpReadAckFirstAddress(const uint8_t* frame, size_t len, const uint8_t mac[6], uint8_t code, DhcpAckInfo& info);` — like `dhcpReadAck`, but the option may hold 4·n bytes (n ≥ 1); `info.value` = first address; length 0 or not a multiple of 4 → `NoOption`
  - `void netStatsRequestDhcpLogServer(bool on); bool netStatsDhcpLogServer(uint8_t out[4]);` — same accepted-ACK rule and "every accepted ACK replaces or clears" as option 224 (`net_stats.cpp:57-87`)
- [ ] **Step 1: Failing tests** — `test_read_first_address_single`, `test_read_first_address_of_three` (option length 12), `test_read_first_address_rejects_length_5`, `test_read_first_address_truncated_frame_is_not_reply` (every truncation of a captured ACK), `test_request_option_7_added_once`.
- [ ] **Step 2–4:** Run FAIL → implement (reuse `walkOptions`) → PASS, sanitizers PASS.
- [ ] **Step 5:** Wire in `net_stats.cpp` (request when syslog mode is `Dhcp`; read before `originalInput`). Build `pio run -e pico`.
- [ ] **Step 6:** Commit — "DHCP: log server (option 7)".

### Task 5: Time service (SNTP)

**Files:**
- Create: `src/time_service.h`, `src/time_service.cpp`
- Modify: `include/lwipopts.h` (after `#include_next`: `#undef SNTP_SET_SYSTEM_TIME_US` / `#define SNTP_SET_SYSTEM_TIME_US(sec, us) timeServiceSetTime(sec, us)` with an `extern "C"` declaration), `platformio.ini` `[env:pico]` build_flags (`-D SNTP_UPDATE_DELAY=900000`, `-D LWIP_DHCP_GET_NTP_SRV=1`, `-D LWIP_DHCP6_GET_NTP_SRV=1`, `MEMP_NUM_SYS_TIMEOUT` +14 instead of +12), `src/main.cpp`, `src/cli.cpp` (status line), `lib/snmp_agent` (hrSystemDate)
- Test: `test/native/test_snmp_agent/test_main.cpp`, `test/hil/test_time_syslog.py` (new)

**Interfaces:**
- Consumes: `currentLogSettings` (Task 2), `formatLocalTime`, `dateAndTime`, `CLOCK_VALID_AFTER` (Task 1)
- Produces:
  - `extern "C" void timeServiceSetTime(uint32_t sec, uint32_t us);` — calls the core's `__setSystemTime`, records the sync uptime
  - `void timeServiceBegin(); void timeServiceApply();` — apply settings: `Dhcp` → `sntp_servermode_dhcp(1)`; `Typed` → `sntp_servermode_dhcp(0)` + `sntp_setservername(0, static copy)`; `None` → `sntp_stop()`. Called at boot and after a save
  - `bool timeServiceSynced(); uint64_t timeServiceEpochMs(); uint32_t timeServiceLastSyncAgeS();`
  - `void timeServiceStatusText(char* out, size_t size);` — e.g. `2026-11-07 19:42:10 CET (NTP pool.ntp.org, synced 3 min ago)` or `not synced (NTP from DHCP, none offered)`
  - `SnmpDeviceState` gains `bool clockValid; uint8_t dateAndTime[11];` filled in `captureState()`; MIB entry `{HR(1, 2), getHrSystemDate}` served only when `clockValid` (otherwise skipped by GETNEXT, `noSuchInstance` on GET as today); update the comment at `snmp_mib.cpp:665` and `snmp_mib.h` object list
  - `LwipIntfDev` calls `sntp_stop(); sntp_init();` on every status change: `timeServiceApply()` must leave lwIP in a state where that restart keeps the chosen mode (set mode before `sntp_init`; for `None`, also call `sntp_stop()` after network-up in `loop`)
- [ ] **Step 1: Failing native tests** — `test_hr_system_date_served_when_clock_valid` (11 bytes, value from the state), `test_hr_system_date_absent_without_clock` (GET → noSuchInstance, walk skips it).
- [ ] **Step 2–4:** FAIL → implement MIB part → PASS.
- [ ] **Step 5:** Implement the service, lwIP flags, status line `Time: ...` in `printStatus()`. Build; check `RAM:` line.
- [ ] **Step 6: HIL** `test_time_syslog.py` `TimeTest` (opt-in `RFPOWER_NTP_SERVER`, e.g. `pool.ntp.org` or the router): `test_typed_ntp_server_syncs_within_60_s` (status `Time:` line shows the PC's local date ± 2 s), `test_hr_system_date_matches_pc_clock` (snmpget, ± 2 s), `test_unresolvable_ntp_name_keeps_running_unsynced` (`no-such-host.invalid` → `not synced`, SNMP still answers), restores the setting it found. Flash, run, PASS.
- [ ] **Step 7:** Commit — "Time: SNTP from DHCP or a typed server, hrSystemDate".

### Task 6: Syslog service and event hooks

**Files:**
- Create: `src/syslog_service.h`, `src/syslog_service.cpp`
- Modify (one call each at the points listed): `src/main.cpp:282-306` (DHCP address up/changed/lost; mains lost/restored), `src/snmp_service.cpp:244` (mains alarm), `src/snmp_service.cpp:109-117` `logAccess` (SNMP access events, only when `record()` returned true, through the throttle), `src/main.cpp` setup after `fwUpdatePrintBoot()` (boot: reset kind, boot reason, version), `src/fw_update_service.cpp` (trial start `:271`, kept `endTrial(Confirmed)`, rollback/fallback `recordFallback()` and the unreachable path), `src/web_service.cpp:258-270` (`LoggedIn`, `LoginFailed`, `LoginLocked`), web saves (`saveNetwork`, `saveIPv6`, `saveCommunity` — text says "community changed", never the value —, `saveDeviceName`, `disableWeb`, `saveUpdateServer`, `webSetPassword` — "web password changed" only), `src/cli.cpp` saves (`CLI_IPV6_CONFIRM`, `CLI_CONFIRM`, `CLI_SNMP_CONFIRM`, option 10, option 6, option 8), `src/net_stats.cpp:171-177` (link up/down), `src/lldp_service.cpp` `lldpPoll` (neighbour changed: compare chassis ID + port ID with the last seen), `src/update_service.cpp` (check-in failing: first failure after a success; OK again: first success after failures)
- Modify: `lib/lldp` — add `bool lldpSameNeighbour(const LldpNeighbour&, const LldpNeighbour&);` (chassis ID and port ID)
- Test: `test/native/test_lldp/test_main.cpp`, `test/hil/test_time_syslog.py`

**Interfaces:**
- Consumes: Tasks 1–5
- Produces:
  - `void syslogEvent(SyslogSeverity sev, const char* msgId, const char* fmt, ...);` — builds the text, pushes a `SyslogEntry` with `millis()`; safe to call before the network is up; only from core 0 (core 1 events reach core 0 through the existing mains flags)
  - `void syslogAccessEvent(const SourceKey& src, AccessEvent ev);` — throttled with `SyslogThrottle(5, 60000)`, adds `"n access events suppressed"` when the window allowed again
  - `void syslogServiceApply(); void syslogServicePoll(bool netReady);` — resolves the target (typed via `parseSyslogTarget`, or DHCP option 7 on port 514, or none); sends from the queue when there is a target and (`timeServiceSynced()` or 2 minutes since the target became known); one packet per poll; dated entries use `syslogEventEpochMs`; dropped/suppressed counts become an extra Notice line
  - `void syslogServiceStatusText(char* out, size_t size);` — e.g. `10.132.0.45:514 (from DHCP), 12 sent` or `off`
  - Sends with lwIP raw UDP (`udp_new` once, `udp_sendto`), under the same locking the trap sender uses in `src/snmp_service.cpp`
  - MSGIDs: `MAINS`, `BOOT`, `FIRMWARE`, `WEB`, `SETTINGS`, `SNMP`, `NETWORK`, `LLDP`, `UPDATE`. Severities as the spec: mains lost Warning, restored Notice, alarm Critical, rollback Error, others Notice/Info
- [ ] **Step 1: Failing native test** — `test_same_neighbour_compares_chassis_and_port`.
- [ ] **Step 2–4:** FAIL → implement → PASS.
- [ ] **Step 5:** Implement the service and the hooks; status line `Syslog: ...`. Build; check RAM.
- [ ] **Step 6: HIL** `SyslogTest` (opt-in `RFPOWER_SYSLOG_TEST=1`): the test opens a UDP listener on the PC (port from `RFPOWER_SYSLOG_PORT`, default 5514), sets the syslog server to `<pc>:<port>` through option 11 (Task 7 helper) and checks: `test_settings_change_is_logged_without_secrets` (change sysLocation → a `SETTINGS` line; the community string never appears), `test_bad_community_burst_is_throttled` (50 bad-community GETs in 10 s → at most 5 `SNMP` lines, then a suppressed count), `test_reboot_sends_boot_line_dated` (menu reboot → `BOOT` line with a timestamp within ± 5 s of the PC clock when NTP is set), `test_mains_loss_is_logged` (opt-in mains switch like `test_snmp.py`), `test_rfc5424_header_is_valid` (regex on each line). Restores settings.
- [ ] **Step 7:** Commit — "Syslog: events to a syslog server".

### Task 7: Serial menu option 11, status lines and access log times

**Files:**
- Modify: `src/cli.cpp` (`CliState` gains `CLI_NTP_SERVER`, `CLI_SYSLOG_SERVER`; `printMenu()` adds `11. Time and syslog`; option 11 prompts `NTP server: address, name, dhcp or none [%s]: ` then `Syslog server: address[:port], dhcp or none [%s]: `, saves with `saveLogSettings()`, then `timeServiceApply(); syslogServiceApply(); netStatsRequestDhcpLogServer(...)`; `printAccessLog()` shows local time when synced: `  %-15s %-14s x%-6lu last 2026-11-07 19:42:10 CET` — keep `x(\d+)\s+last` for `device.py:359`), `test/hil/device.py` (`set_time_syslog(ntp, syslog)`, status keys `time`, `syslog`)
- Test: `test/hil/test_serial_menu.py` (add), `test/hil/test_time_syslog.py`

**Interfaces:**
- Consumes: `classifyServerInput`, `ntpServerTextIsValid`, `parseSyslogTarget` (Task 2); `timeServiceStatusText`, `syslogServiceStatusText` (Tasks 5, 6)
- Produces: `Device.set_time_syslog(ntp: str, syslog: str)` (`""` keeps), `status()["time"]`, `status()["syslog"]`
- [ ] **Step 1: Failing HIL tests** — `test_option_11_rejects_bad_values_and_keeps_old` (`a..b`, `1.2.3.4:0`), `test_option_11_values_survive_reboot`, `test_option_11_dhcp_and_none`, `test_access_log_lines_still_parse`.
- [ ] **Step 2:** Run — FAIL. **Step 3:** Implement. **Step 4:** Flash, run `test_serial_menu.py` and `test_time_syslog.py` — PASS.
- [ ] **Step 5:** Commit — "Menu: option 11 Time and syslog".

### Task 8: Web page fields and time in the status

**Files:**
- Modify: `lib/web_ui/include/web_app.h` (`WEB_BODY_MAX` 11264 → 12288; `WebStatus` gains `char time[64]; char ntpSetting[64]; char syslogSetting[64];`; `WebBackend` gains `virtual bool saveTimeLog(const char* ntp, const char* syslog, const char*& error) = 0;`), `lib/web_ui/src/web_app.cpp` (main page: status row `Time`; a card *Time and syslog* with fields `ntp` and `syslog` (help: `address, name, dhcp or none` / `address[:port], dhcp or none`), POST `/timelog` added to the CSRF allow-list), `src/web_service.cpp` (`DeviceBackend::saveTimeLog` → validate with Task 2 functions, save, apply, `syslogEvent(SETTINGS)`)
- Test: `test/native/test_web_ui/test_main.cpp`, `test/hil/test_web.py`

**Interfaces:**
- Consumes: Task 2 validation, Task 5/6 apply functions
- [ ] **Step 1: Failing native tests** — `test_timelog_needs_session_and_csrf`, `test_timelog_saves_through_backend`, `test_timelog_bad_value_shows_error_and_saves_nothing`, `test_main_page_shows_time_row_and_card`, `test_longest_values_still_fit_the_page` (extend the existing longest-values test with 63-character servers and the time row; must stay under `WEB_BODY_MAX`).
- [ ] **Step 2–4:** FAIL → implement → PASS (and sanitizers).
- [ ] **Step 5: HIL** `test_web.py`: `test_time_and_syslog_from_web_page_apply` (set via the form, read back on the serial status, restore). Flash, run — PASS.
- [ ] **Step 6:** Commit — "Web: time and syslog settings".

### Task 9: Update-site state over SNMP and LibreNMS

**Files:**
- Modify: `src/update_service.cpp` / `.h` (state `lastGoodCheckinMs`, `failedInRow`, set around `scheduler.checkinDone(ok, now)`; `void updateServiceSnmpState(SnmpUpdateState&)`), `lib/snmp_agent/include/snmp_mib.h` (`struct SnmpUpdateState { uint8_t result; uint32_t minutesSinceGood; uint32_t failedInRow; };` in `SnmpDeviceState`), `lib/snmp_agent/src/snmp_mib.cpp` (macro `UPDATE_SITE(n)` → `63050.8.n.0`; entries after `FW_UPDATE(5)`), `src/snmp_service.cpp` `captureState()`, `mibs/RF-POWER-MONITOR-MIB.txt`, `docs/MIB.md`, `librenms/os_discovery/rfpowermonitor.yaml`
- Test: `test/native/test_snmp_agent/test_main.cpp`, `scripts/host_agent_check.sh` (walk includes .8), `test/hil/test_snmp.py` (`all_oids`)

**Interfaces:**
- Produces (MIB `updateSite` group 63050.8):
  - `.1 updCheckinResult` INTEGER { off(0), never(1), ok(2), failed(3) }
  - `.2 updMinutesSinceGood` Gauge32 — minutes since the last good check-in, or since the network came up if none yet; 0 when off
  - `.3 updFailedInRow` Gauge32
  - LibreNMS: sensor `count` (or `state` + `count`) on `updMinutesSinceGood` with `warn_limit: 60`; state sensor on `updCheckinResult` (failed = warning)
- [ ] **Step 1: Failing tests** — `test_update_site_group_values` (each object from the state), `test_update_site_group_walk_order` (after fwUpdate, before the next subtree).
- [ ] **Step 2–4:** FAIL → implement → PASS; `wsl -d Ubuntu -- bash scripts/host_agent_check.sh` PASS (MIB file compiles in net-snmp).
- [ ] **Step 5:** HIL `test_snmp.py` (walk lists match). Flash, run with community `public` (switch and restore) — PASS.
- [ ] **Step 6:** LibreNMS: copy the YAML and MIB into the running `librenms/test` container as `docs/LIBRENMS.md` describes, rediscover the device, check the two sensors appear. Commit — "SNMP: update-site group, LibreNMS sensors".

### Task 10: LibreNMS syslog, docs and release 3.6.0

**Files:**
- Modify: `librenms/test/compose.yml` (a `syslogng` service like `snmptrapd`: `SIDECAR_SYSLOGNG: "1"`, `514:514/udp`, `514:514/tcp`), `docs/LIBRENMS.md` (syslog receiver, Hyper-V firewall rule for UDP 514 like `RFUPS-SNMP-Trap`), `README.md` (option 11, DHCP options 7/42 and DHCPv6 56, syslog events list, local time), user manual sections that list menu options, `docs/TESTING.md` (new tests, opt-in variables `RFPOWER_NTP_SERVER`, `RFPOWER_SYSLOG_TEST`, `RFPOWER_SYSLOG_PORT`), `TODO.md` (section 23, close "SNMP objects for the update server state"), `CHANGELOG.md` ([3.6.0]), `include/version.h` (3.6.0)
- [ ] **Step 1:** Start the syslog-ng sidecar with plain `docker compose up -d` in `librenms/test` (never `down -v`); point the device's syslog at `10.132.0.45` through option 11; check lines appear on the device's LibreNMS syslog page. The Hyper-V firewall rule needs the user (administrator) — ask.
- [ ] **Step 2:** Full verification: `pio test -e native`, sanitizers, `host_agent_check.sh`, tool tests, `pio test -e pico` (then reflash), full HIL (community switched to `public` and back), the opt-in time/syslog tests. Record the `RAM:` and image size lines in the CHANGELOG.
- [ ] **Step 3:** Commit — "Release 3.6.0"; tag `v3.6.0` locally (push only when the user asks).
