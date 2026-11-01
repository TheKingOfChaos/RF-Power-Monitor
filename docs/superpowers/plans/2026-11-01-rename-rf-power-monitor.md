# Rename to RF.Power-Monitor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename the project from RF-UPS-Monitor to RF.Power-Monitor everywhere (firmware, MIB, LibreNMS, tests, hardware generator, docs, diagrams), move the user's Pico, PC variables and test LibreNMS over, and bring docs and diagrams up to date with the firmware; version 3.0.0.

**Architecture:** Rename in layers, each with its tests: firmware strings and identifiers (native tests assert the new names first), MIB and LibreNMS files (net-snmp check loads the renamed MIB), test variables, hardware generator, then the live systems (Pico, LibreNMS, PC variables), then content updates of diagrams and docs. Scripted replacements are always followed by reading the diff.

**Tech Stack:** C++/PlatformIO, Unity, Python HIL, net-snmp (WSL), LibreNMS (Docker), Excalidraw generator (`docs/diagrams/make_diagrams.py`, `render_svg.sh` in Docker), KiCad generator (`hardware/pico-poe-mains-carrier/tools/generate.py`).

**Spec:** docs/superpowers/specs/2026-11-01-rename-rf-power-monitor-design.md

## Global Constraints

- Names (old → new): display `RF-UPS-Monitor` → `RF.Power-Monitor`; MIB `RF-UPS-MONITOR-MIB` → `RF-POWER-MONITOR-MIB` (file `mibs/RF-POWER-MONITOR-MIB.txt`); MIB identifiers `rfUpsMonitor…` → `rfPowerMonitor…`; LibreNMS OS `rfupsmonitor` → `rfpowermonitor`; default hostname `rf-ups-monitor` → `rf-power-monitor`; mDNS instance `RF.Power-Monitor`; test variables `RFUPS_*` → `RFPOWER_*`; C/C++ identifiers `rfups…` → `rfpower…`; board title/silkscreen `RF.Power-Monitor carrier for the W5500-EVB-Pico-PoE` / `RF.Power-Monitor v1.1`.
- Unchanged: enterprise 63050 and all OIDs, object names without the project name, settings file names and JSON keys, board revision, GitHub repo URL and local folder.
- Allowed leftovers of the old name: CHANGELOG history and its "Updating from 2.x" list, `docs/superpowers/` specs and plans, repository URLs, the user's firewall rule name (CLAUDE.local.md).
- Version 3.0.0. Branch `rename-rf-power-monitor`; no merge, push or tag without the user.
- Device settings restored after HIL (community `rfit`, trap 10.132.0.45 v2c); the Pico's hostname ends as `rf-power-monitor`. Never `docker compose down -v`; never read the Dirigera token or `secret/`.

## Review Focus

1. A device with stored sysName `rf-ups-monitor` (every existing device) keeps loading and keeps that name: no silent migration of stored settings — Task 1 `test_stored_old_default_name_is_kept` (settings_codec).
2. Two LibreNMS definitions matching the same sysObjectID (old `rfupsmonitor` left in the container): detection picks the wrong one — Task 6 removes the old files and checks `devices.os = rfpowermonitor`.
3. snmptrapd still running with the old MIB: traps show as `rfPowerMonitorNotifications.N` numbers — Task 6 restarts it and checks one trap's name.
4. A dot in the mDNS instance name sent as two labels: the web service disappears from DNS-SD — Task 5 checks from Windows and falls back to `RF Power Monitor`.
5. The LLDP frame with the longer sysDescr still fits `LLDP_FRAME_MAX` — Task 1 `test_build_frame_fits_with_real_texts` (test_lldp, real sysDescr and a 32-character hostname).

---

### Task 1: Firmware and library names

**Files:** `src/*` (menu title, boot banner, sysDescr `SYS_DESCR`, mDNS instance in `src/hostname.cpp`, `rfupsIPv6Gateway` in `include/lwip_hooks.h` and `src/ipv6_static.cpp`), `lib/settings_codec/src/settings_codec.cpp` (`DEFAULT_SYS_NAME`), `lib/web_ui` (page title/brand), other `lib/` comments with the name; tests in `test/native/*`, `test/embedded/*`.

