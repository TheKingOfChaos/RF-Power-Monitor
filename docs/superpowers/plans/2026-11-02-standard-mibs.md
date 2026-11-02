# Standard MIBs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** LibreNMS shows the device's location, CPU load per core, memory, LittleFS storage, IP addresses, inventory and SNMP/IP/ICMP/UDP/TCP statistics, from the standard MIBs (SNMPv2-MIB, HOST-RESOURCES-MIB, ENTITY-MIB, IP-MIB, UDP-MIB, TCP-MIB).

**Architecture:** Each standard group is new rows in the agent's sorted MIB table (`lib/snmp_agent/src/snmp_mib.cpp`), reading new fields of `SnmpDeviceState`. Large data reaches the state by pointer to firmware statics, as `lldp` does today. A new pure library, `lib/cpu_load`, computes the one-minute load average; `src/host_stats.cpp` feeds it. The IP/ICMP/UDP/TCP counters are lwIP's own MIB-II counters, switched on by our `include/lwipopts.h` and copied in `src/net_stats.cpp`. IP address rows change at run time, so the MIB gets *dynamic columns* whose row indexes come from the state.

**Tech Stack:** C++ (Arduino-Pico core, lwIP), PlatformIO, Unity (native + embedded), Python unittest HIL, net-snmp in WSL (`Ubuntu`), LibreNMS 26.10.2 in Docker.

**Spec:** `docs/superpowers/specs/2026-11-02-standard-mibs-design.md`

## Global Constraints

- All `.clinerules` rules: tests first, and each new test seen failing before the fix. Test names follow `test_<what>_<expected>`. Native builds use `-Wall -Wextra -Werror`. Parsers (frame counting) get truncated and corrupted input tests and pass `scripts/sanitize.sh`.
- No buffer or struct over ~256 bytes on the stack. `SnmpValue` is ~170 bytes and an `Oid` is 132 bytes, so anything holding several goes in a member or a static.
- RAM stays at or under 50% of 262144 bytes (today 45.4%, 119000 bytes). Check `pio run -e pico` after every task and write the number in the commit message. 60% is allowed only if 50% is impossible; in that case also update `.clinerules`.
- Everything is read-only. A Set on any new object answers as it does today (v1 `noSuchName`, v2c `notWritable`).
- v1 behaviour of existing objects is unchanged. Counter64 stays hidden from v1.
- Counters: an event that cannot happen is served as 0. An event that happens but cannot be seen is served as 0 and docs/MIB.md says "not measured". A group is left out only if its main counters cannot be measured.
- SNMP median response stays ~4 ms. The HIL timing tests (`test_snmp.py` latency, web tests) still pass.
- Every task updates `docs/MIB.md` (objects, walk counts), the object list comment in `lib/snmp_agent/include/snmp_mib.h`, `test/host/host_agent.cpp` + `scripts/host_agent_check.sh` (net-snmp names resolve: `-m ALL`), and `TODO.md`. `CHANGELOG.md` gets an entry under [Unreleased] → Added.
- Release as 3.1.0 (`include/version.h`) in the last task.
- Commands: `$P = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"`; native `& $P test -e native`; firmware `& $P run -e pico`; flash `& $P run -e pico -t upload` (COM4); HIL `& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" -m unittest discover -s test/hil -p "<file>" -v`; net-snmp `wsl -d Ubuntu -- bash scripts/host_agent_check.sh`; sanitizers `wsl -d Ubuntu -- bash scripts/sanitize.sh`.

## Review Focus

1. **IP addresses change between two GetNext calls** (DHCP renew, SLAAC address added or deprecated mid-walk). The walk must continue from the next larger OID, never repeat or loop. Test: `test_dynamic_rows_getnext_continues_after_a_removed_row` (Task 7).
2. **No IPv4 address** (link down, DHCP not answered) and **no IPv6 global address**: the tables are empty. Walks skip them, and Get on an old row is noSuchInstance in v2c and noSuchName in v1. Test: `test_ip_tables_empty_without_addresses` (Task 7).
3. **CPU load under sustained load and after a wrap.** `time_us_32` wraps every 71 minutes. Load stays within 0-100 and does not jump at the wrap. Tests: `test_cpu_load_never_exceeds_100`, `test_loop_load_survives_timer_wrap` (Task 4).
4. **lwIP rebuilt with statistics on** changes its behaviour or timing somewhere the protocol tests do not look (DHCP, mDNS, DHCPv6, LLDP, the web server). Expected: nothing changes except the counters. Test: Task 6 Step 5 runs the whole HIL suite once, and Step 4 runs `pio test -e pico`.
5. **The web form with the longest values** (63-character contact and location, all characters that HTML-escape) still fits the page buffer. Test: extend `test_longest_values_still_fit_the_page` (Task 1).

