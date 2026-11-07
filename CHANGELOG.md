# Changelog

All notable changes to the RF-Power-Monitor firmware are recorded here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses [Semantic Versioning](https://semver.org/): the version is `FIRMWARE_VERSION` in [include/version.h](include/version.h), shown in the boot banner and in sysDescr, and each release has a git tag `vX.Y.Z`.

## [Unreleased]

## [3.5.0] - 2026-11-07

Bench setup of new boards: the default web password can be locked once it is on the device label.

### Added
- **Default web password lock**: hidden menu commands `pwlock` and `pwunlock` (for the bench tool). While locked, menu option 7 refuses to replace the default password (`The default web password is locked because it is printed on the device label. Unlock it at the bench first.`). The lock is the file `data/web_default.lock`; like the password it survives factory resets and firmware updates. The status screen shows `Default web password: locked (on the label)` or `unlocked`.
- **Bench tool** `tools/bench_provision.py`: flashes a new board from the release `-usb.uf2` (field key only, unless `--dev`), reads it, locks its default password and writes its 40 x 40 mm label (`bench/<hostname>.png`, white ribbon on black, NIIMBOT M2) and a row in `bench/devices.csv`; `--no-flash` for boards already running the firmware, `--reprint` from the log. Packages in `tools/requirements-bench.txt`. It checks the release's signature with the boot program's own check (`fwsign verify`) before asking for BOOTSEL, refuses when `bench/devices.csv` cannot be written, when two boards appear at once, and a hostname that is not 1-32 letters, digits and `-`.
- **Bench update server default**: with `secret/bench.ini` (`[update]` `url` and `key`) the bench tool sets menu option 10 and keeps it with the hidden command `updatedefault` (files `data/update_default_a.json` / `_b.json`). A factory reset restores it instead of turning check-ins off. The status screen shows `Update server default: ...`; `benchinfo` reports `updatedefault` and `updatedefaultkey` (never the key).
- Hidden menu command `benchinfo`: hostname, MAC, serial number, firmware version, firmware key id, default web password and lock state in a fixed `key=value` block between `BENCHINFO BEGIN` and `BENCHINFO END`, for the bench tool (`lib/bench_info`).

### Changed
- The default web password is kept twice, `data/web_default.txt` (as before, so older firmware still finds it) and `data/web_default_b.txt`. At boot a valid copy is used and the other one repaired; only when neither is valid is a new password made. If that happens while the password is locked, the boot log and the status screen say `Default web password LOST: label is wrong, relabel at the bench`. The report stays (marker `data/web_default.lost`) until the bench locks the password again. The password and its lock are loaded at boot even when Ethernet does not start, so option 7 stays locked without a network. Two valid copies that differ while locked (option 7 of firmware before 3.5.0, after a rollback) also count as lost.
- A factory reset sets the update server to the bench default when there is one (before: always off).
- Static RAM 49.6 % (130,104 of 262,144 bytes, as 3.4.0; `benchinfo` works on the heap). Image 425,476 bytes, 23.2 % of the core's flash figure.

## [3.4.0] - 2026-11-06

An update site: devices check in with it, and it updates the devices you choose.

### Added
- **Update site** (`update-server/`, Python standard library, Docker on TCP 8071, data in the volume `rfupdate-data`): a device list without login (hostname, serial number, IPv4/IPv6 addresses, firmware and slot, last check-in, online/offline after 65 minutes, update status) and an admin page behind a password (`RFUPDATE_ADMIN_PASSWORD`; 5 wrong passwords a minute lock logins; CSRF tokens): upload a signed `.upd`, tick devices and pick a release, generate the shared key. Releases older than 3.4.0, duplicates and files whose signed firmware has another version than their header are refused. An update ends as done, rolled back, refused, failed (three broken downloads) or lost (no report in 2 hours); it is never retried in a loop.
- **Check-ins**: with an update server URL and the shared key the device checks in 60 s after its network is up and then every 30 minutes (retry after 5 minutes). Every check-in and every reply carries a keyed BLAKE2b MAC; the reply also covers the check-in's random nonce, and the site refuses a replayed nonce. Without the key the device does not check in. A reply from the site confirms a firmware on trial.
- **Updates from the site**: a device downloads the release the site named and installs it through the existing path (signature check, other slot, trial, rollback). Both the file header and the signed slot header must name that version. Releases older than 3.4.0 are refused on the device too. A download must finish within 2 minutes plus 2 KB/s of its length, so a server that trickles data cannot keep a device downloading (and not checking in). Failures are reported at the next check-in. The download uses a small TCP window so SNMP keeps answering (measured: 184 of 184 requests answered during a 1 MB download, slowest 21 ms; 1 MB in 10 s). A release whose signed version is not the one the site named is refused before the other slot is touched. A running check-in ends when the update server settings are saved. Error words from unauthenticated replies are reduced to `[a-z0-9-]` before they reach the status line; copies of the key are wiped from temporary buffers, and the web server wipes each request (passwords, key) once it is answered. The site keeps check-in nonces for 400 days, drops silent clients after 30 s and refuses to share its port.
- Serial menu option **10. Update server** (URL, `dhcp` or `none`; the key typed as `*`, never shown back), a status line `Update server: ...`, and a card *Update server* on the web page's Firmware page. A factory reset removes the URL and the key.
- **DHCP option 225** (text): the update server URL, used when the URL is set to `dhcp`, only from a DHCP answer the device accepted. The key never comes from DHCP.
- `tools/fw_release.py --test-version X`: a development-signed build that reports another version (HIL tests). Hidden menu commands `checkin` and `fetchtest <url>` for tests.

### Changed
- The URL and key are stored in their own A/B files (`data/update_a.json`, `data/update_b.json`), so the main settings file is unchanged and a rollback to 3.3.0 keeps all settings.
- The serial menu's input line holds 143 characters (was 95), for a 127-character URL.
- Static RAM 49.6 % (130,040 of 262,144 bytes; 3.3.0: 48.8 %), within the 50 % limit with about 1 KB to spare: check-in and settings work buffers are on the heap only while they are used. Image 421,732 bytes, 23.0 % of the core's flash figure.

## [3.3.0] - 2026-11-06

Signed A/B firmware updates over the web page, with automatic rollback.

### Breaking: the flash layout changed, the first install needs USB
- Flash now holds our own **boot program**, firmware **slot A**, firmware **slot B**, then the settings (LittleFS) and EEPROM sector where they were. A device with firmware older than 3.3.0 cannot be updated over the network to this release: flash the release's `rf-power-monitor-3.3.0-usb.uf2` over USB once (BOOTSEL, see docs/SETUP_MANUAL.md). The settings in LittleFS are kept.
- **Use only the `-usb.uf2` file (or the build's `firmware_full.uf2`) on the BOOTSEL drive.** The `firmware.uf2` that PlatformIO still writes holds only slot A's image, without the boot program, and a device flashed with it does not start. `pio run -e pico -t upload` and `pio test -e pico` send the right file.
- The boot program verifies the active slot at every start: about 0.9 s more start-up for a 360 KB firmware.
- A device flashed with a development build (`pio run`, CI, tests) trusts the development key in `keys/dev`, whose private half is committed: it accepts only development-signed files, and anyone with the repository can sign for it. Field devices need a field build (`tools/fw_release.py --field`, key from `tools/fw_keygen.py`).

### Added
- **Firmware update**: web page **Firmware** (running slot and version, kept or on trial, the other slot, last result, signing key id; upload of a `.upd` file; switch to the other slot) and `POST /firmware/upload` for scripts. The image is written to the slot that is not running and its Ed25519 signature is checked before anything switches. Any correctly signed file is accepted, older versions included.
- **Trial and rollback**: a new firmware starts on trial and is kept after one valid SNMP (right community) or authenticated web request. It is rolled back when it crashes (three watchdog resets in a row), when the network was up for 10 minutes without a valid request, or after 60 minutes on trial. A kept firmware that crashes three times in a row falls back to the other slot. Power cuts and planned restarts never count as crashes. Uploads are refused while on trial.
- **A rolled-back release is forgotten**: after a rollback (crash or unreachable) the firmware that runs again erases the failed slot's header at start-up, so the other slot reads `empty` on the status screen, the web page and SNMP. A later crash-loop fallback or a manual switch can therefore never start a release that already failed. A verified upload whose boot record could not be written is erased the same way.
- `tools/fw_keygen.py` (creates `secret/fw_signing_key` and `include/fw_public_key.h`, refuses to overwrite, never prints the key, prints the key id), `tools/fw_release.py` (builds both slots and writes `release/rf-power-monitor-<version>.upd` and `-usb.uf2`; `--field` signs with the field key), `tools/fw_update.py` (updates one or more devices over the web interface, stops at the first failure; password from `RFPOWER_WEB_PASSWORD` or a prompt; "updated" only once the new firmware runs in the other slot and is kept, so a rebuilt file of the running version is not reported before the restart; a refusal whose answer was lost reads `rejected: no reply (...)`), `tools/fwsign` (host signing program built from the same Monocypher code the device uses), `scripts/slot_build.py` (links the firmware per slot and builds `firmware_full.uf2`).
- Boot program (`boot/`): verifies a slot's signature before starting it, picks the slot from a power-safe two-sector boot record, falls back after three crashes, and blinks 3 short and 1 long when nothing is bootable. Monocypher is vendored in `lib/monocypher`.
- Serial status screen lines `Firmware:`, `Other slot:`, `Last update:` and `Firmware key:`, and boot messages for slot, verification time and fallbacks.
- SNMP: the `fwUpdate` group (`63050.7`: fwRunningSlot, fwSlotAVersion, fwSlotBVersion, fwState, fwLastResult) and the traps `firmwareUpdateKept` (`63050.0.4`) and `firmwareRolledBack` (`63050.0.5`), v1 and v2c. MIB file and docs/MIB.md updated.
- LibreNMS: state sensors *Firmware* (trial is a warning) and *Firmware last update* (rollback and fallback are warnings); the traps trigger a poll like the mains traps. docs/LIBRENMS.md covers the sensors, the traps, restarting snmptrapd after a MIB change, and that `.php` files must stay LF.
- Tests: native suites for the image header and signature (`fw_image`, with RFC 8032 vectors), boot decision and records, the streaming receiver (golden `.upd`, truncation and corruption), trial monitor, web routes; tool tests (`test/tools`); on-device `test_fw_flash`; HIL `test_fw_update.py` (opt-in `RFPOWER_FW_UPDATE_TEST=1`: signed update, rollback on crash and when unreachable, crash-loop fallback, wrong key, switch, optional PoE cycles with `RFPOWER_POE_CYCLE=1`).
- User manual (Firmware page, trial, kept, rolled back, `fw_update.py`, what a rejected upload leaves) and setup manual (generate the key once, back it up, what losing or leaking it means, field builds, the first USB flash).
- Hardware: rev 2 board in `hardware/rf-power-monitor-rev2/` (designed, not built yet). One 2-layer board, 100 x 64 mm, with the RP2040, 2 MB flash, W5500, magnetics, RJ45, a 3.3 V buck and USB-C on it; PoE stays the plug-in WIZnet WIZPoE-P1 module. The mains detector, TMP1075 and factory reset button are the rev 1 circuits with the same pins, so the firmware runs unchanged. KiCad 10 project, JLCPCB fab and assembly files, 3D model, renders and a datasheet. No firmware change.

### Changed
- `platformio.ini` has `default_envs = pico`: a bare `pio run`, `pio test` or upload no longer also builds `pico_b` (the same firmware linked for slot B, which is built by `tools/fw_release.py` and is refused by upload).
- Settings rule (`.clinerules`): a firmware that changes `SETTINGS_VERSION` writes the new layout to new files and keeps the old layout's files up to date, so a rollback or downgrade comes up with its settings. Nothing changes yet (the layout is still version 1).
- CI uploads `firmware_full.uf2` (the USB-installable image) as the artifact, not `firmware.uf2`.
- An upload refused before its body is read (not logged in, bad CSRF token, on trial, too large) is answered after the device has read and dropped the body, so the browser and `fw_update.py` see the reason instead of a reset connection. Every upload must arrive within 5 minutes (and with no pause over 30 s).
- A trial is confirmed by an SNMP request only once its reply has been sent.
- The build fails when slot B would overlap LittleFS (a larger `board_build.filesystem_size`).

### Notes
- Size of the normal build (`pio run -e pico`): firmware image 389,308 bytes, 44 % of a 880,640-byte slot (the 70 % limit); static RAM 48.8 % (128,044 of 262,144 bytes, PlatformIO's figure), within the 50 % limit. An upload allocates nothing: the update receiver writes flash a 256-byte page at a time and is a static 632 bytes, so the peak while an upload runs is the static figure (measured on the device: the status screen's `RAM used`, which also counts both stacks and the heap, was 138,568 bytes before and 138,624 bytes during an upload).
- While an upload runs, each flash erase or write pauses both cores for tens of milliseconds, so mains detection can be late by up to one flash erase. The device is busy during an upload.
- Boot verification time grows with the firmware (about 2.6 us per byte, mostly SHA-512 on the M0+); `scripts/slot_build.py` warns above 550 KB (TODO.md section 20).

## [3.2.0] - 2026-11-02

### Changed
- Default hostname (and SNMP sysName, DHCP and mDNS name) is `RF-Power-` and the last 6 hex digits of the MAC address, e.g. `RF-Power-4EA22C`, so several devices on one LAN no longer share `rf-power-monitor`. A device updated from older firmware that still has the old default switches to the new name; a name you chose stays. Menu option 4 takes `-` for the hostname to go back to the default.

### Fixed
- Setting the hostname while the device had no DHCP lease (the new MAC-based name at start-up, or menu option 4 during DHCP discovery) made the core renew a lease it did not have, and DHCP could stop looking for a server (found with the new default name: no lease). The name is now only renewed once a lease exists; before that the next DHCP message just carries it.

## [3.1.0] - 2026-11-02

Standard MIBs, so LibreNMS and other managers show CPU load, memory, storage, addresses, inventory and protocol statistics without device-specific setup.

### Added
- LibreNMS: CPU and Memory graphs on the device overview, the serial number from ENTITY-MIB. Checked with LibreNMS 26.10.2.
- IP-MIB address tables: ipAddrTable (IPv4) and ipAddressTable (IPv4 and IPv6, with prefix, origin and status), rows that follow the addresses the device has at the moment, so LibreNMS shows the IPv4 and IPv6 addresses on eth0.
- MIB-II ip, icmp, tcp and udp groups (IP-MIB, TCP-MIB, UDP-MIB scalars): lwIP's own MIB-II counters, switched on in the new `include/lwipopts.h` (196 bytes of RAM), so LibreNMS graphs IP, ICMP, TCP and UDP traffic.
- HOST-RESOURCES-MIB (RFC 2790): CPU load of both cores (hrProcessorLoad, one-minute average), RAM and the LittleFS partition (hrStorageTable), hrMemorySize and hrSystem, so LibreNMS graphs processors, memory and storage. Also on the status screen.
- ENTITY-MIB (RFC 6933): one inventory entry with model, hardware and firmware revision, manufacturer and serial number (the flash chip's unique ID).
- SNMPv2-MIB complete (RFC 3418): sysORLastChange and sysORTable (the 9 MIB modules the agent implements), the snmp group (snmpInPkts ... snmpProxyDrops: the agent's own counters, so LibreNMS can graph SNMP traffic) and snmpSetSerialNo (read-only 0). A walk from the root now returns 126 objects (v2c, with an LLDP neighbour).
- Web page: the Device name card also sets sysContact and sysLocation (0-63 printable characters, may be empty), active at once like the hostname; a page still open from older firmware leaves them unchanged. Before, only the serial menu could set them, so LibreNMS showed no location.
- Setup manual (docs/SETUP_MANUAL.md: installing a device, first configuration, LibreNMS, proving the alarm) and user manual (docs/USER_MANUAL.md: daily use, alarms, settings, factory reset, firmware update, troubleshooting).
- HIL tests for the "no link" and "no DHCP server" states (`test/hil/test_switch_port.py`, opt-in `RFPOWER_SWITCH_CONFIG`): they disable the device's switch port or drop DHCP on it over the switch's console, with `commit confirmed` so the switch undoes a change a test could not, and check that the device recovers from both without a reboot.
- LibreNMS plugin `librenms/plugins/RFPowerMonitor` (optional, copy to `app/Plugins/`): a **Switch** box on the device's Overview page with the switch, port and system description the device sees over LLDP, from the same links as the Neighbours tab. The Docker test setup mounts it. Checked with LibreNMS 26.10.2.

### Changed
- SNMP rate limit: a source that has sent the right community may send 200 requests per second with bursts of 400 (was 50/s, burst 100), so plain `snmpwalk`s of the larger MIB run at full speed, also back to back; with the old limit a walk lost a request near object 116 and waited for the manager's retry. All trusted sources together stay within 200/s, and unknown sources keep 50/s with bursts of 100.
- Carrier board: silkscreen and title blocks say RF-Power-Monitor, and the board, schematic, fab files, renders and 3D models are regenerated (the generator's RF-Power-Monitor change in 3.0.0 had not been applied to them yet). ERC and DRC clean; nothing else on the board changed.
- The GitHub repository is now `TheKingOfChaos/RF-Power-Monitor` (GitHub redirects the old address); links in this changelog, the libraries' `library.json` and the MIB's CONTACT-INFO point to it.

### Fixed
- Serial output: every line ends with CR LF. Lines printed with a bare LF (most of the status screen, boot and event messages) staircased across the screen in PuTTY, which does not add the CR itself. HIL test `test_menu_and_status_end_lines_with_crlf`.

## [3.0.0] - 2026-11-01

The project is renamed **RF-Power-Monitor** (was RF-UPS-Monitor): it monitors mains power, not a UPS. The OIDs, the enterprise number 63050 and the settings file are unchanged, so a device updated from 2.x keeps its settings and is still polled by number; the names around it change.

### Changed
- Name in the boot banner, serial menu, web page, sysDescr (`RF-Power-Monitor 3.0.0 mains power monitor (RP2040, W5500)`), LLDP system description and the mDNS web service (`RF-Power-Monitor`).
- MIB module `RF-UPS-MONITOR-MIB` is now `RF-POWER-MONITOR-MIB` (file `mibs/RF-POWER-MONITOR-MIB.txt`), its `rfUpsMonitor...` identifiers are `rfPowerMonitor...`; same OIDs. The MIB description now says v1 and v2c.
- LibreNMS OS `rfupsmonitor` is now `rfpowermonitor` (definition files, logos, sysDescr match `RF-Power-Monitor`).
- Default hostname / sysName `rf-ups-monitor` is now `rf-power-monitor` (new devices and factory reset). A stored name is kept: an updated device keeps `rf-ups-monitor` until it is changed (menu option 4 or the web page).
- Test environment variables `RFUPS_*` are now `RFPOWER_*`, and the diagnostic build flag `RFUPS_DIAG` is `RFPOWER_DIAG`.
- Carrier board generator: title block and silkscreen say RF-Power-Monitor.
- Docs and diagrams brought up to date: LLDP and LLDP-MIB, the watchdog, DHCPv6, the mains alarm, IPv6 trap destinations, current test counts.

### Updating from 2.x
- **LibreNMS:** delete `rfupsmonitor.yaml` (os_detection and os_discovery), `html/images/{os,logos}/rfupsmonitor.svg` and `mibs/rfupsmonitor/`, copy the new files as in docs/LIBRENMS.md, rediscover the device (it becomes OS RF-Power-Monitor; sensor history under the old OS may start over) and restart snmptrapd so it loads the new MIB.
- **Other managers:** load `RF-POWER-MONITOR-MIB` instead of `RF-UPS-MONITOR-MIB`; trap and object OIDs are the same.
- **Hostname:** optionally rename the device to `rf-power-monitor`; its DHCP name, mDNS name and sysName change with it.
- **Tests:** rename `RFUPS_*` environment variables to `RFPOWER_*`.

### Fixed
- HIL watchdog test: the core 1 restart time now comes from when the device stops answering SNMP, not from Windows noticing the USB drop, which sometimes came 2 s late and failed the test (the device restarted after 9.1 s every time). The firmware was right; no firmware change.

## [2.7.0] - 2026-11-01

### Added
- LLDP-MIB over SNMP (v1 and v2c): `lldpLocPortTable` (our port: MAC, `W5500 10/100 Ethernet`) and `lldpRemTable` (one row, `0.1.1`, while the device hears a switch: chassis and port ID, port description, system name and description, capabilities). LibreNMS reads these in its normal discovery, so the device's Neighbours tab shows the switch and port without the switch being in LibreNMS; checked with LibreNMS 26.9.1 (`eth0` → `r169`, `ge-0/0/9.0`). When the switch sends a locally assigned Port ID with a port description (Junos: `523` and `ge-0/0/9.0`), the device serves the description as an interfaceName Port ID, since LibreNMS labels the port with the Port ID. docs/LIBRENMS.md recommends turning off `autodiscovery.xdp` so LibreNMS does not try to add the switch.

### Changed
- A walk from the root (`.1`) now starts with the LLDP-MIB objects (`1.0.8802...` sorts before `1.3.6.1...`): 3 objects, 12 with a neighbour (v1 59 objects, v2c 67). Walks of mib-2 or the enterprise subtree are unchanged.

## [2.6.0] - 2026-11-01

### Added
- Traps to an IPv6 destination: menu option 4 accepts a global or ULA IPv6 address (stored in RFC 5952 form) as well as IPv4, `-` and `dhcp`. IPv6 destinations get v2c traps only (a v1 Trap-PDU's agent-addr is IPv4); the menu refuses trap version 1 with an IPv6 destination and the settings check rejects that combination. Link-local, multicast and `::/8` addresses are refused. `snmpTrapAddress.0` keeps carrying the device's IPv4 address. Settings files from older firmware load unchanged; older firmware rejects a file with an IPv6 trap destination and falls back to the other copy or defaults.
- Hardware watchdog in every build (8 s, armed at the end of start-up): core 0 feeds it only while core 1's heartbeat moves, so a hang on either core restarts the device. The boot log and status screen show `Last restart:` (power-on, reset pin, reboot or watchdog) and the watchdog restarts since power-on. The `RFUPS_DIAG` build no longer feeds the watchdog itself. Hidden menu commands `stall0` / `stall1` hang a core for the HIL test.
- DHCPv6 address (stateful DHCPv6, RFC 8415, one IA_NA): when a router advertisement has the M flag, the device gets an address from a DHCPv6 server next to its link-local, SLAAC and static addresses, after duplicate address detection; renews and rebinds on the server's timers, removes the address when the lease ends, declines a duplicate, keeps an address the server moved it away from until that lease ends, confirms the address when the Ethernet link comes back up (RFC 8415 18.2.12) and honours the server's SOL_MAX_RT. DUID-LL from the MAC, hostname in Client FQDN. No setting; nothing changes on networks without the M flag. Status screen (`DHCPv6: ...`, `(dhcpv6)`) and web page ("IPv6 (DHCPv6)", or why there is no address). lwIP has no stateful DHCPv6, so the client is our own (`lib/dhcp6_client`, host tested with Kea's and dhcpcd's real messages); its frames bypass lwIP's port 546. HIL tests on a test network that sends M only to the device (`RFUPS_DHCP6_TEST=1`).

- Alarm for mains outages longer than 60 s: new object `mainsPowerAlarm` (`1.3.6.1.4.1.63050.6.0`, `1` once mains has been lost for 60 s without a break, back to `0` when it returns) and one `mainsPowerLostTooLong` trap per outage (v2c `1.3.6.1.4.1.63050.0.3`, v1 specific-trap 3, with the mains objects and `mainsPowerAlarm`, stamped with the time of the loss). If no trap can leave when the alarm is raised (no address, link down, no destination), the trap is sent once as soon as one can, unless mains returns first. Status screen `Mains alarm:` and web page row. MIB file updated (new groups 5 and 6).
- LLDP (IEEE 802.1AB): the device announces itself every 30 s, 1 s after the link comes up and about 1 s after its address or hostname changes (MAC as chassis and port ID, hostname, sysDescr, ifDescr as port description, station only, IPv4 and global IPv6 management addresses, TTL 120 s), so the switch lists it as a neighbour; checked with a Juniper EX2200-C. It reads the switch's LLDP and shows the switch and port on the status screen (`Switch: r169, port ge-0/0/9.0`) and a "Switch" row on the web page; text from the switch is cut to 64 characters, non-printable bytes are replaced and the page escapes it. Always on, no setting. Pure `lib/lldp` (host tested incl. a captured EX2200-C frame, every truncation, sanitizers), `src/lldp_service.cpp`. Hidden menu command `lldpdump` prints the last frame from the switch. HIL tests read the switch's neighbour list over its serial console (opt-in `RFUPS_LLDP_SWITCH`, `RFUPS_LLDP_SWITCH_CONSOLE`).
- LibreNMS: state sensor "Mains lost over 60 s" (critical when raised), so the "State Sensor Critical" rule alerts only on outages over 60 s; the trap script also polls on the new trap. A LibreNMS rule delay cannot do this: LibreNMS logs the alert at the poll and the delay only holds back notifications.

### Changed
- LLDP frames are no longer counted as unknown protocol (ifInUnknownProtos) but as received multicast frames, since the device reads them.
- Largest web page 9856 bytes (was 8704): the status page with the longest switch name (every character escaped) needs 9472. About 1 KB more RAM.
- lwIP is built with `SO_REUSE=1`, so the web server's reusable socket can reopen port 80 while finished connections are still in TIME-WAIT. This replaces the firmware's own removal of those entries from lwIP's private list; turning the web interface off and on behaves as before. The HIL suite now has a test that really creates TIME-WAIT entries on the device (logins); the old one passed without the workaround.
- LibreNMS definition: "Mains power" lost is now a warning instead of critical, so a short outage no longer opens an alert. Rediscover the device and restart snmptrapd after updating the files.
- The PlatformIO platform is pinned to commit `24f5f5b` (Arduino-Pico 5.4.3, lwIP 2.2.1, GCC 14.2), the versions every release so far was built and tested with. Before, a fresh install or CI cache miss took the platform's newest commit, by now Arduino-Pico 6.x with GCC 16. Same firmware as before.
- Mains detection requirement is now 80 ms from mains changing to detection (was 10 ms): the carrier board's detector holds a loss 30-50 ms to bridge the zero crossings. The firmware's own share, from GPIO27 changing to detection, stays within 10 ms. No firmware change.

### Fixed
- Trap HIL tests skip with a clear message when UDP 162 is taken (the LibreNMS snmptrapd container) instead of failing and leaving COM4 open, which made the next suite fail too.
- HIL `test_snmp_settings_are_validated_and_cancel_keeps_them` expected the old "is not a valid IPv4 address" message for a bad trap destination; the message is now "not a valid IPv4 or IPv6 address".

## [2.5.1] - 2026-10-31

### Added
- v2c traps end with `snmpTrapAddress.0` (`1.3.6.1.6.3.18.1.3.0`, RFC 3584), the device's IPv4 address, as v1 traps carry it in agent-addr. A manager behind NAT (LibreNMS in Docker Desktop) sees another sender address and can find the device by this one; checked with LibreNMS in Docker. Left out while the device has no IPv4 address.
- LibreNMS Docker setup receives the device's traps: a `snmptrapd` container on UDP 162 that accepts only `TRAP_COMMUNITY`, and `librenms/test/snmptrap-agent-addr.php`, which matches traps to the device by the address in them (v1 agent-addr, v2c snmpTrapAddress.0) because Docker Desktop hides the sender's address. After a mains trap it polls the device's sensors, so the "State Sensor Critical" alert opens and recovers within seconds instead of at the next 5-minute poll. Checked with a real 30 s mains cut: the alert opened 3.4 s after mains went off and recovered 3.4 s after it came back. With WSL in mirrored mode, UDP 162 has to be allowed in the Hyper-V firewall (docs/LIBRENMS.md).

### Fixed
- LibreNMS Docker setup: the poller container has the logo files too; LibreNMS checks the icon file when it polls, so scheduled polls set the generic icon again.

## [2.5.0] - 2026-10-31

### Added
- **Ambient temperature** from the TMP1075 on the carrier board (I2C0, GP4/GP5, address 0x48), read every 2 s: SNMP `ambientTemperature` (`1.3.6.1.4.1.63050.5.0`, 0.1 °C), the status screen, the web page and a LibreNMS sensor "Ambient (TMP1075)" (warning 40 °C, critical 50 °C). Without the sensor (module without the carrier board, or three failed reads in a row) the object has no instance: v2c Get answers noSuchInstance, v1 Get noSuchName, and walks skip it; the status screen and the web page say "no sensor".
- Web page: with DHCP the Network card's boxes show the IP address, subnet mask and gateway in use (from the DHCP server), with the DNS server below them; choosing Static and saving keeps that address. Saving with DHCP still keeps the stored static settings. Before, the boxes showed the stored static settings, which are not in use with DHCP.
- LibreNMS: the device shows the RF logo (from `logo/`) instead of the generic icon (`librenms/images/os/` and `images/logos/`, `icon: rfupsmonitor`); checked with LibreNMS in Docker. Copy the two SVGs when installing (docs/LIBRENMS.md); the icon changes on the next poll.
- Hardware: 3D-printable case for the module on the carrier: Fusion 360 design `hardware/Case/Power Case.f3z`, STEP files for the case and the lid, and the print project `hardware/pico-poe-mains-carrier/3d/Power Case.3mf`.

### Fixed
- CI firmware build (Linux), failing since 2.3.0: the build flag `MEMP_NUM_SYS_TIMEOUT=(LWIP_NUM_SYS_TIMEOUT_INTERNAL+12)` broke the shell command over its parentheses. It is now written without them; the timer pool is the same size.
- Factory reset button: "released after ... ms, no reset" always said 0 ms; it now gives how long the button was held.
- HIL factory reset test: during the 10 s hold it polled SNMP with "public" after changing the community, so it never got a reply; it uses the community it set, and it reconnects before restoring the settings when the device rebooted mid-test.

### Changed
- The RP2040 die temperature is called the **CPU temperature**: status screen, web page, LibreNMS sensor "CPU (RP2040 die)", and the MIB object `boardTemperature` is renamed `cpuTemperature`. The OID (`63050.4.0`) and the value are unchanged, so numeric polling keeps working; scripts that use the name `RF-UPS-MONITOR-MIB::boardTemperature` need the new name. The LibreNMS sensor keeps its index, so its graph history stays.
- Factory reset button tested on the carrier board (SW1 on GP22): a short press is ignored, a 10 s hold resets after 10000-10100 ms and reboots within 2 s.
- The status screen prints temperatures between 0 and -1 °C with their minus sign (it showed -0.5 °C as 0.5 °C).
- Web page buffers (`WEB_BODY_MAX`) 8192 -> 8704 bytes for the temperature rows; the largest status page is 8354 bytes. RAM use 42.2 %.
- Hardware: F1 is a SETfuse SCT1032 (T 1 A, 250 VAC, 100 A breaking capacity, LCSC C19712559) on the same Schurter UMT 250 footprint, so JLCPCB can place it; it was a Schurter T 100 mA without an LCSC part. Fab files, renders and 3D models regenerated.

## [2.4.2] - 2026-10-19

The firmware is unchanged apart from its version number.

### Changed
- Project name is RF-UPS-Monitor (without "v3", which looked like a version number). The firmware already reported this name in sysDescr; the README, CLAUDE.md and the test fixtures follow.
- Hardware: carrier board v1.1. F1 (T 100 mA, 250 VAC, Schurter UMT 250) after the L terminal; the divider is now R1 in L and R2-R4 in N, one series loop through F1. SW1 factory reset button from GP22 to GND, so the firmware's 10 s factory reset works on the carrier. The I2C pull-ups are renumbered R8/R9 (no gap after R7). The silkscreen carries the RF logo (from `logo/`, flattened to a filled polygon by `tools/generate.py`), "RF-UPS-Monitor v1.1" and "Made by Kenneth Foli Jørgensen 2026"; the schematic and board title blocks name RF-UPS-Monitor, the maker and the year. Two more M3 holes, H4 and H5, under the module between the header rows, for mounting the carrier when the module is on headers (a project footprint without courtyard, so the courtyard check stays on for the parts). ERC and DRC clean; fab files, renders and 3D models regenerated.

## [2.4.1] - 2026-10-18

The firmware is unchanged apart from its version number.

### Added
- Hardware: carrier PCB for the W5500-EVB-Pico-PoE in `hardware/pico-poe-mains-carrier/` (KiCad 10 project, fab files for JLCPCB, 3D models, renders), documented in the README with how it maps to the firmware. Open points are in TODO.md: the TMP1075 is not read yet, nothing is on GP22 for the factory reset (the module's buttons are BOOTSEL and RUN), and the detector reports a mains loss about 30-50 ms late.
- Diagrams: web page request flow and where the IPv4/IPv6 addresses come from.
- LibreNMS over IPv6 checked (transport `udp6`, LibreNMS 26.9.1); the Docker test network has IPv6.

### Changed
- Docs: web page section with every feature, the URLs for scripts and troubleshooting; static IPv6 in the settings file and factory reset; project structure.

## [2.4.0] - 2026-10-17

### Added
- **Static IPv6 address and gateway:** serial menu option 9 and an IPv6 card on the web page. One address (/64), added next to the link-local and SLAAC addresses, with duplicate address detection; an optional gateway that becomes the next hop for all off-link IPv6 traffic (lwIP hook `LWIP_HOOK_ND6_GET_GW`, `include/lwip_hooks.h`), otherwise the advertised routers are used. Addresses are checked (no link-local, multicast, ::/8 or zero interface id; gateway link-local or in the /64) and stored in RFC 5952 form. The status screen marks the address `(static)` or `(DUPLICATE ...)` and shows the IPv6 gateway.
- Settings: optional `ipv6Address` and `ipv6Gateway` fields; files from older firmware load unchanged (empty = none), a factory reset clears them.

### Changed
- lwIP keeps 4 IPv6 addresses (`LWIP_IPV6_NUM_ADDRESSES=4`, was 3): link-local, static and two advertised prefixes.
- `SETTINGS_JSON_MAX` 896 -> 1024 bytes for the two new fields.
- The web page's status data (~340 bytes) is a member of the page object instead of a local variable, keeping it off core 0's stack.
- IPv6 text parsing and RFC 5952 formatting live in `lib/netconfig` (`parseIPv6`, `formatIPv6`); the access log uses the same formatter.

## [2.3.0] - 2026-10-17

### Added
- **IPv6 (dual stack):** link-local and SLAAC addresses; SNMP, the web page and mDNS work over IPv6. The status screen and the web page show the IPv6 addresses.
- Rate limiting, trust and the access log work per full source address for IPv6 as for IPv4 (the log shows `fd16:d833:4f0e:3ece:9000:9aaa:490a:3189`); IPv4-mapped addresses count as IPv4, sources in ::/32 are dropped. Not per /64: every host on a LAN shares one /64, so a manager's trust would have passed to all its neighbours (found in review before release).
- Diagnosis build `-DRFUPS_DIAG -Wl,--wrap=panic`: hardware watchdog, loop/hook breadcrumbs, hard-fault PC and panic message reported after the restart (docs/TESTING.md).

### Fixed
- With IPv6, lwIP's timer pool (`MEMP_NUM_SYS_TIMEOUT`) ran empty a few seconds after boot because mDNS keeps timers for both IP versions and the core sizes the pool without mDNS; lwIP then panicked. The pool has 12 more slots (~200 bytes).
- With IPv6, two SNMP requests arriving back to back crashed the device. The core's `UdpContext` marks a helper buffer between queued UDP packets with flags `0xFF`, which includes lwIP's `PBUF_FLAG_IS_CUSTOM`; IPv6 fragmentation turns custom buffers on, and freeing the helper then jumped to garbage. IPv6 fragmentation is off (`LWIP_IPV6_FRAG=0`), so custom buffers are off as in the IPv4 build; IPv6 SNMP replies are limited to 1452 bytes (one frame), larger GetBulk replies are shortened. Found with the diagnosis build (hard fault in `pbuf_free`).
- With IPv6, a burst of web page loads left only ~660 bytes of core 0's 4 KB stack unused; an overflow would silently overwrite core 1's stack. Each core now has its own 8 KB stack (`core1_separate_stack`, 8 KB of RAM); ~5.3 KB stay unused after the same burst.

### Notes
- Earlier docs said lwIP is precompiled. It is compiled from source with the core's `lwipopts.h` and our build flags; the DHCP option 224 and port 80 workarounds still work, but could be replaced by lwIP options (TODO).

## [2.2.0] - 2026-10-17

### Added
- **Trap destination from DHCP:** enter `dhcp` as the trap destination (menu option 4). The device asks for DHCP option 224 in its DHCP messages and sends traps to the address in the ACK for its current lease. Works with any DHCP server that has option 224 configured; tested with Kea 2.2.0 (configuration examples for Kea, ISC dhcpd and dnsmasq in docs/MIB.md). New pure library `lib/dhcp_option`, tested with frames captured from the device and from Kea. Option 224 is only taken from an ACK lwIP accepted (same transaction id and server, lwIP bound by that frame), so a forged ACK from another LAN host cannot redirect traps (found by an automated security review; HIL test sends one).
- **Web interface on/off:** serial menu option 8 and a "Turn off…" button on the page (with confirmation). Off closes TCP port 80, logs every browser out and stops the mDNS announcement; kept across restarts, turned on again by option 8 or a factory reset. Turning it on works at once, also right after the page turned itself off: the core's lwIP is built without SO_REUSE, so a just-closed web connection in TIME-WAIT would block port 80 for 2 minutes; the firmware drops those entries for port 80 before reopening it, and retries every second if opening still fails (found by the HIL suite).
- **Logo** (`logo/RF-canopy-RGB-Orange.svg`) in the top bar, on the login card and as the browser tab icon (`/favicon.svg`).
- Settings fields `trapFromDhcp` and `webEnabled` (optional; older files load unchanged, older firmware ignores them).
- Status screen: `Trap destination source` and `Web interface` lines; the trap destination shown is the one in use.

## [2.1.1] - 2026-10-17

### Changed
- Web page restyled with Bootstrap 5.3.3: dark navigation bar, status card with green/red badges for mains power and the Ethernet link, settings in cards, Bootstrap forms and alerts, two columns on wide screens and one on phones. Bootstrap's CSS is built into the firmware (31 KB gzip in flash, served at `/static/bootstrap-5.3.3.min.css`, cached for a year, no login needed), so no internet access is needed. `scripts/embed_bootstrap.py` regenerates it from a pinned, integrity-checked download.
- Content-Security-Policy now allows the device's own stylesheet and Bootstrap's `data:` images; still no scripts.

## [2.1.0] - 2026-10-17

### Added
- **Hostname:** one name for SNMP sysName, the DHCP hostname (option 12) and mDNS: the device answers as `<hostname>.local` and announces the web page as `_http._tcp`. Set in serial menu option 4 ("Hostname (sysName)") or on the web page ("Device name"); changes apply immediately (mDNS rename, DHCP lease renewal), no restart.
- Status screen and web page show the hostname and the `.local` name.

### Changed
- New sysName values must be valid hostnames (1-32 letters, digits and `-`). A free-text sysName from older firmware still loads; DHCP and mDNS then use a cleaned-up version of it.
- A login with an own web password hashes 20 rounds per loop pass instead of 40 (about 5 ms), for more margin to the 100 ms SNMP limit.

## [2.0.1] - 2026-10-17

### Security
- Serial menu option 7 replaces the device default web password with a new random one, for when the default password became known (the README of 2.0.0 showed the development device's real default password as an example). The old password stops working at once and browsers logged in with it are logged out.

### Fixed
- README: the example default password is made up.

## [2.0.0] - 2026-10-17

A new major version for the second management interface: the configuration web page with login. SNMP, the MIB, the serial menu and the settings files stay compatible: files from 1.x load unchanged, and older firmware ignores the new `webPassword` field (a downgrade only loses the own web password).

### Added
- **Web page** on `http://<device-ip>/` with login (user `admin`): status, DHCP/static IP (saves and restarts), SNMP community (active immediately), restart button, log out. Pure library `lib/web_ui` (HTTP parser, sessions, pages, SHA-256) with host tests; `src/web_service.cpp` serves it without ever blocking SNMP.
- **Web password:** a random 10-character default password per device, generated on first boot, stored in `data/web_default.txt` and shown only on the serial status screen; serial menu option 6 sets an own password (stored as a salted, iterated SHA-256 hash in the new optional settings field `webPassword`) or returns to the default.
- Web security: 30 s lockout after 5 failed logins, HttpOnly SameSite=Strict session cookie, CSRF token in every form, idle sessions end after 15 min, strict Content-Security-Policy, no framing; failed logins and lockouts in the access log.
- The own-password check (2000 SHA-256 rounds, ~0.45 s on the RP2040) runs 40 rounds per loop pass, so a login never delays SNMP (measured: worst 52 ms during a login, before the change 467 ms); found by a code review before release.
- Status screen: web login line and the core 0 stack high-water mark (`Core 0 stack: N bytes never used`).
- Excalidraw diagrams in `docs/diagrams/` (system overview, firmware architecture, SNMP request flow, mains flow, settings storage, test layers), editable `.excalidraw` files with SVG exports shown in the README and docs/TESTING.md.

### Changed
- `Settings` grew by the password hash, so every temporary copy of it on core 0's 4 KB stack became static (serial menu, web page, settings parser). Settings save/load measures 1120 bytes of stack on the device (budget 1536).

## [1.0.2] - 2026-10-17

LibreNMS support tested in a real LibreNMS, and the mains tests automated. The firmware itself is unchanged apart from the version number.

### Fixed
- LibreNMS temperature sensor: low limit set to -20 C. Without it LibreNMS put the low limit 10 C below the first reading, which alarmed when the room cooled down.

### Added
- Documentation overview in the README, a real status screen example, and the trap settings in the settings file description.
- `librenms/test/compose.yml`: a throw-away LibreNMS in Docker for checking the definitions. Tested with LibreNMS 26.9.1 against the device (see docs/LIBRENMS.md), including a real mains cut raising and clearing the "State Sensor Critical" alert.
- Automated mains HIL tests: with `RFUPS_MAINS_SWITCH=dirigera` the tests switch an IKEA Dirigera smart plug feeding the mains detector (`test/hil/mains_switch.py`, `dirigera_outlet.py`). Mains loss/restore over serial and SNMP and the mains traps in v1 and v2c now pass with real mains, plus a new test that a ~1 s outage shows in mainsPowerChanges.

## [1.0.1] - 2026-10-17

### Added
- IF-MIB `ifInDiscards`: received frames the driver dropped because lwIP had no free buffer. Counted by a small subclass of the core's W5500 driver (`CountingW5500` in `src/net_stats.h`).
- IF-MIB `ifInErrors`: received frames shorter than the Ethernet header or longer than 1514 bytes.
- IF-MIB `ifInUnknownProtos`: received frames with an EtherType lwIP does not handle (IPv6 while IPv6 is off, LLDP, spanning tree, VLAN-tagged).
- Status screen line `Ethernet drops` with these counters and the send errors.

### Changed
- Received frames that lwIP drops no longer count as unicast/multicast/broadcast packets (IF-MIB counts only delivered packets there); their octets still count in ifInOctets.

### Known limits
- The W5500 has no counters for CRC errors or frames lost when its own receive buffer is full, so those are not visible. ifOutDiscards stays 0.

## [1.0.0] - 2026-10-17

First release.

### Added
- **Ethernet** with the W5500 on the Arduino-Pico core's lwIP driver: DHCP (default) or static IP, lease renewal, boot without waiting for the lease, and when no DHCP server answers the device keeps retrying instead of falling back to another address. MAC address derived from the chip ID.
- **Serial menu** on USB (115200 8N1): status, DHCP, static IP with validation, SNMP settings, access log. Echo and backspace for terminals without local echo.
- **Settings** as JSON on LittleFS in two copies (A/B) with a sequence number and read-back check, so a power cut during a save never loses them. Factory defaults when no copy is valid; files from older firmware still load.
- **Mains power monitoring** on GPIO27: sampled at 1 kHz on core 1, debounced 5 ms, change detected within 10 ms, handed to core 0 through a sequence lock; logged on the serial port and shown on the status screen.
- **Factory reset** with the GPIO22 button: 10 s hold, non-blocking (sampled on core 1), LED feedback, ignored when held at boot.
- **SNMP agent** (own code) on UDP 161, read-only, SNMPv1 and SNMPv2c; SNMPv3 is dropped and counted.
  - Get, GetNext and in v2c GetBulk; v2c exceptions (noSuchObject, noSuchInstance, endOfMibView) and errors (notWritable, noCreation, tooBig with an empty list).
  - System group with sysName, sysContact and sysLocation set from the menu, active without a reboot.
  - RF-UPS-MONITOR-MIB (enterprise 63050): mainsPowerStatus, mainsPowerChanges, mainsPowerLastChange, boardTemperature; MIB file in `mibs/`, validated with net-snmp.
  - IF-MIB ifTable and ifXTable for the Ethernet port, with 64-bit counters (Counter64, v2c only) counted by the firmware for every frame, and link state and speed from the W5500 PHY.
  - Board temperature from the RP2040's internal sensor, in 0.1 °C.
- **Traps** in SNMPv1 or SNMPv2c (setting, default v2c): coldStart, authenticationFailure (at most one per 10 s), mainsPowerLost and mainsPowerRestored with the mains objects.
- **Security**: per-source rate limit (50/s, burst 100), a global limit for unauthenticated sources (100/s) so faked source addresses cannot bypass it, a bounded separate limit for sources that know the community (200/s, at most 4, expires after 10 minutes idle, revoked when the community changes), GetBulk replies capped at 16 objects against reflection, and an access log of rejected requests (menu option 5 and serial log).
- **LibreNMS** OS detection and discovery definitions in `librenms/` (mains state sensor, temperature sensor, change counter), see [docs/LIBRENMS.md](docs/LIBRENMS.md).
- **Tests**: native unit tests for every pure library, on-device tests, hardware-in-the-loop tests over the serial menu and net-snmp, AddressSanitizer/UBSan runs, the agent checked against the real net-snmp tools without hardware, and GitHub Actions CI. Test rules in `.clinerules`, guide in [docs/TESTING.md](docs/TESTING.md).

### Known limits
- The network stack queues 5 UDP packets; larger bursts are dropped (SNMP managers retry).
- Not yet tested with the real hardware: mains traps with real mains, the factory reset button, the "no link" and "no DHCP server" states. The LibreNMS definitions are validated against the schema but not yet tried in a running LibreNMS (done after 1.0.1).

[Unreleased]: https://github.com/TheKingOfChaos/RF-Power-Monitor/compare/v3.0.0...HEAD
[3.0.0]: https://github.com/TheKingOfChaos/RF-Power-Monitor/compare/v2.7.0...v3.0.0
[2.7.0]: https://github.com/TheKingOfChaos/RF-Power-Monitor/compare/v2.6.0...v2.7.0
[2.6.0]: https://github.com/TheKingOfChaos/RF-Power-Monitor/compare/v2.5.1...v2.6.0
[2.5.1]: https://github.com/TheKingOfChaos/RF-Power-Monitor/compare/v2.5.0...v2.5.1
[2.5.0]: https://github.com/TheKingOfChaos/RF-Power-Monitor/compare/v2.4.2...v2.5.0
[2.4.2]: https://github.com/TheKingOfChaos/RF-Power-Monitor/compare/v2.4.1...v2.4.2
[2.4.1]: https://github.com/TheKingOfChaos/RF-Power-Monitor/compare/v2.4.0...v2.4.1
[2.4.0]: https://github.com/TheKingOfChaos/RF-Power-Monitor/compare/v2.3.0...v2.4.0
[2.3.0]: https://github.com/TheKingOfChaos/RF-Power-Monitor/compare/v2.2.0...v2.3.0
[2.2.0]: https://github.com/TheKingOfChaos/RF-Power-Monitor/compare/v2.1.1...v2.2.0
[2.1.1]: https://github.com/TheKingOfChaos/RF-Power-Monitor/compare/v2.1.0...v2.1.1
[2.1.0]: https://github.com/TheKingOfChaos/RF-Power-Monitor/compare/v2.0.1...v2.1.0
[2.0.1]: https://github.com/TheKingOfChaos/RF-Power-Monitor/compare/v2.0.0...v2.0.1
[2.0.0]: https://github.com/TheKingOfChaos/RF-Power-Monitor/compare/v1.0.2...v2.0.0
[1.0.2]: https://github.com/TheKingOfChaos/RF-Power-Monitor/compare/v1.0.1...v1.0.2
[1.0.1]: https://github.com/TheKingOfChaos/RF-Power-Monitor/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/TheKingOfChaos/RF-Power-Monitor/releases/tag/v1.0.0