- [ ] **Step 1:** Update the native/embedded tests that assert names to the new ones (default sysName `rf-power-monitor`, web page title/brand, any sysDescr/menu text), and add `test_stored_old_default_name_is_kept` (a settings JSON with `"sysName":"rf-ups-monitor"` loads with that name) and `test_build_frame_fits_with_real_texts` (sysDescr `"RF.Power-Monitor 3.0.0 mains power monitor (RP2040, W5500)"`, 32-char hostname, both addresses → length > 0 and ≤ LLDP_FRAME_MAX).
- [ ] **Step 2:** Run `pio test -e native` — the renamed expectations FAIL (old names), the two new tests PASS (they pin existing behaviour; note this in the ledger).
- [ ] **Step 3:** Rename in code (`git grep -n -i -E "rf-?ups|rfups|ups-monitor" -- src include lib test/native test/embedded`), read the diff.
- [ ] **Step 4:** `pio test -e native` all PASS, `sanitize.sh` PASS, `pio run -e pico` builds.
- [ ] **Step 5: Commit** `Rename firmware and libraries to RF.Power-Monitor`.

### Task 2: MIB and LibreNMS files

**Files:** `git mv mibs/RF-UPS-MONITOR-MIB.txt mibs/RF-POWER-MONITOR-MIB.txt` (module name, identifiers `rfUpsMonitor…` → `rfPowerMonitor…`, descriptions, a new REVISION 202611010100Z "Renamed"), `git mv` of `librenms/os_detection/rfupsmonitor.yaml`, `librenms/os_discovery/rfupsmonitor.yaml`, `librenms/images/logos/rfupsmonitor.svg`, `librenms/images/os/rfupsmonitor.svg` to `rfpowermonitor.*` (inside: `os`, `text`, `mib_dir`, MIB names), `librenms/test/*` (trap script regex `RF-POWER-MONITOR-MIB::rfPowerMonitorNotifications\.[123]`, compose mounts), `scripts/host_agent_check.sh` (MIB load check), `test/hil/snmp_tools.py` trap names, `lib/snmp_agent` comments.
- [ ] **Step 1:** Run `host_agent_check.sh` after renaming only the file name → the MIB check FAILs (module name ≠ file); then rename inside.
- [ ] **Step 2:** `host_agent_check.sh`: all PASS incl. "MIB loads without warnings" with `RF-POWER-MONITOR-MIB`; `snmptranslate -m RF-POWER-MONITOR-MIB RF-POWER-MONITOR-MIB::mainsPowerLostTooLong -On` gives `.1.3.6.1.4.1.63050.0.3`. Validate both LibreNMS YAMLs against LibreNMS's `discovery_schema.json`/detection schema in the container (as done before).
- [ ] **Step 3: Commit** `Rename the MIB and LibreNMS definition to RF.Power-Monitor`.

### Task 3: Test variables

**Files:** every `RFUPS_` in `test/`, `scripts/`, `docs/`, `.github/workflows/tests.yml`, `README.md`, `CLAUDE.md`; `CLAUDE.local.md` (local).
- [ ] **Step 1:** Replace `RFUPS_` → `RFPOWER_`; read the diff; `git grep RFUPS_` empty.
- [ ] **Step 2:** Recreate the user's Windows user variables: for each `RFUPS_*` user variable, set `RFPOWER_*` to the same value (`[Environment]::SetEnvironmentVariable(..., 'User')`), then remove the old one; never print values of token-related variables (print names only). Update CLAUDE.local.md names (and note the firewall rule keeps its old name).
- [ ] **Step 3:** A HIL suite that reads the variables runs as before: `test_lldp.py` with `RFPOWER_LLDP_SWITCH=1` → 4 OK.
- [ ] **Step 4: Commit** `Rename test variables to RFPOWER_*`.

### Task 4: Carrier board generator

