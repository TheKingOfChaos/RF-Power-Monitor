# Mains Outage Alarm Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The device raises `mainsPowerAlarm` (and sends one `mainsPowerLostTooLong` trap) once mains has been lost for 60 s, so LibreNMS alerts only on outages longer than 60 s.

**Architecture:** A pure `MainsAlarm` class in `lib/power_monitor` turns the core 1 mains snapshot into an alarm state plus a pending-trap flag. `src/snmp_service.cpp` owns the instance, polls it from the core 0 loop, reports it in the MIB and sends the trap when a destination exists. LibreNMS gets a critical state sensor on the new object, and the old mains sensor's "lost" becomes a warning.

**Tech Stack:** C++17 (Arduino-Pico, lwIP), Unity native tests, net-snmp in WSL `Ubuntu`, Python HIL, LibreNMS in Docker.

**Spec:** `docs/superpowers/specs/2026-11-01-mains-outage-alarm-design.md`

## Global Constraints

- Object `mainsPowerAlarm` = `1.3.6.1.4.1.63050.6.0`, INTEGER, `none(0)`, `lostTooLong(1)`.
- Trap `mainsPowerLostTooLong`: v2c `1.3.6.1.4.1.63050.0.3`; v1 enterprise `1.3.6.1.4.1.63050`, generic 6, specific 3. Varbinds `mainsPowerStatus`, `mainsPowerChanges`, `mainsPowerLastChange`, `mainsPowerAlarm` (v2c then `snmpTrapAddress.0` unless agentAddr is 0.0.0.0). Time-stamp / sysUpTime.0 = `mainsLastChangeTicks`, like the other mains traps.
- `MAINS_ALARM_AFTER_MS = 60000`, fixed. Raised when lost and `now - lastChangeMs >= 60000`.
- Existing objects, traps and v1 behaviour unchanged.
- Pure libs: no `Arduino.h`. Native `-Wall -Wextra -Werror`, sanitizers must pass. No stack buffer > ~256 bytes.
- Every logic change has a test that was seen failing first; tests are named `test_<what>_<expected behaviour>`.
- Device settings restored after HIL: community `rfit`, trap destination 10.132.0.45, v2c. Never `docker compose down -v`. Never read the Dirigera token.

## Review Focus

1. Restore in the same loop pass as the 60 s mark: the alarm must not be raised, no trap pending (Task 1 `test_restore_at_the_mark_raises_nothing`).
2. Trap destination only appears after the alarm (DHCP option 224 late, menu change): the trap goes out once, then never again for that outage (Task 1 `test_pending_trap_survives_until_sent_once`, Task 3 wiring).
3. Flapping: restore and loss inside 60 s restarts the count, judged by the change counter, not only by `present` (Task 1 `test_quick_restore_and_loss_restarts_the_count`, where the two snapshots both read lost with a higher change count).
4. An outage longer than 49.7 days (`millis()` wrap) keeps the alarm raised (Task 1 `test_alarm_stays_raised_across_millis_wrap`).
5. LibreNMS keeps the old `mainsPowerStatus` translations after the definition changes: verify after rediscovery that "lost" shows as warning, not critical (Task 4 manual check).

---

### Task 1: `MainsAlarm` logic (pure)

**Files:**
- Create: `lib/power_monitor/include/mains_alarm.h`, `lib/power_monitor/src/mains_alarm.cpp`
- Test: `test/native/test_mains_alarm/test_main.cpp`

**Interfaces:**
- Produces:
  ```cpp
  static const uint32_t MAINS_ALARM_AFTER_MS = 60000;
  class MainsAlarm {
  public:
      /// Feed the latest mains snapshot; lastChangeMs is the boot time when changes == 0
      void update(bool present, uint32_t changes, uint32_t lastChangeMs, uint32_t nowMs);
      bool raised() const;        ///< mainsPowerAlarm = 1
      bool trapPending() const;   ///< raised and the trap not sent yet
      void trapSent();            ///< clears trapPending until the next outage
  };
  ```
  State: the change count of the outage being timed, a latched `raised`, a `pending` flag. A changed count or `present` resets both; `raised` latches (no recomputation once set), which also covers the `millis()` wrap.