---

### Task 1: sysContact and sysLocation on the web page

**Files:**
- Modify: `lib/web_ui/include/web_app.h` (`WebStatus`, `WebBackend`, `WEB_BODY_MAX`)
- Modify: `lib/web_ui/src/web_app.cpp:358-364` (form), `:498-508` (handler)
- Modify: `src/web_service.cpp` (backend implementation)
- Test: `test/native/test_web_app/test_main.cpp` (find the suite with `grep -l saveHostname test/native`), `test/hil/test_web.py`
- Docs: README (web page section), docs/USER_MANUAL.md, docs/SETUP_MANUAL.md

**Interfaces:**
- Produces: `WebStatus::sysContact[SYS_TEXT_MAX_LENGTH + 1]`, `WebStatus::sysLocation[SYS_TEXT_MAX_LENGTH + 1]`. `virtual bool saveDeviceName(const char* hostname, const char* contact, const char* location) = 0;` replaces `saveHostname` and saves all three in one settings save.

- [ ] **Step 1: Write the failing native tests**
  - `test_device_name_form_shows_contact_and_location`: the page contains `name="contact"` and `name="location"` with the backend's values, HTML-escaped (a value with `"` and `<`).
  - `test_device_name_post_saves_all_three`: POST `/hostname` with `hostname=a1&contact=ops%40x&location=Rack%204` calls `saveDeviceName("a1","ops@x","Rack 4")`, and the reply says "saved and active".
  - `test_device_name_post_accepts_empty_contact_and_location`: empty values are saved as empty.
  - `test_device_name_post_rejects_bad_contact`: 64 characters, or a control character, gives 400 and does not call the backend. Same test for location.
  - `test_device_name_post_without_contact_field_is_rejected`: 400, nothing saved.
  - Extend `test_longest_values_still_fit_the_page`: 63-character contact and location made of `"` (each escapes to `&quot;`).
- [ ] **Step 2: Run, see them fail.** `& $P test -e native -f native/test_web_app` → FAIL (no `saveDeviceName`, no fields).
- [ ] **Step 3: Implement.** Rename the card to "Device name". Add two text inputs: "Contact (sysContact)" and "Location (sysLocation)", help text "0-63 printable characters. Shown in LibreNMS." Validate with `settingsTextIsValid(value, SYS_TEXT_MAX_LENGTH, true)`. Raise `WEB_BODY_MAX` by the measured growth plus a margin, and update its comment with the new longest page size. Backend: copy into `currentSettings`, save once, apply the hostname as `saveHostname` did before. On a failed save, nothing changes in memory.
- [ ] **Step 4: Run native, see them pass.** Then `& $P run -e pico` and note RAM (two page buffers grew).
- [ ] **Step 5: HIL.** In `test/hil/test_web.py`, add `test_contact_and_location_from_web_reach_snmp`: log in, POST the three fields, `snmpget` 1.3.6.1.2.1.1.4.0 and .6.0 return them, the status screen shows them, then restore the old values. Update existing tests that POST only `hostname` to send all three. Flash, then run `test_web.py`. Expected: PASS.
- [ ] **Step 6: Commit** `Web page: sysContact and sysLocation` (with the RAM figure).

### Task 2: MIB lookup returns resolved instances (no behaviour change)

Prepares for dynamic rows and a MIB of more than 255 objects.

**Files:**
- Modify: `lib/snmp_agent/include/snmp_mib.h`, `lib/snmp_agent/src/snmp_mib.cpp`, `lib/snmp_agent/include/snmp_agent.h`, `lib/snmp_agent/src/snmp_agent.cpp`, `lib/snmp_agent/src/snmp_trap.cpp:72-74`
- Test: `test/native/test_snmp_agent/test_main.cpp`, `test/native/test_snmp_trap/test_main.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct MibObject { Oid oid; SnmpValue value; };   // ~300 bytes: never on the stack
  enum class MibView : uint8_t { V2c, V1 };          // V1 also skips Counter64
  /// Exact instance. false if no such instance now (absent instance included)
  bool mibGet(const Oid& oid, const SnmpDeviceState& s, MibObject& out);
  /// First instance after oid that has a value now (and is v1-visible for MibView::V1)
  bool mibGetNext(const Oid& oid, const SnmpDeviceState& s, MibView view, MibObject& out);
  bool mibHasObject(const Oid& oid);                 // unchanged meaning
  uint16_t mibSize(); const MibEntry& mibEntry(uint16_t index);  // static entries, widened
  ```
  The agent keeps `MibObject _found[SNMP_MAX_VARBINDS]` as a member (v1 resolves all varbinds first). `mibFind`/`mibNext` become internal (static in snmp_mib.cpp).