**Files:** `hardware/pico-poe-mains-carrier/tools/generate.py` and its outputs, hardware README.
- [ ] **Step 1:** Read the hardware README for how to regenerate and check (ERC/DRC, fab outputs); change the three strings in `generate.py`.
- [ ] **Step 2:** Regenerate as documented; `git diff --stat` shows only the title/silkscreen text changed in the KiCad files (and the fab outputs the README says are generated); run the documented checks — PASS. If a step needs a tool that is not installed, record it as a ruling and keep the generator change.
- [ ] **Step 3: Commit** `Carrier board: RF.Power-Monitor title and silkscreen`.

### Task 5: The user's Pico

- [ ] **Step 1:** Flash; `pio test -e pico` 20/20; flash again.
- [ ] **Step 2:** Set the Pico's hostname to `rf-power-monitor` (menu 4, keep community and trap settings); status shows `Hostname: rf-power-monitor`.
- [ ] **Step 3:** Recapture the DHCPDISCOVER in `test/native/test_dhcp_option/frames.h` the way its comment describes (with the new hostname), update the test's expectations, `pio test -e native -f native/test_dhcp_option` PASS.
- [ ] **Step 4:** mDNS check from Windows: `Resolve-DnsName rf-power-monitor.local` answers, and the `_http._tcp` instance `RF.Power-Monitor` is seen (e.g. `dns-sd -B _http._tcp` if Bonjour is installed, else a Python zeroconf-free mDNS query script in the scratchpad). If the dot breaks it, switch the instance to `RF Power Monitor` (ruling) and recheck.
- [ ] **Step 5:** HIL suites with `RFPOWER_*`: lldp, serial menu, web, snmp, ipv6, watchdog (community public for the run, restored after). Expected as before the rename (DHCP trap forged-ACK still blocked by the network).
- [ ] **Step 6: Commit** `Recaptured DHCPDISCOVER with the new hostname`.

### Task 6: The test LibreNMS

- [ ] **Step 1:** Copy the new definitions, logos and MIB into the container as docs/LIBRENMS.md describes; delete `rfupsmonitor.*` and `RF-UPS-MONITOR-MIB` there.
- [ ] **Step 2:** Rediscover device 1: `devices.os = rfpowermonitor`, OS text RF.Power-Monitor, sensors (mains, mains alarm, temperatures, changes) present, LLDP link eth0 → r169 / ge-0/0/9.0, still 1 device.
- [ ] **Step 3:** Restart the snmptrapd container (`docker compose restart snmptrapd`) after copying the MIB; trigger one trap (authenticationFailure via a wrong community) and see it in the device's event log as `RF-POWER-MONITOR-MIB::…` or the standard name.
- [ ] **Step 4:** Update docs/LIBRENMS.md's dated records with what was re-run (new date, new name); commit `LibreNMS: moved to rfpowermonitor`.

### Task 7: Diagrams

**Files:** `docs/diagrams/make_diagrams.py`, `*.excalidraw`, `*.svg`.
- [ ] **Step 1:** In `make_diagrams.py`: new name in every title, and the content listed in the spec Goal 6 (firmware architecture: lldp_service, watchdog, dhcp6_service, lib/lldp, lib/watchdog_logic, lib/dhcp6_client; system overview: switch + LLDP; network addresses: DHCPv6; SNMP flow: LLDP-MIB; test layers: LLDP/watchdog HIL, switch console, net-snmp check; mains flow: alarm after 60 s), each checked against the code.
- [ ] **Step 2:** Generate the `.excalidraw` files, render SVGs with `render_svg.sh` (Docker Desktop must run; ask the user to start it if not), open each SVG (Read the image) to check layout and text.
- [ ] **Step 3: Commit** `Diagrams: RF.Power-Monitor, LLDP, watchdog, DHCPv6, mains alarm`.

### Task 8: Docs, version, final grep

**Files:** `include/version.h` (3.0.0), `CHANGELOG.md` (3.0.0 with "Updating from 2.x"), README, docs/*.md, CLAUDE.md, TODO.md, hardware README.
- [ ] **Step 1:** Rename and update content, section by section against the code (spec Goal 5).
- [ ] **Step 2:** `git grep -n -i -E "rf-?ups|rfups|ups-monitor"` lists only the allowed leftovers (Global Constraints).
- [ ] **Step 3:** Full native suite, sanitizers, host check, build; commit `RF.Power-Monitor 3.0.0: docs`.