- [ ] **Step 1: Write the failing tests** (all start from `MainsAlarm a;` and feed snapshots):
  - `test_alarm_not_raised_before_60_s`: lost at 1000, update at 60999 → `raised()==false`, `trapPending()==false`.
  - `test_alarm_raised_at_60_s`: lost at 1000, update at 61000 → `raised()` and `trapPending()` true.
  - `test_restore_clears_alarm_and_pending_trap`: raised, then `update(true, changes+1, t, t)` → both false.
  - `test_restore_at_the_mark_raises_nothing`: lost at 1000, next update is present at 61000 → both false.
  - `test_quick_restore_and_loss_restarts_the_count`: lost (changes 1) at 0; update at 30000 lost with changes 3, lastChange 29000; update at 60000 → not raised; at 89000 → raised.
  - `test_boot_without_mains_counts_from_boot`: `update(false, 0, 0, 59999)` not raised; `(false, 0, 0, 60000)` raised.
  - `test_mains_present_never_raises`: `update(true, 0, 0, 10000000)` → not raised.
  - `test_pending_trap_survives_until_sent_once`: raised; further updates keep `trapPending()`; `trapSent()` → false; later updates in the same outage keep it false and `raised()` true.
  - `test_new_outage_after_restore_sends_a_new_trap`: raise, `trapSent()`, restore, lose again, 60 s → `trapPending()` true.
  - `test_alarm_stays_raised_across_millis_wrap`: lost at 0xFFFF0000, raised at +60000 (wrapped), updates at +0x80000000 and +0xFFFFFFF0 elapsed → still raised.
  - `test_lost_timing_handles_millis_wrap`: lost at 0xFFFFFF00, update at 0x0000E960 (60 000 ms later, wrapped) → raised; at 0x0000E95F → not.

- [ ] **Step 2: Run** `pio test -e native -f native/test_mains_alarm` → FAIL (missing header).
- [ ] **Step 3: Implement** `MainsAlarm` as above, documented per `.clinerules`.
- [ ] **Step 4: Run** the same command → all pass; then `pio test -e native` → all pass.
- [ ] **Step 5: Mutation check:** drop the change-count comparison (use only `present`) → `test_quick_restore_and_loss_restarts_the_count` fails; drop the latch → the wrap test fails. Restore.
- [ ] **Step 6: Commit** `power_monitor: MainsAlarm, raised after 60 s of lost mains`.

### Task 2: MIB object and trap encoding

**Files:**
- Modify: `lib/snmp_agent/include/snmp_mib.h` (`SnmpDeviceState`), `lib/snmp_agent/src/snmp_mib.cpp`, `lib/snmp_agent/include/snmp_trap.h`, `lib/snmp_agent/src/snmp_trap.cpp`, `mibs/RF-UPS-MONITOR-MIB.txt`, `scripts/host_agent_check.sh` (and its host agent state if it sets fixed values), `docs/MIB.md`
- Test: `test/native/test_snmp_trap/test_main.cpp`, `test/native/test_snmp_agent/test_main.cpp`

**Interfaces:**
- Consumes: none.
- Produces:
  ```cpp
  // SnmpDeviceState
  bool mainsAlarm;   ///< mainsPowerAlarm: lost for 60 s or more
  // MainsTrap
  PowerLostTooLong = 3
  size_t snmpBuildMainsAlarmTrap(const char* community, const uint8_t agentAddr[4], const SnmpDeviceState& state,
                                 uint8_t* out, size_t capacity);
  size_t snmpBuildMainsAlarmTrapV2c(const char* community, const uint8_t agentAddr[4], const SnmpDeviceState& state,
                                    int32_t requestId, uint8_t* out, size_t capacity);
  ```

- [ ] **Step 1: Capture net-snmp vectors** in WSL `Ubuntu` the way the existing ones were (listener on a local UDP port, `snmptrap` to it, hex dump of the payload):
  - v1: `snmptrap -v1 -c public <host> 1.3.6.1.4.1.63050 10.10.6.111 6 3 12345 1.3.6.1.4.1.63050.1.0 i 0 1.3.6.1.4.1.63050.2.0 c 1 1.3.6.1.4.1.63050.3.0 t 12345 1.3.6.1.4.1.63050.6.0 i 1`
  - v2c: `snmptrap -v2c -c public <host> 12345 1.3.6.1.4.1.63050.0.3` with the same four varbinds, then `1.3.6.1.6.3.18.1.3.0 a 10.10.6.111`; note the request-id.