- [ ] **Step 1: Write failing tests**
  - `test_mib_get_returns_the_instance_and_value`: sysUpTime.0 → value TimeTicks 123456.
  - `test_mib_get_absent_instance_is_false`: ambientTemperature.0 with `ambientTempValid=false`.
  - `test_mib_getnext_v1_skips_counter64`: from ifXTable.5.1 → ifXTable.14.1 for V1, .6.1 for V2c.
  - Change the walk tests at lines ~747, 881, 965 and 1035 to walk with `mibGetNext` instead of indexing `mibEntry`.
- [ ] **Step 2: Run and see them fail to compile.** `& $P test -e native -f native/test_snmp_agent`
- [ ] **Step 3: Implement** the functions above, and switch `snmp_agent.cpp` (present/v1Visible/mibNextPresent/mibNextV1) and `snmp_trap.cpp` to them.
- [ ] **Step 4: Check that every native suite passes unchanged**, plus sanitizers and `host_agent_check.sh`. RAM after `pio run` is ~+5 KB at most (`_found`).
- [ ] **Step 5: Commit** `SNMP agent: MIB lookup returns resolved instances`.

### Task 3: SNMPv2-MIB complete (sysORTable, snmp group, snmpSetSerialNo)

**Files:**
- Modify: `lib/snmp_agent/include/snmp_agent.h` (`SnmpStats`), `lib/snmp_agent/src/snmp_agent.cpp`, `lib/snmp_agent/include/snmp_mib.h` (`SnmpDeviceState`), `lib/snmp_agent/src/snmp_mib.cpp`, `src/snmp_service.cpp` (`captureState`)
- Test: `test/native/test_snmp_agent/test_main.cpp`, `test/hil/test_snmp.py`