- [ ] **Step 2: Write the failing tests:**
  - `test_mains_alarm_trap_matches_netsnmp_byte_for_byte` (v1, `mainsState(false, 1, 12345)` with `mainsAlarm = true`).
  - `test_v2c_mains_alarm_trap_matches_netsnmp_byte_for_byte` (with the captured request-id).
  - `test_v2c_mains_alarm_trap_without_ipv4_address_ends_with_the_alarm` (agentAddr 0.0.0.0 → last varbind is `.6.0`).
  - In `test_snmp_agent`: `test_get_mains_alarm_reports_state` (0 and 1), `test_getnext_after_ambient_temperature_is_mains_alarm`, `test_getnext_after_mains_alarm_is_end_of_mib` (v1 noSuchName / v2c endOfMibView, whatever the existing last-object test expects). If the table-sorted test exists it must still pass.
- [ ] **Step 3: Run** `pio test -e native -f native/test_snmp_trap -f native/test_snmp_agent` → FAIL.
- [ ] **Step 4: Implement**: `getMainsAlarm` → `makeInteger(s.mainsAlarm ? 1 : 0)`, `PRIVATE(6)` after `PRIVATE(5)`; the two builders reuse the mains trap's code with specific 3 / `.0.3` and the extra varbind.
- [ ] **Step 5: Run** native tests → pass; `wsl -d Ubuntu -- bash scripts/sanitize.sh` → exit 0.
- [ ] **Step 6: MIB file**: `mainsPowerAlarm OBJECT-TYPE` (SYNTAX INTEGER { none(0), lostTooLong(1) }, read-only, `::= { rfUpsMonitor 6 }` or whatever the module's parent is) and `mainsPowerLostTooLong NOTIFICATION-TYPE` (OBJECTS the four objects, `::= { <notifications> 3 }`), added to any OBJECT-GROUP / NOTIFICATION-GROUP present. Check in WSL: `snmptranslate -M +./mibs -m +RF-UPS-MONITOR-MIB -On RF-UPS-MONITOR-MIB::mainsPowerAlarm.0` → `.1.3.6.1.4.1.63050.6.0`, the same for `mainsPowerLostTooLong` → `.1.3.6.1.4.1.63050.0.3`, and `smilint` if the existing docs use it.
- [ ] **Step 7: host_agent_check**: add `check "get mainsPowerAlarm" '63050\.6\.0 = INTEGER: 0' snmpget ...`; run `wsl -d Ubuntu -- bash scripts/host_agent_check.sh` → all pass.
- [ ] **Step 8: Docs**: `docs/MIB.md` object row, trap row (v1 6/3, v2c `.0.3`), alarm timing paragraph (60 s from the debounced loss, from boot without mains, one trap per outage, sent later if no destination, cleared on restore without its own trap).
- [ ] **Step 9: Commit** `snmp_agent: mainsPowerAlarm object and mainsPowerLostTooLong trap`.

### Task 3: Firmware wiring, status, web page, HIL

**Files:**
- Modify: `src/snmp_service.cpp` (+ its header), `src/main.cpp`, `src/cli.cpp` (status screen), `lib/web_ui/include/web_app.h`, `lib/web_ui/src/web_app.cpp`, `src/web_service.cpp`, `test/hil/snmp_tools.py` (decode `.0.3` and v1 (6, 3) as `mainsPowerLostTooLong`), `test/hil/test_snmp.py`, `test/hil/test_snmp_traps.py`, `test/hil/device.py` if the status parser needs the new line
- Test: `test/native/test_web_ui/test_main.cpp`, HIL as below

**Interfaces:**
- Consumes: `MainsAlarm` (Task 1), `snmpBuildMainsAlarmTrap*`, `SnmpDeviceState.mainsAlarm` (Task 2).
- Produces: `void snmpPollMainsAlarm();` (core 0 loop, after the mains-change report; updates the alarm from `mainsRead`, and when `trapPending()` and `snmpTrapTarget()` succeed sends `TrapKind::MainsAlarm` as "mainsPowerLostTooLong" and calls `trapSent()`), `bool snmpMainsAlarm();` for the status screen and web page. `WebStatus.mainsAlarm` (bool).

- [ ] **Step 1: Failing native test** `test_status_page_shows_mains_alarm`: `mainsKnown`, `!mainsPresent`, `mainsAlarm = true` → page contains "Mains alarm" and "lost over 60 s" with the danger badge; with `mainsAlarm = false` → "none". Run `pio test -e native -f native/test_web_ui` → FAIL.
- [ ] **Step 2: Implement** the web row, `web_service.cpp` copy, status screen line `Mains alarm: lost over 60 s` / `Mains alarm: none`, `captureState()` sets `s.mainsAlarm = snmpMainsAlarm()`, `TrapKind::MainsAlarm` in `buildTrap`, `snmpPollMainsAlarm()` call in `main.cpp`. Native tests → pass.
- [ ] **Step 3: Build and flash**: `pio run -e pico` (check RAM/flash still within limits), `pio run -e pico -t upload`; `pio test -e pico` stays 20/20 only if run (it replaces the firmware: reflash afterwards).
- [ ] **Step 4: HIL tests** (Dirigera switch, `@skip_without_switch`):
  - `test_snmp.py` `test_short_mains_loss_does_not_raise_alarm`: switch off 30 s, `mainsPowerAlarm` read every few seconds stays 0; on; still 0.
  - `test_snmp.py` `test_mains_loss_over_60_s_raises_alarm_until_restore`: off; 0 at 50 s; 1 by 70 s (poll until 75 s); status screen shows the alarm line; on → 0 within 5 s.
  - `test_snmp_traps.py` `test_mains_alarm_trap_after_60_s` (v1 and v2c via the shared class): off; `mainsPowerLostTooLong` within 75 s; varbinds `.1.0` = 0, `.6.0` = 1, timestamp = `.3.0`; exactly one such trap until restore; on. Skips while snmptrapd holds UDP 162. Ask the user before stopping the LibreNMS snmptrapd container to run it (`docker stop lnms-test-snmptrapd-1`, start it again afterwards).
- [ ] **Step 5: Run** `python.exe -m unittest discover -s test/hil -p "test_snmp.py" -v` and the traps suite; restore community `rfit`, trap destination 10.132.0.45, v2c.
- [ ] **Step 6: Commit** `Mains alarm in the firmware: MIB, trap, status screen, web page`.

### Task 4: LibreNMS, verification and docs

**Files:**
- Modify: `librenms/os_discovery/rfupsmonitor.yaml`, `librenms/test/snmptrap-agent-addr.php`, `docs/LIBRENMS.md`, `CHANGELOG.md`, `TODO.md`, `README.md` (if it lists objects/traps)

**Interfaces:**
- Consumes: the device from Task 3.

- [ ] **Step 1: Definition**: `mainsPowerStatus` state `lost` → `generic: 1`; new state sensor:
  ```yaml
  -
      oid: RF-UPS-MONITOR-MIB::mainsPowerAlarm
      num_oid: '.1.3.6.1.4.1.63050.6.{{ $index }}'
      descr: 'Mains lost over 60 s'
      index: 'mainsPowerAlarm.{{ $index }}'
      state_name: mainsPowerAlarm
      states:
          - { descr: none, value: 0, generic: 0 }
          - { descr: lostTooLong, value: 1, generic: 2 }
  ```
  Copy the MIB into wherever the LibreNMS container reads it (as done for the existing MIB; check `compose.yml` mounts).
- [ ] **Step 2: Trap script**: `isMainsTrap()` also matches `63050.0.3` (the name `RF-UPS-MONITOR-MIB::mainsPowerLostTooLong` already matches the prefix). Restart only the snmptrapd container if the script is not re-read per trap.
- [ ] **Step 3: Rediscover** the device in the running LibreNMS (`docker exec lnms-test-librenms-1 lnms device:discover <device>` or the equivalent `./discovery.php -h <device>`), then check in the database or UI: the new sensor exists, and `mainsPowerStatus` "lost" is translated as warning (Review Focus 5); fix the translation if LibreNMS kept the old one, and document how.
- [ ] **Step 4: Real mains, short**: switch off ~30 s, on. Expected: event log shows mainsPowerLost/Restored, "Mains power" sensor warning while off, no new alert in the alert log.
- [ ] **Step 5: Real mains, long**: switch off ~90 s, on. Expected: `mainsPowerLostTooLong` in the event log at ~60 s, "State Sensor Critical" alert opens within ~10 s of that, recovers within ~10 s of restore. Record the times.
- [ ] **Step 6: Docs**: `docs/LIBRENMS.md` (new sensor, warning vs critical, why a rule delay does not work, rediscovery after updating, measured times), `CHANGELOG.md` Unreleased/Added, `TODO.md` replace the PHP item with this one ticked (tests named), README if needed.
- [ ] **Step 7: Full run**: `pio test -e native`, sanitizers, `pio test -e pico` (then reflash firmware), HIL suites that changed; restore device settings.
- [ ] **Step 8: Commit** `LibreNMS: alert only on mains outages over 60 s (mainsPowerAlarm sensor)`.