**Interfaces:**
- Consumes: Task 2 API.
- Produces:
  - `SnmpStats` gains `inGetResponses`, `inTraps` (split out of `inUnsupported`, which keeps counting too-many-varbinds), `inTotalReqVars` (varbinds answered with a value by Get/GetNext/GetBulk), `silentDrops` (`SnmpOutcome::NoRoom`).
  - `SnmpDeviceState` gains `const SnmpStats* snmp;` (the agent's live stats; nullptr = all counters 0) and `uint32_t snmpOutTraps;`.
- Values:
  - sysORLastChange `1.3.6.1.2.1.1.8.0` = TimeTicks 0. sysORTable `1.3.6.1.2.1.1.9.1.{2,3,4}.n`, n = 1..9 in this order (sysORID / sysORDescr / sysORUpTime 0):
    1. `1.3.6.1.6.3.1` "SNMPv2-MIB"
    2. `1.3.6.1.2.1.31` "IF-MIB"
    3. `1.3.6.1.2.1.48` "IP-MIB"
    4. `1.3.6.1.2.1.50` "UDP-MIB"
    5. `1.3.6.1.2.1.49` "TCP-MIB"
    6. `1.3.6.1.2.1.25.7.1` "HOST-RESOURCES-MIB"
    7. `1.3.6.1.2.1.47` "ENTITY-MIB"
    8. `1.0.8802.1.1.2` "LLDP-MIB"
    9. `1.3.6.1.4.1.63050` "RF-POWER-MONITOR-MIB"

    Rows 3-7 are added in this task already. Their objects come in Tasks 4-7, all before the release; drop row 5 if Task 6 drops TCP.
  - snmp group `1.3.6.1.2.1.11.N.0`, Counter32 unless stated otherwise:
    - 1 = inPackets
    - 2 = outResponses + snmpOutTraps
    - 3 = inBadVersions
    - 4 = inBadCommunity
    - 5 = 0
    - 6 = inParseErrors
    - 8-12 = 0
    - 13 = inTotalReqVars
    - 14 = 0
    - 15 = inGetRequests
    - 16 = inGetNextRequests
    - 17 = inSetRequests
    - 18 = inGetResponses
    - 19 = inTraps
    - 20 = outTooBig
    - 21 = outNoSuchName
    - 22 = 0
    - 24 = 0
    - 25-27 = 0
    - 28 = outResponses
    - 29 = snmpOutTraps
    - 30 = INTEGER 1 (snmpEnableAuthenTraps)
    - 31 = silentDrops
    - 32 = 0
  - snmpSetSerialNo `1.3.6.1.6.3.1.1.6.1.0` = INTEGER 0.

- [ ] **Step 1: Write failing tests**
  - Extend `test_mib_contains_expected_objects`.
  - `test_sysortable_lists_nine_mibs` (check IDs and descriptions).
  - `test_snmp_group_counts_requests` (two Gets with 3 and 1 varbinds: snmpInPkts 2, snmpInGetRequests 2, snmpInTotalReqVars 4, snmpOutGetResponses 2).
  - `test_snmp_in_pkts_includes_the_current_request` (a Get of snmpInPkts on a fresh agent returns 1).
  - `test_snmp_in_traps_counts_trap_pdus` (a captured net-snmp v2c trap fed to `handle()`).
  - `test_snmp_silent_drops_counts_no_room`.
  - `test_snmp_out_pkts_adds_traps`.
  - `test_snmp_set_serial_no_is_zero_and_read_only` (v2c Set gives notWritable).
- [ ] **Step 2: Run and see them fail.**
- [ ] **Step 3: Implement.** In `captureState`: `s.snmp = &agent.stats(); s.snmpOutTraps = serviceStats.trapsSent;`
- [ ] **Step 4: Native, sanitizers and net-snmp check all pass.** Walk counts in docs/MIB.md and the tests that count walks are updated.
- [ ] **Step 5: HIL.** Add `test_snmp_group_counts_this_test_requests` to `test_snmp.py`: read snmpInGetRequests, do 5 gets, the counter rose by at least 6. Update the expected walk counts. Flash, then run `test_snmp.py`.
- [ ] **Step 6: Commit** `SNMPv2-MIB: sysORTable, snmp group, snmpSetSerialNo`.

### Task 4: CPU load (pure library)

**Files:**
- Create: `lib/cpu_load/include/cpu_load.h`, `lib/cpu_load/src/cpu_load.cpp`, `lib/cpu_load/library.json` (copy the shape of `lib/tmp1075/library.json`)
- Test: `test/native/test_cpu_load/test_main.cpp`

**Interfaces:**
- Produces:
  ```cpp
  /// One-minute average of busy time, hrProcessorLoad (RFC 2790): 60 one-second slots
  class CpuLoad {
  public:
      /// Account busyUs of the elapsed time; closes a slot every 1,000,000 us of nowUs (wrap-safe uint32)
      void add(uint32_t nowUs, uint32_t busyUs);
      /// Average over the last up to 60 completed seconds, 0-100; 0 before the first second
      uint8_t percent() const;
  };
  /// Core 0 never idles: busy = pass time beyond the idle pass time (the shortest pass in the last 60 s)
  class LoopLoad {
  public:
      /// Call once per loop() pass with time_us_32() at its start
      void pass(uint32_t nowUs);
      uint8_t percent() const;
  };
  ```
  The firmware uses `CpuLoad` directly for core 1: busy = loop1 time outside `delay(1)`.
- [ ] **Step 1: Failing tests:**
  - `test_cpu_load_is_zero_before_one_second`
  - `test_cpu_load_half_busy_reads_50`
  - `test_cpu_load_averages_over_sixty_seconds` (30 s at 100% then 30 s at 0% gives 50; after another 60 s idle, 0)
  - `test_cpu_load_never_exceeds_100` (busy > elapsed)
  - `test_cpu_load_survives_timer_wrap` (start at 0xFFF00000)
  - `test_loop_load_idle_loop_reads_zero` (constant 40 us passes)
  - `test_loop_load_busy_passes_raise_load` (1 pass of 500 ms in a second of 40 us passes gives ~50)
  - `test_loop_load_baseline_follows_the_last_minute` (the baseline is not stuck at an early 10 us pass after 61 s of 40 us passes)
  - `test_loop_load_survives_timer_wrap`
- [ ] **Step 2: Run, see them fail.** `& $P test -e native -f native/test_cpu_load`
- [ ] **Step 3: Implement.** Use a ring of 60 `uint8_t` percentages per object. `LoopLoad` keeps a per-second minimum pass and a 60-entry ring of those minima. No floats needed.
- [ ] **Step 4: Run, see them pass, then run sanitizers.**
- [ ] **Step 5: Commit** `cpu_load: one-minute busy average for both cores`.

### Task 5: HOST-RESOURCES-MIB and ENTITY-MIB

**Files:**
- Create: `src/host_stats.cpp`, `src/host_stats.h` (core 0 RAM/LittleFS/core 0 load; core 1 load measured in `loop1`)
- Modify: `src/main.cpp` (`loop()` start: `hostStatsLoopPass()`; `loop1()` around `delay(1)`), `include/version.h` (`HARDWARE_REVISION`), `lib/snmp_agent/include/snmp_mib.h`, `lib/snmp_agent/src/snmp_mib.cpp`, `src/snmp_service.cpp`
- Test: `test/native/test_snmp_agent/test_main.cpp`, new `test/hil/test_host_resources.py`
- Docs: docs/MIB.md sections "Host resources" and "Inventory"

**Interfaces:**
- Consumes: `CpuLoad`, `LoopLoad` (Task 4).
- Produces:
  ```cpp
  struct SnmpHostState {
      uint32_t ramTotalBytes, ramUsedBytes;   // 262144; total minus rp2040.getFreeHeap()
      bool fsValid; uint32_t fsTotalBytes, fsUsedBytes;   // LittleFS.info(), refreshed every 10 s
      uint8_t cpuLoad[2];                     // percent, core 0 and core 1
      const char* serialNumber;               // pico_get_unique_board_id_string, 16 hex digits
  };
  // SnmpDeviceState gains: const SnmpHostState* host;   (nullptr: these objects have no instance)
  void hostStatsBegin(); void hostStatsLoopPass(); void hostStatsCore1Busy(uint32_t nowUs, uint32_t busyUs);
  void hostStatsPoll(); const SnmpHostState& hostStatsSnapshot();
  ```
  The core 1 load crosses cores. Write it as one `uint8_t` per core into a volatile byte (an atomic store on the M0+) and add a `__dmb()` as `lib/mains_input` does.
- Values (HOST-RESOURCES `1.3.6.1.2.1.25`):
  - hrSystemUptime `.1.1.0` TimeTicks = uptimeTicks
  - hrSystemNumUsers `.1.5.0` Gauge32 0
  - hrSystemProcesses `.1.6.0` Gauge32 2
  - hrSystemMaxProcesses `.1.7.0` INTEGER 2
  - hrMemorySize `.2.2.0` INTEGER ramTotal/1024
  - hrStorageTable `.2.3.1.c.i`, i = 1 RAM, 2 LittleFS:
    - 1 Index
    - 2 Type: `1.3.6.1.2.1.25.2.1.2` hrStorageRam / `.9` hrStorageFlashMemory
    - 3 Descr: "RAM" / "LittleFS (settings)"
    - 4 AllocationUnits 1024
    - 5 Size
    - 6 Used (KB, rounded up)
    - 7 AllocationFailures Counter32 0

    The LittleFS row is absent while `!fsValid`.
  - hrDeviceTable `.3.2.1.c.i`, i = 768 core 0, 769 core 1:
    - 1 Index
    - 2 Type `1.3.6.1.2.1.25.3.1.3`
    - 3 Descr "RP2040 core 0 (network, SNMP)" / "RP2040 core 1 (mains input)"
    - 4 ID OID 0.0
    - 5 Status INTEGER 2 running
    - 6 Errors Counter32 0
  - hrProcessorTable `.3.3.1.c.i`: 1 FrwID OID 0.0, 2 Load INTEGER cpuLoad.
- Values (ENTITY-MIB `1.3.6.1.2.1.47.1.1.1.1.c.1`, one row):
  - 2 Descr "RF-Power-Monitor mains power monitor"
  - 3 VendorType `1.3.6.1.4.1.63050`
  - 4 ContainedIn 0
  - 5 Class 3 (chassis)
  - 6 ParentRelPos -1
  - 7 Name = sysName
  - 8 HardwareRev = `HARDWARE_REVISION` "W5500-EVB-Pico-PoE + carrier rev 1.1"
  - 9 FirmwareRev FIRMWARE_VERSION
  - 10 SoftwareRev ""
  - 11 SerialNum = serialNumber
  - 12 MfgName "Kenneth Foli Jørgensen" (UTF-8)
  - 13 ModelName "RF-Power-Monitor"
  - 14 Alias ""
  - 15 AssetID ""
  - 16 IsFRU 2 (false)

  Plus entLastChangeTime `1.3.6.1.2.1.47.1.4.1.0` TimeTicks 0.

  Check in Step 6 whether LibreNMS needs a port row; add it (index 2, class 10 port, contained in 1) only if the Inventory or the ports link needs it.
- [ ] **Step 1: Failing native tests:**
  - `test_hr_storage_reports_ram_and_littlefs`
  - `test_hr_storage_used_rounds_up_to_whole_kb` (1 byte used gives 1)
  - `test_hr_storage_littlefs_row_absent_when_invalid` (walk skips it, v2c Get noSuchInstance)
  - `test_hr_processor_load_per_core`
  - `test_hr_device_rows_describe_both_cores`
  - `test_host_resources_absent_without_host_state`
  - `test_entity_row_names_the_device` (serial, firmware rev, UTF-8 manufacturer bytes)
  - Extend `test_mib_contains_expected_objects` and the walk counts.
- [ ] **Step 2: Run, see them fail.**
- [ ] **Step 3: Implement the MIB rows, then the firmware layer.**
- [ ] **Step 4: Native, sanitizers, net-snmp check** (`snmpwalk ... HOST-RESOURCES-MIB::hrStorageTable` resolves names).
- [ ] **Step 5: HIL.** Add `test/hil/test_host_resources.py`:
  - `test_ram_storage_matches_free_heap_on_status_screen` (add "RAM used" and "CPU load" lines to the status screen in this task, documented in README)
  - `test_cpu_load_idle_is_low` (≤ 5 on both cores after 70 s idle)
  - `test_cpu_load_core0_rises_during_bulk_walks` (60 s of repeated `snmpbulkwalk`, core 0 load ≥ 2× the idle value and ≥ 5)
  - `test_littlefs_storage_is_plausible` (size 256 KB ± one block, used > 0)
  - `test_entity_serial_matches_status_screen`

  Flash, run, and also run `test_snmp.py` and `test_mains*` (core 1 timing unchanged).
- [ ] **Step 6: LibreNMS check.** Rediscover in `librenms/test` (`docker compose exec librenms lnms device:poll <host>` after `discovery`): the processors, mempool, storage and Inventory appear. If the LittleFS row is not discovered as `hrStorageFlashMemory`, switch to `.4` hrStorageFixedDisk and update the test. Write down what LibreNMS showed in the commit message.
- [ ] **Step 7: Commit** `HOST-RESOURCES-MIB and ENTITY-MIB: CPU load, RAM, LittleFS, inventory`.

### Task 6: IP, ICMP, UDP and TCP statistics (lwIP's MIB-II counters)

lwIP keeps the exact MIB-II counters (`lwip_stats.mib2`, all `u32_t`) when `LWIP_STATS` and `MIB2_STATS` are on. The core's `lwipopts.h` sets `LWIP_STATS 0` unconditionally, so a `-D` cannot override it. Our own `include/lwipopts.h` can: `-I include` comes first, so the file loads the core's with `#include_next` and then overrides. A spike on 2026-11-02 built, and every lwIP module referenced `lwip_stats` (492 bytes with all statistics on).

**Files:**
- Create: `include/lwipopts.h`. It starts with `#include_next <lwipopts.h>`, then `#undef` + `#define LWIP_STATS 1` and `MIB2_STATS 1`. It turns off lwIP's debug counters we do not serve (`LINK_STATS`, `ETHARP_STATS`, `IP_STATS`, `IPFRAG_STATS`, `ICMP_STATS`, `IGMP_STATS`, `UDP_STATS`, `TCP_STATS`, `MEM_STATS`, `MEMP_STATS`, `SYS_STATS`, `IP6_STATS`, `ICMP6_STATS`, `IP6_FRAG_STATS`, `MLD6_STATS`, `ND6_STATS` all 0), so only `mib2` costs RAM. Each line gets a comment saying why. Also add a line to the CLAUDE.md "Build notes".
- Modify: `src/net_stats.cpp` and `.h`, adding `void netStatsCaptureProto(SnmpProtoCounters& out)`. It copies `lwip_stats.mib2`, and sets `tcpCurrEstab` by counting `tcp_active_pcbs` in ESTABLISHED or CLOSE_WAIT (lwIP's own snmp agent does the same, in `apps/snmp/snmp_mib2_tcp.c`). Also modify `lib/snmp_agent/include/snmp_mib.h`, `lib/snmp_agent/src/snmp_mib.cpp` and `src/snmp_service.cpp`.
- Test: `test/native/test_snmp_agent/test_main.cpp`, `test/hil/test_snmp.py`

**Interfaces:**
- Produces:
  ```cpp
  /// lwIP's MIB-II counters (field names as in lwIP's struct stats_mib2), copied once per request
  struct SnmpProtoCounters {
      uint32_t ipInReceives, ipInHdrErrors, ipInAddrErrors, ipForwDatagrams, ipInUnknownProtos, ipInDiscards,
               ipInDelivers, ipOutRequests, ipOutDiscards, ipOutNoRoutes, ipReasmOks, ipReasmFails, ipReasmReqds,
               ipFragOks, ipFragFails, ipFragCreates, ipRoutingDiscards;
      uint32_t icmpIn[13], icmpOut[13];   // icmpInMsgs..icmpInAddrMaskReps / icmpOutMsgs..: MIB order .1-.13 / .14-.26
      uint32_t tcpActiveOpens, tcpPassiveOpens, tcpAttemptFails, tcpEstabResets, tcpInSegs, tcpOutSegs,
               tcpRetransSegs, tcpInErrs, tcpOutRsts, tcpCurrEstab;
      uint32_t udpInDatagrams, udpNoPorts, udpInErrors, udpOutDatagrams;
  };
  // SnmpDeviceState gains: const SnmpProtoCounters* proto;   (nullptr: these objects have no instance)
  ```
- Constants: ipForwarding 2 (not forwarding), ipDefaultTTL `IP_DEFAULT_TTL` (255), ipReasmTimeout `IP_REASS_MAXAGE` (verify the unit in lwIP's ip4_frag.c: it counts in `IP_TMR_INTERVAL` ticks, so serve seconds), tcpRtoAlgorithm 4 (vanj), tcpRtoMin/Max in ms as lwIP's snmp agent reports them, tcpMaxConn `MEMP_NUM_TCP_PCB`.
- What lwIP counts is what we serve, in both IPv4 and IPv6 for UDP and TCP (lwIP increments the mib2 UDP/TCP counters for both). IP and ICMP are IPv4 only. Write down in docs/MIB.md any counter lwIP never increments (check each in its source, e.g. ipRoutingDiscards).
- MIB rows:
  - IP scalars `1.3.6.1.2.1.4.{1-19}.0` and `.23.0`
  - ICMP `1.3.6.1.2.1.5.{1-26}.0`
  - TCP `1.3.6.1.2.1.6.{1-12,14,15}.0` (.9 tcpCurrEstab Gauge32)
  - UDP `1.3.6.1.2.1.7.{1-4}.0`

  Keep the table sorted. ipAddrTable (`.4.20`) comes in Task 7 and sorts between `.19` and `.23`.
- [ ] **Step 1: Failing native tests:**
  - `test_ip_scalars_from_state`
  - `test_icmp_scalars_map_in_and_out_arrays` (.1 = icmpIn[0], .13 = icmpIn[12], .14 = icmpOut[0], .26 = icmpOut[12])
  - `test_tcp_curr_estab_is_gauge32`
  - `test_udp_scalars_from_state`
  - `test_protocol_counters_absent_without_proto_state`
  - Extend `test_mib_contains_expected_objects` and the walk counts.
- [ ] **Step 2: Run, see them fail.**
- [ ] **Step 3: Implement.** First `include/lwipopts.h`: build, then check with `arm-none-eabi-nm -S` that `lwip_stats` exists and is small. Then the state, the rows, and `netStatsCaptureProto` (a static `SnmpProtoCounters` in `captureState`, core 0).
- [ ] **Step 4: Native, sanitizers, net-snmp check.** Also `pio test -e pico`, because lwIP is rebuilt with new options, then reflash the firmware.
- [ ] **Step 5: HIL in `test_snmp.py`:**
  - `test_icmp_echo_counters_follow_ping` (10 pings: icmpInEchos and icmpOutEchoReps rise by 10)
  - `test_udp_counters_follow_snmp_requests`
  - `test_udp_no_ports_counts_closed_port` (send to UDP 9)
  - `test_tcp_passive_opens_follow_web_connections`
  - `test_tcp_curr_estab_counts_an_open_web_connection`

  Then run the whole HIL suite once, since every network path now runs with statistics. SNMP median latency is still ~4 ms.
- [ ] **Step 6: Commit** `IP/ICMP/UDP/TCP statistics from lwIP's MIB-II counters`.

### Task 7: Dynamic rows and IP addresses (ipAddrTable, ipAddressTable)

**Files:**
- Modify: `lib/snmp_agent/include/snmp_mib.h`, `lib/snmp_agent/src/snmp_mib.cpp`, `src/net_stats.cpp`/`.h` (address capture), `src/snmp_service.cpp`
- Test: `test/native/test_snmp_agent/test_main.cpp`, `test/hil/test_ipv6.py`, `test/hil/test_snmp.py`

**Interfaces:**
- Consumes: Task 2 (`mibGet`/`mibGetNext` merge static entries and dynamic columns).
- Produces:
  ```cpp
  enum class SnmpAddrOrigin : uint8_t { Manual = 2, Dhcp = 4, LinkLayer = 5, Random = 6 };  // IpAddressOriginTC
  struct SnmpIpAddress { bool ipv6; uint8_t addr[16]; uint8_t prefixLen; SnmpAddrOrigin origin; };
  struct SnmpIpState { uint8_t count; SnmpIpAddress addr[5]; };  // IPv4 first; sorted by (type, address) for the index
  // SnmpDeviceState gains: const SnmpIpState* ip;   (nullptr = no rows)
  /// A table column whose rows come from the state; rows in ascending index order
  struct MibDynamicColumn {
      Oid column;
      uint8_t (*rowCount)(const SnmpDeviceState&);
      void (*rowIndex)(const SnmpDeviceState&, uint8_t row, Oid& index);   // arcs appended to column
      SnmpValue (*get)(const SnmpDeviceState&, uint8_t row);
  };
  ```
  `mibHasObject` also matches dynamic columns, so a Get of a missing row is noSuchInstance.
- Rows:
  - **ipAddrTable** `1.3.6.1.2.1.4.20.1.c.a.b.c.d` (IPv4 rows only): 1 Addr IpAddress, 2 IfIndex 1, 3 NetMask IpAddress, 4 BcastAddr 1, 5 ReasmMaxSize 65535.
  - **ipAddressTable** `1.3.6.1.2.1.4.34.1.c.T.L.<bytes>` (T = 1 ipv4 / 2 ipv6, L = 4/16):
    - 3 IfIndex 1
    - 4 Type 1 unicast
    - 5 Prefix RowPointer `1.3.6.1.2.1.4.32.1.5.1.T.L.<prefix bytes>.<prefixLen>` (ipAddressPrefixTable itself is not served; documented)
    - 6 Origin: IPv4 DHCP 4 / static 2; IPv6 link-local 5, SLAAC 5, DHCPv6 4, static 2
    - 7 Status 1 preferred
    - 8 Created and 9 LastChanged TimeTicks 0
    - 10 RowStatus 1
    - 11 StorageType 2

    Only addresses lwIP holds as valid (preferred or deprecated) appear. Deprecated ones get Status 2.
- [ ] **Step 1: Failing tests:**
  - `test_ip_tables_empty_without_addresses` (walk goes from `.4.19.0` to `.4.23.0`; v1 Get of an old row is noSuchName, v2c noSuchInstance)
  - `test_ipaddrtable_row_for_ipv4`
  - `test_ipaddresstable_rows_sorted_ipv4_then_ipv6`
  - `test_ipaddress_prefix_pointer_carries_prefix_length`
  - `test_dynamic_rows_getnext_continues_after_a_removed_row` (GetNext from a row that no longer exists returns the next larger one)
  - `test_dynamic_rows_getbulk_walks_columns_in_order`
  - `test_ipv6_index_is_28_arcs`
  - Walk count updates.
- [ ] **Step 2: Run, see them fail.**
- [ ] **Step 3: Implement.** Fill `SnmpIpState` from the netif in `captureState` (static, core 0), sorted by index.
- [ ] **Step 4: Native, sanitizers, net-snmp check** (`snmpwalk IP-MIB::ipAddressTable`).
- [ ] **Step 5: HIL.**
  - `test_snmp_ip_addresses_match_status_screen` in `test_snmp.py`: IPv4 and mask.
  - `test_snmp_ipv6_addresses_match_status_screen` in `test_ipv6.py`: the set of IPv6 addresses equals the one on the status screen.
- [ ] **Step 6: Commit** `IP-MIB: ipAddrTable and ipAddressTable`.

### Task 8: LibreNMS, release 3.1.0, full run

**Files:**
- Modify: `librenms/os_detection/rfpowermonitor.yaml` (`over:` adds `{ graph: device_processor, text: 'CPU' }`, `{ graph: device_mempool, text: 'Memory' }`), `librenms/os_discovery/rfpowermonitor.yaml` (only if serial/hardware need setting), `docs/LIBRENMS.md`, `include/version.h` (`3.1.0`), `CHANGELOG.md` (move the entries under `[3.1.0] - <date>`), `TODO.md`, `README.md` (object counts, status screen lines)

- [ ] **Step 1:** Copy the yaml files into the running LibreNMS as `docs/LIBRENMS.md` describes, then rediscover and poll the device.
- [ ] **Step 2: Check each "Success" item from the spec in LibreNMS:**
  - the location (set one on the web page first and remove it afterwards only if the user wants)
  - two processors and the memory graph
  - LittleFS under storage
  - IPv4/IPv6 on eth0
  - Inventory with serial and manufacturer
  - Netstats graphs: IP, ICMP, UDP, TCP, SNMP

  Take a Playwright screenshot per page with the Claude LibreNMS account (credentials only inside the script, never printed).
- [ ] **Step 3:** Check that the hardware and version on the Overview still come from `sysDescr_regex`, and that generic ENTITY discovery has not replaced them with worse values. If it has, set `hardware` explicitly in the os_discovery yaml.
- [ ] **Step 4: Full verification:**
  - `pio test -e native`
  - sanitizers
  - `host_agent_check.sh`
  - `pio test -e pico`, then reflash the firmware
  - all HIL suites (`discover -s test/hil -v`, mains tests included)

  RAM ≤ 50%, flash ≤ 70%.
- [ ] **Step 5: Commit** `Release 3.1.0: standard MIBs`. Tag `v3.1.0` only after the user agrees.
